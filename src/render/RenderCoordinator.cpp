#include "render/RenderCoordinator.h"

#include <algorithm>
#include <utility>

namespace vkpt::render {

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
    run(stop, std::move(tracer));
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

void RenderCoordinator::post_scene(vkpt::pathtracer::RTSceneData scene) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.scene = std::move(scene);
}

void RenderCoordinator::post_settings(vkpt::pathtracer::RenderSettings settings,
                                      vkpt::pathtracer::RTSceneData scene) {
  std::scoped_lock lock(m_commandMutex);
  m_pending.settings = PendingCommands::SettingsAndScene{std::move(settings), std::move(scene)};
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

    if (commands.settings) {
      settings = std::move(commands.settings->settings);
      scene = std::move(commands.settings->scene);
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
