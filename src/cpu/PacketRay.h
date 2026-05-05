#pragma once

#include <cstdint>

namespace vkpt::cpu {

// Maximum packet width supported by any kernel (AVX-512 = 16 lanes).
static constexpr uint32_t kMaxPacketWidth = 16u;

inline uint32_t active_lane_mask(uint32_t lane_count) {
  if (lane_count >= kMaxPacketWidth) {
    return 0xffffu;
  }
  return lane_count == 0u ? 0u : ((1u << lane_count) - 1u);
}

// Ray packet in SoA (Structure of Arrays) layout for SIMD-friendliness.
// Rays in lanes [0, count) are active; remaining lanes are unused.
// Kernels must preserve lane order and mask off lanes outside active_lane_mask(count).
struct RayPacket {
  float origin_x[kMaxPacketWidth] = {};
  float origin_y[kMaxPacketWidth] = {};
  float origin_z[kMaxPacketWidth] = {};
  float dir_x[kMaxPacketWidth] = {};
  float dir_y[kMaxPacketWidth] = {};
  float dir_z[kMaxPacketWidth] = {};
  uint32_t count = 0;  // number of active lanes
};

// Per-ray intersection result in SoA layout.
// hit_mask bit N is set when lane N recorded a hit.
struct HitPacket {
  uint32_t hit_mask = 0;        // bitmask of lanes that produced a hit
  float t[kMaxPacketWidth]    = {};  // ray parameter at hit
  float u[kMaxPacketWidth]    = {};  // barycentric u
  float v[kMaxPacketWidth]    = {};  // barycentric v
  float nx[kMaxPacketWidth]   = {};  // surface normal x
  float ny[kMaxPacketWidth]   = {};  // surface normal y
  float nz[kMaxPacketWidth]   = {};  // surface normal z
  uint32_t material[kMaxPacketWidth] = {};  // material index at hit
};

// Per-triangle data in SOA layout for packet intersection.
struct TriangleSOA {
  // Vertex 0
  float v0x = 0.0f, v0y = 0.0f, v0z = 0.0f;
  // Edge 1 = v1 - v0
  float e1x = 0.0f, e1y = 0.0f, e1z = 0.0f;
  // Edge 2 = v2 - v0
  float e2x = 0.0f, e2y = 0.0f, e2z = 0.0f;
  uint32_t material_index = 0;
};

}  // namespace vkpt::cpu
