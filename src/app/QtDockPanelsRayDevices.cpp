#include "app/QtDockPanelsInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vkpt::app {

std::string QtDockRayDeviceKeyForAccelerator(const vkpt::render::AcceleratorCapabilities& accel) {
  if (!accel.id.empty()) {
    return accel.id;
  }
  std::ostringstream out;
  out << vkpt::render::BackendKindToString(accel.backend_kind) << ':'
      << vkpt::render::AcceleratorKindToString(accel.accelerator_kind) << ':'
      << accel.vendor_id << ':' << accel.device_id << ':' << accel.name;
  return out.str();
}

std::string QtDockFallbackRayDeviceKey(std::string_view selected_backend,
                                       std::string_view renderer_path) {
  return std::string(selected_backend) + ":" + std::string(renderer_path);
}

std::string QtDockActiveRayDeviceKey(const QtDockDeviceStats& device_stats) {
  if (device_stats.has_selected_accelerator) {
    return QtDockRayDeviceKeyForAccelerator(device_stats.selected_accelerator);
  }
  if (!device_stats.active_device_key.empty()) {
    return device_stats.active_device_key;
  }
  return QtDockFallbackRayDeviceKey(device_stats.selected_backend, device_stats.active_renderer_path);
}

std::string QtDockActiveRayDeviceName(const QtDockDeviceStats& device_stats) {
  if (device_stats.has_selected_accelerator && !device_stats.selected_accelerator.name.empty()) {
    return device_stats.selected_accelerator.name;
  }
  if (!device_stats.backend_caps.backend_name.empty() && device_stats.backend_caps.backend_name != "unknown") {
    return device_stats.backend_caps.backend_name;
  }
  if (!device_stats.active_renderer_path.empty()) {
    return device_stats.active_renderer_path;
  }
  return device_stats.selected_backend.empty() ? "unknown" : device_stats.selected_backend;
}

const QtDockRayDeviceMetric* QtDockFindRayMetric(
    const std::vector<QtDockRayDeviceMetric>& metrics,
    std::string_view device_key) {
  const auto it = std::find_if(metrics.begin(), metrics.end(), [&](const QtDockRayDeviceMetric& metric) {
    return metric.device_key == device_key;
  });
  return it == metrics.end() ? nullptr : &*it;
}

void QtDockUpsertRayMetric(std::vector<QtDockRayDeviceMetric>& metrics,
                           QtDockRayDeviceMetric metric) {
  auto it = std::find_if(metrics.begin(), metrics.end(), [&](const QtDockRayDeviceMetric& existing) {
    return existing.device_key == metric.device_key;
  });
  if (it == metrics.end()) {
    metrics.push_back(std::move(metric));
  } else {
    *it = std::move(metric);
  }
}

bool QtDockRayMetricHasRate(const QtDockRayDeviceMetric* metric) {
  return metric != nullptr &&
         (metric->instant_rays_per_second > 0.0 ||
          metric->rolling_rays_per_second > 0.0 ||
          metric->accumulated_rays_per_second > 0.0);
}

void QtDockAppendRayMetricTags(std::vector<std::string>& tags,
                               const QtDockRayDeviceMetric* metric) {
  if (!QtDockRayMetricHasRate(metric)) {
    tags.push_back("no ray counter");
    return;
  }
  if (metric->rolling_rays_per_second > 0.0) {
    tags.push_back("rolling " + QtDockRate(metric->rolling_rays_per_second));
  } else {
    tags.push_back("instant " + QtDockRate(metric->instant_rays_per_second));
  }
  tags.push_back("samples " + std::to_string(metric->sample_count));
}

std::string QtDockShortAcceleratorKind(const vkpt::render::AcceleratorCapabilities& accel) {
  switch (accel.accelerator_kind) {
    case vkpt::render::AcceleratorKind::DiscreteGpu:
      return "dGPU";
    case vkpt::render::AcceleratorKind::IntegratedGpu:
      return "iGPU";
    case vkpt::render::AcceleratorKind::Cpu:
      return "CPU";
    case vkpt::render::AcceleratorKind::Warp:
      return "WARP";
    case vkpt::render::AcceleratorKind::VirtualGpu:
      return "vGPU";
    default:
      return "unknown";
  }
}

bool QtDockSameAccelerator(const vkpt::render::AcceleratorCapabilities& lhs,
                           const vkpt::render::AcceleratorCapabilities& rhs) {
  if (!lhs.id.empty() || !rhs.id.empty()) {
    return lhs.id == rhs.id;
  }
  return lhs.backend_kind == rhs.backend_kind &&
         lhs.accelerator_kind == rhs.accelerator_kind &&
         lhs.vendor_id == rhs.vendor_id &&
         lhs.device_id == rhs.device_id &&
         lhs.name == rhs.name;
}

bool QtDockRayDeviceEligible(const vkpt::render::AcceleratorCapabilities& accel) {
  return accel.available && accel.compute && !accel.warp;
}

std::uint64_t QtDockAcceleratorMemoryBytes(const vkpt::render::AcceleratorCapabilities& accel) {
  return accel.dedicated_video_memory_bytes != 0u
      ? accel.dedicated_video_memory_bytes
      : accel.shared_system_memory_bytes;
}

std::string QtDockAcceleratorSummary(const vkpt::render::AcceleratorCapabilities& accel,
                                     bool active,
                                     const QtDockRayDeviceMetric* metric) {
  std::vector<std::string> tags;
  tags.push_back(QtDockShortAcceleratorKind(accel));
  if (active) {
    tags.push_back("active");
  }
  if (accel.ray_tracing) {
    tags.push_back("DXR");
  }
  if (const auto memory = QtDockAcceleratorMemoryBytes(accel); memory != 0u) {
    tags.push_back(QtDockBytes(memory));
  }
  QtDockAppendRayMetricTags(tags, metric);
  return accel.name + " (" + QtDockJoin(tags, ", ") + ")";
}

std::string QtDockAcceleratorGroupSummary(
    const std::vector<const vkpt::render::AcceleratorCapabilities*>& accelerators,
    const std::vector<QtDockRayDeviceMetric>& metrics,
    std::size_t maxRows) {
  if (accelerators.empty()) {
    return "No eligible ray devices";
  }
  std::ostringstream out;
  const auto limit = std::min(maxRows, accelerators.size());
  for (std::size_t i = 0; i < limit; ++i) {
    if (i > 0) {
      out << " | ";
    }
    const auto* metric = QtDockFindRayMetric(metrics, QtDockRayDeviceKeyForAccelerator(*accelerators[i]));
    out << QtDockAcceleratorSummary(*accelerators[i], false, metric);
  }
  if (accelerators.size() > limit) {
    out << " | +" << (accelerators.size() - limit) << " more";
  }
  return out.str();
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
