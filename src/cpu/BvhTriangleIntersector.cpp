#include "cpu/BvhTriangleIntersector.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if defined(PT_ENABLE_NEON) && (defined(__aarch64__) || defined(_M_ARM64)) && \
    (defined(__ARM_NEON) || defined(__ARM_NEON__))
  #define VKPT_CPU_BVH_NEON 1
  #include <arm_neon.h>
#endif

namespace vkpt::cpu {

#if defined(PT_ENABLE_AVX2)
BvhTriangleHit IntersectBvhTriangleBatchAvx2(const BvhTriangleRay& ray,
                                             const BvhTriangleBatch& batch,
                                             float max_t);
#endif

namespace {

constexpr float kEpsilon = 1e-4f;

void commit_ordered_lane_hit(BvhTriangleHit& out, std::uint32_t lane, float t) {
  if (t <= kEpsilon || t >= out.t) {
    return;
  }
  ++out.accepted_hits;
  out.hit = true;
  out.lane = lane;
  out.t = t;
}

BvhTriangleHit intersect_scalar(const BvhTriangleRay& ray,
                                const BvhTriangleBatch& batch,
                                float max_t) {
  BvhTriangleHit out{};
  out.t = max_t;
  const std::uint32_t count = std::min<std::uint32_t>(
      batch.count, static_cast<std::uint32_t>(kBvhTriangleBatchWidth));

  for (std::uint32_t lane = 0u; lane < count; ++lane) {
    const float hx = ray.dy * batch.e2z[lane] - ray.dz * batch.e2y[lane];
    const float hy = ray.dz * batch.e2x[lane] - ray.dx * batch.e2z[lane];
    const float hz = ray.dx * batch.e2y[lane] - ray.dy * batch.e2x[lane];
    const float det = batch.e1x[lane] * hx + batch.e1y[lane] * hy + batch.e1z[lane] * hz;
    if (std::fabs(det) < kEpsilon) {
      continue;
    }

    const float inv_det = 1.0f / det;
    const float sx = ray.ox - batch.v0x[lane];
    const float sy = ray.oy - batch.v0y[lane];
    const float sz = ray.oz - batch.v0z[lane];
    const float u = (sx * hx + sy * hy + sz * hz) * inv_det;
    if (u < 0.0f || u > 1.0f) {
      continue;
    }

    const float qx = sy * batch.e1z[lane] - sz * batch.e1y[lane];
    const float qy = sz * batch.e1x[lane] - sx * batch.e1z[lane];
    const float qz = sx * batch.e1y[lane] - sy * batch.e1x[lane];
    const float v = (ray.dx * qx + ray.dy * qy + ray.dz * qz) * inv_det;
    if (v < 0.0f || u + v > 1.0f) {
      continue;
    }

    const float t = (batch.e2x[lane] * qx + batch.e2y[lane] * qy + batch.e2z[lane] * qz) * inv_det;
    commit_ordered_lane_hit(out, lane, t);
  }

  return out;
}

#if defined(VKPT_CPU_BVH_NEON)
BvhTriangleHit intersect_neon(const BvhTriangleRay& ray,
                              const BvhTriangleBatch& batch,
                              float max_t) {
  BvhTriangleHit out{};
  out.t = max_t;
  const std::uint32_t count = std::min<std::uint32_t>(
      batch.count, static_cast<std::uint32_t>(kBvhTriangleBatchWidth));

  const float32x4_t eps = vdupq_n_f32(kEpsilon);
  const float32x4_t zero = vdupq_n_f32(0.0f);
  const float32x4_t one = vdupq_n_f32(1.0f);
  const float32x4_t max_t_vec = vdupq_n_f32(max_t);
  const float32x4_t ox = vdupq_n_f32(ray.ox);
  const float32x4_t oy = vdupq_n_f32(ray.oy);
  const float32x4_t oz = vdupq_n_f32(ray.oz);
  const float32x4_t dx = vdupq_n_f32(ray.dx);
  const float32x4_t dy = vdupq_n_f32(ray.dy);
  const float32x4_t dz = vdupq_n_f32(ray.dz);

  for (std::uint32_t base = 0u; base < count; base += 4u) {
    const float32x4_t v0x = vld1q_f32(batch.v0x.data() + base);
    const float32x4_t v0y = vld1q_f32(batch.v0y.data() + base);
    const float32x4_t v0z = vld1q_f32(batch.v0z.data() + base);
    const float32x4_t e1x = vld1q_f32(batch.e1x.data() + base);
    const float32x4_t e1y = vld1q_f32(batch.e1y.data() + base);
    const float32x4_t e1z = vld1q_f32(batch.e1z.data() + base);
    const float32x4_t e2x = vld1q_f32(batch.e2x.data() + base);
    const float32x4_t e2y = vld1q_f32(batch.e2y.data() + base);
    const float32x4_t e2z = vld1q_f32(batch.e2z.data() + base);

    const float32x4_t hx = vsubq_f32(vmulq_f32(dy, e2z), vmulq_f32(dz, e2y));
    const float32x4_t hy = vsubq_f32(vmulq_f32(dz, e2x), vmulq_f32(dx, e2z));
    const float32x4_t hz = vsubq_f32(vmulq_f32(dx, e2y), vmulq_f32(dy, e2x));
    const float32x4_t det = vaddq_f32(vaddq_f32(vmulq_f32(e1x, hx), vmulq_f32(e1y, hy)),
                                      vmulq_f32(e1z, hz));
    const float32x4_t inv_det = vdivq_f32(one, det);
    const float32x4_t sx = vsubq_f32(ox, v0x);
    const float32x4_t sy = vsubq_f32(oy, v0y);
    const float32x4_t sz = vsubq_f32(oz, v0z);
    const float32x4_t u = vmulq_f32(
        vaddq_f32(vaddq_f32(vmulq_f32(sx, hx), vmulq_f32(sy, hy)), vmulq_f32(sz, hz)),
        inv_det);

    const float32x4_t qx = vsubq_f32(vmulq_f32(sy, e1z), vmulq_f32(sz, e1y));
    const float32x4_t qy = vsubq_f32(vmulq_f32(sz, e1x), vmulq_f32(sx, e1z));
    const float32x4_t qz = vsubq_f32(vmulq_f32(sx, e1y), vmulq_f32(sy, e1x));
    const float32x4_t v = vmulq_f32(
        vaddq_f32(vaddq_f32(vmulq_f32(dx, qx), vmulq_f32(dy, qy)), vmulq_f32(dz, qz)),
        inv_det);
    const float32x4_t t = vmulq_f32(
        vaddq_f32(vaddq_f32(vmulq_f32(e2x, qx), vmulq_f32(e2y, qy)), vmulq_f32(e2z, qz)),
        inv_det);

    uint32x4_t valid = vcgeq_f32(vabsq_f32(det), eps);
    valid = vandq_u32(valid, vcgeq_f32(u, zero));
    valid = vandq_u32(valid, vcleq_f32(u, one));
    valid = vandq_u32(valid, vcgeq_f32(v, zero));
    valid = vandq_u32(valid, vcleq_f32(vaddq_f32(u, v), one));
    valid = vandq_u32(valid, vcgtq_f32(t, eps));
    valid = vandq_u32(valid, vcltq_f32(t, max_t_vec));

    alignas(16) std::uint32_t mask_lanes[4] = {};
    alignas(16) float t_lanes[4] = {};
    vst1q_u32(mask_lanes, valid);
    vst1q_f32(t_lanes, t);

    for (std::uint32_t lane = 0u; lane < 4u && base + lane < count; ++lane) {
      if (mask_lanes[lane] != 0u) {
        commit_ordered_lane_hit(out, base + lane, t_lanes[lane]);
      }
    }
  }

  return out;
}
#endif

}  // namespace

BvhTriangleIntersectorMode SelectBvhTriangleIntersectorMode(const CpuFeatureSet& features) {
  (void)features;
#if defined(PT_ENABLE_AVX2)
  if (features.avx2) {
    return BvhTriangleIntersectorMode::X86Avx2;
  }
#endif

#if defined(VKPT_CPU_BVH_NEON)
  if (features.neon) {
    return BvhTriangleIntersectorMode::ArmNeon;
  }
#endif

  return BvhTriangleIntersectorMode::Scalar;
}

const char* ToString(BvhTriangleIntersectorMode mode) {
  switch (mode) {
    case BvhTriangleIntersectorMode::Scalar:
      return "scalar";
    case BvhTriangleIntersectorMode::ArmNeon:
      return "arm-neon";
    case BvhTriangleIntersectorMode::X86Avx2:
      return "x86-avx2";
    default:
      return "unknown";
  }
}

BvhTriangleHit IntersectBvhTriangleBatch(BvhTriangleIntersectorMode mode,
                                         const BvhTriangleRay& ray,
                                         const BvhTriangleBatch& batch,
                                         float max_t) {
  if (batch.count == 0u || !(max_t > kEpsilon)) {
    BvhTriangleHit out{};
    out.t = max_t;
    return out;
  }

  switch (mode) {
#if defined(PT_ENABLE_AVX2)
    case BvhTriangleIntersectorMode::X86Avx2:
      return IntersectBvhTriangleBatchAvx2(ray, batch, max_t);
#endif
#if defined(VKPT_CPU_BVH_NEON)
    case BvhTriangleIntersectorMode::ArmNeon:
      return intersect_neon(ray, batch, max_t);
#endif
    case BvhTriangleIntersectorMode::Scalar:
    default:
      return intersect_scalar(ray, batch, max_t);
  }
}

}  // namespace vkpt::cpu
