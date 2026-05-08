#pragma once

#include <cstdint>
#include <vector>

#include "pathtracer/FilmBuffer.h"
#include "render/TileScheduler.h"
#include "scene/SceneSnapshot.h"

namespace vkpt::render {

struct MotionVectorCell {
  float dx = 0.0f;
  float dy = 0.0f;
  std::uint32_t contributors = 0u;

  bool moving() const { return contributors != 0u; }
};

struct MotionVectorBuffer {
  std::uint32_t source_width = 0u;
  std::uint32_t source_height = 0u;
  std::uint32_t block_size = 1u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
  std::vector<MotionVectorCell> cells;

  bool valid() const;
  bool moving_pixel(std::uint32_t x, std::uint32_t y) const;
  const MotionVectorCell* cell_for_pixel(std::uint32_t x, std::uint32_t y) const;
  std::uint64_t moving_cell_count() const;
  std::uint64_t moving_pixel_count() const;
};

struct HistoryTransitionResult {
  vkpt::scene::SnapshotTransitionAction action =
      vkpt::scene::SnapshotTransitionAction::Continue;
  bool applied = false;
  bool shading_invalidated = false;
  std::uint64_t pixels_kept = 0u;
  std::uint64_t pixels_reset = 0u;
  std::uint64_t pixels_reprojected = 0u;
  std::uint64_t geometry_samples_preserved = 0u;
  double sample_count_variance_before = 0.0;
  double sample_count_variance_after = 0.0;
  double variance_bump = 0.0;
  vkpt::pathtracer::FilmBuffer film;
  MotionVectorBuffer motion_vectors;
};

MotionVectorBuffer RasterizeCoarseMotionVectors(
    const vkpt::scene::RenderSceneSnapshot& previous,
    const vkpt::scene::RenderSceneSnapshot& current,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t block_size);

HistoryTransitionResult ApplyCameraHistoryTransition(
    const vkpt::pathtracer::FilmBuffer& source,
    const vkpt::pathtracer::RTCameraState& previous_camera,
    const vkpt::pathtracer::RTCameraState& current_camera);

HistoryTransitionResult ApplyTransformHistoryTransition(
    const vkpt::pathtracer::FilmBuffer& source,
    const MotionVectorBuffer& motion_vectors);

HistoryTransitionResult ApplyMaterialHistoryTransition(
    const vkpt::pathtracer::FilmBuffer& source);

std::vector<TilePriorityFeedback> BuildDirtyTileFeedback(
    const MotionVectorBuffer& motion_vectors,
    const vkpt::pathtracer::FilmBuffer& film,
    std::uint32_t tile_height);

}  // namespace vkpt::render
