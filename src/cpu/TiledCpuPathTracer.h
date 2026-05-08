#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cpu/CpuContracts.h"
#include "cpu/ParallelBvhBuilder.h"
#include "jobs/JobSystem.h"
#include "pathtracer/PathTracer.h"

namespace vkpt::cpu {

/// Runtime configuration for TiledCpuPathTracer.
struct TiledRenderConfig {
  uint32_t tile_height = 16;   ///< Rows per tile.
  uint32_t worker_count = 0;   ///< 0 = use hardware concurrency.
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    const auto previous = determinism_context();
    deterministic = context.enabled;
    determinism_base_seed = context.base_seed;
    determinism_frame_index = context.frame_index;
    determinism_scenario_id = context.scenario_id;
    vkpt::core::EmitDeterminismChangedIfNeeded("cpu", previous, determinism_context());
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                               determinism_base_seed,
                                               determinism_frame_index,
                                               determinism_scenario_id);
  }
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

  vkpt::core::Status configure(const vkpt::pathtracer::RenderSettings& settings) override;
  vkpt::core::Status load_scene_snapshot(const vkpt::pathtracer::PathTracerSceneSnapshot& scene) override;
  vkpt::core::Status build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool replace_film_history(const vkpt::pathtracer::FilmBuffer& film) override;
  bool update_camera(const vkpt::pathtracer::Vec3& pos,
                     const vkpt::pathtracer::Vec3& target,
                     const vkpt::pathtracer::Vec3& up,
                     float fov_deg) override;
  bool update_camera_state(const vkpt::pathtracer::RTCameraState& camera) override;
  bool update_instance_transforms(
      const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) override;
  vkpt::pathtracer::InstanceTransformPlan plan_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) const override;
  vkpt::pathtracer::InstanceTransformUpdateResult apply_instance_transform_update(
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) override;
  bool update_scene_delta(const vkpt::pathtracer::RTSceneDeltaUpdate& update) override;
  bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) override;
  bool render_tile(const vkpt::pathtracer::RenderTile& tile, uint32_t frame_index) override;
  bool render_sample_batch_cancellable(uint32_t start_y,
                                       uint32_t end_y,
                                       uint32_t sample_index,
                                       uint32_t frame_index,
                                       std::stop_token stop);
  bool render_tile_cancellable(const vkpt::pathtracer::RenderTile& tile,
                               uint32_t frame_index,
                               std::stop_token stop) override;
  bool supports_tile_rendering() const override { return true; }
  std::string_view name() const override { return "tiled-cpu"; }
  bool set_accelerator(vkpt::pathtracer::IRayAccelerator* accelerator) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
  vkpt::pathtracer::PathTracerStatus status() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

  /// Parallel BVH stats from the last build_or_update_acceleration call.
  BvhBuildStats bvh_stats() const { return m_bvhStats; }
  CpuPathTracerStatus cpu_status() const;
  void set_flow_source(const vkpt::core::contracts::IFlowSource* flow_source) {
    m_flowSource = flow_source;
  }

  std::size_t worker_count() const;
  uint32_t tile_height() const { return m_config.tile_height; }

 private:
  void init_tile_tracers();
  void merge_tiles();

  TiledRenderConfig m_config;
  vkpt::pathtracer::RenderSettings m_settings;
  vkpt::pathtracer::PathTracerSceneSnapshot m_scene;
  vkpt::pathtracer::FilmBuffer m_film;
  vkpt::pathtracer::SampleCounters m_counters{};
  vkpt::pathtracer::IRayAccelerator* m_externalAccelerator = nullptr;
  std::unique_ptr<vkpt::pathtracer::IRayAccelerator> m_sharedAccelerator;

  std::unique_ptr<vkpt::jobs::JobSystem> m_jobSystem;
  BvhBuildStats m_bvhStats{};
  CpuPathTracerStatus m_status{};
  const vkpt::core::contracts::IFlowSource* m_flowSource = nullptr;

  // One scalar tracer per tile; each receives the shared accelerator above.
  struct TileState {
    uint32_t start_y = 0;
    uint32_t end_y = 0;
    std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
    vkpt::pathtracer::ICpuRayKernel* kernel = nullptr;
  };
  std::vector<TileState> m_tiles;
  bool m_configured = false;
  bool m_hasScene = false;
  bool m_initialized = false;
  bool m_failed = false;
  std::uint32_t m_currentSample = 0u;
  std::uint64_t m_accumulationGen = 0u;
  std::string m_lastError;
};

}  // namespace vkpt::cpu
