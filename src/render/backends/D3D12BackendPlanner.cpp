#include "render/backends/D3D12Backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vkpt::render {
namespace {

std::uint32_t SelectPlannerWorkerThreads(const AcceleratorCapabilities& accel) {
  if (!accel.cpu) {
    return 1u;
  }
  const auto logical = std::max<std::uint32_t>(1u, accel.node_count);
  if (logical <= 4u) {
    return 1u;
  }
  return std::max<std::uint32_t>(1u, std::min(logical / 2u, logical - 2u));
}

double EstimatePlannerRaysPerMs(const AcceleratorCapabilities& accel) {
  if (!accel.cpu) {
    return accel.estimated_rays_per_ms;
  }
  const auto logical = std::max<std::uint32_t>(1u, accel.node_count);
  const auto workers = SelectPlannerWorkerThreads(accel);
  return accel.estimated_rays_per_ms * (static_cast<double>(workers) / static_cast<double>(logical));
}

int AutoAcceleratorPriority(const AcceleratorCapabilities& accel) {
  switch (accel.accelerator_kind) {
    case AcceleratorKind::DiscreteGpu:
      return 300;
    case AcceleratorKind::IntegratedGpu:
      return 200;
    case AcceleratorKind::Cpu:
      return 100;
    case AcceleratorKind::Warp:
      return 10;
    default:
      return 0;
  }
}

std::uint64_t RoundDownToBatch(std::uint64_t rays, std::uint64_t batch) {
  if (batch == 0u) {
    return rays;
  }
  return (rays / batch) * batch;
}


}  // namespace

RayBudgetPlan BuildD3D12RayBudgetPlan(const RayBudgetRequest& request) {
  RayBudgetPlan plan;
  plan.polygon_frame_budget_ms = request.polygon_frame_budget_ms;
  plan.reserved_polygon_ms = request.reserved_polygon_ms;
  plan.merge_budget_ms = request.merge_budget_ms;
  plan.width = request.width;
  plan.height = request.height;
  plan.ray_budget_ms = std::max(0.0,
                                request.polygon_frame_budget_ms -
                                    std::max(0.0, request.reserved_polygon_ms) -
                                    std::max(0.0, request.merge_budget_ms));

  if (request.width == 0u || request.height == 0u) {
    plan.diagnostics.push_back("invalid render dimensions; ray targets cannot be converted to samples per pixel");
  }
  if (plan.ray_budget_ms <= 0.0) {
    plan.diagnostics.push_back("no ray budget remains after polygon and merge reservations");
  }
  plan.diagnostics.push_back("ray rates are conservative planning estimates until calibrated per accelerator");
  if (request.accelerator_preset == AcceleratorSelectionPreset::Auto) {
    plan.diagnostics.push_back("auto preset selects one accelerator by priority: discrete GPU, integrated GPU, CPU");
  } else {
    plan.diagnostics.push_back("high-performance preset selects every eligible accelerator; WARP remains opt-in");
  }

  auto accelerators = EnumerateD3D12Accelerators(request.include_cpu, request.include_warp);
  std::sort(accelerators.begin(), accelerators.end(), [](const AcceleratorCapabilities& lhs,
                                                         const AcceleratorCapabilities& rhs) {
    const int lhs_priority = AutoAcceleratorPriority(lhs);
    const int rhs_priority = AutoAcceleratorPriority(rhs);
    if (lhs_priority != rhs_priority) {
      return lhs_priority > rhs_priority;
    }
    const double lhs_rate = EstimatePlannerRaysPerMs(lhs);
    const double rhs_rate = EstimatePlannerRaysPerMs(rhs);
    if (lhs_rate != rhs_rate) {
      return lhs_rate > rhs_rate;
    }
    return lhs.name < rhs.name;
  });

  const auto inactive_reason = [&](const AcceleratorCapabilities& accel) -> std::string {
    if (!accel.available) {
      return "inactive: accelerator unavailable";
    }
    if (!accel.compute) {
      return "inactive: compute queue/backend unavailable";
    }
    if (accel.cpu && !request.include_cpu) {
      return "inactive: CPU participation disabled by request";
    }
    if (accel.accelerator_kind == AcceleratorKind::IntegratedGpu && !request.include_integrated_gpu) {
      return "inactive: integrated GPU participation disabled by request";
    }
    if (accel.warp && !request.include_warp) {
      return "inactive: WARP software adapter disabled by request";
    }
    if (request.require_ray_tracing && !accel.ray_tracing) {
      return "inactive: DXR required but unavailable";
    }
    if (plan.ray_budget_ms <= 0.0) {
      return "inactive: no ray time remains in frame budget";
    }
    if (EstimatePlannerRaysPerMs(accel) <= 0.0) {
      return "inactive: no ray throughput estimate";
    }
    const auto raw_target = static_cast<std::uint64_t>(
        std::max(0.0, std::floor(EstimatePlannerRaysPerMs(accel) * plan.ray_budget_ms)));
    const auto target = RoundDownToBatch(raw_target, request.min_rays_per_batch);
    if (target == 0u && !(raw_target > 0u && request.min_rays_per_batch == 0u)) {
      return "inactive: estimated ray count is below the minimum batch size";
    }
    return {};
  };

  std::string auto_selected_id;
  if (request.accelerator_preset == AcceleratorSelectionPreset::Auto) {
    for (const auto& accel : accelerators) {
      if (inactive_reason(accel).empty()) {
        auto_selected_id = accel.id;
        break;
      }
    }
  }

  for (const auto& accel : accelerators) {
    RayBudgetAssignment assignment;
    assignment.accelerator_id = accel.id;
    assignment.accelerator_name = accel.name;
    assignment.accelerator_kind = accel.accelerator_kind;
    assignment.backend_kind = accel.backend_kind;
    assignment.backend_name = accel.backend_caps.backend_name;
    assignment.uses_dxr = accel.ray_tracing && accel.d3d12;
    assignment.worker_threads = SelectPlannerWorkerThreads(accel);
    assignment.budget_ms = plan.ray_budget_ms;
    assignment.estimated_rays_per_ms = EstimatePlannerRaysPerMs(accel);

    const auto rejection = inactive_reason(accel);
    if (!rejection.empty()) {
      assignment.reason = rejection;
    } else if (request.accelerator_preset == AcceleratorSelectionPreset::Auto &&
               accel.id != auto_selected_id) {
      assignment.reason = "inactive: auto preset selected a higher-priority accelerator";
    } else {
      const auto raw_target = static_cast<std::uint64_t>(
          std::max(0.0, std::floor(assignment.estimated_rays_per_ms * plan.ray_budget_ms)));
      assignment.target_rays = RoundDownToBatch(raw_target, request.min_rays_per_batch);
      if (assignment.target_rays == 0u && raw_target > 0u && request.min_rays_per_batch == 0u) {
        assignment.target_rays = raw_target;
      }
      if (assignment.target_rays == 0u) {
        assignment.reason = "inactive: estimated ray count is below the minimum batch size";
      } else {
        assignment.active = true;
        if (request.accelerator_preset == AcceleratorSelectionPreset::Auto) {
          assignment.reason = "active: auto preset selected this accelerator";
        } else {
          assignment.reason = accel.cpu
              ? "active: CPU worker count is capped to leave cores for polygon/raster work"
              : "active: high-performance preset selected this accelerator";
        }
        plan.total_target_rays += assignment.target_rays;
      }
    }

    plan.assignments.push_back(std::move(assignment));
  }

  const auto pixels = static_cast<std::uint64_t>(request.width) * static_cast<std::uint64_t>(request.height);
  if (pixels > 0u) {
    plan.estimated_samples_per_pixel = static_cast<double>(plan.total_target_rays) / static_cast<double>(pixels);
  }
  if (plan.total_target_rays == 0u) {
    plan.diagnostics.push_back("no accelerator received work under the current budget/filter settings");
  }
  return plan;
}


}  // namespace vkpt::render
