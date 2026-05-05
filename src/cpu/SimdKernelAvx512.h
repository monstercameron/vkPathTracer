#pragma once

// D18: x86 AVX-512F packet intersection kernel — 16-wide float (__m512).
// Guard: only compiled when __AVX512F__ is defined.
// Note: AVX-512 frequency throttling may reduce benefit on some CPUs;
// benchmark results should be compared against AVX2 before assuming a speedup.

#if defined(__AVX512F__)

#include <immintrin.h>
#include <limits>

#include "cpu/PacketRay.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"

namespace vkpt::cpu {

static constexpr uint32_t kAvx512LaneWidth = 16u;

// AVX-512F 16-wide Möller–Trumbore ray/triangle intersection.
// Uses __m512 (512-bit = 16×f32) and __mmask16 for predication.
// Updates hits where new t < existing hits.t[lane].
inline uint32_t intersect_triangle_packet_avx512(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {

  const uint32_t active = (packet.count < 16u) ? packet.count : 16u;
  if (active == 0u) return 0u;

  // Active lane mask for the first `active` lanes
  const __mmask16 active_mask = static_cast<__mmask16>((active < 16u) ? ((1u << active) - 1u) : 0xFFFFu);

  constexpr float kEps = 1e-7f;
  __m512 eps16 = _mm512_set1_ps(kEps);

  __m512 ox = _mm512_loadu_ps(packet.origin_x);
  __m512 oy = _mm512_loadu_ps(packet.origin_y);
  __m512 oz = _mm512_loadu_ps(packet.origin_z);
  __m512 dx = _mm512_loadu_ps(packet.dir_x);
  __m512 dy = _mm512_loadu_ps(packet.dir_y);
  __m512 dz = _mm512_loadu_ps(packet.dir_z);

  __m512 e1x = _mm512_set1_ps(tri.e1x);
  __m512 e1y = _mm512_set1_ps(tri.e1y);
  __m512 e1z = _mm512_set1_ps(tri.e1z);
  __m512 e2x = _mm512_set1_ps(tri.e2x);
  __m512 e2y = _mm512_set1_ps(tri.e2y);
  __m512 e2z = _mm512_set1_ps(tri.e2z);
  __m512 v0x = _mm512_set1_ps(tri.v0x);
  __m512 v0y = _mm512_set1_ps(tri.v0y);
  __m512 v0z = _mm512_set1_ps(tri.v0z);

  // h = cross(d, e2)
  __m512 hx = _mm512_fmsub_ps(dy, e2z, _mm512_mul_ps(dz, e2y));
  __m512 hy = _mm512_fmsub_ps(dz, e2x, _mm512_mul_ps(dx, e2z));
  __m512 hz = _mm512_fmsub_ps(dx, e2y, _mm512_mul_ps(dy, e2x));

  // a = dot(e1, h)
  __m512 a = _mm512_fmadd_ps(e1x, hx, _mm512_fmadd_ps(e1y, hy, _mm512_mul_ps(e1z, hz)));

  // valid: |a| > eps (use masked absolute compare)
  __mmask16 valid = _mm512_mask_cmp_ps_mask(active_mask, _mm512_abs_ps(a), eps16, _CMP_GT_OS);

  // inv_a = 1/a
  __m512 inv_a = _mm512_div_ps(_mm512_set1_ps(1.0f), a);

  // s = orig - v0
  __m512 sx = _mm512_sub_ps(ox, v0x);
  __m512 sy = _mm512_sub_ps(oy, v0y);
  __m512 sz = _mm512_sub_ps(oz, v0z);

  // u = dot(s, h) * inv_a
  __m512 u16 = _mm512_mul_ps(
      _mm512_fmadd_ps(sx, hx, _mm512_fmadd_ps(sy, hy, _mm512_mul_ps(sz, hz))),
      inv_a);
  valid = _mm512_mask_cmp_ps_mask(valid, u16, _mm512_setzero_ps(), _CMP_GE_OS);
  valid = _mm512_mask_cmp_ps_mask(valid, u16, _mm512_set1_ps(1.0f), _CMP_LE_OS);

  // q = cross(s, e1)
  __m512 qx = _mm512_fmsub_ps(sy, e1z, _mm512_mul_ps(sz, e1y));
  __m512 qy = _mm512_fmsub_ps(sz, e1x, _mm512_mul_ps(sx, e1z));
  __m512 qz = _mm512_fmsub_ps(sx, e1y, _mm512_mul_ps(sy, e1x));

  // v = dot(d, q) * inv_a
  __m512 v16 = _mm512_mul_ps(
      _mm512_fmadd_ps(dx, qx, _mm512_fmadd_ps(dy, qy, _mm512_mul_ps(dz, qz))),
      inv_a);
  valid = _mm512_mask_cmp_ps_mask(valid, v16, _mm512_setzero_ps(), _CMP_GE_OS);
  __m512 uv = _mm512_add_ps(u16, v16);
  valid = _mm512_mask_cmp_ps_mask(valid, uv, _mm512_set1_ps(1.0f), _CMP_LE_OS);

  // t = dot(e2, q) * inv_a
  __m512 t16 = _mm512_mul_ps(
      _mm512_fmadd_ps(e2x, qx, _mm512_fmadd_ps(e2y, qy, _mm512_mul_ps(e2z, qz))),
      inv_a);
  valid = _mm512_mask_cmp_ps_mask(valid, t16, eps16, _CMP_GT_OS);

  // Compare with existing best t
  __m512 best_t = _mm512_loadu_ps(hits.t);
  __mmask16 closer = _mm512_mask_cmp_ps_mask(valid, t16, best_t, _CMP_LT_OS);

  if (closer == 0u) return 0u;

  float t_vals[16], u_vals[16], v_vals[16];
  _mm512_storeu_ps(t_vals, t16);
  _mm512_storeu_ps(u_vals, u16);
  _mm512_storeu_ps(v_vals, v16);

  uint32_t hit_bits = 0u;
  for (uint32_t i = 0; i < active; ++i) {
    if (closer & (1u << i)) {
      hits.t[i] = t_vals[i]; hits.u[i] = u_vals[i]; hits.v[i] = v_vals[i];
      hits.material[i] = tri.material_index;
      hits.hit_mask |= (1u << i);
      hit_bits |= (1u << i);
    }
  }
  return hit_bits;
}

class Avx512SimdKernel final : public ISimdKernel {
 public:
  SimdKernelInfo info() const override {
    return {SimdMode::AVX512, kAvx512LaneWidth, PacketLanePolicy::ZeroPaddedTail, true, true, "avx512"};
  }

  uint32_t intersect_triangle_packet(const RayPacket& packet,
                                     const TriangleSOA& triangle,
                                     HitPacket& hits) const override {
    return intersect_triangle_packet_avx512(packet, triangle, hits);
  }
};

}  // namespace vkpt::cpu

#endif  // __AVX512F__
