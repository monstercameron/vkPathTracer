#pragma once

#include "cpu/CpuFeatures.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace vkpt::cpu {

constexpr std::size_t kBvhTriangleBatchWidth = 8u;

/// SIMD strategy used by the BVH leaf triangle batch intersector.
enum class BvhTriangleIntersectorMode : std::uint8_t {
  Scalar = 0,
  ArmNeon,
  X86Avx2,
};

/// Single ray formatted for triangle batch tests.
struct BvhTriangleRay {
  float ox = 0.0f;
  float oy = 0.0f;
  float oz = 0.0f;
  float dx = 0.0f;
  float dy = 0.0f;
  float dz = 0.0f;
};

/// Up to kBvhTriangleBatchWidth triangles in SoA form for SIMD-friendly tests.
struct BvhTriangleBatch {
  std::array<float, kBvhTriangleBatchWidth> v0x{};
  std::array<float, kBvhTriangleBatchWidth> v0y{};
  std::array<float, kBvhTriangleBatchWidth> v0z{};
  std::array<float, kBvhTriangleBatchWidth> e1x{};
  std::array<float, kBvhTriangleBatchWidth> e1y{};
  std::array<float, kBvhTriangleBatchWidth> e1z{};
  std::array<float, kBvhTriangleBatchWidth> e2x{};
  std::array<float, kBvhTriangleBatchWidth> e2y{};
  std::array<float, kBvhTriangleBatchWidth> e2z{};
  std::uint32_t count = 0u;
};

/// Closest accepted hit within one BvhTriangleBatch.
struct BvhTriangleHit {
  bool hit = false;
  std::uint32_t lane = 0u;
  std::uint32_t accepted_hits = 0u;
  float t = 0.0f;
};

/// Pick the fastest compiled triangle batch intersector supported at runtime.
BvhTriangleIntersectorMode SelectBvhTriangleIntersectorMode(const CpuFeatureSet& features);
const char* ToString(BvhTriangleIntersectorMode mode);

/// Intersect one ray against a compact triangle batch and return the closest hit
/// with t < max_t.
BvhTriangleHit IntersectBvhTriangleBatch(BvhTriangleIntersectorMode mode,
                                         const BvhTriangleRay& ray,
                                         const BvhTriangleBatch& batch,
                                         float max_t);

}  // namespace vkpt::cpu
