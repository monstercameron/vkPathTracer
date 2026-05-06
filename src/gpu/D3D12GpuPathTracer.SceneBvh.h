#pragma once

#include <cstdint>
#include <vector>

namespace vkpt::gpu {

struct BvhTriRef {
  uint32_t orig_idx;
  float centroid[3];
  float bmin[3];
  float bmax[3];
};

struct BvhBuildConfig {
  uint32_t leaf_size = 4u;
  uint32_t bucket_count = 8u;
  bool use_median_split = false;
};

uint32_t bvh_build(std::vector<BvhTriRef>& refs,
                   uint32_t start,
                   uint32_t count,
                   const std::vector<uint32_t>& orig_idx,
                   const std::vector<uint32_t>& orig_tri_mat,
                   const BvhBuildConfig& config,
                   std::vector<float>& gpu_bvh,
                   std::vector<uint32_t>& reord_idx,
                   std::vector<uint32_t>& reord_tri_mat,
                   uint32_t& node_count);

uint32_t BuildDynamicInstanceBvhFromPackedInstances(const std::vector<uint32_t>& insts,
                                                    uint32_t instanceCount,
                                                    std::vector<float>& outBvh);

uint32_t RefitDynamicInstanceBvhFromPackedInstances(const std::vector<uint32_t>& insts,
                                                    uint32_t instanceCount,
                                                    std::vector<float>& bvh);

}  // namespace vkpt::gpu
