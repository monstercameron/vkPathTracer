#pragma once

#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "core/log/Log.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vkpt::gpu {

struct GpuBackendIntrospection {
  std::string adapter_name;
  std::uint64_t vram_bytes_used = 0;
  std::uint64_t vram_bytes_total = 0;
  std::uint32_t pending_dispatches = 0;
  std::uint64_t last_present_us = 0;
  std::uint64_t last_fence_wait_us = 0;
  std::uint64_t timeline_value = 0;
  bool device_lost_recent = false;
  bool fence_timeout_recent = false;
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Ready;
  std::uint64_t current_flow_id = 0;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  std::string last_error;

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    deterministic = context.enabled;
    determinism_base_seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                               determinism_base_seed,
                                               determinism_frame_index,
                                               determinism_scenario_id);
  }
};

using GpuBackendStatus = GpuBackendIntrospection;

inline vkpt::core::health::Report GpuHealthReportFromIntrospection(
    const GpuBackendIntrospection& info);

struct GpuBackendStateTransitionContract {
  vkpt::core::contracts::ComponentLifecycle from =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  const char* operation = "";
  vkpt::core::contracts::ComponentLifecycle to =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  const char* postcondition = "";
};

struct GpuBackendContract {
  std::string schema_version = "gpu.backend.contract.v1";
  bool operations_return_status = true;
  bool exposes_determinism_context = true;
  bool naming_uses_gpu_backend_status = true;
  std::array<GpuBackendStateTransitionContract, 5> state_machine{{
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       "initialize",
       vkpt::core::contracts::ComponentLifecycle::Initializing,
       "device selection and API objects are created or a typed Status failure is returned"},
      {vkpt::core::contracts::ComponentLifecycle::Initializing,
       "configure",
       vkpt::core::contracts::ComponentLifecycle::Initializing,
       "render settings, determinism context, and film resources are current"},
      {vkpt::core::contracts::ComponentLifecycle::Initializing,
       "load_scene_snapshot",
       vkpt::core::contracts::ComponentLifecycle::Initializing,
       "CPU scene snapshot is retained; GPU acceleration is not yet ready"},
      {vkpt::core::contracts::ComponentLifecycle::Initializing,
       "build_or_update_acceleration",
       vkpt::core::contracts::ComponentLifecycle::Ready,
       "GPU scene buffers and acceleration resources are ready for dispatch"},
      {vkpt::core::contracts::ComponentLifecycle::Ready,
       "shutdown",
       vkpt::core::contracts::ComponentLifecycle::ShuttingDown,
       "queued GPU work is drained before resources are released"},
  }};
};

inline GpuBackendContract BuildStandardGpuBackendContract() {
  return {};
}

inline bool ValidateStandardGpuBackendContract(
    const GpuBackendContract& contract,
    std::vector<std::string>* diagnostics = nullptr) {
  if (diagnostics) {
    diagnostics->clear();
  }
  bool ok = true;
  auto require = [&](bool condition, const char* message) {
    if (!condition) {
      ok = false;
      if (diagnostics) {
        diagnostics->push_back(message);
      }
    }
  };

  require(contract.schema_version == "gpu.backend.contract.v1",
          "unexpected GPU backend contract schema version");
  require(contract.operations_return_status,
          "GPU backend operational failures must be available as Status");
  require(contract.exposes_determinism_context,
          "GPU backend status must expose DeterminismContext");
  require(contract.naming_uses_gpu_backend_status,
          "GPU status type must use GpuBackendStatus as the canonical name");
  require(contract.state_machine.size() == 5u,
          "GPU backend state machine must publish five transitions");
  require(std::string_view(contract.state_machine[0].operation) == "initialize",
          "GPU backend state machine missing initialize transition");
  require(std::string_view(contract.state_machine[1].operation) == "configure",
          "GPU backend state machine missing configure transition");
  require(std::string_view(contract.state_machine[2].operation) == "load_scene_snapshot",
          "GPU backend state machine missing scene load transition");
  require(std::string_view(contract.state_machine[3].operation) ==
              "build_or_update_acceleration",
          "GPU backend state machine missing acceleration transition");
  require(std::string_view(contract.state_machine[4].operation) == "shutdown",
          "GPU backend state machine missing shutdown transition");

  if (ok && diagnostics) {
    diagnostics->push_back("standard GPU backend contract is valid");
  }
  return ok;
}

inline vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const GpuBackendStatus& status,
    std::string_view name = "gpu") {
  const auto health = GpuHealthReportFromIntrospection(status);
  auto subsystem = vkpt::core::contracts::MakeSubsystemStatus(
      name,
      health.status == vkpt::core::health::Status::Failed
          ? vkpt::core::contracts::SubsystemHealth::Failed
          : (health.status == vkpt::core::health::Status::Degraded
                 ? vkpt::core::contracts::SubsystemHealth::Degraded
                 : vkpt::core::contracts::SubsystemHealth::Ok));
  subsystem.last_error = status.last_error;
  subsystem.last_tick_ns = status.last_present_us;
  subsystem.ticks_total = status.timeline_value;
  subsystem.set_custom("adapter", status.adapter_name);
  subsystem.set_custom("lifecycle",
                       vkpt::core::contracts::ComponentLifecycleName(status.lifecycle));
  subsystem.set_custom("current_flow_id", std::to_string(status.current_flow_id));
  subsystem.set_custom("deterministic", status.deterministic ? "true" : "false");
  subsystem.set_custom("determinism_base_seed",
                       std::to_string(status.determinism_base_seed));
  subsystem.set_custom("determinism_frame_index",
                       std::to_string(status.determinism_frame_index));
  subsystem.set_custom("determinism_scenario_id", status.determinism_scenario_id);
  return subsystem;
}

inline vkpt::core::Status GpuBackendOperationStatus(
    std::string_view operation,
    bool succeeded,
    std::string_view last_error,
    vkpt::core::Status::Code error_code = vkpt::core::Status::Code::InternalError) {
  if (succeeded) {
    return vkpt::core::Status::ok(std::string(operation) + " succeeded");
  }
  std::string message(last_error);
  if (message.empty()) {
    message = std::string(operation) + " failed";
  }
  return vkpt::core::Status::error(error_code, std::move(message));
}

inline vkpt::core::health::Report GpuHealthReportFromIntrospection(
    const GpuBackendIntrospection& info) {
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;
  if (info.lifecycle == vkpt::core::contracts::ComponentLifecycle::Failed ||
      !info.last_error.empty()) {
    return Report{Status::Failed,
                  info.last_error.empty() ? "GPU backend failed"
                                          : info.last_error};
  }
  if (info.device_lost_recent || info.fence_timeout_recent) {
    return Report{Status::Failed, "device lost or fence timeout within 5s"};
  }
  if (info.vram_bytes_total > 0 &&
      info.vram_bytes_used * 10ull >= info.vram_bytes_total * 9ull) {
    return Report{Status::Degraded, "VRAM usage is above 90 percent"};
  }
  return Report{Status::Ok, "ok"};
}

class IGpuBackendIntrospect {
 public:
  virtual ~IGpuBackendIntrospect() = default;
  virtual GpuBackendIntrospection introspect() const = 0;
};

inline void EmitGpuBackendConfig(std::string_view backend,
                                 const GpuBackendIntrospection& info) {
  VKP_LIFECYCLE_CONFIG("gpu",
                       "backend",
                       backend,
                       "adapter",
                       info.adapter_name,
                       "flow_id",
                       info.current_flow_id,
                       "vram_total_bytes",
                       info.vram_bytes_total);
}

inline void EmitGpuBackendStarted(std::string_view backend,
                                  const GpuBackendIntrospection& info) {
  VKP_LIFECYCLE_STARTED("gpu",
                        "backend",
                        backend,
                        "adapter",
                        info.adapter_name,
                        "flow_id",
                        info.current_flow_id,
                        "timeline",
                        info.timeline_value);
}

inline void EmitGpuBackendStopped(std::string_view backend,
                                  const GpuBackendIntrospection& info) {
  VKP_LIFECYCLE_STOPPED("gpu",
                        "backend",
                        backend,
                        "adapter",
                        info.adapter_name,
                        "flow_id",
                        info.current_flow_id,
                        "pending_dispatches",
                        info.pending_dispatches);
}

inline void EmitGpuBackendAnomaly(std::string_view backend,
                                  std::string_view operation,
                                  std::string_view reason,
                                  const GpuBackendIntrospection& info) {
  VKP_LOG(Warn,
          "gpu",
          "operation_failed",
          "backend",
          backend,
          "operation",
          operation,
          "reason",
          reason,
          "flow_id",
          info.current_flow_id);
}

inline std::shared_ptr<vkpt::core::health::IHealthProbe>
CreateGpuBackendHealthProbe(const IGpuBackendIntrospect& backend,
                            std::string name = "gpu") {
  class GpuBackendHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    GpuBackendHealthProbe(const IGpuBackendIntrospect& backend, std::string name)
        : m_backend(&backend), m_name(std::move(name)) {}

    std::string name() const override { return m_name; }

    vkpt::core::health::Report check() override {
      if (m_backend == nullptr) {
        return {vkpt::core::health::Status::Failed,
                "GPU backend probe has no backend"};
      }
      return GpuHealthReportFromIntrospection(m_backend->introspect());
    }

   private:
    const IGpuBackendIntrospect* m_backend = nullptr;
    std::string m_name;
  };

  return std::make_shared<GpuBackendHealthProbe>(backend, std::move(name));
}

}  // namespace vkpt::gpu
