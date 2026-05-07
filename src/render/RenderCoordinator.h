#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "pathtracer/PathTracer.h"
#include "render/FrameHandoff.h"

namespace vkpt::render {

/// Camera update posted from the app thread to the render worker.
struct RenderCameraCommand {
  vkpt::pathtracer::Vec3 position{};
  vkpt::pathtracer::Vec3 target{};
  vkpt::pathtracer::Vec3 up{};
  float fov_deg = 0.0f;
};

/// Runtime knobs for how aggressively the worker publishes display frames.
struct RenderCoordinatorConfig {
  std::uint32_t publish_hz = 60u;
  std::uint32_t immediate_publish_count = 4u;
};

/// Thread-safe snapshot of render worker state and latest handoff counters.
struct RenderCoordinatorStats {
  bool running = false;
  bool failed = false;
  std::string error;
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
};

struct InstanceTransformCommand {
  std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
  vkpt::pathtracer::InstanceTransformUpdateOptions options{};
};

/// Owns the background path-tracing loop and consumes app-thread updates.
///
/// The coordinator applies pending scene/settings/camera changes between sample
/// batches, resets accumulation when the scene generation changes, and publishes
/// resolved LDR frames through FrameHandoff.
class RenderCoordinator {
 public:
  RenderCoordinator(std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer,
                    vkpt::pathtracer::RenderSettings settings,
                    vkpt::pathtracer::RTSceneData scene,
                    RenderCoordinatorConfig config = {});
  ~RenderCoordinator();

  RenderCoordinator(const RenderCoordinator&) = delete;
  RenderCoordinator& operator=(const RenderCoordinator&) = delete;

  bool start();
  void stop();

  /// Queue camera basis/FOV replacement; supersedes any older queued camera command.
  void post_camera(RenderCameraCommand camera);
  /// Queue full camera-state replacement; supersedes any older queued camera state.
  void post_camera_state(vkpt::pathtracer::RTCameraState camera);
  /// Queue transform updates. Continuous motion uses latest-wins semantics.
  void post_instance_transforms(
      std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates);
  void post_instance_transforms(
      std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      vkpt::pathtracer::InstanceTransformUpdateOptions options);
  /// Queue a mergeable scene delta.
  void post_scene_delta(vkpt::pathtracer::RTSceneDeltaUpdate update);
  /// Queue a full scene replacement and discard partial scene updates.
  void post_scene(vkpt::pathtracer::RTSceneData scene);
  /// Queue render-settings replacement.
  void post_settings(vkpt::pathtracer::RenderSettings settings);
  /// Queue settings plus scene replacement as one generation change.
  void post_settings(vkpt::pathtracer::RenderSettings settings,
                     vkpt::pathtracer::RTSceneData scene);
  /// Update display-frame publication cadence without restarting the worker.
  void set_publish_hz(std::uint32_t publish_hz);

  /// Acquire the newest resolved display frame, if the worker has published one.
  std::optional<DisplayFrame> acquire_latest_frame();
  /// Return coordinator and handoff statistics.
  RenderCoordinatorStats stats() const;

 private:
  /// Coalesced command mailbox drained by the render worker once per loop.
  struct PendingCommands {
    std::optional<RenderCameraCommand> camera;
    std::optional<vkpt::pathtracer::RTCameraState> camera_state;
    std::optional<InstanceTransformCommand> instance_transforms;
    std::optional<vkpt::pathtracer::RTSceneDeltaUpdate> scene_delta;
    std::optional<vkpt::pathtracer::RTSceneData> scene;
    struct SettingsCommand {
      vkpt::pathtracer::RenderSettings settings;
      std::optional<vkpt::pathtracer::RTSceneData> scene;
    };
    std::optional<SettingsCommand> settings;
  };

  void run(std::stop_token stop, std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer);
  PendingCommands drain_commands();
  void mark_failed(std::string error);
  void update_stats(std::uint64_t generation,
                    std::uint32_t sample_count,
                    const vkpt::pathtracer::RenderSettings& settings,
                    const vkpt::pathtracer::SampleCounters& counters);

  vkpt::pathtracer::RenderSettings m_initialSettings{};
  vkpt::pathtracer::RTSceneData m_initialScene{};
  RenderCoordinatorConfig m_config{};
  std::unique_ptr<vkpt::pathtracer::IPathTracer> m_initialTracer;
  std::atomic<std::uint32_t> m_publishHz{60u};

  mutable std::mutex m_commandMutex;
  PendingCommands m_pending;

  mutable std::mutex m_statsMutex;
  RenderCoordinatorStats m_stats{};

  FrameHandoff m_handoff;
  std::jthread m_thread;
  std::atomic_bool m_started{false};
};

}  // namespace vkpt::render
