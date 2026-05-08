#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pathtracer/FilmBuffer.h"
#include "render/FrameHandoff.h"
#include "render/TileScheduler.h"

namespace vkpt::render {

struct MultiGpuAccumulationConfig {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t tile_height = 16u;
  std::uint32_t gpu_count = 1u;
  vkpt::pathtracer::FilmResolveSettings resolve{};
};

struct MultiGpuAccumulationStats {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t tile_height = 0u;
  std::uint32_t gpu_count = 0u;
  std::uint64_t accepted_tiles = 0u;
  std::uint64_t rejected_tiles = 0u;
  std::uint32_t resolved_min_sample_count = 0u;
  std::uint32_t resolved_max_sample_count = 0u;
  std::vector<std::uint64_t> accepted_tiles_per_gpu;
  std::vector<std::uint64_t> sampled_pixels_per_gpu;
};

struct MultiGpuScalingEstimate {
  std::uint32_t gpu_count = 0u;
  std::uint32_t scheduled_tile_count = 0u;
  std::uint32_t parallel_step_count = 0u;
  double normalized_tiles_per_step = 0.0;
  double relative_samples_per_second = 0.0;
  double relative_noise_for_fixed_time = 0.0;
  std::vector<std::uint32_t> scheduled_tiles_per_gpu;
};

class MultiGpuAccumulation {
 public:
  bool configure(MultiGpuAccumulationConfig config);
  void reset();

  bool accumulate_tile(const vkpt::pathtracer::RenderTile& tile,
                       const vkpt::pathtracer::Vec3& color);

  const vkpt::pathtracer::FilmBuffer* slice(std::uint32_t gpu_id) const;
  vkpt::pathtracer::FilmBuffer resolve_film() const;
  DisplayFrame resolve_display_frame(
      std::uint64_t generation,
      std::uint32_t sample_count,
      const vkpt::pathtracer::SampleCounters& counters = {}) const;
  MultiGpuAccumulationStats stats() const;

 private:
  bool tile_matches_contract(const vkpt::pathtracer::RenderTile& tile) const;
  std::uint32_t tile_count() const;
  std::uint32_t tile_start_y(std::uint32_t tile_id) const;
  std::uint32_t tile_end_y(std::uint32_t tile_id) const;

  MultiGpuAccumulationConfig m_config{};
  std::vector<vkpt::pathtracer::FilmBuffer> m_slices;
  MultiGpuAccumulationStats m_stats{};
};

std::vector<MultiGpuScalingEstimate> EstimateDeterministicMultiGpuScaling(
    TileSchedulerConfig base_config,
    std::span<const std::uint32_t> gpu_counts);

}  // namespace vkpt::render
