#include "cpu/BvhTriangleIntersector.h"

#include <algorithm>
#include <cstdint>
#include <immintrin.h>

namespace vkpt::cpu {

namespace {

constexpr float kEpsilon = 1e-4f;

__m256 load_ps(const std::array<float, kBvhTriangleBatchWidth>& values) {
  return _mm256_loadu_ps(values.data());
}

__m256 abs_ps(__m256 value) {
  const __m256 sign_mask = _mm256_set1_ps(-0.0f);
  return _mm256_andnot_ps(sign_mask, value);
}

void commit_ordered_lane_hit(BvhTriangleHit& out, std::uint32_t lane, float t) {
  if (t <= kEpsilon || t >= out.t) {
    return;
  }
  ++out.accepted_hits;
  out.hit = true;
  out.lane = lane;
  out.t = t;
}

}  // namespace

BvhTriangleHit IntersectBvhTriangleBatchAvx2(const BvhTriangleRay& ray,
                                             const BvhTriangleBatch& batch,
                                             float max_t) {
  BvhTriangleHit out{};
  out.t = max_t;
  const std::uint32_t count = std::min<std::uint32_t>(
      batch.count, static_cast<std::uint32_t>(kBvhTriangleBatchWidth));
  if (count == 0u) {
    return out;
  }

  const __m256 zero = _mm256_setzero_ps();
  const __m256 one = _mm256_set1_ps(1.0f);
  const __m256 eps = _mm256_set1_ps(kEpsilon);
  const __m256 max_t_vec = _mm256_set1_ps(max_t);
  const __m256 ox = _mm256_set1_ps(ray.ox);
  const __m256 oy = _mm256_set1_ps(ray.oy);
  const __m256 oz = _mm256_set1_ps(ray.oz);
  const __m256 dx = _mm256_set1_ps(ray.dx);
  const __m256 dy = _mm256_set1_ps(ray.dy);
  const __m256 dz = _mm256_set1_ps(ray.dz);

  const __m256 v0x = load_ps(batch.v0x);
  const __m256 v0y = load_ps(batch.v0y);
  const __m256 v0z = load_ps(batch.v0z);
  const __m256 e1x = load_ps(batch.e1x);
  const __m256 e1y = load_ps(batch.e1y);
  const __m256 e1z = load_ps(batch.e1z);
  const __m256 e2x = load_ps(batch.e2x);
  const __m256 e2y = load_ps(batch.e2y);
  const __m256 e2z = load_ps(batch.e2z);

  const __m256 hx = _mm256_sub_ps(_mm256_mul_ps(dy, e2z), _mm256_mul_ps(dz, e2y));
  const __m256 hy = _mm256_sub_ps(_mm256_mul_ps(dz, e2x), _mm256_mul_ps(dx, e2z));
  const __m256 hz = _mm256_sub_ps(_mm256_mul_ps(dx, e2y), _mm256_mul_ps(dy, e2x));
  const __m256 det = _mm256_add_ps(
      _mm256_add_ps(_mm256_mul_ps(e1x, hx), _mm256_mul_ps(e1y, hy)),
      _mm256_mul_ps(e1z, hz));
  const __m256 inv_det = _mm256_div_ps(one, det);
  const __m256 sx = _mm256_sub_ps(ox, v0x);
  const __m256 sy = _mm256_sub_ps(oy, v0y);
  const __m256 sz = _mm256_sub_ps(oz, v0z);
  const __m256 u = _mm256_mul_ps(
      _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(sx, hx), _mm256_mul_ps(sy, hy)),
                    _mm256_mul_ps(sz, hz)),
      inv_det);

  const __m256 qx = _mm256_sub_ps(_mm256_mul_ps(sy, e1z), _mm256_mul_ps(sz, e1y));
  const __m256 qy = _mm256_sub_ps(_mm256_mul_ps(sz, e1x), _mm256_mul_ps(sx, e1z));
  const __m256 qz = _mm256_sub_ps(_mm256_mul_ps(sx, e1y), _mm256_mul_ps(sy, e1x));
  const __m256 v = _mm256_mul_ps(
      _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, qx), _mm256_mul_ps(dy, qy)),
                    _mm256_mul_ps(dz, qz)),
      inv_det);
  const __m256 t = _mm256_mul_ps(
      _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(e2x, qx), _mm256_mul_ps(e2y, qy)),
                    _mm256_mul_ps(e2z, qz)),
      inv_det);

  __m256 valid = _mm256_cmp_ps(abs_ps(det), eps, _CMP_GE_OQ);
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(u, zero, _CMP_GE_OQ));
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(u, one, _CMP_LE_OQ));
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(v, zero, _CMP_GE_OQ));
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(_mm256_add_ps(u, v), one, _CMP_LE_OQ));
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(t, eps, _CMP_GT_OQ));
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(t, max_t_vec, _CMP_LT_OQ));

  std::uint32_t mask = static_cast<std::uint32_t>(_mm256_movemask_ps(valid));
  mask &= (1u << count) - 1u;
  if (mask == 0u) {
    return out;
  }

  alignas(32) float t_lanes[kBvhTriangleBatchWidth] = {};
  _mm256_store_ps(t_lanes, t);
  for (std::uint32_t lane = 0u; lane < count; ++lane) {
    if ((mask & (1u << lane)) != 0u) {
      commit_ordered_lane_hit(out, lane, t_lanes[lane]);
    }
  }
  return out;
}

}  // namespace vkpt::cpu
