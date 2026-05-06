#include "pathtracer/RayAccelerator.h"

#include "cpu/BvhTriangleIntersector.h"
#include "cpu/CpuFeatures.h"
#include "cpu/ParallelBvhBuilder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace {

constexpr float kEpsilon = 1e-4f;

using vkpt::pathtracer::Ray;
using vkpt::pathtracer::Vec3;

Vec3 add_vec3(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 sub_vec3(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 mul_vec3(const Vec3& lhs, float rhs) {
  return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

float dot_vec3(const Vec3& lhs, const Vec3& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 cross_vec3(const Vec3& lhs, const Vec3& rhs) {
  return {
      lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.z * rhs.x - lhs.x * rhs.z,
      lhs.x * rhs.y - lhs.y * rhs.x,
  };
}

Vec3 normalize_vec3(const Vec3& value) {
  const float len_sq = dot_vec3(value, value);
  if (len_sq <= kEpsilon * kEpsilon) {
    return {0.0f, 1.0f, 0.0f};
  }
  return mul_vec3(value, 1.0f / std::sqrt(len_sq));
}

struct RayAabbQuery {
  std::array<float, 3> origin{};
  std::array<float, 3> inv_direction{};
  std::array<bool, 3> parallel{};
};

RayAabbQuery make_ray_aabb_query(const Ray& ray) {
  RayAabbQuery query{};
  const std::array<float, 3> origin{ray.origin.x, ray.origin.y, ray.origin.z};
  const std::array<float, 3> direction{ray.direction.x, ray.direction.y, ray.direction.z};
  query.origin = origin;
  for (std::size_t axis = 0; axis < 3u; ++axis) {
    query.parallel[axis] = std::fabs(direction[axis]) <= kEpsilon;
    query.inv_direction[axis] = query.parallel[axis] ? 0.0f : 1.0f / direction[axis];
  }
  return query;
}

bool intersect_aabb(const vkpt::cpu::BvhAabb& aabb,
                    const RayAabbQuery& ray,
                    float max_t,
                    float& t_near) {
  // Slab intersection with precomputed reciprocal direction. max_t is the
  // current closest triangle hit, so whole subtrees beyond that distance can be
  // rejected before visiting leaves.
  float t_min = kEpsilon;
  float t_max = max_t;
  for (std::size_t axis = 0; axis < 3u; ++axis) {
    if (ray.parallel[axis]) {
      if (ray.origin[axis] < aabb.min[axis] || ray.origin[axis] > aabb.max[axis]) {
        return false;
      }
      continue;
    }
    float t0 = (aabb.min[axis] - ray.origin[axis]) * ray.inv_direction[axis];
    float t1 = (aabb.max[axis] - ray.origin[axis]) * ray.inv_direction[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    t_min = std::max(t_min, t0);
    t_max = std::min(t_max, t1);
    if (t_min > t_max) {
      return false;
    }
  }
  t_near = t_min;
  return t_max > kEpsilon;
}

class CpuBvhAccelerator final : public vkpt::pathtracer::IRayAccelerator {
 public:
  CpuBvhAccelerator()
      : m_triangle_intersector(vkpt::cpu::SelectBvhTriangleIntersectorMode(vkpt::cpu::QueryCpuFeatures())) {}

  bool build(const vkpt::pathtracer::RTSceneData& scene, bool deterministic) override {
    reset();
    m_info.deterministic = deterministic;
    const auto& vertices = scene.vertices;

    std::size_t triangle_capacity = 0u;
    for (const auto& instance : scene.instances) {
      triangle_capacity += instance.triangle_count;
    }
    std::vector<vkpt::cpu::BvhAabb> primitive_aabbs;
    primitive_aabbs.reserve(triangle_capacity);
    m_primitives.reserve(triangle_capacity);
    // Flatten render instances into one primitive array. The BVH stores only
    // primitive indices; material and edge data stay in m_primitives so leaves
    // can execute Moller-Trumbore tests without chasing scene instance ranges.
    for (const auto& instance : scene.instances) {
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t base_tri = (instance.first_triangle + triangle) * 3u;
        if (base_tri + 2u >= scene.indices.size()) {
          continue;
        }
        const vkpt::pathtracer::RTTriangle tri{
            scene.indices[base_tri + 0u],
            scene.indices[base_tri + 1u],
            scene.indices[base_tri + 2u],
        };
        if (tri.i0 >= vertices.size() || tri.i1 >= vertices.size() || tri.i2 >= vertices.size()) {
          continue;
        }

        const auto& v0 = vertices[tri.i0];
        const auto& v1 = vertices[tri.i1];
        const auto& v2 = vertices[tri.i2];
        vkpt::cpu::BvhAabb aabb{};
        aabb.min[0] = std::min({v0.x, v1.x, v2.x}) - kEpsilon;
        aabb.min[1] = std::min({v0.y, v1.y, v2.y}) - kEpsilon;
        aabb.min[2] = std::min({v0.z, v1.z, v2.z}) - kEpsilon;
        aabb.max[0] = std::max({v0.x, v1.x, v2.x}) + kEpsilon;
        aabb.max[1] = std::max({v0.y, v1.y, v2.y}) + kEpsilon;
        aabb.max[2] = std::max({v0.z, v1.z, v2.z}) + kEpsilon;

        const auto e1 = sub_vec3(v1, v0);
        const auto e2 = sub_vec3(v2, v0);
        m_primitives.push_back({instance.material_index,
                                static_cast<uint32_t>(m_primitives.size()),
                                v0,
                                e1,
                                e2,
                                normalize_vec3(cross_vec3(e1, e2))});
        primitive_aabbs.push_back(aabb);
      }
    }

    m_info.primitive_count = m_primitives.size();
    if (m_primitives.empty()) {
      m_info.built = true;
      return true;
    }

    const std::size_t leaf_threshold =
        m_triangle_intersector == vkpt::cpu::BvhTriangleIntersectorMode::X86Avx2
            ? vkpt::cpu::kBvhTriangleBatchWidth
            : vkpt::cpu::ParallelBvhBuilder::kDefaultLeafThreshold;
    m_bvh = m_builder.build(primitive_aabbs, nullptr, deterministic, leaf_threshold);
    const auto stats = m_builder.last_stats();
    m_info.built = true;
    m_info.node_count = stats.node_count;
    m_info.leaf_count = stats.leaf_count;
    m_info.build_ms = stats.build_ms;
    return !m_bvh.nodes.empty();
  }

  bool intersect(const Ray& ray,
                 vkpt::pathtracer::RayQueryHit& out,
                 vkpt::pathtracer::RayQueryStats* stats = nullptr) const override {
    out = {};
    if (m_bvh.nodes.empty() || m_primitives.empty()) {
      return false;
    }

    float best_t = std::numeric_limits<float>::infinity();
    const RayAabbQuery aabb_ray = make_ray_aabb_query(ray);
    std::array<int32_t, 128u> stack{};
    std::vector<int32_t> overflow_stack;
    std::size_t stack_size = 0u;
    stack[stack_size++] = 0;
    auto push_node = [&](int32_t child) {
      if (stack_size < stack.size()) {
        stack[stack_size++] = child;
      } else {
        overflow_stack.push_back(child);
      }
    };

    while (stack_size > 0u || !overflow_stack.empty()) {
      // Depth-first traversal keeps a small fixed stack hot for normal trees
      // and spills only unusually deep cases to the vector. Children are pushed
      // far-first so the nearer child is tested next and can tighten best_t.
      const int32_t node_index = stack_size > 0u
          ? stack[--stack_size]
          : [&]() {
              const int32_t value = overflow_stack.back();
              overflow_stack.pop_back();
              return value;
            }();
      if (node_index < 0 || static_cast<std::size_t>(node_index) >= m_bvh.nodes.size()) {
        continue;
      }

      const auto& node = m_bvh.nodes[static_cast<std::size_t>(node_index)];
      if (stats) {
        ++stats->bvh_node_visits;
      }
      float node_t = 0.0f;
      if (!intersect_aabb(node.aabb, aabb_ray, best_t, node_t)) {
        continue;
      }

      if (node.is_leaf()) {
        if (stats) {
          ++stats->bvh_leaf_visits;
        }
        const vkpt::cpu::BvhTriangleRay triangle_ray{
            ray.origin.x, ray.origin.y, ray.origin.z,
            ray.direction.x, ray.direction.y, ray.direction.z};
        for (int32_t i = 0; i < node.prim_count;) {
          vkpt::cpu::BvhTriangleBatch batch{};
          std::array<uint32_t, vkpt::cpu::kBvhTriangleBatchWidth> primitive_lanes{};
          while (i < node.prim_count && batch.count < vkpt::cpu::kBvhTriangleBatchWidth) {
            const int32_t ordered_index = node.first_prim + i;
            ++i;
            if (ordered_index < 0 || static_cast<std::size_t>(ordered_index) >= m_bvh.prim_indices.size()) {
              continue;
            }
            const uint32_t primitive_index = m_bvh.prim_indices[static_cast<std::size_t>(ordered_index)];
            if (primitive_index >= m_primitives.size()) {
              continue;
            }
            const std::uint32_t lane = batch.count++;
            primitive_lanes[lane] = primitive_index;
            write_triangle_lane(batch, lane, m_primitives[primitive_index]);
          }
          if (batch.count == 0u) {
            continue;
          }
          if (stats) {
            stats->triangle_tests += batch.count;
          }
          const auto hit = vkpt::cpu::IntersectBvhTriangleBatch(m_triangle_intersector, triangle_ray, batch, best_t);
          if (!hit.hit) {
            continue;
          }
          if (stats) {
            stats->triangle_hits += hit.accepted_hits;
          }
          const auto& primitive = m_primitives[primitive_lanes[hit.lane]];
          best_t = hit.t;
          out.hit = true;
          out.t = hit.t;
          out.position = add_vec3(ray.origin, mul_vec3(ray.direction, hit.t));
          out.normal = primitive.normal;
          out.material_index = primitive.material_index;
          out.primitive_index = primitive.primitive_index;
        }
        continue;
      }

      float left_t = 0.0f;
      float right_t = 0.0f;
      const bool hit_left = node.left_child >= 0 &&
          intersect_aabb(m_bvh.nodes[static_cast<std::size_t>(node.left_child)].aabb, aabb_ray, best_t, left_t);
      const bool hit_right = node.right_child >= 0 &&
          intersect_aabb(m_bvh.nodes[static_cast<std::size_t>(node.right_child)].aabb, aabb_ray, best_t, right_t);
      if (hit_left && hit_right) {
        if (left_t <= right_t) {
          push_node(node.right_child);
          push_node(node.left_child);
        } else {
          push_node(node.left_child);
          push_node(node.right_child);
        }
      } else if (hit_left) {
        push_node(node.left_child);
      } else if (hit_right) {
        push_node(node.right_child);
      }
    }

    return out.hit;
  }

  vkpt::pathtracer::RayAcceleratorBuildInfo build_info() const override {
    return m_info;
  }

  void reset() override {
    m_primitives.clear();
    m_bvh = {};
    m_info = {};
  }

 private:
  struct Primitive {
    uint32_t material_index = 0;
    uint32_t primitive_index = 0;
    Vec3 v0{};
    Vec3 e1{};
    Vec3 e2{};
    Vec3 normal{};
  };

  static void write_triangle_lane(vkpt::cpu::BvhTriangleBatch& batch,
                                  std::uint32_t lane,
                                  const Primitive& primitive) {
    batch.v0x[lane] = primitive.v0.x;
    batch.v0y[lane] = primitive.v0.y;
    batch.v0z[lane] = primitive.v0.z;
    batch.e1x[lane] = primitive.e1.x;
    batch.e1y[lane] = primitive.e1.y;
    batch.e1z[lane] = primitive.e1.z;
    batch.e2x[lane] = primitive.e2.x;
    batch.e2y[lane] = primitive.e2.y;
    batch.e2z[lane] = primitive.e2.z;
  }

  std::vector<Primitive> m_primitives;
  vkpt::cpu::ParallelBvhBuilder m_builder;
  vkpt::cpu::BvhBuildResult m_bvh;
  vkpt::pathtracer::RayAcceleratorBuildInfo m_info{};
  vkpt::cpu::BvhTriangleIntersectorMode m_triangle_intersector = vkpt::cpu::BvhTriangleIntersectorMode::Scalar;
};

}  // namespace

namespace vkpt::pathtracer {

std::unique_ptr<IRayAccelerator> CreateCpuBvhAccelerator() {
  return std::make_unique<CpuBvhAccelerator>();
}

}  // namespace vkpt::pathtracer
