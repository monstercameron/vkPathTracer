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

void LogTransformCommand(vkpt::log::Severity severity,
                         std::string_view message,
                         std::size_t updateCount,
                         const vkpt::pathtracer::InstanceTransformUpdateOptions& options,
                         vkpt::pathtracer::InstanceTransformUpdateStatus status,
                         std::uint32_t matchedOrAppliedCount = 0u,
                         const char* detail = nullptr) {
  auto& logger = vkpt::log::Logger::instance();
  if (!logger.enabled(severity)) {
    return;
  }
  logger.log(
      severity,
      "render",
      message,
      {{"updates", std::to_string(updateCount)},
       {"matched_or_applied", std::to_string(matchedOrAppliedCount)},
       {"reason", vkpt::pathtracer::ToString(options.reason)},
       {"policy", vkpt::pathtracer::ToString(options.fallback_policy)},
       {"status", vkpt::pathtracer::ToString(status)},
       {"source", options.source_system ? options.source_system : ""},
       {"detail", detail ? detail : ""}},
      options.source_frame);
}

std::string_view SourceSystemView(const vkpt::pathtracer::InstanceTransformUpdateOptions& options) {
  return options.source_system ? std::string_view(options.source_system) : std::string_view();
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

bool CanCoalesceTransformOptions(
    const vkpt::pathtracer::InstanceTransformUpdateOptions& lhs,
    const vkpt::pathtracer::InstanceTransformUpdateOptions& rhs) {
  return lhs.coalesce &&
         rhs.coalesce &&
         lhs.reason == rhs.reason &&
         lhs.fallback_policy == rhs.fallback_policy &&
         lhs.reset_accumulation == rhs.reset_accumulation &&
         lhs.allow_partial == rhs.allow_partial &&
         SourceSystemView(lhs) == SourceSystemView(rhs);
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
  m_publishHz.store(m_config.publish_hz, std::memory_order_relaxed);
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
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Warning,
      "render",
      "legacy instance transform command posted without reason or fallback policy",
      {{"updates", std::to_string(updates.size())}});
  auto options = vkpt::pathtracer::MakeStandardTransformUpdateOptions(
      vkpt::pathtracer::RenderUpdateReason::LegacyUnknown,
      0u,
      "legacy");
  post_instance_transforms(std::move(updates), options);
}

void RenderCoordinator::post_instance_transforms(
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates,
    vkpt::pathtracer::InstanceTransformUpdateOptions options) {
  if (updates.empty()) {
    return;
  }
  LogTransformCommand(vkpt::log::Severity::Debug,
                      "queued instance transform command",
                      updates.size(),
                      options,
                      vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported);
  std::scoped_lock lock(m_commandMutex);
  if (!m_pending.instance_transforms) {
    m_pending.instance_transforms = InstanceTransformCommand{std::move(updates), options};
    return;
  }
  auto& pending_command = *m_pending.instance_transforms;
  auto& pending = pending_command.updates;
  if (!CanCoalesceTransformOptions(pending_command.options, options)) {
    LogTransformCommand(vkpt::log::Severity::Debug,
                        "replaced pending instance transform command",
                        updates.size(),
                        options,
                        vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported);
    pending_command = InstanceTransformCommand{std::move(updates), options};
    return;
  }
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

void RenderCoordinator::set_publish_hz(std::uint32_t publish_hz) {
  m_publishHz.store(std::max<std::uint32_t>(1u, publish_hz),
                    std::memory_order_relaxed);
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
  auto lastPublish = std::chrono::steady_clock::time_point{};
  auto nextSampleStart = std::chrono::steady_clock::time_point{};

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
      const bool hasReplacementScene = commands.settings->scene.has_value();
      if (hasReplacementScene) {
        scene = std::move(*commands.settings->scene);
      }
      ++generation;
      sample = 0u;
      bool settingsApplied = false;
      if (!hasReplacementScene) {
        settingsApplied =
            tracer->update_render_settings(settings) &&
            tracer->reset_accumulation();
      }
      if (!settingsApplied &&
          (!tracer->configure(settings) ||
           !tracer->load_scene_snapshot(scene) ||
           !tracer->build_or_update_acceleration() ||
           !tracer->reset_accumulation())) {
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

    if (commands.instance_transforms && !commands.instance_transforms->updates.empty()) {
      auto& command = *commands.instance_transforms;
      auto& updates = command.updates;
      auto& options = command.options;
      {
        std::scoped_lock lock(m_statsMutex);
        ++m_stats.instance_transform_commands;
        m_stats.instance_transform_updates_requested += updates.size();
      }

      const auto plan = tracer->plan_instance_transform_update(updates, options);
      if (!vkpt::pathtracer::TransformUpdateStatusAllowedByPolicy(plan.status,
                                                                  options.fallback_policy)) {
        {
          std::scoped_lock lock(m_statsMutex);
          ++m_stats.instance_transform_policy_rejections;
          if (plan.status == vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild) {
            ++m_stats.instance_transform_full_accel_required;
          } else if (plan.status == vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullSceneReload) {
            ++m_stats.instance_transform_full_scene_required;
          } else {
            ++m_stats.instance_transform_failures;
          }
        }
        LogTransformCommand(vkpt::log::Severity::Warning,
                            "rejected instance transform update by fallback policy",
                            updates.size(),
                            options,
                            plan.status,
                            plan.matched_count,
                            plan.message);
      } else if (plan.can_apply_without_full_fallback()) {
        LogTransformCommand(vkpt::log::Severity::Debug,
                            "planned direct instance transform update",
                            updates.size(),
                            options,
                            plan.status,
                            plan.matched_count,
                            plan.message);
        const auto result = tracer->apply_instance_transform_update(updates, options);
        if (!result.applied()) {
          {
            std::scoped_lock lock(m_statsMutex);
            ++m_stats.instance_transform_failures;
          }
          LogTransformCommand(vkpt::log::Severity::Warning,
                              "instance transform update failed without committing state",
                              updates.size(),
                              options,
                              result.status,
                              result.applied_count,
                              result.message);
        } else {
          if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(
                  scene,
                  updates,
                  vkpt::pathtracer::RTInstanceTransformApplyMode::MetadataOnly)) {
            mark_failed("render coordinator failed to commit instance transform metadata");
            break;
          }
          ++generation;
          sample = 0u;
          if (options.reset_accumulation && !tracer->reset_accumulation()) {
            mark_failed("render coordinator instance transform accumulation reset failed");
            break;
          }
          {
            std::scoped_lock lock(m_statsMutex);
            m_stats.instance_transform_updates_applied += result.applied_count;
            if (result.status == vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate) {
              ++m_stats.instance_transform_dynamic_accel_updates;
            }
          }
          LogTransformCommand(vkpt::log::Severity::Debug,
                              "applied direct instance transform update",
                              updates.size(),
                              options,
                              result.status,
                              result.applied_count,
                              result.message);
          resetPublishClock = true;
        }
      } else if (plan.status == vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild &&
                 options.fallback_policy >= vkpt::pathtracer::TransformFallbackPolicy::AllowFullStaticAccelerationBuild) {
        LogTransformCommand(vkpt::log::Severity::Info,
                            "using full acceleration rebuild for instance transform update",
                            updates.size(),
                            options,
                            plan.status,
                            plan.matched_count,
                            plan.message);
        auto nextScene = scene;
        if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(nextScene, updates) ||
            !tracer->load_scene_snapshot(nextScene) ||
            !tracer->build_or_update_acceleration()) {
          (void)(tracer->load_scene_snapshot(scene) &&
                 tracer->build_or_update_acceleration() &&
                 (!options.reset_accumulation || tracer->reset_accumulation()));
          mark_failed("render coordinator instance transform full acceleration fallback failed");
          break;
        }
        scene = std::move(nextScene);
        ++generation;
        sample = 0u;
        if (options.reset_accumulation && !tracer->reset_accumulation()) {
          mark_failed("render coordinator instance transform fallback accumulation reset failed");
          break;
        }
        {
          std::scoped_lock lock(m_statsMutex);
          m_stats.instance_transform_updates_applied += updates.size();
          ++m_stats.instance_transform_full_accel_required;
        }
        LogTransformCommand(vkpt::log::Severity::Debug,
                            "applied full acceleration rebuild for instance transform update",
                            updates.size(),
                            options,
                            vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedFullStaticAccelRebuild,
                            static_cast<std::uint32_t>(updates.size()));
        resetPublishClock = true;
      } else if ((plan.status == vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullSceneReload ||
                  plan.status == vkpt::pathtracer::InstanceTransformUpdateStatus::Unsupported) &&
                 options.fallback_policy >= vkpt::pathtracer::TransformFallbackPolicy::AllowFullSceneReload) {
        LogTransformCommand(vkpt::log::Severity::Info,
                            "using full scene reload for instance transform update",
                            updates.size(),
                            options,
                            plan.status,
                            plan.matched_count,
                            plan.message);
        auto nextScene = scene;
        if (!vkpt::pathtracer::ApplyInstanceTransformUpdates(nextScene, updates) ||
            !tracer->load_scene_snapshot(nextScene) ||
            !tracer->build_or_update_acceleration() ||
            (options.reset_accumulation && !tracer->reset_accumulation())) {
          (void)(tracer->load_scene_snapshot(scene) &&
                 tracer->build_or_update_acceleration() &&
                 (!options.reset_accumulation || tracer->reset_accumulation()));
          mark_failed("render coordinator instance transform full scene fallback failed");
          break;
        }
        scene = std::move(nextScene);
        ++generation;
        sample = 0u;
        {
          std::scoped_lock lock(m_statsMutex);
          m_stats.instance_transform_updates_applied += updates.size();
          ++m_stats.instance_transform_full_scene_required;
        }
        LogTransformCommand(vkpt::log::Severity::Debug,
                            "applied full scene reload for instance transform update",
                            updates.size(),
                            options,
                            vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedFullSceneReload,
                            static_cast<std::uint32_t>(updates.size()));
        resetPublishClock = true;
      }
    }

    if (resetPublishClock) {
      lastPublish = std::chrono::steady_clock::time_point{};
      nextSampleStart = std::chrono::steady_clock::time_point{};
      m_handoff.clear();
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
    nextSampleStart = sampleStart + publishInterval;
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
