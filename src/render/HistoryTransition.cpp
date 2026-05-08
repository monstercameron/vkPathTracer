#include "render/HistoryTransition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace vkpt::render {

namespace {

using vkpt::pathtracer::FilmBuffer;
using vkpt::pathtracer::Quat4;
using vkpt::pathtracer::RTCameraState;
using vkpt::pathtracer::RTInstance;
using vkpt::pathtracer::Vec2;
using vkpt::pathtracer::Vec3;
using vkpt::scene::RenderSceneSnapshot;

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1.0e-5f;

Vec3 Add(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 Sub(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 Mul(const Vec3& lhs, float rhs) {
  return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

float Dot(const Vec3& lhs, const Vec3& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 Cross(const Vec3& lhs, const Vec3& rhs) {
  return {
      lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.z * rhs.x - lhs.x * rhs.z,
      lhs.x * rhs.y - lhs.y * rhs.x};
}

float LengthSq(const Vec3& value) {
  return Dot(value, value);
}

Vec3 Normalize(const Vec3& value) {
  const float lenSq = LengthSq(value);
  if (lenSq <= kEpsilon * kEpsilon) {
    return {0.0f, 1.0f, 0.0f};
  }
  const float invLen = 1.0f / std::sqrt(lenSq);
  return Mul(value, invLen);
}

float Radians(float degrees) {
  return degrees * (kPi / 180.0f);
}

Vec3 RotateQuat(const Vec3& value, const Quat4& rotation) {
  const float lenSq = rotation.x * rotation.x +
                      rotation.y * rotation.y +
                      rotation.z * rotation.z +
                      rotation.w * rotation.w;
  if (lenSq <= kEpsilon * kEpsilon) {
    return value;
  }
  const float invLen = 1.0f / std::sqrt(lenSq);
  const Vec3 qv{
      rotation.x * invLen,
      rotation.y * invLen,
      rotation.z * invLen};
  const float qw = rotation.w * invLen;
  const auto t = Mul(Cross(qv, value), 2.0f);
  return Add(Add(value, Mul(t, qw)), Cross(qv, t));
}

Vec3 TransformInstanceVertex(const Vec3& local, const RTInstance& instance) {
  const Vec3 scaled{
      local.x * instance.scale.x,
      local.y * instance.scale.y,
      local.z * instance.scale.z};
  return Add(RotateQuat(scaled, instance.rotation), instance.translation);
}

struct CameraBasis {
  Vec3 position{};
  Vec3 forward{0.0f, 0.0f, -1.0f};
  Vec3 right{1.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  float tan_half_fov = 1.0f;
  float aspect = 1.0f;
};

CameraBasis BuildCameraBasis(const RTCameraState& camera,
                             std::uint32_t width,
                             std::uint32_t height) {
  CameraBasis out;
  out.position = camera.position;
  out.forward = Normalize(Sub(camera.target, camera.position));
  out.right = Normalize(Cross(out.forward, camera.up));
  if (LengthSq(out.right) <= kEpsilon * kEpsilon) {
    out.right = {1.0f, 0.0f, 0.0f};
  }
  out.up = Normalize(Cross(out.right, out.forward));
  if (LengthSq(out.up) <= kEpsilon * kEpsilon) {
    out.up = {0.0f, 1.0f, 0.0f};
  }
  out.tan_half_fov = std::tan(0.5f * Radians(std::clamp(camera.fov_deg, 1.0f, 179.0f)));
  out.aspect = static_cast<float>(std::max(1u, width)) /
               static_cast<float>(std::max(1u, height));
  return out;
}

bool ProjectWorldToPixel(const CameraBasis& camera,
                         const Vec3& world,
                         std::uint32_t width,
                         std::uint32_t height,
                         Vec2& out) {
  const Vec3 rel = Sub(world, camera.position);
  const float z = Dot(rel, camera.forward);
  if (z <= kEpsilon || !std::isfinite(z)) {
    return false;
  }
  const float invZ = 1.0f / z;
  const float ndcX = Dot(rel, camera.right) * invZ /
                     std::max(kEpsilon, camera.aspect * camera.tan_half_fov);
  const float ndcY = Dot(rel, camera.up) * invZ /
                     std::max(kEpsilon, camera.tan_half_fov);
  if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
    return false;
  }
  out.u = ((ndcX + 1.0f) * 0.5f) * static_cast<float>(width) - 0.5f;
  out.v = ((1.0f - ndcY) * 0.5f) * static_cast<float>(height) - 0.5f;
  return true;
}

Vec3 UnprojectPixelOnUnitPlane(const CameraBasis& camera,
                               std::uint32_t x,
                               std::uint32_t y,
                               std::uint32_t width,
                               std::uint32_t height) {
  const float ndcX =
      ((static_cast<float>(x) + 0.5f) / static_cast<float>(std::max(1u, width))) *
          2.0f -
      1.0f;
  const float ndcY =
      1.0f -
      ((static_cast<float>(y) + 0.5f) / static_cast<float>(std::max(1u, height))) *
          2.0f;
  const Vec3 dir = Normalize(Add(
      Add(camera.forward, Mul(camera.right, ndcX * camera.aspect * camera.tan_half_fov)),
      Mul(camera.up, ndcY * camera.tan_half_fov)));
  const float denom = std::max(kEpsilon, Dot(dir, camera.forward));
  return Add(camera.position, Mul(dir, 1.0f / denom));
}

std::uint64_t PixelCount(const FilmBuffer& film) {
  return static_cast<std::uint64_t>(film.width()) *
         static_cast<std::uint64_t>(film.height());
}

double SampleCountVariance(const FilmBuffer& film) {
  const auto& counts = film.sample_counts();
  if (counts.empty()) {
    return 0.0;
  }
  double mean = 0.0;
  for (const auto count : counts) {
    mean += static_cast<double>(count);
  }
  mean /= static_cast<double>(counts.size());

  double variance = 0.0;
  for (const auto count : counts) {
    const double delta = static_cast<double>(count) - mean;
    variance += delta * delta;
  }
  return variance / static_cast<double>(counts.size());
}

std::uint64_t SumSampleCounts(const FilmBuffer& film) {
  std::uint64_t total = 0u;
  for (const auto count : film.sample_counts()) {
    total += count;
  }
  return total;
}

bool ResolveLocalVertexIndex(const RTInstance& instance,
                             std::uint32_t index,
                             std::size_t vertex_count,
                             std::size_t& out) {
  if (index >= instance.local_first_vertex &&
      index < instance.local_first_vertex + instance.local_vertex_count &&
      index < vertex_count) {
    out = index;
    return true;
  }
  const std::uint64_t relative =
      static_cast<std::uint64_t>(instance.local_first_vertex) + index;
  if (index < instance.local_vertex_count && relative < vertex_count) {
    out = static_cast<std::size_t>(relative);
    return true;
  }
  if (index < vertex_count) {
    out = index;
    return true;
  }
  return false;
}

bool BuildTriangleWorld(const RenderSceneSnapshot& snapshot,
                        const RTInstance& instance,
                        std::uint32_t triangle,
                        std::array<Vec3, 3u>& out) {
  const auto localVertices = snapshot.local_vertices.view();
  const auto localIndices = snapshot.local_indices.view();
  const std::uint64_t localBase =
      static_cast<std::uint64_t>(instance.local_first_index) +
      static_cast<std::uint64_t>(triangle) * 3u;
  if (instance.local_vertex_count > 0u &&
      localBase + 2u < localIndices.size() &&
      instance.local_first_vertex < localVertices.size()) {
    for (std::uint32_t corner = 0u; corner < 3u; ++corner) {
      std::size_t localVertex = 0u;
      if (!ResolveLocalVertexIndex(instance,
                                   localIndices[static_cast<std::size_t>(localBase + corner)],
                                   localVertices.size(),
                                   localVertex)) {
        return false;
      }
      out[corner] = TransformInstanceVertex(localVertices[localVertex], instance);
    }
    return true;
  }

  const auto vertices = snapshot.vertices.view();
  const auto indices = snapshot.indices.view();
  const std::uint64_t base =
      (static_cast<std::uint64_t>(instance.first_triangle) + triangle) * 3u;
  if (base + 2u >= indices.size()) {
    return false;
  }
  for (std::uint32_t corner = 0u; corner < 3u; ++corner) {
    const std::uint32_t vertexIndex = indices[static_cast<std::size_t>(base + corner)];
    if (vertexIndex >= vertices.size()) {
      return false;
    }
    out[corner] = vertices[vertexIndex];
  }
  return true;
}

void AccumulateCell(MotionVectorBuffer& buffer,
                    std::uint32_t cell_x,
                    std::uint32_t cell_y,
                    float dx,
                    float dy) {
  if (cell_x >= buffer.width || cell_y >= buffer.height) {
    return;
  }
  auto& cell = buffer.cells[static_cast<std::size_t>(cell_y) * buffer.width + cell_x];
  cell.dx += dx;
  cell.dy += dy;
  ++cell.contributors;
}

}  // namespace

bool MotionVectorBuffer::valid() const {
  return source_width != 0u &&
         source_height != 0u &&
         block_size != 0u &&
         width != 0u &&
         height != 0u &&
         cells.size() == static_cast<std::size_t>(width) * height;
}

const MotionVectorCell* MotionVectorBuffer::cell_for_pixel(std::uint32_t x,
                                                           std::uint32_t y) const {
  if (!valid() || x >= source_width || y >= source_height) {
    return nullptr;
  }
  const std::uint32_t cellX = std::min(width - 1u, x / block_size);
  const std::uint32_t cellY = std::min(height - 1u, y / block_size);
  return &cells[static_cast<std::size_t>(cellY) * width + cellX];
}

bool MotionVectorBuffer::moving_pixel(std::uint32_t x, std::uint32_t y) const {
  const auto* cell = cell_for_pixel(x, y);
  return cell != nullptr && cell->moving();
}

std::uint64_t MotionVectorBuffer::moving_cell_count() const {
  std::uint64_t count = 0u;
  for (const auto& cell : cells) {
    if (cell.moving()) {
      ++count;
    }
  }
  return count;
}

std::uint64_t MotionVectorBuffer::moving_pixel_count() const {
  if (!valid()) {
    return 0u;
  }
  std::uint64_t count = 0u;
  for (std::uint32_t y = 0u; y < source_height; ++y) {
    for (std::uint32_t x = 0u; x < source_width; ++x) {
      if (moving_pixel(x, y)) {
        ++count;
      }
    }
  }
  return count;
}

MotionVectorBuffer RasterizeCoarseMotionVectors(
    const RenderSceneSnapshot& previous,
    const RenderSceneSnapshot& current,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t block_size) {
  MotionVectorBuffer out;
  out.source_width = width;
  out.source_height = height;
  out.block_size = std::max(1u, block_size);
  out.width = width == 0u ? 0u : (width + out.block_size - 1u) / out.block_size;
  out.height = height == 0u ? 0u : (height + out.block_size - 1u) / out.block_size;
  out.cells.assign(static_cast<std::size_t>(out.width) * out.height, {});
  if (!out.valid() || current.instance_motion.empty()) {
    return out;
  }

  const auto camera = BuildCameraBasis(current.camera, width, height);
  for (const auto& motion : current.instance_motion.view()) {
    if (!motion.previous_valid || motion.current.triangle_count == 0u) {
      continue;
    }
    for (std::uint32_t triangle = 0u; triangle < motion.current.triangle_count; ++triangle) {
      std::array<Vec3, 3u> previousWorld{};
      std::array<Vec3, 3u> currentWorld{};
      if (!BuildTriangleWorld(previous, motion.previous, triangle, previousWorld) ||
          !BuildTriangleWorld(current, motion.current, triangle, currentWorld)) {
        continue;
      }

      std::array<Vec2, 3u> previousScreen{};
      std::array<Vec2, 3u> currentScreen{};
      bool projected = true;
      for (std::uint32_t corner = 0u; corner < 3u; ++corner) {
        projected = ProjectWorldToPixel(camera,
                                        previousWorld[corner],
                                        width,
                                        height,
                                        previousScreen[corner]) &&
                    ProjectWorldToPixel(camera,
                                        currentWorld[corner],
                                        width,
                                        height,
                                        currentScreen[corner]) &&
                    projected;
      }
      if (!projected) {
        continue;
      }

      Vec2 previousCentroid{};
      Vec2 currentCentroid{};
      float minX = std::numeric_limits<float>::infinity();
      float minY = std::numeric_limits<float>::infinity();
      float maxX = -std::numeric_limits<float>::infinity();
      float maxY = -std::numeric_limits<float>::infinity();
      for (std::uint32_t corner = 0u; corner < 3u; ++corner) {
        previousCentroid.u += previousScreen[corner].u;
        previousCentroid.v += previousScreen[corner].v;
        currentCentroid.u += currentScreen[corner].u;
        currentCentroid.v += currentScreen[corner].v;
        minX = std::min({minX, previousScreen[corner].u, currentScreen[corner].u});
        minY = std::min({minY, previousScreen[corner].v, currentScreen[corner].v});
        maxX = std::max({maxX, previousScreen[corner].u, currentScreen[corner].u});
        maxY = std::max({maxY, previousScreen[corner].v, currentScreen[corner].v});
      }
      previousCentroid.u /= 3.0f;
      previousCentroid.v /= 3.0f;
      currentCentroid.u /= 3.0f;
      currentCentroid.v /= 3.0f;

      if (!std::isfinite(minX) || !std::isfinite(minY) ||
          !std::isfinite(maxX) || !std::isfinite(maxY)) {
        continue;
      }
      const auto x0 = static_cast<std::int32_t>(
          std::floor(std::clamp(minX, 0.0f, static_cast<float>(width - 1u))));
      const auto y0 = static_cast<std::int32_t>(
          std::floor(std::clamp(minY, 0.0f, static_cast<float>(height - 1u))));
      const auto x1 = static_cast<std::int32_t>(
          std::ceil(std::clamp(maxX, 0.0f, static_cast<float>(width - 1u))));
      const auto y1 = static_cast<std::int32_t>(
          std::ceil(std::clamp(maxY, 0.0f, static_cast<float>(height - 1u))));
      if (x1 < x0 || y1 < y0) {
        continue;
      }
      const float dx = currentCentroid.u - previousCentroid.u;
      const float dy = currentCentroid.v - previousCentroid.v;
      const std::uint32_t cellX0 = static_cast<std::uint32_t>(x0) / out.block_size;
      const std::uint32_t cellY0 = static_cast<std::uint32_t>(y0) / out.block_size;
      const std::uint32_t cellX1 = static_cast<std::uint32_t>(x1) / out.block_size;
      const std::uint32_t cellY1 = static_cast<std::uint32_t>(y1) / out.block_size;
      for (std::uint32_t cellY = cellY0; cellY <= std::min(cellY1, out.height - 1u); ++cellY) {
        for (std::uint32_t cellX = cellX0; cellX <= std::min(cellX1, out.width - 1u); ++cellX) {
          AccumulateCell(out, cellX, cellY, dx, dy);
        }
      }
    }
  }

  for (auto& cell : out.cells) {
    if (cell.contributors != 0u) {
      const float inv = 1.0f / static_cast<float>(cell.contributors);
      cell.dx *= inv;
      cell.dy *= inv;
    }
  }
  return out;
}

HistoryTransitionResult ApplyCameraHistoryTransition(
    const FilmBuffer& source,
    const RTCameraState& previous_camera,
    const RTCameraState& current_camera) {
  HistoryTransitionResult result;
  result.action = vkpt::scene::SnapshotTransitionAction::ReprojectCamera;
  result.sample_count_variance_before = SampleCountVariance(source);
  result.film = FilmBuffer{source.width(), source.height()};
  result.film.set_resolve_settings(source.resolve_settings());
  if (source.width() == 0u || source.height() == 0u) {
    result.applied = true;
    return result;
  }

  const auto previousBasis =
      BuildCameraBasis(previous_camera, source.width(), source.height());
  const auto currentBasis =
      BuildCameraBasis(current_camera, source.width(), source.height());
  const auto& raw = source.raw();
  const auto& counts = source.sample_counts();
  const auto& invalid = source.invalid_samples();

  for (std::uint32_t y = 0u; y < source.height(); ++y) {
    for (std::uint32_t x = 0u; x < source.width(); ++x) {
      const Vec3 world =
          UnprojectPixelOnUnitPlane(currentBasis, x, y, source.width(), source.height());
      Vec2 previousPixel{};
      if (!ProjectWorldToPixel(previousBasis,
                               world,
                               source.width(),
                               source.height(),
                               previousPixel)) {
        ++result.pixels_reset;
        continue;
      }
      const auto srcX = static_cast<std::int32_t>(std::lround(previousPixel.u));
      const auto srcY = static_cast<std::int32_t>(std::lround(previousPixel.v));
      if (srcX < 0 ||
          srcY < 0 ||
          srcX >= static_cast<std::int32_t>(source.width()) ||
          srcY >= static_cast<std::int32_t>(source.height())) {
        ++result.pixels_reset;
        continue;
      }
      const std::size_t srcIndex =
          static_cast<std::size_t>(srcY) * source.width() +
          static_cast<std::size_t>(srcX);
      if (srcIndex >= raw.size() || srcIndex >= counts.size()) {
        ++result.pixels_reset;
        continue;
      }
      result.film.set_pixel_raw(x,
                                y,
                                raw[srcIndex],
                                counts[srcIndex],
                                srcIndex < invalid.size() ? invalid[srcIndex] : 0.0f);
      if (counts[srcIndex] == 0u) {
        ++result.pixels_reset;
      } else {
        ++result.pixels_reprojected;
        ++result.pixels_kept;
      }
    }
  }

  result.sample_count_variance_after = SampleCountVariance(result.film);
  result.variance_bump =
      std::max(0.0, result.sample_count_variance_after - result.sample_count_variance_before);
  result.applied = true;
  return result;
}

HistoryTransitionResult ApplyTransformHistoryTransition(
    const FilmBuffer& source,
    const MotionVectorBuffer& motion_vectors) {
  HistoryTransitionResult result;
  result.action = vkpt::scene::SnapshotTransitionAction::ResetMovingPixels;
  result.sample_count_variance_before = SampleCountVariance(source);
  result.motion_vectors = motion_vectors;
  result.film = FilmBuffer{source.width(), source.height()};
  result.film.set_resolve_settings(source.resolve_settings());
  if (!result.film.copy_from(source)) {
    return result;
  }

  for (std::uint32_t y = 0u; y < source.height(); ++y) {
    for (std::uint32_t x = 0u; x < source.width(); ++x) {
      const std::size_t index = static_cast<std::size_t>(y) * source.width() + x;
      const bool moving = motion_vectors.moving_pixel(x, y);
      if (moving) {
        result.film.reset_pixel(x, y);
        ++result.pixels_reset;
      } else {
        if (index < source.sample_counts().size() && source.sample_counts()[index] != 0u) {
          ++result.pixels_kept;
        }
      }
    }
  }
  result.sample_count_variance_after = SampleCountVariance(result.film);
  result.variance_bump =
      std::max(0.0, result.sample_count_variance_after - result.sample_count_variance_before);
  result.applied = motion_vectors.valid();
  return result;
}

HistoryTransitionResult ApplyMaterialHistoryTransition(const FilmBuffer& source) {
  HistoryTransitionResult result;
  result.action = vkpt::scene::SnapshotTransitionAction::InvalidateShading;
  result.shading_invalidated = true;
  result.sample_count_variance_before = SampleCountVariance(source);
  result.geometry_samples_preserved = SumSampleCounts(source);
  result.film = FilmBuffer{source.width(), source.height()};
  result.film.set_resolve_settings(source.resolve_settings());
  result.pixels_reset = PixelCount(source);
  result.sample_count_variance_after = SampleCountVariance(result.film);
  result.variance_bump =
      std::max(0.0, result.sample_count_variance_after - result.sample_count_variance_before);
  result.applied = true;
  return result;
}

std::vector<TilePriorityFeedback> BuildDirtyTileFeedback(
    const MotionVectorBuffer& motion_vectors,
    const FilmBuffer& film,
    std::uint32_t tile_height) {
  std::vector<TilePriorityFeedback> feedback;
  const std::uint32_t safeTileHeight = std::max(1u, tile_height);
  const std::uint32_t tileCount =
      film.height() == 0u ? 0u : (film.height() + safeTileHeight - 1u) / safeTileHeight;
  feedback.resize(tileCount);
  for (std::uint32_t tileId = 0u; tileId < tileCount; ++tileId) {
    TilePriorityFeedback item;
    item.tile_id = tileId;
    const std::uint32_t y0 = tileId * safeTileHeight;
    const std::uint32_t y1 = std::min(film.height(), y0 + safeTileHeight);
    std::uint64_t sampleSum = 0u;
    std::uint64_t samplePixels = 0u;
    for (std::uint32_t y = y0; y < y1; ++y) {
      for (std::uint32_t x = 0u; x < film.width(); ++x) {
        const std::size_t index = static_cast<std::size_t>(y) * film.width() + x;
        if (index < film.sample_counts().size()) {
          sampleSum += film.sample_counts()[index];
          ++samplePixels;
        }
        item.dirty = item.dirty || motion_vectors.moving_pixel(x, y);
      }
    }
    item.sample_count = samplePixels == 0u
        ? 0u
        : static_cast<std::uint32_t>(sampleSum / samplePixels);
    feedback[tileId] = item;
  }
  return feedback;
}

}  // namespace vkpt::render
