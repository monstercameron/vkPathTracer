#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "jobs/JobSystem.h"

namespace vkpt::cpu {

/// Axis-aligned bounds for one primitive or one BVH node.
struct BvhAabb {
  float min[3] = {0.f, 0.f, 0.f};
  float max[3] = {0.f, 0.f, 0.f};
};

/// Compact binary BVH node.
///
/// Internal nodes reference child indices in BvhBuildResult::nodes. Leaf nodes
/// reference a contiguous primitive range in BvhBuildResult::prim_indices.
struct BvhNode {
  BvhAabb aabb{};
  int32_t left_child = -1;   // index into nodes; -1 if leaf
  int32_t right_child = -1;
  int32_t first_prim = -1;   // index into prim_indices; -1 if internal
  int32_t prim_count = 0;    // 0 = internal node

  bool is_leaf() const { return prim_count > 0; }
};

/// Output from ParallelBvhBuilder::build.
struct BvhBuildResult {
  std::vector<BvhNode> nodes;
  std::vector<uint32_t> prim_indices;  // leaf primitive order
  std::size_t worker_count = 0;
  double build_ms = 0.0;
  bool deterministic = false;
};

/// Summary counters for the most recent BVH build.
struct BvhBuildStats {
  std::size_t node_count = 0;
  std::size_t leaf_count = 0;
  std::size_t prim_count = 0;
  double build_ms = 0.0;
  std::size_t worker_count = 0;
  bool deterministic = false;
};

/// Builds a binary BVH over primitive AABBs using midpoint centroid splits.
///
/// The builder owns no scene data; callers keep triangle/material payloads and
/// use prim_indices to map leaf ranges back to their payload arrays.
class ParallelBvhBuilder {
 public:
  // Primitives below this threshold form a leaf.
  static constexpr std::size_t kDefaultLeafThreshold = 4u;
  // Subtrees with more primitives than this threshold are split in parallel.
  static constexpr std::size_t kParallelThreshold = 256u;

  /// Build a BVH from a list of primitive AABBs.
  ///
  /// If a job system is provided, large subtrees may build concurrently. When
  /// deterministic is true, stable partitioning preserves primitive order for
  /// reproducible leaf contents.
  BvhBuildResult build(
      const std::vector<BvhAabb>& prim_aabbs,
      vkpt::jobs::IJobSystem* jobs = nullptr,
      bool deterministic = false,
      std::size_t leaf_threshold = kDefaultLeafThreshold);

  BvhBuildStats last_stats() const { return m_lastStats; }

 private:
  struct BuildContext {
    const std::vector<BvhAabb>* prim_aabbs = nullptr;
    std::vector<BvhNode>* nodes = nullptr;
    std::vector<uint32_t>* prim_indices = nullptr;
    std::atomic<int32_t>* node_alloc = nullptr;
    vkpt::jobs::IJobSystem* jobs = nullptr;
    bool deterministic = false;
    std::size_t leaf_threshold = kDefaultLeafThreshold;
  };

  static BvhAabb compute_aabb(
      const std::vector<BvhAabb>& prim_aabbs,
      const uint32_t* indices,
      std::size_t count);

  static BvhAabb compute_centroid_aabb(
      const std::vector<BvhAabb>& prim_aabbs,
      const uint32_t* indices,
      std::size_t count);

  static void build_node(
      const BuildContext& ctx,
      int32_t node_idx,
      uint32_t* indices,
      std::size_t count);

  BvhBuildStats m_lastStats{};
};

}  // namespace vkpt::cpu
