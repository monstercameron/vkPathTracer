#include "render/RenderCoordinator.h"

#include "core/Logging.h"
#include "core/contracts/Lifecycle.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "diagnostics/CrashRecorder.h"
#include "render/HistoryTransition.h"
#include "jobs/JobSystem.h"
#include "render/TileScheduler.h"
#include "scene/SceneSnapshot.h"
#include "scene/SnapshotRing.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace vkpt::render {

namespace {

std::uint64_t SteadyNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

vkpt::core::contracts::SubsystemHealth ToSubsystemHealth(
    vkpt::core::health::Status status) noexcept {
  switch (status) {
    case vkpt::core::health::Status::Ok:
      return vkpt::core::contracts::SubsystemHealth::Ok;
    case vkpt::core::health::Status::Degraded:
      return vkpt::core::contracts::SubsystemHealth::Degraded;
    case vkpt::core::health::Status::Failed:
      return vkpt::core::contracts::SubsystemHealth::Failed;
  }
  return vkpt::core::contracts::SubsystemHealth::Failed;
}

vkpt::core::contracts::ComponentLifecycle ToComponentLifecycle(
    RenderCoordinatorLifecycle lifecycle) noexcept {
  using vkpt::core::contracts::ComponentLifecycle;
  switch (lifecycle) {
    case RenderCoordinatorLifecycle::NotStarted:
      return ComponentLifecycle::Uninitialized;
    case RenderCoordinatorLifecycle::Running:
      return ComponentLifecycle::Busy;
    case RenderCoordinatorLifecycle::Stopped:
      return ComponentLifecycle::ShuttingDown;
    case RenderCoordinatorLifecycle::Failed:
      return ComponentLifecycle::Failed;
  }
  return ComponentLifecycle::Failed;
}

bool AssertCoordinatorState(
    std::string_view method,
    RenderCoordinatorLifecycle current,
    std::initializer_list<RenderCoordinatorLifecycle> allowed) {
  using vkpt::core::contracts::ComponentLifecycle;
  if (allowed.size() == 1u) {
    const ComponentLifecycle only = ToComponentLifecycle(*allowed.begin());
    return vkpt::core::contracts::assert_state(method,
                                               ToComponentLifecycle(current),
                                               {only});
  }
  bool ok = false;
  for (const auto lifecycle : allowed) {
    ok = ok || lifecycle == current;
  }
#ifndef NDEBUG
  assert(ok && "render coordinator lifecycle state contract violated");
#endif
  (void)method;
  return ok;
}

void LogCoordinatorException(std::string_view error) noexcept {
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error,
        "render",
        "render coordinator thread exception",
        {{"error", std::string(error)}});
  } catch (...) {
  }
  try {
    auto& recorder = vkpt::diagnostics::CrashRecorder::instance();
    recorder.set_last_error(error);
    recorder.record_checkpoint(
        "render_coordinator_exception", 0, "render", error, false);
  } catch (...) {
  }
}

void SleepRenderWorkerUntil(std::stop_token stop,
                            std::chrono::steady_clock::time_point target) {
  while (!stop.stop_requested() && std::chrono::steady_clock::now() < target) {
    const auto remaining = target - std::chrono::steady_clock::now();
    if (remaining > std::chrono::milliseconds(2)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } else {
      std::this_thread::yield();
    }
  }
}

vkpt::scene::RenderSceneSnapshot::Ptr BuildInitialSnapshot(
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene) {
  vkpt::scene::RenderSceneSnapshotRevisions revisions{};
  revisions.generation = 1u;
  revisions.topology_revision = 1u;
  revisions.transform_revision = 1u;
  revisions.camera_revision = 1u;
  revisions.material_revision = 1u;
  return vkpt::scene::BuildRenderSceneSnapshot(scene, nullptr, revisions);
}

void ObserveFrameHandoffEvent(const FrameHandoffEvent& event) {
  if (event.type == FrameHandoffEvent::Type::Dropped) {
    VKP_LOG(Warn,
            "render",
            "frame_dropped",
            "frame_id",
            event.frame_id,
            "gen",
            event.generation,
            "flow_id",
            event.generation,
            "reason",
            FrameDropReasonName(event.drop_reason),
            "sample_count",
            event.sample_count);
    return;
  }

  VKP_LOG_SAMPLED(1'000'000'000ull,
                  Info,
                  "render",
                  "frame_published",
                  "frame_id",
                  event.frame_id,
                  "gen",
                  event.generation,
                  "flow_id",
                  event.generation,
                  "sample_count",
                  event.sample_count,
                  "width",
                  event.width,
                  "height",
                  event.height);
}

void SetGauge(vkpt::core::metrics::MetricsRegistry& registry,
              std::string_view name,
              double value) {
  registry.gauge(name).set(value);
}

void IncMirroredCounter(vkpt::core::metrics::MetricsRegistry& registry,
                        std::string_view name,
                        std::uint64_t current,
                        std::uint64_t previous) {
  auto& counter = registry.counter(name);
  if (current >= previous) {
    const auto delta = current - previous;
    if (delta != 0u) {
      counter.inc(delta);
    }
  } else if (current != 0u) {
    counter.inc(current);
  }
}

}  // namespace

std::string_view RenderCoordinatorLifecycleName(
    RenderCoordinatorLifecycle lifecycle) noexcept {
  switch (lifecycle) {
    case RenderCoordinatorLifecycle::NotStarted:
      return "not_started";
    case RenderCoordinatorLifecycle::Running:
      return "running";
    case RenderCoordinatorLifecycle::Stopped:
      return "stopped";
    case RenderCoordinatorLifecycle::Failed:
      return "failed";
  }
  return "unknown";
}

std::string_view RenderTileBudgetStateName(RenderTileBudgetState state) noexcept {
  switch (state) {
    case RenderTileBudgetState::Unknown:
      return "unknown";
    case RenderTileBudgetState::WithinBudget:
      return "within_budget";
    case RenderTileBudgetState::OverBudget:
      return "over_budget";
  }
  return "unknown";
}

vkpt::core::health::Report EvaluateRenderCoordinatorHealth(
    const RenderCoordinatorStatus& status) {
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;

  if (status.lifecycle == RenderCoordinatorLifecycle::Failed) {
    return Report{Status::Failed,
                  status.last_error.empty() ? "render coordinator failed"
                                            : status.last_error};
  }

  if (status.work_available) {
    std::uint64_t lastProgressNs = status.work_available_since_ns;
    if (status.last_frame_published_ns > lastProgressNs) {
      lastProgressNs = status.last_frame_published_ns;
    }
    if (lastProgressNs != 0u &&
        status.observed_at_ns > lastProgressNs + 1'000'000'000ull) {
      return Report{Status::Failed,
                    "no frame published in 1s while render work is available"};
    }
  }

  if (status.tracer_gen_lag > 5u) {
    return Report{Status::Degraded, "tracer gen_lag exceeds 5"};
  }

  if (status.lifecycle == RenderCoordinatorLifecycle::Stopped) {
    return Report{Status::Ok, "stopped"};
  }
  if (status.lifecycle == RenderCoordinatorLifecycle::NotStarted) {
    return Report{Status::Ok, "not started"};
  }
  return Report{Status::Ok, "ok"};
}

vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const RenderCoordinatorStatus& status) {
  auto out = vkpt::core::contracts::MakeSubsystemStatus(status.name, status.health);
  out.started_at_ns = status.started_at_ns;
  out.last_tick_ns = status.last_tick_ns;
  out.last_error = status.last_error;
  out.ticks_total = status.ticks_total;
  out.errors_total = status.errors_total;
  out.set_custom("lifecycle", std::string(RenderCoordinatorLifecycleName(status.lifecycle)));
  out.set_custom("health_reason", status.health_reason);
  out.set_custom("generation", std::to_string(status.generation));
  out.set_custom("snapshot_generation", std::to_string(status.snapshot_generation));
  out.set_custom("gen_lag", std::to_string(status.tracer_gen_lag));
  out.set_custom("sample_count", std::to_string(status.sample_count));
  out.set_custom("target_sample_count", std::to_string(status.target_sample_count));
  out.set_custom("tile_budget", std::string(RenderTileBudgetStateName(status.tile_budget)));
  out.set_custom("frames_published_total", std::to_string(status.frames_published_total));
  return out;
}

std::string FormatRenderCoordinatorStatus(const RenderCoordinatorStatus& status) {
  std::ostringstream os;
  os << "render status: "
     << vkpt::core::contracts::SubsystemHealthName(status.health) << '\n'
     << "  lifecycle: " << RenderCoordinatorLifecycleName(status.lifecycle) << '\n'
     << "  health_reason: " << status.health_reason << '\n'
     << "  generation: " << status.generation << '\n'
     << "  snapshot_generation: " << status.snapshot_generation << '\n'
     << "  gen_lag: " << status.tracer_gen_lag << '\n'
     << "  sample: " << status.sample_count << '/'
     << status.target_sample_count << '\n'
     << "  frame: published=" << status.frames_published_total
     << " latest_id=" << status.latest_published_frame_id
     << " latest_gen=" << status.latest_published_generation
     << " age_ms=" << status.ms_since_last_frame_published << '\n'
     << "  tile_budget: " << RenderTileBudgetStateName(status.tile_budget)
     << " last_us=" << status.tile_latency_last_us
     << " budget_us=" << status.tile_latency_budget_us
     << " over_budget_total=" << status.tile_latency_over_budget_total << '\n'
     << "  work_available: " << (status.work_available ? "true" : "false")
     << '\n';
  if (!status.last_error.empty()) {
    os << "  last_error: " << status.last_error << '\n';
  }
  if (!status.error_history.empty()) {
    os << "  error_history: " << status.error_history.size() << '\n';
  }
  return os.str();
}

RenderCoordinator::RenderCoordinator(std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer,
                                     vkpt::pathtracer::RenderSettings settings,
                                     vkpt::pathtracer::PathTracerSceneSnapshot scene,
                                     RenderCoordinatorConfig config)
    : m_initialSettings(std::move(settings)),
      m_initialSnapshot(BuildInitialSnapshot(scene)),
      m_config(config),
      m_initialTracer(std::move(tracer)) {
  m_config.publish_hz = std::max<std::uint32_t>(1u, m_config.publish_hz);
  m_config.tile_height = std::max<std::uint32_t>(1u, m_config.tile_height);
  m_config.gpu_count = std::max<std::uint32_t>(1u, m_config.gpu_count);
  m_config.motion_vector_block_size =
      std::max<std::uint32_t>(1u, m_config.motion_vector_block_size);
  m_config.foveated_center_radius =
      std::clamp(m_config.foveated_center_radius, 0.0, 0.5);
  if (m_config.tile_latency_budget_us <= 0.0) {
    m_config.tile_latency_budget_us = 2000.0;
  }
  m_publishHz.store(m_config.publish_hz, std::memory_order_relaxed);
  m_handoff.set_observer(ObserveFrameHandoffEvent);
}

RenderCoordinator::~RenderCoordinator() {
  stop();
}

vkpt::core::Status RenderCoordinator::start() {
  if (m_started.exchange(true, std::memory_order_acq_rel)) {
    return vkpt::core::Status::ok("render coordinator already running");
  }
  {
    std::scoped_lock lock(m_statsMutex);
    (void)AssertCoordinatorState("RenderCoordinator::start",
                                 m_lifecycle,
                                 {RenderCoordinatorLifecycle::NotStarted});
  }
  if (!m_initialTracer) {
    m_started.store(false, std::memory_order_release);
    constexpr const char* kError = "render coordinator has no tracer";
    mark_failed(kError);
    return vkpt::core::Status::error(
        vkpt::core::StatusCode::NotReady,
        kError,
        "construct RenderCoordinator with a valid IPathTracer");
  }

  const auto nowNs = SteadyNowNs();
  {
    std::scoped_lock lock(m_statsMutex);
    m_lifecycle = RenderCoordinatorLifecycle::Running;
    m_startedAtNs = nowNs;
    m_stoppedAtNs = 0u;
    m_lastTickNs = nowNs;
    m_targetSampleCount = m_initialSettings.spp;
    m_workAvailable = m_initialSettings.spp > 0u;
    m_workAvailableSinceNs = m_workAvailable ? nowNs : 0u;
  }
  VKP_LIFECYCLE_CONFIG("render",
                       "flow_id",
                       std::uint64_t{0},
                       "publish_hz",
                       m_publishHz.load(std::memory_order_relaxed),
                       "tile_height",
                       m_config.tile_height,
                       "gpu_count",
                       m_config.gpu_count);
  VKP_LIFECYCLE_STARTED("render",
                        "flow_id",
                        std::uint64_t{0},
                        "target_samples",
                        m_initialSettings.spp,
                        "width",
                        m_initialSettings.width,
                        "height",
                        m_initialSettings.height);

  auto tracer = std::move(m_initialTracer);
  m_thread = std::jthread([this, tracer = std::move(tracer)](std::stop_token stop) mutable {
    vkpt::jobs::ApplyCurrentThreadPriority(vkpt::jobs::WorkerThreadPriority::Background);
    const auto failFromException = [this](std::string_view error) noexcept {
      if (error.empty()) {
        error = "render coordinator exception";
      }
      try {
        mark_failed(std::string(error));
      } catch (...) {
      }
      LogCoordinatorException(error);
    };
    try {
      run(stop, std::move(tracer));
    } catch (const std::bad_alloc& ex) {
      (void)ex;
      failFromException("render coordinator out of memory");
    } catch (const std::exception& ex) {
      failFromException(ex.what());
    } catch (...) {
      failFromException("render coordinator non-standard exception");
    }
  });
  return vkpt::core::Status::ok("render coordinator started");
}

void RenderCoordinator::stop() {
  if (!m_started.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  {
    std::scoped_lock lock(m_statsMutex);
    (void)AssertCoordinatorState("RenderCoordinator::stop",
                                 m_lifecycle,
                                 {RenderCoordinatorLifecycle::Running,
                                  RenderCoordinatorLifecycle::Failed});
  }
  if (m_thread.joinable()) {
    m_thread.request_stop();
    m_thread.join();
  }
}

void RenderCoordinator::set_render_settings(vkpt::pathtracer::RenderSettings settings) {
  {
    std::scoped_lock lock(m_statsMutex);
    (void)AssertCoordinatorState("RenderCoordinator::set_render_settings",
                                 m_lifecycle,
                                 {RenderCoordinatorLifecycle::NotStarted,
                                  RenderCoordinatorLifecycle::Running});
  }
  std::scoped_lock lock(m_settingsMutex);
  m_pendingSettings = std::move(settings);
}

void RenderCoordinator::set_publish_hz(std::uint32_t publish_hz) {
  m_publishHz.store(std::max<std::uint32_t>(1u, publish_hz),
                    std::memory_order_relaxed);
}

std::optional<DisplayFrame> RenderCoordinator::acquire_latest_frame() {
  return m_handoff.acquire_latest();
}

RenderCoordinatorStats RenderCoordinator::stats() const {
  RenderCoordinatorStats out;
  {
    std::scoped_lock lock(m_statsMutex);
    out = m_stats;
  }
  out.handoff = m_handoff.stats();
  return out;
}

RenderCoordinatorStatus RenderCoordinator::status() const {
  RenderCoordinatorStatus out;
  const auto nowNs = SteadyNowNs();
  {
    std::scoped_lock lock(m_statsMutex);
    out.lifecycle = m_lifecycle;
    out.observed_at_ns = nowNs;
    out.started_at_ns = m_startedAtNs;
    out.stopped_at_ns = m_stoppedAtNs;
    out.last_tick_ns = m_lastTickNs;
    out.ticks_total = m_ticksTotal;
    out.errors_total = m_errorsTotal;
    out.last_error = m_stats.error;
    out.error_history = m_errorHistory;
    out.generation = m_stats.generation;
    out.snapshot_generation = m_stats.snapshot_generation;
    out.tracer_gen_lag = m_stats.tracer_gen_lag;
    out.sample_count = m_stats.sample_count;
    out.target_sample_count = m_targetSampleCount;
    out.width = m_stats.width;
    out.height = m_stats.height;
    out.tiles_rendered_total = m_stats.tiles_rendered_total;
    out.tile_latency_over_budget_total = m_stats.tile_latency_over_budget_total;
    out.tile_latency_last_us = m_stats.tile_latency_last_us;
    out.tile_latency_budget_us = m_config.tile_latency_budget_us;
    out.work_available = m_workAvailable;
    out.work_available_since_ns = m_workAvailableSinceNs;
    out.last_frame_published_ns = m_lastFramePublishedNs;
  }

  if (out.tiles_rendered_total == 0u) {
    out.tile_budget = RenderTileBudgetState::Unknown;
  } else if (out.tile_latency_last_us > out.tile_latency_budget_us) {
    out.tile_budget = RenderTileBudgetState::OverBudget;
  } else {
    out.tile_budget = RenderTileBudgetState::WithinBudget;
  }

  const auto handoff = m_handoff.stats();
  out.frames_published_total = handoff.published;
  out.latest_published_frame_id = handoff.latest_published_id;
  out.latest_published_generation = handoff.latest_generation;
  if (out.last_frame_published_ns != 0u && nowNs >= out.last_frame_published_ns) {
    out.ms_since_last_frame_published =
        (nowNs - out.last_frame_published_ns) / 1'000'000ull;
  }

  const auto report = EvaluateRenderCoordinatorHealth(out);
  out.health = ToSubsystemHealth(report.status);
  out.health_reason = report.reason;
  return out;
}

std::shared_ptr<vkpt::core::health::IHealthProbe>
RenderCoordinator::create_health_probe() const {
  class RenderHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit RenderHealthProbe(const RenderCoordinator* coordinator)
        : m_coordinator(coordinator) {}

    std::string name() const override { return "render"; }

    vkpt::core::health::Report check() override {
      if (m_coordinator == nullptr) {
        return {vkpt::core::health::Status::Failed,
                "render coordinator probe has no coordinator"};
      }
      return EvaluateRenderCoordinatorHealth(m_coordinator->status());
    }

   private:
    const RenderCoordinator* m_coordinator = nullptr;
  };

  return std::make_shared<RenderHealthProbe>(this);
}

std::optional<vkpt::pathtracer::RenderSettings> RenderCoordinator::drain_render_settings() {
  std::scoped_lock lock(m_settingsMutex);
  auto out = std::move(m_pendingSettings);
  m_pendingSettings.reset();
  return out;
}

void RenderCoordinator::mark_failed(std::string error) {
  const auto nowNs = SteadyNowNs();
  RenderCoordinatorStats snapshot;
  {
    std::scoped_lock lock(m_statsMutex);
    m_stats.failed = true;
    m_stats.running = false;
    m_stats.error = std::move(error);
    if (!m_stats.error.empty()) {
      m_errorHistory.push_back(m_stats.error);
      constexpr std::size_t kMaxRenderErrorHistory = 16u;
      if (m_errorHistory.size() > kMaxRenderErrorHistory) {
        m_errorHistory.erase(m_errorHistory.begin(),
                             m_errorHistory.begin() +
                                 static_cast<std::ptrdiff_t>(
                                     m_errorHistory.size() -
                                     kMaxRenderErrorHistory));
      }
      m_stats.error_history = m_errorHistory;
    }
    m_lifecycle = RenderCoordinatorLifecycle::Failed;
    m_stoppedAtNs = nowNs;
    m_lastTickNs = nowNs;
    ++m_errorsTotal;
    m_workAvailable = false;
    m_workAvailableSinceNs = 0u;
    snapshot = m_stats;
  }
  snapshot.handoff = m_handoff.stats();
  mirror_stats_to_metrics(snapshot);
}

void RenderCoordinator::update_stats(std::uint64_t generation,
                                     std::uint32_t sample_count,
                                     const vkpt::pathtracer::RenderSettings& settings,
                                     const vkpt::pathtracer::SampleCounters& counters) {
  const auto nowNs = SteadyNowNs();
  RenderCoordinatorStats snapshot;
  {
    std::scoped_lock lock(m_statsMutex);
    m_stats.running = m_started.load(std::memory_order_acquire);
    m_stats.generation = generation;
    m_stats.sample_count = sample_count;
    m_stats.width = settings.width;
    m_stats.height = settings.height;
    m_stats.counters = counters;
    m_targetSampleCount = settings.spp;
    m_lastTickNs = nowNs;
    ++m_ticksTotal;
    const bool workAvailable =
        m_stats.running && !m_stats.failed && sample_count < settings.spp;
    if (workAvailable && !m_workAvailable) {
      m_workAvailableSinceNs = nowNs;
    } else if (!workAvailable) {
      m_workAvailableSinceNs = 0u;
    }
    m_workAvailable = workAvailable;
    snapshot = m_stats;
  }
  snapshot.handoff = m_handoff.stats();
  mirror_stats_to_metrics(snapshot);
}

void RenderCoordinator::record_frame_published(std::uint64_t now_ns) {
  std::scoped_lock lock(m_statsMutex);
  m_lastFramePublishedNs = now_ns;
}

void RenderCoordinator::mirror_stats_to_metrics(const RenderCoordinatorStats& stats) {
  auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
  std::scoped_lock lock(m_metricsMutex);
  const auto& previous = m_lastMirroredStats;

  SetGauge(registry, "vkp.render.running", stats.running ? 1.0 : 0.0);
  SetGauge(registry, "vkp.render.failed", stats.failed ? 1.0 : 0.0);
  SetGauge(registry, "vkp.render.generation", stats.generation);
  SetGauge(registry, "vkp.render.sample_count", stats.sample_count);
  SetGauge(registry, "vkp.render.width", stats.width);
  SetGauge(registry, "vkp.render.height", stats.height);
  SetGauge(registry, "vkp.render.counters.samples", stats.counters.samples);
  SetGauge(registry, "vkp.render.counters.rays", stats.counters.rays);
  SetGauge(registry, "vkp.render.counters.triangle_tests",
           stats.counters.triangle_tests);
  SetGauge(registry, "vkp.render.counters.sdf_tests", stats.counters.sdf_tests);
  SetGauge(registry, "vkp.render.counters.sdf_steps", stats.counters.sdf_steps);
  SetGauge(registry, "vkp.render.counters.triangle_hits",
           stats.counters.triangle_hits);
  SetGauge(registry, "vkp.render.counters.sdf_hits", stats.counters.sdf_hits);
  SetGauge(registry, "vkp.render.counters.sdf_misses", stats.counters.sdf_misses);
  SetGauge(registry, "vkp.render.counters.bvh_node_visits",
           stats.counters.bvh_node_visits);
  SetGauge(registry, "vkp.render.counters.bvh_leaf_visits",
           stats.counters.bvh_leaf_visits);
  SetGauge(registry, "vkp.render.counters.shadow_tests",
           stats.counters.shadow_tests);
  SetGauge(registry, "vkp.render.snapshot_generation", stats.snapshot_generation);
  SetGauge(registry, "vkp.render.tracer_gen_lag", stats.tracer_gen_lag);
  SetGauge(registry,
           "vkp.render.snapshot_first_tile_publish_max_tiles",
           stats.snapshot_first_tile_publish_max_tiles);
  SetGauge(registry,
           "vkp.render.handoff.latest_published_id",
           stats.handoff.latest_published_id);
  SetGauge(registry,
           "vkp.render.handoff.latest_acquired_id",
           stats.handoff.latest_acquired_id);
  SetGauge(registry, "vkp.render.handoff.latest_generation",
           stats.handoff.latest_generation);
  SetGauge(registry, "vkp.render.handoff.latest_sample_count",
           stats.handoff.latest_sample_count);
  SetGauge(registry, "vkp.render.handoff.latest_width",
           static_cast<double>(stats.handoff.latest_width));
  SetGauge(registry, "vkp.render.handoff.latest_height",
           static_cast<double>(stats.handoff.latest_height));
  SetGauge(registry, "vkp.render.handoff.latest_dropped_id",
           stats.handoff.latest_dropped_id);
  SetGauge(registry, "vkp.render.handoff.latest_dropped_generation",
           stats.handoff.latest_dropped_generation);
  SetGauge(registry, "vkp.render.handoff.latest_drop_reason",
           static_cast<double>(stats.handoff.latest_drop_reason));

  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_commands",
                     stats.instance_transform_commands,
                     previous.instance_transform_commands);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_updates_requested",
                     stats.instance_transform_updates_requested,
                     previous.instance_transform_updates_requested);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_updates_applied",
                     stats.instance_transform_updates_applied,
                     previous.instance_transform_updates_applied);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_dynamic_accel_updates",
                     stats.instance_transform_dynamic_accel_updates,
                     previous.instance_transform_dynamic_accel_updates);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_full_accel_required",
                     stats.instance_transform_full_accel_required,
                     previous.instance_transform_full_accel_required);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_full_scene_required",
                     stats.instance_transform_full_scene_required,
                     previous.instance_transform_full_scene_required);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_policy_rejections",
                     stats.instance_transform_policy_rejections,
                     previous.instance_transform_policy_rejections);
  IncMirroredCounter(registry,
                     "vkp.render.instance_transform_failures",
                     stats.instance_transform_failures,
                     previous.instance_transform_failures);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_publish_total",
                     stats.snapshot_publish_total,
                     previous.snapshot_publish_total);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_dropped_total",
                     stats.snapshot_dropped_total,
                     previous.snapshot_dropped_total);
  IncMirroredCounter(registry,
                     "vkp.render.tiles_rendered_total",
                     stats.tiles_rendered_total,
                     previous.tiles_rendered_total);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_reset_total",
                     stats.snapshot_reset_total,
                     previous.snapshot_reset_total);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_reproject_total",
                     stats.snapshot_reproject_total,
                     previous.snapshot_reproject_total);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_per_pixel_total",
                     stats.snapshot_per_pixel_total,
                     previous.snapshot_per_pixel_total);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_reshade_total",
                     stats.snapshot_reshade_total,
                     previous.snapshot_reshade_total);
  IncMirroredCounter(registry,
                     "vkp.render.history_pixels_kept_total",
                     stats.history_pixels_kept_total,
                     previous.history_pixels_kept_total);
  IncMirroredCounter(registry,
                     "vkp.render.history_pixels_reset_total",
                     stats.history_pixels_reset_total,
                     previous.history_pixels_reset_total);
  IncMirroredCounter(registry,
                     "vkp.render.motion_vector_cells_total",
                     stats.motion_vector_cells_total,
                     previous.motion_vector_cells_total);
  IncMirroredCounter(registry,
                     "vkp.render.snapshot_first_tile_publish_total",
                     stats.snapshot_first_tile_publish_total,
                     previous.snapshot_first_tile_publish_total);
  IncMirroredCounter(registry,
                     "vkp.render.tile_latency_over_budget_total",
                     stats.tile_latency_over_budget_total,
                     previous.tile_latency_over_budget_total);
  IncMirroredCounter(registry,
                     "vkp.render.handoff.published",
                     stats.handoff.published,
                     previous.handoff.published);
  IncMirroredCounter(registry,
                     "vkp.render.handoff.acquired",
                     stats.handoff.acquired,
                     previous.handoff.acquired);
  IncMirroredCounter(registry,
                     "vkp.render.handoff.dropped",
                     stats.handoff.dropped,
                     previous.handoff.dropped);
  IncMirroredCounter(registry,
                     "vkp.render.frame_published_total",
                     stats.handoff.published,
                     previous.handoff.published);
  IncMirroredCounter(registry,
                     "vkp.render.frame_dropped_total",
                     stats.handoff.dropped,
                     previous.handoff.dropped);

  for (std::size_t i = 0; i < stats.gpu_tiles_rendered_total.size(); ++i) {
    const auto previousValue =
        i < previous.gpu_tiles_rendered_total.size()
            ? previous.gpu_tiles_rendered_total[i]
            : 0u;
    const std::string metricName =
        "vkp.render.gpu_tiles_rendered_total.gpu" + std::to_string(i);
    IncMirroredCounter(registry,
                       std::string_view(metricName),
                       stats.gpu_tiles_rendered_total[i],
                       previousValue);
  }

  m_lastMirroredStats = stats;
}

void RenderCoordinator::run(std::stop_token stop,
                            std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer) {
  {
    std::scoped_lock lock(m_statsMutex);
    (void)AssertCoordinatorState("RenderCoordinator::run",
                                 m_lifecycle,
                                 {RenderCoordinatorLifecycle::Running});
  }
  std::uint64_t generation = 0u;
  std::uint32_t sample = 0u;
  auto settings = m_initialSettings;
  auto lastPublish = std::chrono::steady_clock::time_point{};
  auto nextSampleStart = std::chrono::steady_clock::time_point{};
  vkpt::scene::SnapshotRing ownedSnapshotRing;
  auto& snapshotRing = m_config.snapshot_ring != nullptr ? *m_config.snapshot_ring : ownedSnapshotRing;
  if (!snapshotRing.current() && m_initialSnapshot) {
    snapshotRing.publish(m_initialSnapshot);
  }
  const std::uint32_t tracerSnapshotReader = snapshotRing.register_reader("tracer");
  vkpt::scene::RenderSceneSnapshot::Ptr activeSnapshot;
  TileScheduler tileScheduler;
  std::uint64_t pendingFirstTilePublishGeneration = 0u;
  std::uint32_t tilesSinceSnapshotChange = 0u;

  auto configureTileScheduler = [&]() {
    tileScheduler.configure(TileSchedulerConfig{
        settings.width,
        settings.height,
        tracer && tracer->supports_tile_rendering()
            ? m_config.tile_height
            : std::max(1u, settings.height),
        m_config.gpu_count,
        m_config.foveated_center_extra_samples,
        m_config.foveated_center_radius});
    std::scoped_lock lock(m_statsMutex);
    m_stats.gpu_tiles_rendered_total.resize(m_config.gpu_count, 0u);
  };

  auto refreshSnapshotStats = [&]() {
    const auto ringStats = snapshotRing.stats();
    const auto readerStats = snapshotRing.reader_stats(tracerSnapshotReader);
    std::scoped_lock lock(m_statsMutex);
    m_stats.snapshot_publish_total = ringStats.publish_total;
    m_stats.snapshot_dropped_total = ringStats.dropped_total;
    m_stats.snapshot_generation = ringStats.latest_generation;
    m_stats.tracer_gen_lag = readerStats.lag;
    VKP_METRIC_SET("vkp.tracer.gen_lag", readerStats.lag);
  };

  auto loadSnapshotIntoTracer =
      [&](const vkpt::scene::RenderSceneSnapshot& snapshot) -> bool {
    const auto& snapshotScene = snapshot.path_tracer_scene_snapshot();
    return tracer &&
           tracer->load_scene_snapshot(snapshotScene) &&
           tracer->build_or_update_acceleration();
  };

  auto consumeCurrentSnapshot = [&]() -> bool {
    const auto currentSnapshot = snapshotRing.current(tracerSnapshotReader);
    refreshSnapshotStats();
    if (!currentSnapshot) {
      return true;
    }
    if (activeSnapshot &&
        activeSnapshot->generation == currentSnapshot->generation) {
      return true;
    }

    const auto fromGeneration = activeSnapshot ? activeSnapshot->generation : 0u;
    vkpt::scene::SnapshotTransitionCapabilities transitionCaps;
    transitionCaps.camera_reprojection = true;
    transitionCaps.transform_motion_vectors = true;
    transitionCaps.material_reshade = true;
    auto decision =
        vkpt::scene::DecideSnapshotTransition(activeSnapshot.get(),
                                              *currentSnapshot,
                                              transitionCaps);
    const bool visualSnapshotChange =
        activeSnapshot &&
        decision.changes != vkpt::scene::RenderSceneSnapshotChange::None;
    if (activeSnapshot && decision.changes != vkpt::scene::RenderSceneSnapshotChange::None) {
      try {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Info,
            "tracer",
            "tracer.gen_change",
            {{"from_gen", std::to_string(fromGeneration)},
             {"to_gen", std::to_string(currentSnapshot->generation)},
             {"reason", vkpt::scene::SnapshotChangeReason(decision.changes)},
             {"decision", vkpt::scene::ToString(decision.action)}});
      } catch (...) {
      }
    }
    VKP_LOG_SAMPLED(1'000'000'000ull,
                    Debug,
                    "render",
                    "snapshot_consumed",
                    "gen",
                    currentSnapshot->generation,
                    "flow_id",
                    currentSnapshot->generation,
                    "from_gen",
                    fromGeneration,
                    "change",
                    vkpt::scene::SnapshotChangeReason(decision.changes),
                    "action",
                    vkpt::scene::ToString(decision.action));

    const auto resetToCurrentSnapshot = [&]() -> bool {
      if (!loadSnapshotIntoTracer(*currentSnapshot) ||
          !tracer->reset_accumulation()) {
        mark_failed("render coordinator snapshot transition reset failed");
        return false;
      }
      sample = 0u;
      std::scoped_lock lock(m_statsMutex);
      ++m_stats.snapshot_reset_total;
      VKP_METRIC_INC("vkp.tracer.reset_total");
      return true;
    };

    const auto recordHistoryTransition =
        [&](const HistoryTransitionResult& history) {
      std::scoped_lock lock(m_statsMutex);
      m_stats.history_pixels_kept_total += history.pixels_kept;
      m_stats.history_pixels_reset_total += history.pixels_reset;
      m_stats.motion_vector_cells_total += history.motion_vectors.moving_cell_count();
      if (history.action == vkpt::scene::SnapshotTransitionAction::ReprojectCamera) {
        ++m_stats.snapshot_reproject_total;
        VKP_METRIC_INC("vkp.tracer.reproject_total");
      } else if (history.action ==
                 vkpt::scene::SnapshotTransitionAction::ResetMovingPixels) {
        ++m_stats.snapshot_per_pixel_total;
      } else if (history.action ==
                 vkpt::scene::SnapshotTransitionAction::InvalidateShading) {
        ++m_stats.snapshot_reshade_total;
      }
    };

    generation = currentSnapshot->generation;
    if (decision.reset_accumulation) {
      if (!resetToCurrentSnapshot()) {
        return false;
      }
    } else if (decision.action ==
               vkpt::scene::SnapshotTransitionAction::ReprojectCamera) {
      const auto previousFilm = tracer->film();
      bool cameraOk = tracer->update_camera_state(currentSnapshot->camera);
      if (!cameraOk) {
        cameraOk = tracer->load_scene_snapshot(currentSnapshot->path_tracer_scene_snapshot());
      }
      auto history = ApplyCameraHistoryTransition(previousFilm,
                                                  activeSnapshot->camera,
                                                  currentSnapshot->camera);
      if (!cameraOk ||
          !history.applied ||
          !tracer->replace_film_history(history.film)) {
        decision.action = vkpt::scene::SnapshotTransitionAction::ResetAccumulation;
        decision.reset_accumulation = true;
        if (!resetToCurrentSnapshot()) {
          return false;
        }
      } else {
        recordHistoryTransition(history);
      }
    } else if (decision.action ==
               vkpt::scene::SnapshotTransitionAction::ResetMovingPixels) {
      const auto previousFilm = tracer->film();
      const auto updates =
          vkpt::scene::DiffInstanceTransforms(*activeSnapshot, *currentSnapshot);
      auto options = vkpt::pathtracer::MakeStandardTransformUpdateOptions(
          vkpt::pathtracer::RenderUpdateReason::PhysicsMotion,
          0u,
          "snapshot");
      options.reset_accumulation = false;
      options.fallback_policy =
          vkpt::pathtracer::TransformFallbackPolicy::AllowDynamicAcceleration;
      const auto updateResult =
          tracer->apply_instance_transform_update(updates, options);
      auto motionVectors = RasterizeCoarseMotionVectors(*activeSnapshot,
                                                        *currentSnapshot,
                                                        settings.width,
                                                        settings.height,
                                                        m_config.motion_vector_block_size);
      auto history =
          ApplyTransformHistoryTransition(previousFilm, motionVectors);
      if (!updateResult.applied() ||
          !history.applied ||
          !tracer->replace_film_history(history.film)) {
        decision.action = vkpt::scene::SnapshotTransitionAction::ResetAccumulation;
        decision.reset_accumulation = true;
        if (!resetToCurrentSnapshot()) {
          return false;
        }
      } else {
        tileScheduler.set_feedback(BuildDirtyTileFeedback(
            history.motion_vectors,
            history.film,
            tracer && tracer->supports_tile_rendering()
                ? m_config.tile_height
                : std::max(1u, settings.height)));
        recordHistoryTransition(history);
      }
    } else if (decision.action ==
               vkpt::scene::SnapshotTransitionAction::InvalidateShading) {
      const auto previousFilm = tracer->film();
      bool materialOk = true;
      const auto& previousScene = activeSnapshot->path_tracer_scene_snapshot();
      const auto& currentScene = currentSnapshot->path_tracer_scene_snapshot();
      const auto delta =
          vkpt::pathtracer::BuildSceneDeltaUpdate(previousScene, currentScene);
      if (delta &&
          (!delta->materials.empty() ||
           !delta->lights.empty() ||
           delta->environment_color_changed)) {
        materialOk = tracer->update_scene_delta(*delta);
      }
      auto history = ApplyMaterialHistoryTransition(previousFilm);
      if (!materialOk ||
          !history.applied ||
          !tracer->replace_film_history(history.film)) {
        decision.action = vkpt::scene::SnapshotTransitionAction::ResetAccumulation;
        decision.reset_accumulation = true;
        if (!resetToCurrentSnapshot()) {
          return false;
        }
      } else {
        sample = 0u;
        recordHistoryTransition(history);
      }
    }

    if (decision.action == vkpt::scene::SnapshotTransitionAction::ReprojectCamera) {
      std::scoped_lock lock(m_statsMutex);
      VKP_METRIC_SET("vkp.tracer.history_pixels_reset",
                     m_stats.history_pixels_reset_total);
    }
    if (decision.rebuild_tile_schedule) {
      tileScheduler.begin_sample(generation, sample);
    }
    if (visualSnapshotChange) {
      pendingFirstTilePublishGeneration = generation;
      tilesSinceSnapshotChange = 0u;
    }
    activeSnapshot = currentSnapshot;
    return true;
  };

  activeSnapshot = snapshotRing.current(tracerSnapshotReader);
  if (!activeSnapshot) {
    mark_failed("render coordinator has no initial scene snapshot");
    if (tracer) {
      tracer->shutdown();
    }
    return;
  }
  generation = activeSnapshot->generation;
  refreshSnapshotStats();

  {
    std::scoped_lock lock(m_statsMutex);
    m_stats.running = true;
    m_stats.failed = false;
    m_stats.error.clear();
    m_stats.generation = generation;
    m_stats.sample_count = sample;
    m_stats.width = settings.width;
    m_stats.height = settings.height;
    m_stats.snapshot_generation = generation;
    m_stats.gpu_tiles_rendered_total.assign(m_config.gpu_count, 0u);
    m_lifecycle = RenderCoordinatorLifecycle::Running;
    m_targetSampleCount = settings.spp;
  }

  if (!tracer ||
      !tracer->configure(settings) ||
      !loadSnapshotIntoTracer(*activeSnapshot) ||
      !tracer->reset_accumulation()) {
    mark_failed("render coordinator tracer initialization failed");
    if (tracer) {
      tracer->shutdown();
    }
    return;
  }
  configureTileScheduler();

  auto publishResolvedFrame = [&](std::uint32_t publishedSampleCount,
                                  const vkpt::pathtracer::SampleCounters& counters) {
    auto ldr = tracer->resolve_ldr();
    DisplayFrame frame;
    frame.rgba8 = std::move(ldr.rgba8);
    frame.width = ldr.width;
    frame.height = ldr.height;
    frame.generation = generation;
    frame.sample_count = publishedSampleCount;
    frame.counters = counters;
    m_handoff.publish(std::move(frame));
    const auto now = std::chrono::steady_clock::now();
    lastPublish = now;
    record_frame_published(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch())
            .count()));
  };

  while (!stop.stop_requested()) {
    bool resetPublishClock = false;

    if (auto pendingSettings = drain_render_settings()) {
      settings = std::move(*pendingSettings);
      sample = 0u;
      bool settingsApplied =
          tracer->update_render_settings(settings) &&
          tracer->reset_accumulation();
      if (!settingsApplied &&
          (!tracer->configure(settings) ||
           !loadSnapshotIntoTracer(*activeSnapshot) ||
           !tracer->reset_accumulation())) {
        mark_failed("render coordinator settings update failed");
        break;
      }
      configureTileScheduler();
      resetPublishClock = true;
    }

    if (resetPublishClock) {
      lastPublish = std::chrono::steady_clock::time_point{};
      nextSampleStart = std::chrono::steady_clock::time_point{};
      m_handoff.clear(FrameDropReason::AccumulationReset);
    }

    update_stats(generation, sample, settings, tracer->read_counters());
    if (sample >= settings.spp) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    const auto publishHz =
        std::max<std::uint32_t>(1u, m_publishHz.load(std::memory_order_relaxed));
    const auto publishInterval = std::chrono::microseconds(
        std::max<std::uint32_t>(1u, 1000000u / publishHz));
    if (nextSampleStart != std::chrono::steady_clock::time_point{}) {
      SleepRenderWorkerUntil(stop, nextSampleStart);
      if (stop.stop_requested()) {
        break;
      }
    }
    const auto sampleStart = std::chrono::steady_clock::now();

    tileScheduler.begin_sample(generation, sample);
    vkpt::pathtracer::RenderTile tile;
    std::uint32_t tilesRenderedThisSample = 0u;
    bool tileFailed = false;
    while (true) {
      if (stop.stop_requested()) {
        break;
      }
      if (!consumeCurrentSnapshot()) {
        tileFailed = true;
        break;
      }
      if (!tileScheduler.next_tile(tile)) {
        break;
      }
      const auto tileStart = std::chrono::steady_clock::now();
      if (!tracer->render_tile_cancellable(tile, 0u, stop)) {
        if (!stop.stop_requested()) {
          mark_failed("render coordinator tile failed");
        }
        tileFailed = true;
        break;
      }
      const auto tileEnd = std::chrono::steady_clock::now();
      const double tileUs =
          std::chrono::duration<double, std::micro>(tileEnd - tileStart).count();
      ++tilesRenderedThisSample;
      VKP_METRIC_INC("vkp.tracer.tiles_total");
      VKP_METRIC_OBSERVE("vkp.render.tile_latency_us", tileUs);
      {
        std::scoped_lock lock(m_statsMutex);
        ++m_stats.tiles_rendered_total;
        if (tile.gpu_id >= m_stats.gpu_tiles_rendered_total.size()) {
          m_stats.gpu_tiles_rendered_total.resize(
              static_cast<std::size_t>(tile.gpu_id) + 1u,
              0u);
        }
        ++m_stats.gpu_tiles_rendered_total[tile.gpu_id];
        m_stats.tile_latency_last_us = tileUs;
        m_stats.tile_latency_max_us = std::max(m_stats.tile_latency_max_us, tileUs);
        if (tileUs > m_config.tile_latency_budget_us) {
          ++m_stats.tile_latency_over_budget_total;
        }
        const auto readerStats = snapshotRing.reader_stats(tracerSnapshotReader);
        m_stats.tracer_gen_lag = readerStats.lag;
      }
      auto& logger = vkpt::log::Logger::instance();
      if (logger.enabled(vkpt::log::Severity::Debug)) {
        logger.log(
            vkpt::log::Severity::Debug,
            "tracer",
            "tracer.tile_done",
            {{"tile_id", std::to_string(tile.tile_id)},
             {"gen", std::to_string(generation)},
             {"gpu_id", std::to_string(tile.gpu_id)},
             {"samples", std::to_string(tile.sample_index + 1u)},
             {"tile_us", std::to_string(tileUs)},
             {"variance", "0"}});
      }
      if (pendingFirstTilePublishGeneration == generation) {
        ++tilesSinceSnapshotChange;
        const auto countersAfterTile = tracer->read_counters();
        publishResolvedFrame(sample, countersAfterTile);
        {
          std::scoped_lock lock(m_statsMutex);
          ++m_stats.snapshot_first_tile_publish_total;
          m_stats.snapshot_first_tile_publish_max_tiles =
              std::max(m_stats.snapshot_first_tile_publish_max_tiles,
                       tilesSinceSnapshotChange);
        }
        pendingFirstTilePublishGeneration = 0u;
        tilesSinceSnapshotChange = 0u;
      } else if (pendingFirstTilePublishGeneration != 0u) {
        ++tilesSinceSnapshotChange;
      }
    }
    if (stop.stop_requested()) {
      break;
    }
    if (tilesRenderedThisSample == 0u) {
      mark_failed("render coordinator produced no tiles");
      break;
    }
    if (tileFailed) {
      break;
    }

    ++sample;
    const auto counters = tracer->read_counters();
    update_stats(generation, sample, settings, counters);

    const auto now = std::chrono::steady_clock::now();
    nextSampleStart = sampleStart + publishInterval;
    // Publish the first few samples immediately for responsiveness, then throttle
    // steady-state updates to the configured display cadence.
    const bool publishNow =
        sample <= m_config.immediate_publish_count ||
        lastPublish == std::chrono::steady_clock::time_point{} ||
        (now - lastPublish) >= publishInterval;
    if (publishNow) {
      publishResolvedFrame(sample, counters);
    }
  }

  if (tracer) {
    tracer->shutdown();
  }

  RenderCoordinatorStats snapshot;
  {
    std::scoped_lock lock(m_statsMutex);
    m_stats.running = false;
    if (!m_stats.failed && m_lifecycle != RenderCoordinatorLifecycle::Failed) {
      m_lifecycle = RenderCoordinatorLifecycle::Stopped;
      m_stoppedAtNs = SteadyNowNs();
    }
    m_workAvailable = false;
    m_workAvailableSinceNs = 0u;
    snapshot = m_stats;
  }
  snapshot.handoff = m_handoff.stats();
  VKP_LIFECYCLE_STOPPED("render",
                        "flow_id",
                        snapshot.generation,
                        "frames_published",
                        snapshot.handoff.published,
                        "errors_total",
                        m_errorsTotal);
  mirror_stats_to_metrics(snapshot);
}

}  // namespace vkpt::render
