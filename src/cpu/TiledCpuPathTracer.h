#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "cpu/CpuFeatures.h"
#include "cpu/ParallelBvhBuilder.h"
#include "jobs/JobSystem.h"
#include "pathtracer/PathTracer.h"

namespace vkpt::cpu {

struct TiledRenderConfig {
  uint32_t tile_height = 16;   // rows per tile
  uint32_t worker_count = 0;   // 0 = use hardware concurrency
  bool deterministic = false;
};

// Implements IPathTracer using a tile-based multithreaded render via IJobSystem.
// Each tile renders a horizontal band of rows on its own ScalarCpuPathTracer instance
// to avoid data races on the film buffer. Tiles are merged after each render_sample_batch.
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
  bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) override;
  std::string_view name() const override { return "tiled-cpu"; }
  bool set_accelerator(vkpt::pathtracer::IRayAccelerator* accelerator) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
  const vkpt::pathtracer::FilmBuffer& film() const override { return m_film; }
  void shutdown() override;

  // Parallel BVH stats from the last build_or_update_acceleration call.
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

  std::unique_ptr<vkpt::jobs::JobSystem> m_jobSystem;
  ParallelBvhBuilder m_bvhBuilder;
  BvhBuildStats m_bvhStats{};
  SimdDispatchInfo m_simdDispatch;

  // One IPathTracer per tile (Scalar or AVX2 depending on CPU features)
  struct TileState {
    uint32_t start_y = 0;
    uint32_t end_y = 0;
    std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
  };
  std::vector<TileState> m_tiles;
  bool m_initialized = false;
};

}  // namespace vkpt::cpu
