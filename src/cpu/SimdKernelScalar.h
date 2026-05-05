#pragma once

// D16/D17-fallback: Scalar packet intersection kernel.
// Processes each lane sequentially — always available, used as baseline.

#include <cmath>
#include <limits>

#include "cpu/PacketRay.h"
#include "cpu/SimdKernel.h"

namespace vkpt::cpu {

// Möller–Trumbore ray/triangle intersection — scalar 1-lane implementation.
// Returns true if this lane hits the triangle, updates t/u/v.
inline bool intersect_triangle_scalar(
    float ox, float oy, float oz,
    float dx, float dy, float dz,
    float e1x, float e1y, float e1z,   // edge1 = v1 - v0
    float e2x, float e2y, float e2z,   // edge2 = v2 - v0
    float v0x, float v0y, float v0z,
    float& t_out, float& u_out, float& v_out) {

  constexpr float kEps = 1e-7f;

  // h = cross(d, e2)
  const float hx = dy * e2z - dz * e2y;
  const float hy = dz * e2x - dx * e2z;
  const float hz = dx * e2y - dy * e2x;

  // a = dot(e1, h)
  const float a = e1x * hx + e1y * hy + e1z * hz;
  if (std::fabs(a) < kEps) return false;

  const float inv_a = 1.0f / a;

  // s = orig - v0
  const float sx = ox - v0x;
  const float sy = oy - v0y;
  const float sz = oz - v0z;

  // u = dot(s, h) * inv_a
  const float u = (sx * hx + sy * hy + sz * hz) * inv_a;
  if (u < 0.0f || u > 1.0f) return false;

  // q = cross(s, e1)
  const float qx = sy * e1z - sz * e1y;
  const float qy = sz * e1x - sx * e1z;
  const float qz = sx * e1y - sy * e1x;

  // v = dot(d, q) * inv_a
  const float v = (dx * qx + dy * qy + dz * qz) * inv_a;
  if (v < 0.0f || u + v > 1.0f) return false;

  // t = dot(e2, q) * inv_a
  const float t = (e2x * qx + e2y * qy + e2z * qz) * inv_a;
  if (t < kEps) return false;

  t_out = t;
  u_out = u;
  v_out = v;
  return true;
}

// Process a full RayPacket against one triangle (scalar loop over lanes).
// Updates HitPacket where the new t is less than the existing best t.
inline uint32_t intersect_triangle_packet_scalar(
    const RayPacket& packet,
    const TriangleSOA& tri,
    HitPacket& hits) {

  uint32_t newly_hit = 0u;
  for (uint32_t lane = 0; lane < packet.count; ++lane) {
    float t = hits.t[lane];  // current best t for this lane
    float u = 0.0f, v = 0.0f;
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
      newly_hit |= (1u << lane);
    }
  }
  return newly_hit;
}

// Initialize a HitPacket with +inf sentinel t values (no hits).
inline void reset_hit_packet(HitPacket& hits, uint32_t lane_count) {
  hits.hit_mask = 0u;
  for (uint32_t i = 0; i < lane_count; ++i) {
    hits.t[i] = std::numeric_limits<float>::infinity();
    hits.u[i] = hits.v[i] = 0.0f;
    hits.material[i] = 0u;
  }
}

class ScalarSimdKernel final : public ISimdKernel {
 public:
  SimdKernelInfo info() const override {
    return {SimdMode::Scalar, 1u, PacketLanePolicy::ScalarTail, true, true, "scalar"};
  }

  uint32_t intersect_triangle_packet(const RayPacket& packet,
                                     const TriangleSOA& triangle,
                                     HitPacket& hits) const override {
    return intersect_triangle_packet_scalar(packet, triangle, hits);
  }
};

}  // namespace vkpt::cpu
