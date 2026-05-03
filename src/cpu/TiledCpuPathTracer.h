#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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
class TiledCpuPathTracer final : public vkpt::pathtracer::IPathTracer {
 public:
  explicit TiledCpuPathTracer(TiledRenderConfig config = {});
  ~TiledCpuPathTracer() override;

  bool configure(const vkpt::pathtracer::RenderSettings& settings) override;
  bool load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) override;
  bool build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) override;
  vkpt::pathtracer::FilmLdr resolve_ldr() const override;
  vkpt::pathtracer::FilmHdr resolve_hdr() const override;
  vkpt::pathtracer::SampleCounters read_counters() const override;
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

  std::unique_ptr<vkpt::jobs::JobSystem> m_jobSystem;
  ParallelBvhBuilder m_bvhBuilder;
  BvhBuildStats m_bvhStats{};

  // One ScalarCpuPathTracer per tile
  struct TileState {
    uint32_t start_y = 0;
    uint32_t end_y = 0;
    std::unique_ptr<vkpt::pathtracer::ScalarCpuPathTracer> tracer;
  };
  std::vector<TileState> m_tiles;
  bool m_initialized = false;
};

}  // namespace vkpt::cpu
