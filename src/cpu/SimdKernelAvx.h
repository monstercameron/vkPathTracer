#pragma once

// x86 AVX packet intersection kernel. This path uses 256-bit float operations
// without AVX2/FMA requirements, so it is usable as the explicit AVX tier.

#if defined(__AVX__)

#include <immintrin.h>

#include "cpu/PacketRay.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"

namespace vkpt::cpu {

static constexpr uint32_t kAvxLaneWidth = 8u;

inline uint32_t intersect_triangle_packet_avx(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {
  const uint32_t active = (packet.count < kAvxLaneWidth) ? packet.count : kAvxLaneWidth;
  if (active == 0u) {
    return 0u;
  }

  constexpr float kEps = 1e-7f;
  const __m256 eps8 = _mm256_set1_ps(kEps);
  const __m256 zero8 = _mm256_setzero_ps();
  const __m256 one8 = _mm256_set1_ps(1.0f);

  const __m256 ox = _mm256_loadu_ps(packet.origin_x);
  const __m256 oy = _mm256_loadu_ps(packet.origin_y);
  const __m256 oz = _mm256_loadu_ps(packet.origin_z);
  const __m256 dx = _mm256_loadu_ps(packet.dir_x);
  const __m256 dy = _mm256_loadu_ps(packet.dir_y);
  const __m256 dz = _mm256_loadu_ps(packet.dir_z);

  const __m256 e1x = _mm256_set1_ps(tri.e1x);
  const __m256 e1y = _mm256_set1_ps(tri.e1y);
  const __m256 e1z = _mm256_set1_ps(tri.e1z);
  const __m256 e2x = _mm256_set1_ps(tri.e2x);
  const __m256 e2y = _mm256_set1_ps(tri.e2y);
  const __m256 e2z = _mm256_set1_ps(tri.e2z);
  const __m256 v0x = _mm256_set1_ps(tri.v0x);
  const __m256 v0y = _mm256_set1_ps(tri.v0y);
  const __m256 v0z = _mm256_set1_ps(tri.v0z);

  const __m256 hx = _mm256_sub_ps(_mm256_mul_ps(dy, e2z), _mm256_mul_ps(dz, e2y));
  const __m256 hy = _mm256_sub_ps(_mm256_mul_ps(dz, e2x), _mm256_mul_ps(dx, e2z));
  const __m256 hz = _mm256_sub_ps(_mm256_mul_ps(dx, e2y), _mm256_mul_ps(dy, e2x));

  const __m256 a = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(e1x, hx), _mm256_mul_ps(e1y, hy)),
                                 _mm256_mul_ps(e1z, hz));
  const __m256 abs_a = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a);
  __m256 valid = _mm256_cmp_ps(abs_a, eps8, _CMP_GT_OS);
  const __m256 inv_a = _mm256_div_ps(one8, a);

  const __m256 sx = _mm256_sub_ps(ox, v0x);
  const __m256 sy = _mm256_sub_ps(oy, v0y);
  const __m256 sz = _mm256_sub_ps(oz, v0z);

  __m256 u8 = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(sx, hx), _mm256_mul_ps(sy, hy)),
                            _mm256_mul_ps(sz, hz));
  u8 = _mm256_mul_ps(u8, inv_a);
  valid = _mm256_and_ps(valid,
      _mm256_and_ps(_mm256_cmp_ps(u8, zero8, _CMP_GE_OS), _mm256_cmp_ps(u8, one8, _CMP_LE_OS)));

  const __m256 qx = _mm256_sub_ps(_mm256_mul_ps(sy, e1z), _mm256_mul_ps(sz, e1y));
  const __m256 qy = _mm256_sub_ps(_mm256_mul_ps(sz, e1x), _mm256_mul_ps(sx, e1z));
  const __m256 qz = _mm256_sub_ps(_mm256_mul_ps(sx, e1y), _mm256_mul_ps(sy, e1x));

  __m256 v8 = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, qx), _mm256_mul_ps(dy, qy)),
                            _mm256_mul_ps(dz, qz));
  v8 = _mm256_mul_ps(v8, inv_a);
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(v8, zero8, _CMP_GE_OS));
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(_mm256_add_ps(u8, v8), one8, _CMP_LE_OS));

  __m256 t8 = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(e2x, qx), _mm256_mul_ps(e2y, qy)),
                            _mm256_mul_ps(e2z, qz));
  t8 = _mm256_mul_ps(t8, inv_a);
  valid = _mm256_and_ps(valid, _mm256_cmp_ps(t8, eps8, _CMP_GT_OS));

  const __m256 best_t = _mm256_loadu_ps(hits.t);
  const __m256 closer = _mm256_and_ps(valid, _mm256_cmp_ps(t8, best_t, _CMP_LT_OS));
  const int close_mask = _mm256_movemask_ps(closer);
  if (close_mask == 0) {
    return 0u;
  }

  float t_vals[8] = {};
  float u_vals[8] = {};
  float v_vals[8] = {};
  _mm256_storeu_ps(t_vals, t8);
  _mm256_storeu_ps(u_vals, u8);
  _mm256_storeu_ps(v_vals, v8);

  uint32_t hit_bits = 0u;
  for (uint32_t i = 0; i < active; ++i) {
    if (close_mask & (1 << i)) {
      hits.t[i] = t_vals[i];
      hits.u[i] = u_vals[i];
      hits.v[i] = v_vals[i];
      hits.material[i] = tri.material_index;
      hits.hit_mask |= (1u << i);
      hit_bits |= (1u << i);
    }
  }
  return hit_bits;
}

inline uint32_t intersect_triangle_packet_avx_full(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {
  uint32_t all_hits = 0u;
  uint32_t lane = 0u;
  while (lane + kAvxLaneWidth <= packet.count) {
    RayPacket p8{};
    p8.count = kAvxLaneWidth;
    for (uint32_t i = 0; i < kAvxLaneWidth; ++i) {
      p8.origin_x[i] = packet.origin_x[lane + i];
      p8.origin_y[i] = packet.origin_y[lane + i];
      p8.origin_z[i] = packet.origin_z[lane + i];
      p8.dir_x[i] = packet.dir_x[lane + i];
      p8.dir_y[i] = packet.dir_y[lane + i];
      p8.dir_z[i] = packet.dir_z[lane + i];
    }
    HitPacket h8{};
    for (uint32_t i = 0; i < kAvxLaneWidth; ++i) {
      h8.t[i] = hits.t[lane + i];
    }
    const uint32_t bits = intersect_triangle_packet_avx(p8, tri, h8);
    for (uint32_t i = 0; i < kAvxLaneWidth; ++i) {
      if (bits & (1u << i)) {
        hits.t[lane + i] = h8.t[i];
        hits.u[lane + i] = h8.u[i];
        hits.v[lane + i] = h8.v[i];
        hits.material[lane + i] = h8.material[i];
        hits.hit_mask |= (1u << (lane + i));
        all_hits |= (1u << (lane + i));
      }
    }
    lane += kAvxLaneWidth;
  }
  for (; lane < packet.count; ++lane) {
    float t = hits.t[lane];
    float u = 0.0f;
    float v = 0.0f;
    if (intersect_triangle_scalar(packet.origin_x[lane], packet.origin_y[lane], packet.origin_z[lane],
                                  packet.dir_x[lane], packet.dir_y[lane], packet.dir_z[lane],
                                  tri.e1x, tri.e1y, tri.e1z,
                                  tri.e2x, tri.e2y, tri.e2z,
                                  tri.v0x, tri.v0y, tri.v0z,
                                  t, u, v) && t < hits.t[lane]) {
      hits.t[lane] = t;
      hits.u[lane] = u;
      hits.v[lane] = v;
      hits.material[lane] = tri.material_index;
      hits.hit_mask |= (1u << lane);
      all_hits |= (1u << lane);
    }
  }
  return all_hits;
}

class AvxSimdKernel final : public ISimdKernel {
 public:
  SimdKernelInfo info() const override {
    return {SimdMode::AVX, kAvxLaneWidth, PacketLanePolicy::ZeroPaddedTail, true, true, "avx"};
  }

  uint32_t intersect_triangle_packet(const RayPacket& packet,
                                     const TriangleSOA& triangle,
                                     HitPacket& hits) const override {
    return intersect_triangle_packet_avx_full(packet, triangle, hits);
  }
};

}  // namespace vkpt::cpu

#endif  // __AVX__
