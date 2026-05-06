#include "render/RenderCoordinator.h"

#include "core/Logging.h"
#include "diagnostics/CrashRecorder.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace vkpt::render {

namespace {

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

}  // namespace

RenderCoordinator::RenderCoordinator(std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer,
                                     vkpt::pathtracer::RenderSettings settings,
                                     vkpt::pathtracer::RTSceneData scene,
                                     RenderCoordinatorConfig config)
    : m_initialSettings(std::move(settings)),
      m_initialScene(std::move(scene)),
      m_config(config),
      m_initialTracer(std::move(tracer)) {
  m_config.publish_hz = std::max<std::uint32_t>(1u, m_config.publish_hz);
}

RenderCoordinator::~RenderCoordinator() {
  stop();
}

bool RenderCoordinator::start() {
  if (m_started.exchange(true, std::memory_order_acq_rel)) {
    return true;
  }
  if (!m_initialTracer) {
    m_started.store(false, std::memory_order_release);
    mark_failed("render coordinator has no tracer");
    return false;
  }

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
  return true;
}

void RenderCoordinator::stop() {
  if (!m_started.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (m_thread.joinable()) {
    m_thread.request_stop();
    m_thread.join();
  }
}

void RenderCoordinator::post_camera(RenderCameraCommand camera) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.camera = camera;
}

void RenderCoordinator::post_camera_state(vkpt::pathtracer::RTCameraState camera) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.camera_state = camera;
}

void RenderCoordinator::post_instance_transforms(
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates) {
  if (updates.empty()) {
    return;
  }
  std::scoped_lock lock(m_commandMutex);
  if (!m_pending.instance_transforms) {
    m_pending.instance_transforms = std::move(updates);
    return;
  }
  auto& pending = *m_pending.instance_transforms;
  pending.reserve(pending.size() + updates.size());
  const auto sameTarget =
      [](const vkpt::pathtracer::RTInstanceTransformUpdate& lhs,
         const vkpt::pathtracer::RTInstanceTransformUpdate& rhs) {
    if (lhs.instance_index != vkpt::pathtracer::kInvalidRTInstanceIndex &&
        rhs.instance_index != vkpt::pathtracer::kInvalidRTInstanceIndex) {
      return lhs.instance_index == rhs.instance_index;
    }
    return lhs.entity_id != 0u && lhs.entity_id == rhs.entity_id;
  };
  for (auto& update : updates) {
    const auto it = std::find_if(pending.begin(),
                                 pending.end(),
                                 [&](const auto& existing) {
                                   return sameTarget(existing, update);
                                 });
    if (it != pending.end()) {
      *it = std::move(update);
    } else {
      pending.push_back(std::move(update));
    }
  }
}

void RenderCoordinator::post_scene_delta(vkpt::pathtracer::RTSceneDeltaUpdate update) {
  if (update.materials.empty() &&
      update.lights.empty() &&
      !update.environment_color_changed) {
    return;
  }
  std::scoped_lock lock(m_commandMutex);
  if (m_pending.scene_delta) {
    vkpt::pathtracer::MergeSceneDeltaUpdates(*m_pending.scene_delta, update);
  } else {
    m_pending.scene_delta = std::move(update);
  }
}

void RenderCoordinator::post_scene(vkpt::pathtracer::RTSceneData scene) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.scene = std::move(scene);
  m_pending.scene_delta.reset();
  m_pending.instance_transforms.reset();
}

void RenderCoordinator::post_settings(vkpt::pathtracer::RenderSettings settings) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.settings = PendingCommands::SettingsCommand{std::move(settings), std::nullopt};
}

void RenderCoordinator::post_settings(vkpt::pathtracer::RenderSettings settings,
                                      vkpt::pathtracer::RTSceneData scene) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.settings = PendingCommands::SettingsCommand{std::move(settings), std::move(scene)};
  m_pending.scene_delta.reset();
  m_pending.instance_transforms.reset();
}

std::optional<DisplayFrame> RenderCoordinator::acquire_latest_frame() {
  return m_handoff.acquire_latest();
}

RenderCoordinatorStats RenderCoordinator::stats() const {
  std::scoped_lock lock(m_statsMutex);
  auto out = m_stats;
  out.handoff = m_handoff.stats();
  return out;
}

RenderCoordinator::PendingCommands RenderCoordinator::drain_commands() {
  std::scoped_lock lock(m_commandMutex);
  // Drain by move so bursts of UI changes collapse into one worker-side update.
  PendingCommands out = std::move(m_pending);
  m_pending = {};
  return out;
}

void RenderCoordinator::mark_failed(std::string error) {
  std::scoped_lock lock(m_statsMutex);
  m_stats.failed = true;
  m_stats.running = false;
  m_stats.error = std::move(error);
}

void RenderCoordinator::update_stats(std::uint64_t generation,
                                     std::uint32_t sample_count,
                                     const vkpt::pathtracer::RenderSettings& settings,
                                     const vkpt::pathtracer::SampleCounters& counters) {
  std::scoped_lock lock(m_statsMutex);
  m_stats.running = m_started.load(std::memory_order_acquire);
  m_stats.generation = generation;
  m_stats.sample_count = sample_count;
  m_stats.width = settings.width;
  m_stats.height = settings.height;
  m_stats.counters = counters;
}

void RenderCoordinator::run(std::stop_token stop,
                            std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer) {
  std::uint64_t generation = 1u;
  std::uint32_t sample = 0u;
  auto settings = m_initialSettings;
  auto scene = m_initialScene;
  const auto publishInterval = std::chrono::microseconds(
      std::max<std::uint32_t>(1u, 1000000u / std::max<std::uint32_t>(1u, m_config.publish_hz)));
  auto lastPublish = std::chrono::steady_clock::time_point{};

  {
    std::scoped_lock lock(m_statsMutex);
    m_stats.running = true;
    m_stats.failed = false;
    m_stats.error.clear();
    m_stats.generation = generation;
    m_stats.sample_count = sample;
    m_stats.width = settings.width;
    m_stats.height = settings.height;
  }

  if (!tracer ||
      !tracer->configure(settings) ||
      !tracer->load_scene_snapshot(scene) ||
      !tracer->build_or_update_acceleration() ||
      !tracer->reset_accumulation()) {
    mark_failed("render coordinator tracer initialization failed");
    if (tracer) {
      tracer->shutdown();
    }
    return;
  }

  while (!stop.stop_requested()) {
    auto commands = drain_commands();
    bool resetPublishClock = false;

    // Apply structural updates before sample work. Each accepted mutation bumps
    // the generation, resets accumulation, and clears stale display frames.
    if (commands.settings) {
      settings = std::move(commands.settings->settings);
      if (commands.settings->scene) {
        scene = std::move(*commands.settings->scene);
      }
      ++generation;
      sample = 0u;
      if (!tracer->configure(settings) ||
          !tracer->load_scene_snapshot(scene) ||
          !tracer->build_or_update_acceleration() ||
          !tracer->reset_accumulation()) {
        mark_failed("render coordinator settings update failed");
        break;
      }
      resetPublishClock = true;
    } else if (commands.scene) {
      scene = std::move(*commands.scene);
      ++generation;
      sample = 0u;
      if (!tracer->load_scene_snapshot(scene) ||
          !tracer->build_or_update_acceleration() ||
          !tracer->reset_accumulation()) {
        mark_failed("render coordinator scene update failed");
        break;
      }
      resetPublishClock = true;
    }

    if (commands.scene_delta) {
      auto nextScene = scene;
      if (!vkpt::pathtracer::ApplySceneDeltaUpdate(nextScene, *commands.scene_delta)) {
        mark_failed("render coordinator scene delta was invalid");
        break;
      }
      scene = std::move(nextScene);
      ++generation;
      sample = 0u;
      bool deltaOk = tracer->update_scene_delta(*commands.scene_delta);
      if (!deltaOk) {
        deltaOk = tracer->load_scene_snapshot(scene) &&
                  tracer->build_or_update_acceleration();
      }
      if (!deltaOk || !tracer->reset_accumulation()) {
        mark_failed("render coordinator scene delta update failed");
        break;
      }
      resetPublishClock = true;
    }

    if (commands.camera) {
      scene.camera_position = commands.camera->position;
      scene.camera_target = commands.camera->target;
      scene.camera_up = commands.camera->up;
      scene.camera_fov_deg = commands.camera->fov_deg;
      ++generation;
      sample = 0u;
      bool cameraOk = tracer->update_camera(
          commands.camera->position,
          commands.camera->target,
          commands.camera->up,
          commands.camera->fov_deg);
      if (!cameraOk) {
        cameraOk = tracer->load_scene_snapshot(scene) &&
                   tracer->build_or_update_acceleration();
      }
      if (!cameraOk || !tracer->reset_accumulation()) {
        mark_failed("render coordinator camera update failed");
        break;
      }
      resetPublishClock = true;
    }

    if (commands.camera_state) {
      vkpt::pathtracer::ApplyCameraState(scene, *commands.camera_state);
      ++generation;
      sample = 0u;
      bool cameraOk = tracer->update_camera_state(*commands.camera_state);
      if (!cameraOk) {
        cameraOk = tracer->load_scene_snapshot(scene) &&
                   tracer->build_or_update_acceleration();
      }
      if (!cameraOk || !tracer->reset_accumulation()) {
        mark_failed("render coordinator camera state update failed");
        break;
      }
      resetPublishClock = true;
    }

    if (commands.instance_transforms && !commands.instance_transforms->empty()) {
      vkpt::pathtracer::ApplyInstanceTransformUpdates(scene, *commands.instance_transforms);
      ++generation;
      sample = 0u;
      bool transformOk = tracer->update_instance_transforms(*commands.instance_transforms);
      if (!transformOk) {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Warning,
            "render",
            "instance transform update fell back to scene rebuild",
            {{"updates", std::to_string(commands.instance_transforms->size())}});
        transformOk = tracer->load_scene_snapshot(scene) &&
                      tracer->build_or_update_acceleration();
      }
      if (!transformOk || !tracer->reset_accumulation()) {
        mark_failed("render coordinator instance transform update failed");
        break;
      }
      resetPublishClock = true;
    }

    if (resetPublishClock) {
      lastPublish = std::chrono::steady_clock::time_point{};
      m_handoff.clear();
    }

    update_stats(generation, sample, settings, tracer->read_counters());
    if (sample >= settings.spp) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    if (!tracer->render_sample_batch_cancellable(0u, settings.height, sample, 0u, stop)) {
      if (!stop.stop_requested()) {
        mark_failed("render coordinator sample failed");
      }
      break;
    }

    ++sample;
    const auto counters = tracer->read_counters();
    update_stats(generation, sample, settings, counters);

    const auto now = std::chrono::steady_clock::now();
    // Publish the first few samples immediately for responsiveness, then throttle
    // steady-state updates to the configured display cadence.
    const bool publishNow =
        sample <= m_config.immediate_publish_count ||
        lastPublish == std::chrono::steady_clock::time_point{} ||
        (now - lastPublish) >= publishInterval;
    if (publishNow) {
      auto ldr = tracer->resolve_ldr();
      DisplayFrame frame;
      frame.rgba8 = std::move(ldr.rgba8);
      frame.width = ldr.width;
      frame.height = ldr.height;
      frame.generation = generation;
      frame.sample_count = sample;
      frame.counters = counters;
      m_handoff.publish(std::move(frame));
      lastPublish = now;
    }
  }

  if (tracer) {
    tracer->shutdown();
  }

  {
    std::scoped_lock lock(m_statsMutex);
    m_stats.running = false;
  }
}

}  // namespace vkpt::render
