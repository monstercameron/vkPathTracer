#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "pathtracer/PathTracer.h"

namespace vkpt::render {

struct TileSchedulerConfig {
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::uint32_t tile_height = 16u;
  std::uint32_t gpu_count = 1u;
  std::uint32_t foveated_center_extra_samples = 0u;
  double foveated_center_radius = 0.25;
};

struct TileSchedulerStats {
  std::uint64_t generation = 0u;
  std::uint32_t sample_index = 0u;
  std::uint32_t tile_count = 0u;
  std::uint32_t next_tile = 0u;
  std::uint32_t scheduled_tile_count = 0u;
  std::vector<std::uint32_t> gpu_scheduled_tile_count;
};

struct TilePriorityFeedback {
  std::uint32_t tile_id = 0u;
  double variance = 0.0;
  std::uint32_t sample_count = 0u;
  bool dirty = false;
};

class TileScheduler {
 public:
  void configure(TileSchedulerConfig config);
  void set_feedback(std::span<const TilePriorityFeedback> feedback);
  void begin_sample(std::uint64_t generation, std::uint32_t sample_index);
  bool next_tile(vkpt::pathtracer::RenderTile& out);
  TileSchedulerStats stats() const;

 private:
  struct ScheduledTile {
    std::uint32_t tile_id = 0u;
    std::uint32_t sample_offset = 0u;
  };

  void rebuild_order();
  bool is_foveated_center_tile(std::uint32_t tile_id) const;

  TileSchedulerConfig m_config{};
  std::uint64_t m_generation = 0u;
  std::uint32_t m_sampleIndex = 0u;
  std::uint32_t m_nextTile = 0u;
  std::uint32_t m_tileCount = 0u;
  std::vector<TilePriorityFeedback> m_feedback;
  std::vector<ScheduledTile> m_order;
};

}  // namespace vkpt::render
