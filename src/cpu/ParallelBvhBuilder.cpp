#include "cpu/ParallelBvhBuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace vkpt::cpu {

namespace {

float aabb_surface_area(const BvhAabb& a) {
  const float dx = a.max[0] - a.min[0];
  const float dy = a.max[1] - a.min[1];
  const float dz = a.max[2] - a.min[2];
  return 2.0f * (dx * dy + dy * dz + dz * dx);
}

BvhAabb merge_aabb(const BvhAabb& a, const BvhAabb& b) {
  BvhAabb out;
  for (int i = 0; i < 3; ++i) {
    out.min[i] = std::min(a.min[i], b.min[i]);
    out.max[i] = std::max(a.max[i], b.max[i]);
  }
  return out;
}

}  // namespace

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
    for (int k = 0; k < 3; ++k) {
      out.min[k] = std::min(out.min[k], a.min[k]);
      out.max[k] = std::max(out.max[k], a.max[k]);
    }
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
    for (int k = 0; k < 3; ++k) {
      const float c = 0.5f * (a.min[k] + a.max[k]);
      out.min[k] = std::min(out.min[k], c);
      out.max[k] = std::max(out.max[k], c);
    }
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

  // Write node aabb (each node_idx is unique per invocation — no lock needed)
  {
    std::scoped_lock lock(*ctx.nodes_mutex);
    (*ctx.nodes)[static_cast<std::size_t>(node_idx)].aabb = bounds;
  }

  if (count <= ctx.leaf_threshold) {
    // Determine where these primitives sit in the prim_indices array.
    // indices is already a subrange of ctx.prim_indices — compute offset.
    const int32_t first = static_cast<int32_t>(
        indices - ctx.prim_indices->data());

    std::scoped_lock lock(*ctx.nodes_mutex);
    auto& node = (*ctx.nodes)[static_cast<std::size_t>(node_idx)];
    node.left_child = -1;
    node.right_child = -1;
    node.first_prim = first;
    node.prim_count = static_cast<int32_t>(count);
    return;
  }

  // Find longest axis of centroid AABB to split on
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

  // Midpoint split on the best axis
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

  // Guard against degenerate splits (all on same side)
  const std::size_t safe_left = (left_count == 0 || right_count == 0) ? count / 2 : left_count;
  const std::size_t safe_right = count - safe_left;

  // Allocate child node slots atomically
  const int32_t left_idx = ctx.node_alloc->fetch_add(1, std::memory_order_relaxed);
  const int32_t right_idx = ctx.node_alloc->fetch_add(1, std::memory_order_relaxed);

  {
    std::scoped_lock lock(*ctx.nodes_mutex);
    auto& node = (*ctx.nodes)[static_cast<std::size_t>(node_idx)];
    node.left_child = left_idx;
    node.right_child = right_idx;
    node.first_prim = -1;
    node.prim_count = 0;
  }

  const bool use_parallel = (ctx.jobs != nullptr) && (count >= kParallelThreshold);

  if (use_parallel) {
    // Submit left and right as parallel jobs, then wait
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
    ctx.jobs->wait_group({left_handle, right_handle});
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
    m_lastStats = BvhBuildStats{};
    return result;
  }

  const std::size_t n = prim_aabbs.size();
  // Maximum nodes in a binary tree with n leaves is 2n-1
  const std::size_t max_nodes = 2u * n;

  result.nodes.resize(max_nodes);
  result.prim_indices.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    result.prim_indices[i] = static_cast<uint32_t>(i);
  }

  std::atomic<int32_t> node_alloc{1};  // root is at index 0
  std::mutex nodes_mutex;

  BuildContext ctx;
  ctx.prim_aabbs = &prim_aabbs;
  ctx.nodes = &result.nodes;
  ctx.prim_indices = &result.prim_indices;
  ctx.node_alloc = &node_alloc;
  ctx.nodes_mutex = &nodes_mutex;
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

  return result;
}

}  // namespace vkpt::cpu
