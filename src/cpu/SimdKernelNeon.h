#pragma once

// D19: ARM NEON packet intersection kernel — 4-wide float32x4_t.
// Guard: only included when __ARM_NEON is defined (all AArch64 targets).

#if defined(__ARM_NEON)

#include <arm_neon.h>
#include <limits>

#include "cpu/PacketRay.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"  // reset_hit_packet

namespace vkpt::cpu {

static constexpr uint32_t kNeonLaneWidth = 4u;

// NEON 4-wide Möller–Trumbore ray/triangle intersection.
// Processes exactly 4 lanes simultaneously using float32x4_t.
// Lanes past packet.count are inactive (masked out after computation).
// Updates hits where new t < existing hits.t[lane].
inline uint32_t intersect_triangle_packet_neon_4(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {

  const uint32_t active = (packet.count < 4u) ? packet.count : 4u;
  if (active == 0u) return 0u;

  constexpr float kEps = 1e-7f;
  const float32x4_t eps4 = vdupq_n_f32(kEps);
  const float32x4_t zero4 = vdupq_n_f32(0.0f);
  const float32x4_t one4  = vdupq_n_f32(1.0f);

  // Load ray origins and directions for 4 lanes
  const float32x4_t ox = vld1q_f32(packet.origin_x);
  const float32x4_t oy = vld1q_f32(packet.origin_y);
  const float32x4_t oz = vld1q_f32(packet.origin_z);
  const float32x4_t dx = vld1q_f32(packet.dir_x);
  const float32x4_t dy = vld1q_f32(packet.dir_y);
  const float32x4_t dz = vld1q_f32(packet.dir_z);

  // Broadcast triangle data (same for all 4 lanes)
  const float32x4_t e1x = vdupq_n_f32(tri.e1x);
  const float32x4_t e1y = vdupq_n_f32(tri.e1y);
  const float32x4_t e1z = vdupq_n_f32(tri.e1z);
  const float32x4_t e2x = vdupq_n_f32(tri.e2x);
  const float32x4_t e2y = vdupq_n_f32(tri.e2y);
  const float32x4_t e2z = vdupq_n_f32(tri.e2z);
  const float32x4_t v0x = vdupq_n_f32(tri.v0x);
  const float32x4_t v0y = vdupq_n_f32(tri.v0y);
  const float32x4_t v0z = vdupq_n_f32(tri.v0z);

  // h = cross(d, e2)
  const float32x4_t hx = vsubq_f32(vmulq_f32(dy, e2z), vmulq_f32(dz, e2y));
  const float32x4_t hy = vsubq_f32(vmulq_f32(dz, e2x), vmulq_f32(dx, e2z));
  const float32x4_t hz = vsubq_f32(vmulq_f32(dx, e2y), vmulq_f32(dy, e2x));

  // a = dot(e1, h)
  const float32x4_t a = vaddq_f32(vaddq_f32(vmulq_f32(e1x, hx), vmulq_f32(e1y, hy)), vmulq_f32(e1z, hz));

  // Cull near-parallel rays (|a| < eps)
  const float32x4_t abs_a = vabsq_f32(a);
  const uint32x4_t valid0 = vcgtq_f32(abs_a, eps4);

  // inv_a = 1 / a  (only needed in valid lanes, but computing all is safe)
  const float32x4_t inv_a = vrecpeq_f32(a);
  // Newton-Raphson refinement for better precision
  const float32x4_t inv_a2 = vmulq_f32(vrecpsq_f32(a, inv_a), inv_a);

  // s = orig - v0
  const float32x4_t sx = vsubq_f32(ox, v0x);
  const float32x4_t sy = vsubq_f32(oy, v0y);
  const float32x4_t sz = vsubq_f32(oz, v0z);

  // u = dot(s, h) * inv_a
  const float32x4_t u4 = vmulq_f32(
      vaddq_f32(vaddq_f32(vmulq_f32(sx, hx), vmulq_f32(sy, hy)), vmulq_f32(sz, hz)),
      inv_a2);

  // u in [0, 1]
  const uint32x4_t valid1 = vandq_u32(valid0,
      vandq_u32(vcgeq_f32(u4, zero4), vcleq_f32(u4, one4)));

  // q = cross(s, e1)
  const float32x4_t qx = vsubq_f32(vmulq_f32(sy, e1z), vmulq_f32(sz, e1y));
  const float32x4_t qy = vsubq_f32(vmulq_f32(sz, e1x), vmulq_f32(sx, e1z));
  const float32x4_t qz = vsubq_f32(vmulq_f32(sx, e1y), vmulq_f32(sy, e1x));

  // v = dot(d, q) * inv_a
  const float32x4_t v4 = vmulq_f32(
      vaddq_f32(vaddq_f32(vmulq_f32(dx, qx), vmulq_f32(dy, qy)), vmulq_f32(dz, qz)),
      inv_a2);

  // v >= 0 and u + v <= 1
  const float32x4_t uv = vaddq_f32(u4, v4);
  const uint32x4_t valid2 = vandq_u32(valid1,
      vandq_u32(vcgeq_f32(v4, zero4), vcleq_f32(uv, one4)));

  // t = dot(e2, q) * inv_a
  const float32x4_t t4 = vmulq_f32(
      vaddq_f32(vaddq_f32(vmulq_f32(e2x, qx), vmulq_f32(e2y, qy)), vmulq_f32(e2z, qz)),
      inv_a2);

  // t > eps
  const uint32x4_t valid3 = vandq_u32(valid2, vcgtq_f32(t4, eps4));

  // Compare against existing best t
  float cur_t[4];
  vst1q_f32(cur_t, vld1q_f32(hits.t));
  const float32x4_t best_t = vld1q_f32(cur_t);
  const uint32x4_t is_closer = vandq_u32(valid3, vcltq_f32(t4, best_t));

  // Extract lane mask
  uint32_t lane_mask[4];
  vst1q_u32(lane_mask, is_closer);
  uint32_t hit_bits = 0u;
  for (uint32_t i = 0; i < active; ++i) {
    if (lane_mask[i]) hit_bits |= (1u << i);
  }

  if (hit_bits == 0u) return 0u;

  // Write back results for hit lanes
  float t_vals[4], u_vals[4], v_vals[4];
  vst1q_f32(t_vals, t4);
  vst1q_f32(u_vals, u4);
  vst1q_f32(v_vals, v4);

  for (uint32_t i = 0; i < active; ++i) {
    if (hit_bits & (1u << i)) {
      hits.t[i] = t_vals[i];
      hits.u[i] = u_vals[i];
      hits.v[i] = v_vals[i];
      hits.material[i] = tri.material_index;
      hits.hit_mask |= (1u << i);
    }
  }
  return hit_bits;
}

// Full-packet NEON kernel: handles any packet size by processing 4 lanes at a time.
inline uint32_t intersect_triangle_packet_neon(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {

  uint32_t all_hits = 0u;
  uint32_t lane = 0u;

  // Process full 4-lane groups
  while (lane + 4u <= packet.count) {
    // Build a 4-lane sub-packet
    RayPacket p4{};
    p4.count = 4u;
    for (uint32_t i = 0; i < 4u; ++i) {
      p4.origin_x[i] = packet.origin_x[lane + i];
      p4.origin_y[i] = packet.origin_y[lane + i];
      p4.origin_z[i] = packet.origin_z[lane + i];
      p4.dir_x[i]    = packet.dir_x[lane + i];
      p4.dir_y[i]    = packet.dir_y[lane + i];
      p4.dir_z[i]    = packet.dir_z[lane + i];
    }
    HitPacket h4{};
    for (uint32_t i = 0; i < 4u; ++i) h4.t[i] = hits.t[lane + i];
    const uint32_t bits = intersect_triangle_packet_neon_4(p4, tri, h4);
    for (uint32_t i = 0; i < 4u; ++i) {
      if (bits & (1u << i)) {
        hits.t[lane + i] = h4.t[i];
        hits.u[lane + i] = h4.u[i];
        hits.v[lane + i] = h4.v[i];
        hits.material[lane + i] = h4.material[i];
        hits.hit_mask |= (1u << (lane + i));
        all_hits |= (1u << (lane + i));
      }
    }
    lane += 4u;
  }

  // Tail: remaining lanes (0-3) fall back to scalar
  for (; lane < packet.count; ++lane) {
    float t = hits.t[lane], u = 0.0f, v = 0.0f;
    using namespace vkpt::cpu;
    if (intersect_triangle_scalar(
            packet.origin_x[lane], packet.origin_y[lane], packet.origin_z[lane],
            packet.dir_x[lane],    packet.dir_y[lane],    packet.dir_z[lane],
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

}  // namespace vkpt::cpu

#endif  // __ARM_NEON
