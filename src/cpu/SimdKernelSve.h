#pragma once

// D20: ARM SVE packet intersection kernel — variable-width float vectors.
// Guard: only compiled when __ARM_FEATURE_SVE is defined
// (requires -march=armv8-a+sve or equivalent).

#if defined(__ARM_FEATURE_SVE)

#include <arm_sve.h>
#include <limits>

#include "cpu/PacketRay.h"
#include "cpu/SimdKernel.h"
#include "cpu/SimdKernelScalar.h"  // intersect_triangle_scalar, reset_hit_packet

namespace vkpt::cpu {

// SVE vector length query (number of 32-bit float lanes at runtime).
inline uint32_t sve_float_lane_count() {
  return static_cast<uint32_t>(svcntw());  // count of 32-bit words per SVE vector
}

// SVE Möller–Trumbore packet intersection.
// Processes min(packet.count, svcntw()) lanes per SVE vector operation.
// Updates hits where new t < existing t.
inline uint32_t intersect_triangle_packet_sve(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {

  constexpr float kEps = 1e-7f;
  const uint32_t lane_width = sve_float_lane_count();
  uint32_t all_hits = 0u;
  uint32_t lane = 0u;

  while (lane < packet.count) {
    const uint32_t remaining = packet.count - lane;
    // Active predicate: first min(remaining, lane_width) lanes are active
    const svbool_t pg = svwhilelt_b32(lane, packet.count);

    // Load ray data (gather from SoA arrays)
    svfloat32_t ox = svld1(pg, packet.origin_x + lane);
    svfloat32_t oy = svld1(pg, packet.origin_y + lane);
    svfloat32_t oz = svld1(pg, packet.origin_z + lane);
    svfloat32_t dx = svld1(pg, packet.dir_x + lane);
    svfloat32_t dy = svld1(pg, packet.dir_y + lane);
    svfloat32_t dz = svld1(pg, packet.dir_z + lane);

    // Broadcast triangle scalars
    svfloat32_t e1x = svdup_n_f32(tri.e1x);
    svfloat32_t e1y = svdup_n_f32(tri.e1y);
    svfloat32_t e1z = svdup_n_f32(tri.e1z);
    svfloat32_t e2x = svdup_n_f32(tri.e2x);
    svfloat32_t e2y = svdup_n_f32(tri.e2y);
    svfloat32_t e2z = svdup_n_f32(tri.e2z);
    svfloat32_t v0x = svdup_n_f32(tri.v0x);
    svfloat32_t v0y = svdup_n_f32(tri.v0y);
    svfloat32_t v0z = svdup_n_f32(tri.v0z);
    svfloat32_t eps = svdup_n_f32(kEps);
    svfloat32_t zero = svdup_n_f32(0.0f);
    svfloat32_t one  = svdup_n_f32(1.0f);

    // h = cross(d, e2)
    svfloat32_t hx = svsub_f32_z(pg, svmul_f32_z(pg, dy, e2z), svmul_f32_z(pg, dz, e2y));
    svfloat32_t hy = svsub_f32_z(pg, svmul_f32_z(pg, dz, e2x), svmul_f32_z(pg, dx, e2z));
    svfloat32_t hz = svsub_f32_z(pg, svmul_f32_z(pg, dx, e2y), svmul_f32_z(pg, dy, e2x));

    // a = dot(e1, h)
    svfloat32_t a = svadd_f32_z(pg,
        svadd_f32_z(pg, svmul_f32_z(pg, e1x, hx), svmul_f32_z(pg, e1y, hy)),
        svmul_f32_z(pg, e1z, hz));

    // |a| > eps
    svbool_t valid = svacgt_n_f32(pg, a, kEps);

    // inv_a = 1/a
    svfloat32_t inv_a = svdivr_n_f32_z(valid, a, 1.0f);

    // s = orig - v0
    svfloat32_t sx = svsub_f32_z(pg, ox, v0x);
    svfloat32_t sy = svsub_f32_z(pg, oy, v0y);
    svfloat32_t sz = svsub_f32_z(pg, oz, v0z);

    // u = dot(s, h) * inv_a
    svfloat32_t u4 = svmul_f32_z(valid,
        svadd_f32_z(pg, svadd_f32_z(pg, svmul_f32_z(pg, sx, hx), svmul_f32_z(pg, sy, hy)), svmul_f32_z(pg, sz, hz)),
        inv_a);
    valid = svand_b_z(pg, valid, svcmpge_n_f32(valid, u4, 0.0f));
    valid = svand_b_z(pg, valid, svcmple_n_f32(valid, u4, 1.0f));

    // q = cross(s, e1)
    svfloat32_t qx = svsub_f32_z(pg, svmul_f32_z(pg, sy, e1z), svmul_f32_z(pg, sz, e1y));
    svfloat32_t qy = svsub_f32_z(pg, svmul_f32_z(pg, sz, e1x), svmul_f32_z(pg, sx, e1z));
    svfloat32_t qz = svsub_f32_z(pg, svmul_f32_z(pg, sx, e1y), svmul_f32_z(pg, sy, e1x));

    // v = dot(d, q) * inv_a
    svfloat32_t v4 = svmul_f32_z(valid,
        svadd_f32_z(pg, svadd_f32_z(pg, svmul_f32_z(pg, dx, qx), svmul_f32_z(pg, dy, qy)), svmul_f32_z(pg, dz, qz)),
        inv_a);
    valid = svand_b_z(pg, valid, svcmpge_n_f32(valid, v4, 0.0f));
    svfloat32_t uv = svadd_f32_z(pg, u4, v4);
    valid = svand_b_z(pg, valid, svcmple_n_f32(valid, uv, 1.0f));

    // t = dot(e2, q) * inv_a
    svfloat32_t t4 = svmul_f32_z(valid,
        svadd_f32_z(pg, svadd_f32_z(pg, svmul_f32_z(pg, e2x, qx), svmul_f32_z(pg, e2y, qy)), svmul_f32_z(pg, e2z, qz)),
        inv_a);
    valid = svand_b_z(pg, valid, svcmpgt_n_f32(valid, t4, kEps));

    // Compare against existing best t
    svfloat32_t best_t = svld1(pg, hits.t + lane);
    svbool_t closer = svand_b_z(pg, valid, svcmplt_f32(valid, t4, best_t));

    // Scatter results back
    if (svptest_any(pg, closer)) {
      // Extract to scalar arrays
      const uint32_t actual = (remaining < lane_width) ? remaining : lane_width;
      float t_arr[kMaxPacketWidth] = {}, u_arr[kMaxPacketWidth] = {}, v_arr[kMaxPacketWidth] = {};
      uint32_t close_arr[kMaxPacketWidth] = {};
      svst1(pg, t_arr, t4);
      svst1(pg, u_arr, u4);
      svst1(pg, v_arr, v4);
      // Predicate to bitmask
      for (uint32_t i = 0; i < actual; ++i) {
        svbool_t lane_pg = svwhilelt_b32(i, i + 1);
        close_arr[i] = svptest_any(lane_pg, svand_b_z(lane_pg, closer, lane_pg)) ? 1u : 0u;
      }
      for (uint32_t i = 0; i < actual; ++i) {
        if (close_arr[i]) {
          hits.t[lane + i] = t_arr[i];
          hits.u[lane + i] = u_arr[i];
          hits.v[lane + i] = v_arr[i];
          hits.material[lane + i] = tri.material_index;
          hits.hit_mask |= (1u << (lane + i));
          all_hits |= (1u << (lane + i));
        }
      }
    }
    lane += lane_width;
  }
  return all_hits;
}

class SveSimdKernel final : public ISimdKernel {
 public:
  SimdKernelInfo info() const override {
    return {SimdMode::SVE, sve_float_lane_count(), PacketLanePolicy::PreserveLaneOrder, true, true, "sve"};
  }

  uint32_t intersect_triangle_packet(const RayPacket& packet,
                                     const TriangleSOA& triangle,
                                     HitPacket& hits) const override {
    return intersect_triangle_packet_sve(packet, triangle, hits);
  }
};

}  // namespace vkpt::cpu

#endif  // __ARM_FEATURE_SVE
