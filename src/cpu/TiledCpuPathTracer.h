#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "cpu/ParallelBvhBuilder.h"
#include "jobs/JobSystem.h"
#include "pathtracer/PathTracer.h"

namespace vkpt::cpu {

/// Runtime configuration for TiledCpuPathTracer.
struct TiledRenderConfig {
  uint32_t tile_height = 16;   ///< Rows per tile.
  uint32_t worker_count = 0;   ///< 0 = use hardware concurrency.
  bool deterministic = false;
};

/// Tile-based CPU path tracer backed by IJobSystem.
///
/// Each tile owns a ScalarCpuPathTracer for a horizontal row band. Tiles share
/// one accelerator when available, render independently, and merge their film
/// buffers after each sample batch to avoid concurrent writes to m_film.
class TiledCpuPathTracer final : public vkpt::pathtracer::IPathTracer, public vkpt::pathtracer::ICpuRayKernel {
 public:
  using vkpt::pathtracer::IPathTracer::configure;

  explicit TiledCpuPathTracer(TiledRenderConfig config = {});
  ~TiledCpuPathTracer() override;

  bool configure(const vkpt::pathtracer::RenderSettings& settings) override;
  bool load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) override;
  bool build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool update_camera(const vkpt::pathtracer::Vec3& pos,
                     const vkpt::pathtracer::Vec3& target,
                     const vkpt::pathtracer::Vec3& up,
                     float fov_deg) override;
  bool update_camera_state(const vkpt::pathtracer::RTCameraState& camera) override;
  bool update_instance_transforms(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) override;
  vkpt::pathtracer::InstanceTransformUpdatePlan plan_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) const override;
  vkpt::pathtracer::InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) override;
  bool update_scene_delta(const vkpt::pathtracer::RTSceneDeltaUpdate& update) override;
  bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) override;
  bool render_sample_batch_cancellable(uint32_t start_y,
                                       uint32_t end_y,
                                       uint32_t sample_index,
                                       uint32_t frame_index,
                                       std::stop_token stop) override;
  std::string_view name() const override { return "tiled-cpu"; }
  bool set_accelerator(vkpt::pathtracer::IRayAccelerator* accelerator) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

  /// Parallel BVH stats from the last build_or_update_acceleration call.
  BvhBuildStats bvh_stats() const { return m_bvhStats; }

  std::size_t worker_count() const;
  uint32_t tile_height() const { return m_config.tile_height; }

 private:
  void init_tile_tracers();
  void merge_tiles();

  TiledRenderConfig m_config;
  vkpt::pathtracer::RenderSettings m_settings;
  vkpt::pathtracer::RTSceneData m_scene;
  vkpt::pathtracer::FilmBuffer m_film;
  vkpt::pathtracer::SampleCounters m_counters{};
  vkpt::pathtracer::IRayAccelerator* m_externalAccelerator = nullptr;
  std::unique_ptr<vkpt::pathtracer::IRayAccelerator> m_sharedAccelerator;

  std::unique_ptr<vkpt::jobs::JobSystem> m_jobSystem;
  BvhBuildStats m_bvhStats{};

  // One scalar tracer per tile; each receives the shared accelerator above.
  struct TileState {
    uint32_t start_y = 0;
    uint32_t end_y = 0;
    std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
    vkpt::pathtracer::ICpuRayKernel* kernel = nullptr;
  };
  std::vector<TileState> m_tiles;
  bool m_initialized = false;
};

}  // namespace vkpt::cpu
