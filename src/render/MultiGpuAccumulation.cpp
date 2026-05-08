#include "render/MultiGpuAccumulation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace vkpt::render {

namespace {

std::uint32_t MaxGpuScheduledTiles(const std::vector<std::uint32_t>& counts) {
  std::uint32_t maxCount = 0u;
  for (const std::uint32_t count : counts) {
    maxCount = std::max(maxCount, count);
  }
  return maxCount;
}

void UpdateResolvedSampleRange(const vkpt::pathtracer::FilmBuffer& film,
                               MultiGpuAccumulationStats& stats) {
  const auto& counts = film.sample_counts();
  if (counts.empty()) {
    stats.resolved_min_sample_count = 0u;
    stats.resolved_max_sample_count = 0u;
    return;
  }
  std::uint32_t minCount = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t maxCount = 0u;
  for (const std::uint32_t count : counts) {
    minCount = std::min(minCount, count);
    maxCount = std::max(maxCount, count);
  }
  stats.resolved_min_sample_count = minCount;
  stats.resolved_max_sample_count = maxCount;
}

}  // namespace

bool MultiGpuAccumulation::configure(MultiGpuAccumulationConfig config) {
  config.tile_height = std::max(1u, config.tile_height);
  config.gpu_count = std::max(1u, config.gpu_count);
  if (config.width == 0u || config.height == 0u) {
    m_config = {};
    m_slices.clear();
    m_stats = {};
    return false;
  }

  m_config = config;
  m_slices.assign(config.gpu_count,
                  vkpt::pathtracer::FilmBuffer(config.width, config.height));
  for (auto& slice : m_slices) {
    slice.set_resolve_settings(config.resolve);
    slice.clear();
  }

  m_stats = {};
  m_stats.width = config.width;
  m_stats.height = config.height;
  m_stats.tile_height = config.tile_height;
  m_stats.gpu_count = config.gpu_count;
  m_stats.accepted_tiles_per_gpu.assign(config.gpu_count, 0u);
  m_stats.sampled_pixels_per_gpu.assign(config.gpu_count, 0u);
  return true;
}

void MultiGpuAccumulation::reset() {
  for (auto& slice : m_slices) {
    slice.clear();
  }
  const auto width = m_stats.width;
  const auto height = m_stats.height;
  const auto tileHeight = m_stats.tile_height;
  const auto gpuCount = m_stats.gpu_count;
  m_stats = {};
  m_stats.width = width;
  m_stats.height = height;
  m_stats.tile_height = tileHeight;
  m_stats.gpu_count = gpuCount;
  m_stats.accepted_tiles_per_gpu.assign(gpuCount, 0u);
  m_stats.sampled_pixels_per_gpu.assign(gpuCount, 0u);
}

bool MultiGpuAccumulation::accumulate_tile(
    const vkpt::pathtracer::RenderTile& tile,
    const vkpt::pathtracer::Vec3& color) {
  if (!tile_matches_contract(tile)) {
    ++m_stats.rejected_tiles;
    return false;
  }

  auto& slice = m_slices[tile.gpu_id];
  const std::uint32_t endY = tile.y + tile.height;
  for (std::uint32_t y = tile.y; y < endY; ++y) {
    for (std::uint32_t x = 0u; x < m_config.width; ++x) {
      slice.add_sample(x, y, color);
    }
  }

  ++m_stats.accepted_tiles;
  ++m_stats.accepted_tiles_per_gpu[tile.gpu_id];
  m_stats.sampled_pixels_per_gpu[tile.gpu_id] +=
      static_cast<std::uint64_t>(tile.width) * tile.height;
  return true;
}

const vkpt::pathtracer::FilmBuffer* MultiGpuAccumulation::slice(
    std::uint32_t gpu_id) const {
  if (gpu_id >= m_slices.size()) {
    return nullptr;
  }
  return &m_slices[gpu_id];
}

vkpt::pathtracer::FilmBuffer MultiGpuAccumulation::resolve_film() const {
  vkpt::pathtracer::FilmBuffer resolved(m_config.width, m_config.height);
  resolved.set_resolve_settings(m_config.resolve);
  if (m_slices.empty()) {
    return resolved;
  }

  const std::uint32_t count = tile_count();
  for (std::uint32_t tileId = 0u; tileId < count; ++tileId) {
    const std::uint32_t owner = tileId % m_config.gpu_count;
    resolved.import_tile(m_slices[owner],
                         tile_start_y(tileId),
                         tile_end_y(tileId));
  }
  return resolved;
}

DisplayFrame MultiGpuAccumulation::resolve_display_frame(
    std::uint64_t generation,
    std::uint32_t sample_count,
    const vkpt::pathtracer::SampleCounters& counters) const {
  auto film = resolve_film();
  auto ldr = film.resolve_ldr(m_config.resolve);

  DisplayFrame frame;
  frame.rgba8 = std::move(ldr.rgba8);
  frame.width = ldr.width;
  frame.height = ldr.height;
  frame.generation = generation;
  frame.sample_count = sample_count;
  frame.counters = counters;
  return frame;
}

MultiGpuAccumulationStats MultiGpuAccumulation::stats() const {
  auto out = m_stats;
  UpdateResolvedSampleRange(resolve_film(), out);
  return out;
}

bool MultiGpuAccumulation::tile_matches_contract(
    const vkpt::pathtracer::RenderTile& tile) const {
  if (m_slices.empty() || tile.gpu_id >= m_config.gpu_count) {
    return false;
  }
  if (tile.width != m_config.width || tile.x != 0u || tile.height == 0u) {
    return false;
  }
  if (tile.y >= m_config.height || tile.height > m_config.height - tile.y) {
    return false;
  }
  if (tile.tile_id >= tile_count() || tile.y != tile_start_y(tile.tile_id)) {
    return false;
  }
  if (tile.height != tile_end_y(tile.tile_id) - tile.y) {
    return false;
  }
  return tile.gpu_id == tile.tile_id % m_config.gpu_count;
}

std::uint32_t MultiGpuAccumulation::tile_count() const {
  if (m_config.height == 0u) {
    return 0u;
  }
  return (m_config.height + m_config.tile_height - 1u) / m_config.tile_height;
}

std::uint32_t MultiGpuAccumulation::tile_start_y(std::uint32_t tile_id) const {
  return tile_id * m_config.tile_height;
}

std::uint32_t MultiGpuAccumulation::tile_end_y(std::uint32_t tile_id) const {
  return std::min(m_config.height, tile_start_y(tile_id) + m_config.tile_height);
}

std::vector<MultiGpuScalingEstimate> EstimateDeterministicMultiGpuScaling(
    TileSchedulerConfig base_config,
    std::span<const std::uint32_t> gpu_counts) {
  std::vector<MultiGpuScalingEstimate> estimates;
  estimates.reserve(gpu_counts.size());

  double baselineTilesPerStep = 0.0;
  for (const std::uint32_t requestedGpuCount : gpu_counts) {
    TileSchedulerConfig config = base_config;
    config.gpu_count = std::max(1u, requestedGpuCount);

    TileScheduler scheduler;
    scheduler.configure(config);
    scheduler.begin_sample(0u, 0u);
    const auto schedulerStats = scheduler.stats();
    const std::uint32_t parallelSteps =
        MaxGpuScheduledTiles(schedulerStats.gpu_scheduled_tile_count);
    const double tilesPerStep = parallelSteps == 0u
        ? 0.0
        : static_cast<double>(schedulerStats.scheduled_tile_count) /
              static_cast<double>(parallelSteps);
    if (baselineTilesPerStep == 0.0 && tilesPerStep > 0.0) {
      baselineTilesPerStep = tilesPerStep;
    }
    const double relativeSamplesPerSecond = baselineTilesPerStep == 0.0
        ? 0.0
        : tilesPerStep / baselineTilesPerStep;

    MultiGpuScalingEstimate estimate;
    estimate.gpu_count = config.gpu_count;
    estimate.scheduled_tile_count = schedulerStats.scheduled_tile_count;
    estimate.parallel_step_count = parallelSteps;
    estimate.normalized_tiles_per_step = tilesPerStep;
    estimate.relative_samples_per_second = relativeSamplesPerSecond;
    estimate.relative_noise_for_fixed_time = relativeSamplesPerSecond <= 0.0
        ? 0.0
        : 1.0 / std::sqrt(relativeSamplesPerSecond);
    estimate.scheduled_tiles_per_gpu =
        schedulerStats.gpu_scheduled_tile_count;
    estimates.push_back(std::move(estimate));
  }

  return estimates;
}

}  // namespace vkpt::render
