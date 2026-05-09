#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/contracts/Result.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "pathtracer/PathTracer.h"
#include "render/FrameHandoff.h"
#include "render/MultiGpuAccumulation.h"

namespace vkpt::scene {
class SnapshotRing;
struct RenderSceneSnapshot;
}

namespace vkpt::render {

/// Runtime knobs for how aggressively the worker publishes display frames.
struct RenderCoordinatorConfig {
  std::uint32_t publish_hz = 60u;
  std::uint32_t immediate_publish_count = 4u;
  std::uint32_t tile_height = 16u;
  std::uint32_t gpu_count = 1u;
  std::uint32_t foveated_center_extra_samples = 0u;
  double foveated_center_radius = 0.25;
  std::uint32_t motion_vector_block_size = 4u;
  double tile_latency_budget_us = 2000.0;
  vkpt::scene::SnapshotRing* snapshot_ring = nullptr;
};

enum class RenderCoordinatorLifecycle : std::uint8_t {
  NotStarted,
  Running,
  Stopped,
  Failed,
};

enum class RenderTileBudgetState : std::uint8_t {
  Unknown,
  WithinBudget,
  OverBudget,
};

std::string_view RenderCoordinatorLifecycleName(RenderCoordinatorLifecycle lifecycle) noexcept;
std::string_view RenderTileBudgetStateName(RenderTileBudgetState state) noexcept;

struct RenderCoordinatorStateTransitionContract {
  RenderCoordinatorLifecycle from = RenderCoordinatorLifecycle::NotStarted;
  const char* operation = "";
  RenderCoordinatorLifecycle to = RenderCoordinatorLifecycle::NotStarted;
  const char* result = "";
};

struct RenderCoordinatorStandardContract {
  std::string schema_version = "render.coordinator.contract.v1";
  std::array<RenderCoordinatorStateTransitionContract, 5> state_machine{{
      {RenderCoordinatorLifecycle::NotStarted,
       "start",
       RenderCoordinatorLifecycle::Running,
       "Status::ok when a tracer exists; worker owns the tracer after transition"},
      {RenderCoordinatorLifecycle::Running,
       "stop",
       RenderCoordinatorLifecycle::Stopped,
       "joins worker and preserves cumulative stats plus last published frame"},
      {RenderCoordinatorLifecycle::Running,
       "mark_failed",
       RenderCoordinatorLifecycle::Failed,
       "records bounded error_history, clears work_available, and mirrors metrics"},
      {RenderCoordinatorLifecycle::NotStarted,
       "set_render_settings",
       RenderCoordinatorLifecycle::NotStarted,
       "queues initial settings replacement without starting the worker"},
      {RenderCoordinatorLifecycle::Running,
       "set_render_settings",
       RenderCoordinatorLifecycle::Running,
       "queues latest-wins settings replacement for the worker to apply between samples"},
  }};
};

/// Thread-safe snapshot of render worker state and latest handoff counters.
struct RenderCoordinatorStats {
  bool running = false;
  bool failed = false;
  std::string error;
  std::vector<std::string> error_history;
  std::uint64_t generation = 0u;
  std::uint32_t sample_count = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  vkpt::pathtracer::SampleCounters counters{};
  FrameHandoffStats handoff{};
  std::uint64_t instance_transform_commands = 0u;
  std::uint64_t instance_transform_updates_requested = 0u;
  std::uint64_t instance_transform_updates_applied = 0u;
  std::uint64_t instance_transform_dynamic_accel_updates = 0u;
  std::uint64_t instance_transform_full_accel_required = 0u;
  std::uint64_t instance_transform_full_scene_required = 0u;
  std::uint64_t instance_transform_policy_rejections = 0u;
  std::uint64_t instance_transform_failures = 0u;
  std::uint64_t snapshot_publish_total = 0u;
  std::uint64_t snapshot_dropped_total = 0u;
  std::uint64_t snapshot_generation = 0u;
  std::uint64_t tracer_gen_lag = 0u;
  std::uint64_t tiles_rendered_total = 0u;
  std::uint64_t snapshot_reset_total = 0u;
  std::uint64_t snapshot_reproject_total = 0u;
  std::uint64_t snapshot_per_pixel_total = 0u;
  std::uint64_t snapshot_reshade_total = 0u;
  std::uint64_t history_pixels_kept_total = 0u;
  std::uint64_t history_pixels_reset_total = 0u;
  std::uint64_t motion_vector_cells_total = 0u;
  std::uint64_t snapshot_first_tile_publish_total = 0u;
  std::uint32_t snapshot_first_tile_publish_max_tiles = 0u;
  std::uint64_t tile_latency_over_budget_total = 0u;
  double tile_latency_last_us = 0.0;
  double tile_latency_max_us = 0.0;
  std::vector<std::uint64_t> gpu_tiles_rendered_total;
};

/// Lifecycle-oriented status for REPL, health probes, and dashboards.
struct RenderCoordinatorStatus {
  std::string name = "render";
  vkpt::core::contracts::SubsystemHealth health =
      vkpt::core::contracts::SubsystemHealth::Ok;
  RenderCoordinatorLifecycle lifecycle = RenderCoordinatorLifecycle::NotStarted;
  RenderTileBudgetState tile_budget = RenderTileBudgetState::Unknown;
  std::uint64_t observed_at_ns = 0u;
  std::uint64_t started_at_ns = 0u;
  std::uint64_t stopped_at_ns = 0u;
  std::uint64_t last_tick_ns = 0u;
  std::uint64_t ticks_total = 0u;
  std::uint64_t errors_total = 0u;
  std::string last_error;
  std::vector<std::string> error_history;
  std::string health_reason = "ok";
  std::uint64_t generation = 0u;
  std::uint64_t snapshot_generation = 0u;
  std::uint64_t tracer_gen_lag = 0u;
  std::uint32_t sample_count = 0u;
  std::uint32_t target_sample_count = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint64_t tiles_rendered_total = 0u;
  std::uint64_t tile_latency_over_budget_total = 0u;
  double tile_latency_last_us = 0.0;
  double tile_latency_budget_us = 0.0;
  bool work_available = false;
  std::uint64_t work_available_since_ns = 0u;
  std::uint64_t last_frame_published_ns = 0u;
  std::uint64_t ms_since_last_frame_published = 0u;
  std::uint64_t frames_published_total = 0u;
  std::uint64_t latest_published_frame_id = 0u;
  std::uint64_t latest_published_generation = 0u;
};

vkpt::core::health::Report EvaluateRenderCoordinatorHealth(
    const RenderCoordinatorStatus& status);
vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const RenderCoordinatorStatus& status);
std::string FormatRenderCoordinatorStatus(const RenderCoordinatorStatus& status);

/// Owns the background path-tracing loop and consumes app-thread updates.
///
/// The coordinator consumes immutable scene snapshots from SnapshotRing between
/// tiles, resets accumulation when the snapshot generation changes, and
/// publishes resolved LDR frames through FrameHandoff.
///
/// RenderCoordinator state machine contract:
///
/// state\method  start       stop      set_render_settings  set_publish_hz  acquire_latest_frame  status/stats
/// NotStarted    ->Running   noop      ok                   ok              noop                  ok
/// Running       noop        ->Stopped ok                   ok              ok|noop               ok
/// Stopped       error       noop      illegal              ok              ok|noop               ok
/// Failed        error       noop      illegal              ok              ok|noop               ok
///
/// A coordinator is single-use after start(): stop() joins the worker, but the
/// initial tracer has moved into that worker and start() must not be called
/// again. Failures are Status values at API boundaries and are also retained in
/// RenderCoordinatorStatus::last_error/error_history for diagnostics.
class RenderCoordinator {
 public:
  RenderCoordinator(std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer,
                    vkpt::pathtracer::RenderSettings settings,
                    vkpt::pathtracer::PathTracerSceneSnapshot scene,
                    RenderCoordinatorConfig config = {});
  /// Multi-tracer constructor. The primary tracer owns gpu_id 0 and is
  /// followed by `additional_tracers` for gpu_id 1..N. When the additional
  /// list is empty this is byte-identical to the single-tracer ctor.
  RenderCoordinator(
      std::unique_ptr<vkpt::pathtracer::IPathTracer> primary_tracer,
      vkpt::pathtracer::RenderSettings settings,
      vkpt::pathtracer::PathTracerSceneSnapshot scene,
      RenderCoordinatorConfig config,
      std::vector<std::unique_ptr<vkpt::pathtracer::IPathTracer>> additional_tracers);
  ~RenderCoordinator();

  RenderCoordinator(const RenderCoordinator&) = delete;
  RenderCoordinator& operator=(const RenderCoordinator&) = delete;

  vkpt::core::Status start();
  void stop();

  /// Queue render-settings replacement. Scene data still flows through SnapshotRing.
  void set_render_settings(vkpt::pathtracer::RenderSettings settings);
  /// Update display-frame publication cadence without restarting the worker.
  void set_publish_hz(std::uint32_t publish_hz);

  /// Acquire the newest resolved display frame, if the worker has published one.
  std::optional<DisplayFrame> acquire_latest_frame();
  /// Return coordinator and handoff statistics.
  RenderCoordinatorStats stats() const;
  /// Return lifecycle-oriented render status for diagnostics and health checks.
  RenderCoordinatorStatus status() const;
  /// Build the render health probe; hosts can register it with HealthRegistry.
  std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const;

 private:
  void run(std::stop_token stop,
           std::vector<std::unique_ptr<vkpt::pathtracer::IPathTracer>> tracers);
  std::optional<vkpt::pathtracer::RenderSettings> drain_render_settings();
  void mark_failed(std::string error);
  void update_stats(std::uint64_t generation,
                    std::uint32_t sample_count,
                    const vkpt::pathtracer::RenderSettings& settings,
                    const vkpt::pathtracer::SampleCounters& counters);
  void record_frame_published(std::uint64_t now_ns);
  void mirror_stats_to_metrics(const RenderCoordinatorStats& stats);

  vkpt::pathtracer::RenderSettings m_initialSettings{};
  std::shared_ptr<const vkpt::scene::RenderSceneSnapshot> m_initialSnapshot;
  RenderCoordinatorConfig m_config{};
  std::unique_ptr<vkpt::pathtracer::IPathTracer> m_initialTracer;
  std::vector<std::unique_ptr<vkpt::pathtracer::IPathTracer>> m_initialAdditionalTracers;
  std::atomic<std::uint32_t> m_publishHz{60u};

  mutable std::mutex m_settingsMutex;
  std::optional<vkpt::pathtracer::RenderSettings> m_pendingSettings;

  mutable std::mutex m_statsMutex;
  RenderCoordinatorStats m_stats{};
  RenderCoordinatorLifecycle m_lifecycle = RenderCoordinatorLifecycle::NotStarted;
  std::uint64_t m_startedAtNs = 0u;
  std::uint64_t m_stoppedAtNs = 0u;
  std::uint64_t m_lastTickNs = 0u;
  std::uint64_t m_ticksTotal = 0u;
  std::uint64_t m_errorsTotal = 0u;
  std::vector<std::string> m_errorHistory;
  std::uint64_t m_lastFramePublishedNs = 0u;
  std::uint64_t m_workAvailableSinceNs = 0u;
  std::uint32_t m_targetSampleCount = 0u;
  bool m_workAvailable = false;
  mutable std::mutex m_metricsMutex;
  RenderCoordinatorStats m_lastMirroredStats{};

  FrameHandoff m_handoff;
  std::jthread m_thread;
  std::atomic_bool m_started{false};
};

template <typename HealthRegistryT>
void RegisterRenderCoordinatorHealthProbe(RenderCoordinator& coordinator,
                                          HealthRegistryT& registry) {
  registry.register_probe(coordinator.create_health_probe());
}

template <typename ReplT>
void RegisterRenderCoordinatorRepl(RenderCoordinator& coordinator, ReplT& repl) {
  repl.register_command(
      "render",
      "render status - show render coordinator status",
      [&coordinator](const std::vector<std::string>& args) -> std::string {
        if (args.size() == 1u && args[0] == "status") {
          return FormatRenderCoordinatorStatus(coordinator.status());
        }
        return "usage: render status\n";
      });
}

inline RenderCoordinatorStandardContract BuildStandardRenderCoordinatorContract() {
  return {};
}

inline bool ValidateStandardRenderCoordinatorContract(
    const RenderCoordinatorStandardContract& contract,
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

  require(contract.schema_version == "render.coordinator.contract.v1",
          "unexpected render coordinator contract schema version");
  require(contract.state_machine.size() == 5u,
          "render coordinator state machine must publish five standard transitions");
  require(contract.state_machine[0].from == RenderCoordinatorLifecycle::NotStarted &&
              std::string_view(contract.state_machine[0].operation) == "start" &&
              contract.state_machine[0].to == RenderCoordinatorLifecycle::Running,
          "render coordinator contract missing start transition");
  require(contract.state_machine[1].from == RenderCoordinatorLifecycle::Running &&
              std::string_view(contract.state_machine[1].operation) == "stop" &&
              contract.state_machine[1].to == RenderCoordinatorLifecycle::Stopped,
          "render coordinator contract missing stop transition");
  require(contract.state_machine[2].from == RenderCoordinatorLifecycle::Running &&
              std::string_view(contract.state_machine[2].operation) == "mark_failed" &&
              contract.state_machine[2].to == RenderCoordinatorLifecycle::Failed,
          "render coordinator contract missing failure transition");
  return ok;
}

}  // namespace vkpt::render
