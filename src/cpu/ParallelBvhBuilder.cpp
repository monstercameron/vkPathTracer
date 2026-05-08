#include "cpu/ParallelBvhBuilder.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <numeric>

#include "core/log/Log.h"
#include "core/metrics/Metrics.h"

namespace vkpt::cpu {

// static
BvhAabb ParallelBvhBuilder::compute_aabb(
    const std::vector<BvhAabb>& prim_aabbs,
    const uint32_t* indices,
    std::size_t count) {
  BvhAabb out;
  for (int i = 0; i < 3; ++i) {
    out.min[i] = std::numeric_limits<float>::max();
    out.max[i] = -std::numeric_limits<float>::max();
  }
  for (std::size_t i = 0; i < count; ++i) {
    const auto& a = prim_aabbs[indices[i]];
    out.min[0] = std::min(out.min[0], a.min[0]);
    out.min[1] = std::min(out.min[1], a.min[1]);
    out.min[2] = std::min(out.min[2], a.min[2]);
    out.max[0] = std::max(out.max[0], a.max[0]);
    out.max[1] = std::max(out.max[1], a.max[1]);
    out.max[2] = std::max(out.max[2], a.max[2]);
  }
  return out;
}

// static
BvhAabb ParallelBvhBuilder::compute_centroid_aabb(
    const std::vector<BvhAabb>& prim_aabbs,
    const uint32_t* indices,
    std::size_t count) {
  BvhAabb out;
  for (int i = 0; i < 3; ++i) {
    out.min[i] = std::numeric_limits<float>::max();
    out.max[i] = -std::numeric_limits<float>::max();
  }
  for (std::size_t i = 0; i < count; ++i) {
    const auto& a = prim_aabbs[indices[i]];
    const float c0 = 0.5f * (a.min[0] + a.max[0]);
    const float c1 = 0.5f * (a.min[1] + a.max[1]);
    const float c2 = 0.5f * (a.min[2] + a.max[2]);
    out.min[0] = std::min(out.min[0], c0);
    out.min[1] = std::min(out.min[1], c1);
    out.min[2] = std::min(out.min[2], c2);
    out.max[0] = std::max(out.max[0], c0);
    out.max[1] = std::max(out.max[1], c1);
    out.max[2] = std::max(out.max[2], c2);
  }
  return out;
}

// static
void ParallelBvhBuilder::build_node(
    const BuildContext& ctx,
    int32_t node_idx,
    uint32_t* indices,
    std::size_t count) {

  const BvhAabb bounds = compute_aabb(*ctx.prim_aabbs, indices, count);

  // Each node_idx is unique per invocation, so this write does not need a lock.
  (*ctx.nodes)[static_cast<std::size_t>(node_idx)].aabb = bounds;

  if (count <= ctx.leaf_threshold) {
    // Determine where these primitives sit in the prim_indices array.
    // indices is already a subrange of ctx.prim_indices; compute offset.
    const int32_t first = static_cast<int32_t>(
        indices - ctx.prim_indices->data());

    auto& node = (*ctx.nodes)[static_cast<std::size_t>(node_idx)];
    node.left_child = -1;
    node.right_child = -1;
    node.first_prim = first;
    node.prim_count = static_cast<int32_t>(count);
    return;
  }

  // Split along the widest centroid extent; this is cheap to compute and keeps
  // construction deterministic when paired with stable_partition below.
  const BvhAabb centroid_bounds = compute_centroid_aabb(*ctx.prim_aabbs, indices, count);
  int best_axis = 0;
  float best_extent = centroid_bounds.max[0] - centroid_bounds.min[0];
  for (int k = 1; k < 3; ++k) {
    const float e = centroid_bounds.max[k] - centroid_bounds.min[k];
    if (e > best_extent) {
      best_extent = e;
      best_axis = k;
    }
  }

  // Midpoint split on the selected axis. Degenerate distributions are handled
  // after partitioning so all leaves continue to make progress.
  const float split = 0.5f * (centroid_bounds.min[best_axis] + centroid_bounds.max[best_axis]);

  // Partition in-place (stable for deterministic mode)
  const auto split_it = [&]() {
    if (ctx.deterministic) {
      // stable partition preserves relative ordering
      return std::stable_partition(indices, indices + count, [&](uint32_t idx) {
        const auto& a = (*ctx.prim_aabbs)[idx];
        const float c = 0.5f * (a.min[best_axis] + a.max[best_axis]);
        return c < split;
      });
    } else {
      return std::partition(indices, indices + count, [&](uint32_t idx) {
        const auto& a = (*ctx.prim_aabbs)[idx];
        const float c = 0.5f * (a.min[best_axis] + a.max[best_axis]);
        return c < split;
      });
    }
  }();

  const std::size_t left_count = static_cast<std::size_t>(split_it - indices);
  const std::size_t right_count = count - left_count;

  // Guard against degenerate splits where all centroids fall on one side.
  const std::size_t safe_left = (left_count == 0 || right_count == 0) ? count / 2 : left_count;
  const std::size_t safe_right = count - safe_left;

  // Allocate child slots atomically because the two recursive calls below may
  // run on different worker threads.
  const int32_t left_idx = ctx.node_alloc->fetch_add(1, std::memory_order_relaxed);
  const int32_t right_idx = ctx.node_alloc->fetch_add(1, std::memory_order_relaxed);

  auto& node = (*ctx.nodes)[static_cast<std::size_t>(node_idx)];
  node.left_child = left_idx;
  node.right_child = right_idx;
  node.first_prim = -1;
  node.prim_count = 0;

  const bool use_parallel = (ctx.jobs != nullptr) &&
      ctx.jobs->waiting_thread_runs_jobs() &&
      (count >= kParallelThreshold);

  if (use_parallel) {
    // Build sibling subtrees in parallel once the subtree is large enough to
    // amortize job overhead. The wait path can run work-stealing jobs.
    uint32_t* left_indices = indices;
    uint32_t* right_indices = indices + safe_left;
    const auto left_sz = safe_left;
    const auto right_sz = safe_right;

    auto left_handle = ctx.jobs->submit_job([&ctx, left_idx, left_indices, left_sz]() {
      build_node(ctx, left_idx, left_indices, left_sz);
    });
    auto right_handle = ctx.jobs->submit_job([&ctx, right_idx, right_indices, right_sz]() {
      build_node(ctx, right_idx, right_indices, right_sz);
    });
    (void)ctx.jobs->wait(left_handle);
    (void)ctx.jobs->wait(right_handle);
  } else {
    build_node(ctx, left_idx, indices, safe_left);
    build_node(ctx, right_idx, indices + safe_left, safe_right);
  }
}

BvhBuildResult ParallelBvhBuilder::build(
    const std::vector<BvhAabb>& prim_aabbs,
    vkpt::jobs::IJobSystem* jobs,
    bool deterministic,
    std::size_t leaf_threshold) {

  const auto t0 = std::chrono::high_resolution_clock::now();

  BvhBuildResult result;
  result.deterministic = deterministic;
  result.worker_count = jobs ? jobs->worker_count() : 0u;

  if (prim_aabbs.empty()) {
    const auto t1 = std::chrono::high_resolution_clock::now();
    const auto build_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    m_lastStats = BvhBuildStats{};
    m_lastStats.build_ms = static_cast<double>(build_us) / 1000.0;
    m_lastStats.worker_count = result.worker_count;
    m_lastStats.deterministic = deterministic;
    VKP_METRIC_OBSERVE("vkp.cpu.bvh_build_us",
                       build_us < 0 ? 0u : static_cast<std::uint64_t>(build_us));
    VKP_LOG(Info,
            "bvh",
            "build_completed",
            "node_count",
            static_cast<std::uint64_t>(m_lastStats.node_count),
            "prim_count",
            static_cast<std::uint64_t>(m_lastStats.prim_count),
            "worker_count",
            static_cast<std::uint64_t>(m_lastStats.worker_count));
    return result;
  }

  const std::size_t n = prim_aabbs.size();
  // Maximum nodes in a binary tree with n leaves is 2n-1.
  const std::size_t max_nodes = 2u * n - 1u;

  result.nodes.resize(max_nodes);
  result.prim_indices.resize(n);
  std::iota(result.prim_indices.begin(), result.prim_indices.end(), 0u);

  std::atomic<int32_t> node_alloc{1};  // root is at index 0

  BuildContext ctx;
  ctx.prim_aabbs = &prim_aabbs;
  ctx.nodes = &result.nodes;
  ctx.prim_indices = &result.prim_indices;
  ctx.node_alloc = &node_alloc;
  ctx.jobs = jobs;
  ctx.deterministic = deterministic;
  ctx.leaf_threshold = (leaf_threshold == 0u) ? kDefaultLeafThreshold : leaf_threshold;

  build_node(ctx, 0, result.prim_indices.data(), n);

  // Trim unused node slots
  const std::size_t actual_nodes = static_cast<std::size_t>(node_alloc.load(std::memory_order_relaxed));
  result.nodes.resize(std::min(actual_nodes, max_nodes));

  const auto t1 = std::chrono::high_resolution_clock::now();
  const double build_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
  result.build_ms = build_ms;
  VKP_METRIC_OBSERVE("vkp.cpu.bvh_build_us",
                     static_cast<std::uint64_t>(build_ms * 1000.0));

  // Compute stats
  std::size_t leaf_count = 0;
  for (const auto& node : result.nodes) {
    if (node.is_leaf()) {
      ++leaf_count;
    }
  }

  m_lastStats.node_count = result.nodes.size();
  m_lastStats.leaf_count = leaf_count;
  m_lastStats.prim_count = n;
  m_lastStats.build_ms = build_ms;
  m_lastStats.worker_count = result.worker_count;
  m_lastStats.deterministic = deterministic;

  VKP_LOG(Info,
          "bvh",
          "build_completed",
          "node_count",
          static_cast<std::uint64_t>(m_lastStats.node_count),
          "prim_count",
          static_cast<std::uint64_t>(m_lastStats.prim_count),
          "worker_count",
          static_cast<std::uint64_t>(m_lastStats.worker_count));

  return result;
}

}  // namespace vkpt::cpu
