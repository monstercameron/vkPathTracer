#include "render/MultiGpuAccumulation.h"
#include "render/TileScheduler.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "multi_gpu_accumulation_smoke: " << message << "\n";
    return false;
  }
  return true;
}

bool CloseEnough(float lhs, float rhs) {
  return std::abs(lhs - rhs) <= 1.0e-5f;
}

bool CheckAccumulationResolve() {
  vkpt::render::TileSchedulerConfig schedulerConfig;
  schedulerConfig.width = 8u;
  schedulerConfig.height = 8u;
  schedulerConfig.tile_height = 2u;
  schedulerConfig.gpu_count = 2u;

  vkpt::render::TileScheduler scheduler;
  scheduler.configure(schedulerConfig);
  scheduler.begin_sample(21u, 0u);

  vkpt::render::MultiGpuAccumulation accumulation;
  vkpt::render::MultiGpuAccumulationConfig accumulationConfig;
  accumulationConfig.width = schedulerConfig.width;
  accumulationConfig.height = schedulerConfig.height;
  accumulationConfig.tile_height = schedulerConfig.tile_height;
  accumulationConfig.gpu_count = schedulerConfig.gpu_count;
  if (!Check(accumulation.configure(accumulationConfig),
             "accumulation should configure")) {
    return false;
  }

  std::vector<vkpt::pathtracer::RenderTile> tiles;
  vkpt::pathtracer::RenderTile tile;
  while (scheduler.next_tile(tile)) {
    tiles.push_back(tile);
    const vkpt::pathtracer::Vec3 color{
        0.25f + static_cast<float>(tile.gpu_id),
        0.5f + static_cast<float>(tile.tile_id),
        0.75f};
    if (!Check(accumulation.accumulate_tile(tile, color),
               "accumulation should accept scheduler-owned tiles")) {
      return false;
    }
  }

  if (!Check(tiles.size() == 4u, "scheduler should cover four row tiles")) {
    return false;
  }

  auto wrongGpuTile = tiles.front();
  wrongGpuTile.gpu_id = 1u;
  if (!Check(!accumulation.accumulate_tile(wrongGpuTile, {1.0f, 0.0f, 0.0f}),
             "accumulation should reject a tile assigned to the wrong GPU")) {
    return false;
  }

  const auto* gpu0 = accumulation.slice(0u);
  const auto* gpu1 = accumulation.slice(1u);
  if (!Check(gpu0 != nullptr && gpu1 != nullptr,
             "accumulation should expose per-GPU slices")) {
    return false;
  }

  for (std::uint32_t y = 0u; y < schedulerConfig.height; ++y) {
    const std::uint32_t tileId = y / schedulerConfig.tile_height;
    const std::uint32_t owner = tileId % schedulerConfig.gpu_count;
    for (std::uint32_t x = 0u; x < schedulerConfig.width; ++x) {
      const std::size_t pixel =
          static_cast<std::size_t>(y) * schedulerConfig.width + x;
      const auto gpu0Count = gpu0->sample_counts()[pixel];
      const auto gpu1Count = gpu1->sample_counts()[pixel];
      if (!Check((owner == 0u && gpu0Count == 1u && gpu1Count == 0u) ||
                     (owner == 1u && gpu0Count == 0u && gpu1Count == 1u),
                 "only the owning GPU slice should accumulate a tile row")) {
        return false;
      }
    }
  }

  const auto resolved = accumulation.resolve_film();
  for (std::uint32_t y = 0u; y < schedulerConfig.height; ++y) {
    const std::uint32_t tileId = y / schedulerConfig.tile_height;
    const std::uint32_t owner = tileId % schedulerConfig.gpu_count;
    for (std::uint32_t x = 0u; x < schedulerConfig.width; ++x) {
      const std::size_t pixel =
          static_cast<std::size_t>(y) * schedulerConfig.width + x;
      const auto& raw = resolved.raw()[pixel];
      if (!Check(resolved.sample_counts()[pixel] == 1u &&
                     CloseEnough(raw.x, 0.25f + static_cast<float>(owner)) &&
                     CloseEnough(raw.y, 0.5f + static_cast<float>(tileId)) &&
                     CloseEnough(raw.z, 0.75f),
                 "resolve should composite all GPU slices into one film")) {
        return false;
      }
    }
  }

  const auto frame = accumulation.resolve_display_frame(77u, 1u);
  vkpt::render::FrameHandoff handoff;
  handoff.publish(frame);
  const auto acquired = handoff.acquire_latest();
  const auto stats = accumulation.stats();
  return Check(frame.width == schedulerConfig.width &&
                   frame.height == schedulerConfig.height &&
                   frame.generation == 77u &&
                   frame.sample_count == 1u &&
                   frame.rgba8.size() ==
                       static_cast<std::size_t>(schedulerConfig.width) *
                           schedulerConfig.height * 4u,
               "resolve should produce one display frame") &&
         Check(acquired.has_value() &&
                   acquired->width == schedulerConfig.width &&
                   acquired->height == schedulerConfig.height &&
                   acquired->generation == 77u &&
                   acquired->rgba8.size() == frame.rgba8.size(),
               "resolved composite should publish as one frame handoff entry") &&
         Check(stats.accepted_tiles == 4u && stats.rejected_tiles == 1u,
               "accumulation should count accepted and rejected tiles") &&
         Check(stats.accepted_tiles_per_gpu.size() == 2u &&
                   stats.accepted_tiles_per_gpu[0] == 2u &&
                   stats.accepted_tiles_per_gpu[1] == 2u &&
                   stats.resolved_min_sample_count == 1u &&
                   stats.resolved_max_sample_count == 1u,
               "accumulation stats should expose balanced slices and resolved samples");
}

bool CheckDeterministicScalingContract() {
  vkpt::render::TileSchedulerConfig config;
  config.width = 64u;
  config.height = 64u;
  config.tile_height = 4u;
  config.gpu_count = 1u;

  const std::array<std::uint32_t, 3u> gpuCounts = {1u, 2u, 4u};
  const auto estimates =
      vkpt::render::EstimateDeterministicMultiGpuScaling(config, gpuCounts);
  if (!Check(estimates.size() == gpuCounts.size(),
             "scaling contract should return every requested GPU count")) {
    return false;
  }

  return Check(estimates[0].gpu_count == 1u &&
                   estimates[1].gpu_count == 2u &&
                   estimates[2].gpu_count == 4u,
               "scaling contract should preserve GPU counts") &&
         Check(estimates[0].relative_samples_per_second == 1.0 &&
                   estimates[1].relative_samples_per_second >= 1.8 &&
                   estimates[2].relative_samples_per_second >= 3.6,
               "scaling contract should improve samples/sec for 1/2/4 GPUs") &&
         Check(estimates[0].relative_noise_for_fixed_time >
                   estimates[1].relative_noise_for_fixed_time &&
                   estimates[1].relative_noise_for_fixed_time >
                       estimates[2].relative_noise_for_fixed_time,
               "scaling contract should lower fixed-time noise") &&
         Check(estimates[1].scheduled_tiles_per_gpu.size() == 2u &&
                   estimates[1].scheduled_tiles_per_gpu[0] == 8u &&
                   estimates[1].scheduled_tiles_per_gpu[1] == 8u &&
                   estimates[2].scheduled_tiles_per_gpu.size() == 4u &&
                   estimates[2].scheduled_tiles_per_gpu[0] == 4u &&
                   estimates[2].scheduled_tiles_per_gpu[1] == 4u &&
                   estimates[2].scheduled_tiles_per_gpu[2] == 4u &&
                   estimates[2].scheduled_tiles_per_gpu[3] == 4u,
               "scaling contract should balance fixed-scene tiles");
}

}  // namespace

int main() {
  if (!CheckAccumulationResolve()) {
    return 1;
  }
  if (!CheckDeterministicScalingContract()) {
    return 1;
  }
  std::cout << "multi_gpu_accumulation_smoke: ok\n";
  return 0;
}
