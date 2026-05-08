#include "render/TileScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace vkpt::render {

void TileScheduler::configure(TileSchedulerConfig config) {
  config.tile_height = std::max(1u, config.tile_height);
  config.gpu_count = std::max(1u, config.gpu_count);
  config.foveated_center_radius =
      std::clamp(config.foveated_center_radius, 0.0, 0.5);
  m_config = config;
  m_tileCount = config.height == 0u
      ? 0u
      : (config.height + config.tile_height - 1u) / config.tile_height;
  m_feedback.clear();
  m_feedback.resize(m_tileCount);
  for (std::uint32_t tileId = 0u; tileId < m_tileCount; ++tileId) {
    m_feedback[tileId].tile_id = tileId;
  }
  m_order.clear();
  m_nextTile = 0u;
}

void TileScheduler::set_feedback(std::span<const TilePriorityFeedback> feedback) {
  for (const auto& item : feedback) {
    if (item.tile_id < m_feedback.size()) {
      m_feedback[item.tile_id] = item;
    }
  }
}

void TileScheduler::begin_sample(std::uint64_t generation,
                                 std::uint32_t sample_index) {
  m_generation = generation;
  m_sampleIndex = sample_index;
  m_nextTile = 0u;
  rebuild_order();
}

bool TileScheduler::next_tile(vkpt::pathtracer::RenderTile& out) {
  if (m_config.width == 0u || m_config.height == 0u || m_nextTile >= m_order.size()) {
    return false;
  }

  const auto entry = m_order[m_nextTile];
  const std::uint32_t tileId = entry.tile_id;
  ++m_nextTile;
  const std::uint32_t y = tileId * m_config.tile_height;
  const std::uint64_t sampleStride =
      m_config.foveated_center_extra_samples == 0u
          ? 1u
          : static_cast<std::uint64_t>(m_config.foveated_center_extra_samples) + 1u;
  const std::uint64_t sampleIndex =
      static_cast<std::uint64_t>(m_sampleIndex) * sampleStride + entry.sample_offset;
  out = {};
  out.x = 0u;
  out.y = y;
  out.width = m_config.width;
  out.height = std::min(m_config.tile_height, m_config.height - y);
  out.sample_index = sampleIndex > std::numeric_limits<std::uint32_t>::max()
      ? std::numeric_limits<std::uint32_t>::max()
      : static_cast<std::uint32_t>(sampleIndex);
  out.tile_id = tileId;
  out.gpu_id = tileId % m_config.gpu_count;
  return true;
}

TileSchedulerStats TileScheduler::stats() const {
  std::vector<std::uint32_t> gpuScheduled(
      static_cast<std::size_t>(std::max(1u, m_config.gpu_count)),
      0u);
  for (const auto& entry : m_order) {
    ++gpuScheduled[entry.tile_id % m_config.gpu_count];
  }
  return TileSchedulerStats{
      m_generation,
      m_sampleIndex,
      m_tileCount,
      m_nextTile,
      static_cast<std::uint32_t>(m_order.size()),
      std::move(gpuScheduled)};
}

void TileScheduler::rebuild_order() {
  std::vector<std::uint32_t> rankedTileIds(m_tileCount);
  std::iota(rankedTileIds.begin(), rankedTileIds.end(), 0u);
  std::stable_sort(rankedTileIds.begin(), rankedTileIds.end(), [&](std::uint32_t lhs,
                                                                   std::uint32_t rhs) {
    const auto& left = m_feedback[lhs];
    const auto& right = m_feedback[rhs];
    if (left.dirty != right.dirty) {
      return left.dirty && !right.dirty;
    }
    if (left.variance != right.variance) {
      return left.variance > right.variance;
    }
    if (left.sample_count != right.sample_count) {
      return left.sample_count < right.sample_count;
    }
    return lhs < rhs;
  });

  m_order.clear();
  const std::uint32_t extraSamples = m_config.foveated_center_extra_samples;
  m_order.reserve(static_cast<std::size_t>(m_tileCount) *
                  static_cast<std::size_t>(extraSamples + 1u));
  for (const std::uint32_t tileId : rankedTileIds) {
    m_order.push_back(ScheduledTile{tileId, 0u});
  }
  if (extraSamples == 0u) {
    return;
  }
  for (std::uint32_t sampleOffset = 1u;
       sampleOffset <= extraSamples;
       ++sampleOffset) {
    for (const std::uint32_t tileId : rankedTileIds) {
      if (is_foveated_center_tile(tileId)) {
        m_order.push_back(ScheduledTile{tileId, sampleOffset});
      }
    }
  }
}

bool TileScheduler::is_foveated_center_tile(std::uint32_t tile_id) const {
  if (m_tileCount == 0u || m_config.height == 0u) {
    return false;
  }
  const std::uint32_t y = tile_id * m_config.tile_height;
  if (y >= m_config.height) {
    return false;
  }
  const std::uint32_t tileHeight =
      std::min(m_config.tile_height, m_config.height - y);
  const double tileCenter =
      (static_cast<double>(y) + static_cast<double>(tileHeight) * 0.5) /
      static_cast<double>(m_config.height);
  return std::abs(tileCenter - 0.5) <= m_config.foveated_center_radius;
}

}  // namespace vkpt::render
