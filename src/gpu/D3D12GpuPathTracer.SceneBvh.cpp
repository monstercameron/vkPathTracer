#ifdef PT_ENABLE_D3D12

#include "gpu/D3D12GpuPathTracer.SceneBvh.h"

#include "gpu/D3D12GpuPathTracerInternal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vkpt::gpu {

struct DynamicInstanceRef {
  uint32_t instance_index = 0u;
  float centroid[3]{};
  float bmin[3]{};
  float bmax[3]{};
};

static float bvh_sa(const float bmin[3], const float bmax[3]) {
  float dx = bmax[0]-bmin[0], dy = bmax[1]-bmin[1], dz = bmax[2]-bmin[2];
  if (dx < 0) dx = 0; if (dy < 0) dy = 0; if (dz < 0) dz = 0;
  return 2.0f * (dx*dy + dy*dz + dz*dx);
}

// GPU BVH node layout is intentionally shader-friendly rather than type-safe:
//   [0..2] = min bounds, [3..5] = max bounds,
//   [6]    = left child or leaf flag|first triangle,
//   [7]    = right child or primitive count.
// Child/leaf indices are bit-cast through floats because the compute shader
// consumes one raw float buffer for both bounds and integer metadata.
uint32_t bvh_build(
  std::vector<BvhTriRef>&        refs,
  uint32_t start, uint32_t count,
  const std::vector<uint32_t>&   orig_idx,
  const std::vector<uint32_t>&   orig_tri_mat,
  const BvhBuildConfig&          config,
  std::vector<float>&            gpu_bvh,
  std::vector<uint32_t>&         reord_idx,
  std::vector<uint32_t>&         reord_tri_mat,
  uint32_t&                      node_count)
{
  auto as_fb = [](uint32_t u) -> float {
    float f; std::memcpy(&f, &u, sizeof(f)); return f;
  };

  const uint32_t ni   = node_count++;
  const uint32_t base = ni * 8u;

  // Compute node AABB
  float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (uint32_t i = start; i < start + count; ++i) {
    for (int k = 0; k < 3; ++k) {
      bmin[k] = std::min(bmin[k], refs[i].bmin[k]);
      bmax[k] = std::max(bmax[k], refs[i].bmax[k]);
    }
  }
  gpu_bvh[base+0u]=bmin[0]; gpu_bvh[base+1u]=bmin[1]; gpu_bvh[base+2u]=bmin[2];
  gpu_bvh[base+3u]=bmax[0]; gpu_bvh[base+4u]=bmax[1]; gpu_bvh[base+5u]=bmax[2];

  if (count <= config.leaf_size) {
    // Leaves compact referenced triangles into traversal order. The shader can
    // then walk consecutive triangle/material records without indirection back
    // to the original scene index stream.
    const uint32_t first_tri = static_cast<uint32_t>(reord_idx.size() / 3u);
    for (uint32_t i = start; i < start + count; ++i) {
      const uint32_t t = refs[i].orig_idx;
      reord_idx.push_back(orig_idx[t*3u+0u]);
      reord_idx.push_back(orig_idx[t*3u+1u]);
      reord_idx.push_back(orig_idx[t*3u+2u]);
      reord_tri_mat.push_back(orig_tri_mat[t]);
    }
    gpu_bvh[base+6u] = as_fb(0x80000000u | first_tri); // leaf flag in bit 31
    gpu_bvh[base+7u] = as_fb(count);
    return ni;
  }

  if (config.use_median_split) {
    // Median split is deterministic and cheap. It is useful for validation or
    // scenes where SAH build cost outweighs the traversal savings.
    float cmin[3] = {1e30f, 1e30f, 1e30f};
    float cmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = start; i < start + count; ++i) {
      for (int k = 0; k < 3; ++k) {
        cmin[k] = std::min(cmin[k], refs[i].centroid[k]);
        cmax[k] = std::max(cmax[k], refs[i].centroid[k]);
      }
    }
    float extent[3] = {cmax[0] - cmin[0], cmax[1] - cmin[1], cmax[2] - cmin[2]};
    int axis = 0;
    if (extent[1] > extent[axis]) axis = 1;
    if (extent[2] > extent[axis]) axis = 2;
    const uint32_t mid = start + count / 2u;
    std::nth_element(refs.begin() + start, refs.begin() + mid, refs.begin() + start + count,
      [axis](const BvhTriRef& a, const BvhTriRef& b) {
        return a.centroid[axis] < b.centroid[axis];
      });

    const uint32_t left_child  = bvh_build(refs, start, mid - start,
                        orig_idx, orig_tri_mat, config,
                        gpu_bvh, reord_idx, reord_tri_mat, node_count);
    const uint32_t right_child = bvh_build(refs, mid, (start + count) - mid,
                        orig_idx, orig_tri_mat, config,
                        gpu_bvh, reord_idx, reord_tri_mat, node_count);
    gpu_bvh[base+6u] = as_fb(left_child);
    gpu_bvh[base+7u] = as_fb(right_child);
    return ni;
  }

  // Bucketed SAH approximates the exact O(n^2) surface-area search with a small
  // fixed bucket count. It balances build time against traversal quality and is
  // the default path for larger static triangle sets.
  constexpr int kMaxBvhBuckets = 16;
  const int bucket_count = static_cast<int>(std::clamp(config.bucket_count, 2u, 16u));
  const float node_sa_v = std::max(1e-9f, bvh_sa(bmin, bmax));
  int   best_axis      = 0;
  float best_split_val = 0.0f;
  float best_cost      = 1e30f;
  bool  found_split    = false;

  for (int axis = 0; axis < 3; ++axis) {
    float cmin = 1e30f, cmax = -1e30f;
    for (uint32_t i = start; i < start + count; ++i) {
      cmin = std::min(cmin, refs[i].centroid[axis]);
      cmax = std::max(cmax, refs[i].centroid[axis]);
    }
    const float extent = cmax - cmin;
    if (extent < 1e-6f) continue;

    struct Bkt { float mn[3], mx[3]; uint32_t n; };
    Bkt bkts[kMaxBvhBuckets];
    for (int b = 0; b < bucket_count; ++b) {
      bkts[b].n = 0;
      for (int k=0;k<3;++k) { bkts[b].mn[k]=1e30f; bkts[b].mx[k]=-1e30f; }
    }
    for (uint32_t i = start; i < start + count; ++i) {
      const int b = std::min(bucket_count - 1,
                             static_cast<int>((refs[i].centroid[axis] - cmin) / extent *
                                              static_cast<float>(bucket_count)));
      bkts[b].n++;
      for (int k=0;k<3;++k) {
        bkts[b].mn[k] = std::min(bkts[b].mn[k], refs[i].bmin[k]);
        bkts[b].mx[k] = std::max(bkts[b].mx[k], refs[i].bmax[k]);
      }
    }
    for (int s = 1; s < bucket_count; ++s) {
      float lmn[3]={1e30f,1e30f,1e30f}, lmx[3]={-1e30f,-1e30f,-1e30f};
      float rmn[3]={1e30f,1e30f,1e30f}, rmx[3]={-1e30f,-1e30f,-1e30f};
      uint32_t lc = 0, rc = 0;
      for (int b=0;b<s;++b) {
        if (!bkts[b].n) continue;
        lc += bkts[b].n;
        for (int k=0;k<3;++k) { lmn[k]=std::min(lmn[k],bkts[b].mn[k]); lmx[k]=std::max(lmx[k],bkts[b].mx[k]); }
      }
      for (int b=s;b<bucket_count;++b) {
        if (!bkts[b].n) continue;
        rc += bkts[b].n;
        for (int k=0;k<3;++k) { rmn[k]=std::min(rmn[k],bkts[b].mn[k]); rmx[k]=std::max(rmx[k],bkts[b].mx[k]); }
      }
      if (!lc || !rc) continue;
      const float cost = (bvh_sa(lmn,lmx)*(float)lc + bvh_sa(rmn,rmx)*(float)rc) / node_sa_v;
      if (cost < best_cost) {
        best_cost      = cost;
        best_axis      = axis;
        best_split_val = cmin + static_cast<float>(s) / static_cast<float>(bucket_count) * extent;
        found_split    = true;
      }
    }
  }

  // Partition refs around the best split. If all centroids land on one side,
  // fall back to an even split so recursion always makes progress.
  uint32_t mid;
  if (found_split) {
    const int   ax  = best_axis;
    const float spv = best_split_val;
    auto it = std::stable_partition(
      refs.begin() + start, refs.begin() + start + count,
      [ax, spv](const BvhTriRef& r) { return r.centroid[ax] < spv; });
    mid = static_cast<uint32_t>(it - refs.begin());
    if (mid <= start || mid >= start + count) mid = start + count / 2u;
  } else {
    mid = start + count / 2u;
  }

  const uint32_t left_child  = bvh_build(refs, start, mid - start,
                      orig_idx, orig_tri_mat, config,
                      gpu_bvh, reord_idx, reord_tri_mat, node_count);
  const uint32_t right_child = bvh_build(refs, mid, (start + count) - mid,
                      orig_idx, orig_tri_mat, config,
                      gpu_bvh, reord_idx, reord_tri_mat, node_count);

  // Write internal node children (bit 31 = 0 means internal)
  gpu_bvh[base+6u] = as_fb(left_child);
  gpu_bvh[base+7u] = as_fb(right_child);
  return ni;
}

static uint32_t dynamic_bvh_build(
  std::vector<DynamicInstanceRef>& refs,
  uint32_t start,
  uint32_t count,
  std::vector<float>& gpu_bvh,
  uint32_t& node_count)
{
  auto as_fb = [](uint32_t u) -> float {
    float f; std::memcpy(&f, &u, sizeof(f)); return f;
  };

  const uint32_t ni = node_count++;
  const uint32_t base = ni * 8u;
  float bmin[3] = {1e30f, 1e30f, 1e30f};
  float bmax[3] = {-1e30f, -1e30f, -1e30f};
  for (uint32_t i = start; i < start + count; ++i) {
    for (int k = 0; k < 3; ++k) {
      bmin[k] = std::min(bmin[k], refs[i].bmin[k]);
      bmax[k] = std::max(bmax[k], refs[i].bmax[k]);
    }
  }
  gpu_bvh[base+0u]=bmin[0]; gpu_bvh[base+1u]=bmin[1]; gpu_bvh[base+2u]=bmin[2];
  gpu_bvh[base+3u]=bmax[0]; gpu_bvh[base+4u]=bmax[1]; gpu_bvh[base+5u]=bmax[2];

  if (count <= 1u) {
    // Dynamic instance leaves encode the instance index directly. The shader
    // uses this separate top-level tree to cheaply reject unchanged static
    // geometry while animated/physics instances move every frame.
    gpu_bvh[base+6u] = as_fb(0x80000000u | refs[start].instance_index);
    gpu_bvh[base+7u] = as_fb(1u);
    return ni;
  }

  float extent[3] = {bmax[0]-bmin[0], bmax[1]-bmin[1], bmax[2]-bmin[2]};
  int axis = 0;
  if (extent[1] > extent[axis]) axis = 1;
  if (extent[2] > extent[axis]) axis = 2;
  const uint32_t mid = start + count / 2u;
  std::nth_element(refs.begin() + start, refs.begin() + mid, refs.begin() + start + count,
                   [axis](const DynamicInstanceRef& lhs, const DynamicInstanceRef& rhs) {
                     return lhs.centroid[axis] < rhs.centroid[axis];
                   });

  const uint32_t left = dynamic_bvh_build(refs, start, mid - start, gpu_bvh, node_count);
  const uint32_t right = dynamic_bvh_build(refs, mid, start + count - mid, gpu_bvh, node_count);
  gpu_bvh[base+6u] = as_fb(left);
  gpu_bvh[base+7u] = as_fb(right);
  return ni;
}

static vkpt::pathtracer::Vec3 Cross(const vkpt::pathtracer::Vec3& a, const vkpt::pathtracer::Vec3& b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

static vkpt::pathtracer::Vec3 RotateQuat(const vkpt::pathtracer::Vec3& value,
                                  vkpt::pathtracer::Quat4 q) {
  const float len2 = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
  if (len2 > 1.0e-12f) {
    const float invLen = 1.0f / std::sqrt(len2);
    q.x *= invLen; q.y *= invLen; q.z *= invLen; q.w *= invLen;
  } else {
    q = {};
  }
  const vkpt::pathtracer::Vec3 qv{q.x, q.y, q.z};
  const auto t = Cross(qv, value);
  const vkpt::pathtracer::Vec3 doubled{t.x * 2.0f, t.y * 2.0f, t.z * 2.0f};
  const auto c = Cross(qv, doubled);
  return {value.x + doubled.x * q.w + c.x,
          value.y + doubled.y * q.w + c.y,
          value.z + doubled.z * q.w + c.z};
}

static bool LoadDynamicInstanceRefFromPacked(const std::vector<uint32_t>& insts,
                                             uint32_t instanceIndex,
                                             DynamicInstanceRef& ref) {
  const std::size_t ib = static_cast<std::size_t>(instanceIndex) * kGpuInstanceStrideU32;
  if (ib + 22u >= insts.size()) {
    return false;
  }
  const uint32_t flags = insts[ib + 3u];
  const uint32_t triCount = insts[ib + 1u];
  if ((flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u ||
      triCount == 0u) {
    return false;
  }
  const vkpt::pathtracer::Vec3 translation{
      UintBitsToFloat(insts[ib + 4u]),
      UintBitsToFloat(insts[ib + 5u]),
      UintBitsToFloat(insts[ib + 6u])};
  const vkpt::pathtracer::Quat4 rotation{
      UintBitsToFloat(insts[ib + 8u]),
      UintBitsToFloat(insts[ib + 9u]),
      UintBitsToFloat(insts[ib + 10u]),
      UintBitsToFloat(insts[ib + 11u])};
  const vkpt::pathtracer::Vec3 scale{
      UintBitsToFloat(insts[ib + 12u]),
      UintBitsToFloat(insts[ib + 13u]),
      UintBitsToFloat(insts[ib + 14u])};
  const vkpt::pathtracer::Vec3 localMin{
      UintBitsToFloat(insts[ib + 16u]),
      UintBitsToFloat(insts[ib + 17u]),
      UintBitsToFloat(insts[ib + 18u])};
  const vkpt::pathtracer::Vec3 localMax{
      UintBitsToFloat(insts[ib + 20u]),
      UintBitsToFloat(insts[ib + 21u]),
      UintBitsToFloat(insts[ib + 22u])};

  ref = {};
  ref.instance_index = instanceIndex;
  for (int k = 0; k < 3; ++k) {
    ref.bmin[k] = 1e30f;
    ref.bmax[k] = -1e30f;
  }
  for (uint32_t corner = 0u; corner < 8u; ++corner) {
    const vkpt::pathtracer::Vec3 local{
        (corner & 1u) ? localMax.x : localMin.x,
        (corner & 2u) ? localMax.y : localMin.y,
        (corner & 4u) ? localMax.z : localMin.z};
    const vkpt::pathtracer::Vec3 scaled{local.x * scale.x, local.y * scale.y, local.z * scale.z};
    const auto rotated = RotateQuat(scaled, rotation);
    const vkpt::pathtracer::Vec3 world{
        rotated.x + translation.x,
        rotated.y + translation.y,
        rotated.z + translation.z};
    ref.bmin[0] = std::min(ref.bmin[0], world.x);
    ref.bmin[1] = std::min(ref.bmin[1], world.y);
    ref.bmin[2] = std::min(ref.bmin[2], world.z);
    ref.bmax[0] = std::max(ref.bmax[0], world.x);
    ref.bmax[1] = std::max(ref.bmax[1], world.y);
    ref.bmax[2] = std::max(ref.bmax[2], world.z);
  }
  ref.centroid[0] = (ref.bmin[0] + ref.bmax[0]) * 0.5f;
  ref.centroid[1] = (ref.bmin[1] + ref.bmax[1]) * 0.5f;
  ref.centroid[2] = (ref.bmin[2] + ref.bmax[2]) * 0.5f;
  return true;
}

uint32_t BuildDynamicInstanceBvhFromPackedInstances(const std::vector<uint32_t>& insts,
                                                    uint32_t instanceCount,
                                                    std::vector<float>& outBvh) {
  // Reconstruct world-space bounds from the packed instance buffer that is also
  // uploaded to the shader. Keeping this CPU builder on the packed format makes
  // transform update validation match the GPU traversal inputs byte-for-byte.
  std::vector<DynamicInstanceRef> refs;
  refs.reserve(instanceCount);
  for (uint32_t instanceIndex = 0u; instanceIndex < instanceCount; ++instanceIndex) {
    DynamicInstanceRef ref{};
    if (!LoadDynamicInstanceRefFromPacked(insts, instanceIndex, ref)) {
      continue;
    }
    refs.push_back(ref);
  }

  if (refs.empty()) {
    StoreEmptyBvh(outBvh);
    return 0u;
  }
  outBvh.assign(std::max<std::size_t>(1u, refs.size() * 2u) * 8u, 0.0f);
  uint32_t nodeCount = 0u;
  dynamic_bvh_build(refs, 0u, static_cast<uint32_t>(refs.size()), outBvh, nodeCount);
  outBvh.resize(static_cast<std::size_t>(nodeCount) * 8u);
  return static_cast<uint32_t>(refs.size());
}

uint32_t RefitDynamicInstanceBvhFromPackedInstances(const std::vector<uint32_t>& insts,
                                                    uint32_t instanceCount,
                                                    std::vector<float>& bvh) {
  auto as_u32 = [](float f) -> uint32_t {
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
  };
  const uint32_t nodeCount = static_cast<uint32_t>(bvh.size() / 8u);
  if (nodeCount == 0u || bvh.size() % 8u != 0u) {
    return 0u;
  }

  uint32_t leafCount = 0u;
  const auto refitNode = [&](const auto& self, uint32_t nodeIndex) -> bool {
    if (nodeIndex >= nodeCount) {
      return false;
    }
    const uint32_t base = nodeIndex * 8u;
    const uint32_t leftOrLeaf = as_u32(bvh[base + 6u]);
    const uint32_t rightOrCount = as_u32(bvh[base + 7u]);
    if ((leftOrLeaf & 0x80000000u) != 0u) {
      const uint32_t instanceIndex = leftOrLeaf & 0x7fffffffu;
      if (rightOrCount != 1u || instanceIndex >= instanceCount) {
        return false;
      }
      DynamicInstanceRef ref{};
      if (!LoadDynamicInstanceRefFromPacked(insts, instanceIndex, ref)) {
        return false;
      }
      bvh[base + 0u] = ref.bmin[0];
      bvh[base + 1u] = ref.bmin[1];
      bvh[base + 2u] = ref.bmin[2];
      bvh[base + 3u] = ref.bmax[0];
      bvh[base + 4u] = ref.bmax[1];
      bvh[base + 5u] = ref.bmax[2];
      ++leafCount;
      return true;
    }

    const uint32_t left = leftOrLeaf;
    const uint32_t right = rightOrCount;
    if (!self(self, left) || !self(self, right)) {
      return false;
    }
    const uint32_t leftBase = left * 8u;
    const uint32_t rightBase = right * 8u;
    bvh[base + 0u] = std::min(bvh[leftBase + 0u], bvh[rightBase + 0u]);
    bvh[base + 1u] = std::min(bvh[leftBase + 1u], bvh[rightBase + 1u]);
    bvh[base + 2u] = std::min(bvh[leftBase + 2u], bvh[rightBase + 2u]);
    bvh[base + 3u] = std::max(bvh[leftBase + 3u], bvh[rightBase + 3u]);
    bvh[base + 4u] = std::max(bvh[leftBase + 4u], bvh[rightBase + 4u]);
    bvh[base + 5u] = std::max(bvh[leftBase + 5u], bvh[rightBase + 5u]);
    return true;
  };

  if (!refitNode(refitNode, 0u)) {
    return 0u;
  }
  return leafCount;
}

}  // namespace vkpt::gpu

#endif  // PT_ENABLE_D3D12
