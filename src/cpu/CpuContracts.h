#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/contracts/Determinism.h"
#include "core/contracts/IFlowSource.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"

namespace vkpt::cpu {

enum class CpuPathTracerLifecycle : std::uint8_t {
  Uninitialized,
  Configured,
  SceneLoaded,
  Ready,
  Failed,
  ShuttingDown,
};

inline const char* CpuPathTracerLifecycleName(CpuPathTracerLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case CpuPathTracerLifecycle::Uninitialized:
      return "uninitialized";
    case CpuPathTracerLifecycle::Configured:
      return "configured";
    case CpuPathTracerLifecycle::SceneLoaded:
      return "scene_loaded";
    case CpuPathTracerLifecycle::Ready:
      return "ready";
    case CpuPathTracerLifecycle::Failed:
      return "failed";
    case CpuPathTracerLifecycle::ShuttingDown:
      return "shutting_down";
  }
  return "unknown";
}

struct CpuPathTracerStatus {
  std::string name = "cpu";
  std::string backend = "tiled-cpu";
  CpuPathTracerLifecycle lifecycle = CpuPathTracerLifecycle::Uninitialized;
  vkpt::core::contracts::SubsystemHealth health =
      vkpt::core::contracts::SubsystemHealth::Ok;
  std::string health_reason = "ok";
  std::string kernel = "scalar";
  bool configured = false;
  bool scene_loaded = false;
  bool accel_valid = false;
  bool ready_to_render = false;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  std::uint64_t determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  std::uint64_t current_flow_id = 0u;
  std::uint32_t current_sample = 0u;
  std::uint64_t total_samples = 0u;
  std::uint64_t total_rays = 0u;
  std::size_t worker_count = 0u;
  std::uint32_t tile_height = 0u;
  std::size_t tile_count = 0u;
  std::uint64_t last_tile_us_p99 = 0u;
  std::uint64_t last_build_us = 0u;
  std::string last_error;
};

using CpuPathStatus = CpuPathTracerStatus;

struct CpuPathTracerStateTransitionContract {
  CpuPathTracerLifecycle from = CpuPathTracerLifecycle::Uninitialized;
  const char* operation = "";
  CpuPathTracerLifecycle to = CpuPathTracerLifecycle::Uninitialized;
  const char* postcondition = "";
};

struct CpuPathTracerStandardContract {
  std::string schema_version = "cpu.pathtracer.contract.v1";
  bool status_api = true;
  bool operations_return_status = true;
  bool exposes_determinism_context = true;
  bool naming_uses_cpu_path_tracer_status = true;
  std::array<CpuPathTracerStateTransitionContract, 5> state_machine{{
      {CpuPathTracerLifecycle::Uninitialized,
       "configure",
       CpuPathTracerLifecycle::Configured,
       "render settings, film storage, and CPU worker topology are initialized"},
      {CpuPathTracerLifecycle::Configured,
       "load_scene_snapshot",
       CpuPathTracerLifecycle::SceneLoaded,
       "immutable path-tracer scene data is loaded; acceleration is invalidated"},
      {CpuPathTracerLifecycle::SceneLoaded,
       "build_or_update_acceleration",
       CpuPathTracerLifecycle::Ready,
       "shared CPU acceleration is valid and tiles are ready to render"},
      {CpuPathTracerLifecycle::Ready,
       "render_tile",
       CpuPathTracerLifecycle::Ready,
       "one tile of work is accumulated without mutating lifecycle state"},
      {CpuPathTracerLifecycle::Ready,
       "shutdown",
       CpuPathTracerLifecycle::Uninitialized,
       "tile workers are stopped and scene/accumulation state is released"},
  }};
};

inline CpuPathTracerStandardContract BuildStandardCpuPathTracerContract() {
  return {};
}

inline bool ValidateStandardCpuPathTracerContract(
    const CpuPathTracerStandardContract& contract,
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

  require(contract.schema_version == "cpu.pathtracer.contract.v1",
          "unexpected CPU path tracer contract schema version");
  require(contract.status_api,
          "CPU path tracer contract must expose status()");
  require(contract.operations_return_status,
          "CPU path tracer configure/load/build operations must return Status");
  require(contract.exposes_determinism_context,
          "CPU path tracer contract must expose DeterminismContext");
  require(contract.naming_uses_cpu_path_tracer_status,
          "CPU status type must use CpuPathTracerStatus as the canonical name");
  require(contract.state_machine.size() == 5u,
          "CPU path tracer state machine must publish five transitions");
  require(contract.state_machine[0].from == CpuPathTracerLifecycle::Uninitialized &&
              std::string_view(contract.state_machine[0].operation) == "configure" &&
              contract.state_machine[0].to == CpuPathTracerLifecycle::Configured,
          "CPU path tracer state machine missing configure transition");
  require(contract.state_machine[1].from == CpuPathTracerLifecycle::Configured &&
              std::string_view(contract.state_machine[1].operation) == "load_scene_snapshot" &&
              contract.state_machine[1].to == CpuPathTracerLifecycle::SceneLoaded,
          "CPU path tracer state machine missing scene load transition");
  require(contract.state_machine[2].from == CpuPathTracerLifecycle::SceneLoaded &&
              std::string_view(contract.state_machine[2].operation) == "build_or_update_acceleration" &&
              contract.state_machine[2].to == CpuPathTracerLifecycle::Ready,
          "CPU path tracer state machine missing acceleration transition");
  require(contract.state_machine[4].from == CpuPathTracerLifecycle::Ready &&
              std::string_view(contract.state_machine[4].operation) == "shutdown" &&
              contract.state_machine[4].to == CpuPathTracerLifecycle::Uninitialized,
          "CPU path tracer state machine missing shutdown transition");

  if (ok && diagnostics) {
    diagnostics->push_back("standard CPU path tracer contract is valid");
  }
  return ok;
}

inline vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const CpuPathTracerStatus& status) {
  auto out = vkpt::core::contracts::MakeSubsystemStatus(status.name, status.health);
  out.last_error = status.last_error;
  out.set_custom("backend", status.backend);
  out.set_custom("lifecycle", CpuPathTracerLifecycleName(status.lifecycle));
  out.set_custom("health_reason", status.health_reason);
  out.set_custom("kernel", status.kernel);
  out.set_custom("ready_to_render", status.ready_to_render ? "true" : "false");
  out.set_custom("deterministic", status.deterministic ? "true" : "false");
  out.set_custom("flow_id", std::to_string(status.current_flow_id));
  out.set_custom("worker_count", std::to_string(status.worker_count));
  out.set_custom("tile_count", std::to_string(status.tile_count));
  out.set_custom("last_build_us", std::to_string(status.last_build_us));
  out.set_custom("last_tile_us_p99", std::to_string(status.last_tile_us_p99));
  return out;
}

inline vkpt::core::health::Report EvaluateCpuPathTracerHealth(
    const CpuPathTracerStatus& status) {
  if (status.lifecycle == CpuPathTracerLifecycle::Failed) {
    return {vkpt::core::health::Status::Failed,
            status.last_error.empty() ? "CPU path tracer failed" : status.last_error};
  }
  if (status.configured && status.scene_loaded && !status.accel_valid) {
    return {vkpt::core::health::Status::Degraded,
            "CPU path tracer has scene data but acceleration is not valid"};
  }
  return {vkpt::core::health::Status::Ok, status.ready_to_render ? "ready" : "idle"};
}

template <typename StatusFn>
std::shared_ptr<vkpt::core::health::IHealthProbe>
CreateCpuPathTracerHealthProbe(StatusFn status_fn) {
  class CpuPathTracerHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit CpuPathTracerHealthProbe(StatusFn fn) : m_statusFn(std::move(fn)) {}

    std::string name() const override { return "cpu"; }

    vkpt::core::health::Report check() override {
      return EvaluateCpuPathTracerHealth(m_statusFn());
    }

   private:
    StatusFn m_statusFn;
  };

  return std::make_shared<CpuPathTracerHealthProbe>(std::move(status_fn));
}

inline std::uint64_t CurrentFlowId(
    const vkpt::core::contracts::IFlowSource* flow_source) noexcept {
  return flow_source != nullptr ? flow_source->current_flow_id() : 0u;
}

}  // namespace vkpt::cpu
