#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "core/contracts/Lifecycle.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "pathtracer/PathTracer.h"

namespace vkpt::pathtracer::observability {

inline std::uint64_t NowUs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

inline std::uint64_t ElapsedUsSince(std::uint64_t start_us) {
  const auto end_us = NowUs();
  return end_us >= start_us ? end_us - start_us : 0u;
}

inline std::uint64_t Delta(std::uint64_t before, std::uint64_t after) {
  return after >= before ? after - before : 0u;
}

inline void ObservePathTracerSampleUs(std::uint64_t sample_us) {
  VKP_METRIC_OBSERVE("vkp.pathtracer.sample_us", sample_us);
}

inline vkpt::core::contracts::ComponentLifecycle ToComponentLifecycle(
    PathTracerLifecycle lifecycle) {
  using vkpt::core::contracts::ComponentLifecycle;
  switch (lifecycle) {
    case PathTracerLifecycle::Configured:
    case PathTracerLifecycle::SceneLoaded:
      return ComponentLifecycle::Initializing;
    case PathTracerLifecycle::Ready:
      return ComponentLifecycle::Ready;
    case PathTracerLifecycle::Failed:
      return ComponentLifecycle::Failed;
    case PathTracerLifecycle::Uninitialized:
      return ComponentLifecycle::Uninitialized;
  }
  return ComponentLifecycle::Failed;
}

inline vkpt::core::health::Report EvaluatePathTracerHealth(
    const PathTracerStatus& status) {
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;
  if (status.lifecycle == PathTracerLifecycle::Failed ||
      !status.last_error.empty()) {
    return Report{Status::Failed,
                  status.last_error.empty() ? "path tracer failed"
                                            : status.last_error};
  }
  if (status.scene_loaded && !status.accel_valid) {
    return Report{Status::Degraded, "scene loaded without valid acceleration"};
  }
  return Report{Status::Ok, "ok"};
}

inline vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const PathTracerStatus& status) {
  const auto health = EvaluatePathTracerHealth(status);
  vkpt::core::contracts::SubsystemHealth subsystemHealth =
      vkpt::core::contracts::SubsystemHealth::Ok;
  if (health.status == vkpt::core::health::Status::Degraded) {
    subsystemHealth = vkpt::core::contracts::SubsystemHealth::Degraded;
  } else if (health.status == vkpt::core::health::Status::Failed) {
    subsystemHealth = vkpt::core::contracts::SubsystemHealth::Failed;
  }
  auto out = vkpt::core::contracts::MakeSubsystemStatus("pathtracer",
                                                       subsystemHealth);
  out.last_error = status.last_error;
  out.set_custom("backend", status.backend);
  out.set_custom("lifecycle", ToString(status.lifecycle));
  out.set_custom("scene_loaded", status.scene_loaded ? "true" : "false");
  out.set_custom("accel_valid", status.accel_valid ? "true" : "false");
  out.set_custom("ready_to_render", status.ready_to_render ? "true" : "false");
  out.set_custom("current_sample", std::to_string(status.current_sample));
  out.set_custom("total_samples", std::to_string(status.total_samples));
  out.set_custom("accumulation_gen", std::to_string(status.accumulation_gen));
  return out;
}

template <typename StatusFn>
std::shared_ptr<vkpt::core::health::IHealthProbe> CreatePathTracerHealthProbe(
    StatusFn status_fn) {
  class PathTracerHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit PathTracerHealthProbe(StatusFn fn) : m_statusFn(std::move(fn)) {}

    std::string name() const override { return "pathtracer"; }

    vkpt::core::health::Report check() override {
      return EvaluatePathTracerHealth(m_statusFn());
    }

   private:
    StatusFn m_statusFn;
  };

  return std::make_shared<PathTracerHealthProbe>(std::move(status_fn));
}

inline void EmitPathTracerConfig(std::string_view backend,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t spp,
                                 bool deterministic) {
  VKP_LIFECYCLE_CONFIG("pathtracer",
                       "backend",
                       backend,
                       "flow_id",
                       0u,
                       "width",
                       width,
                       "height",
                       height,
                       "spp",
                       spp,
                       "deterministic",
                       deterministic);
}

inline void EmitPathTracerStarted(std::string_view backend,
                                  std::uint64_t flow_id,
                                  std::uint64_t primitive_count) {
  VKP_LIFECYCLE_STARTED("pathtracer",
                        "backend",
                        backend,
                        "flow_id",
                        flow_id,
                        "primitive_count",
                        primitive_count);
}

inline void EmitPathTracerStopped(std::string_view backend,
                                  std::uint64_t flow_id,
                                  std::uint64_t total_samples) {
  VKP_LIFECYCLE_STOPPED("pathtracer",
                        "backend",
                        backend,
                        "flow_id",
                        flow_id,
                        "total_samples",
                        total_samples);
}

inline void EmitPathTracerAnomaly(std::string_view backend,
                                  std::string_view operation,
                                  std::string_view reason,
                                  std::uint64_t flow_id) {
  VKP_LOG(Warn,
          "pathtracer",
          "operation_failed",
          "backend",
          backend,
          "operation",
          operation,
          "reason",
          reason,
          "flow_id",
          flow_id);
}

inline void RecordSampleCounterDeltas(const SampleCounters& before,
                                      const SampleCounters& after) {
  VKP_METRIC_INC_BY("vkp.pathtracer.bvh_node_visits",
                    Delta(before.bvh_node_visits, after.bvh_node_visits));
  VKP_METRIC_INC_BY("vkp.pathtracer.triangle_tests",
                    Delta(before.triangle_tests, after.triangle_tests));
  VKP_METRIC_INC_BY("vkp.pathtracer.ray_count",
                    Delta(before.rays, after.rays));
}

inline void EmitAccumulationReset(std::uint64_t gen,
                                  PathTracerAccumulationResetReason reason,
                                  bool success,
                                  bool accel_valid) {
  VKP_LOG(Info,
          "pathtracer",
          "accumulation_reset",
          "gen",
          gen,
          "flow_id",
          gen,
          "reason",
          ToString(reason),
          "success",
          success,
          "accel_valid",
          accel_valid);
}

inline void EmitSceneDeltaApplied(std::uint64_t gen,
                                  std::uint64_t material_count,
                                  std::uint64_t light_count,
                                  std::uint64_t instance_count,
                                  bool environment_changed,
                                  bool accel_valid) {
  VKP_LOG(Debug,
          "pathtracer",
          "scene_delta_applied",
          "gen",
          gen,
          "flow_id",
          gen,
          "material_count",
          material_count,
          "light_count",
          light_count,
          "instance_count",
          instance_count,
          "environment_changed",
          environment_changed,
          "accel_valid",
          accel_valid);
}

}  // namespace vkpt::pathtracer::observability
