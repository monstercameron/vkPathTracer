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

struct RenderCameraCommand {
  vkpt::pathtracer::Vec3 position{};
  vkpt::pathtracer::Vec3 target{};
  vkpt::pathtracer::Vec3 up{};
  float fov_deg = 0.0f;
};

struct RenderCoordinatorConfig {
  std::uint32_t publish_hz = 30u;
  std::uint32_t immediate_publish_count = 4u;
};

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
};

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

  void post_camera(RenderCameraCommand camera);
  void post_camera_state(vkpt::pathtracer::RTCameraState camera);
  void post_instance_transforms(
      std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates);
  void post_scene(vkpt::pathtracer::RTSceneData scene);
  void post_settings(vkpt::pathtracer::RenderSettings settings);
  void post_settings(vkpt::pathtracer::RenderSettings settings,
                     vkpt::pathtracer::RTSceneData scene);

  std::optional<DisplayFrame> acquire_latest_frame();
  RenderCoordinatorStats stats() const;

 private:
  struct PendingCommands {
    std::optional<RenderCameraCommand> camera;
    std::optional<vkpt::pathtracer::RTCameraState> camera_state;
    std::optional<std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>> instance_transforms;
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

  mutable std::mutex m_commandMutex;
  PendingCommands m_pending;

  mutable std::mutex m_statsMutex;
  RenderCoordinatorStats m_stats{};

  FrameHandoff m_handoff;
  std::jthread m_thread;
  std::atomic_bool m_started{false};
};

}  // namespace vkpt::render
