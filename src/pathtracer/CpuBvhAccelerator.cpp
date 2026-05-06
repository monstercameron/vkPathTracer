#include "pathtracer/RayAccelerator.h"

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
  bool build(const vkpt::pathtracer::RTSceneData& scene, bool deterministic) override {
    reset();
    m_vertices = scene.vertices;
    m_info.deterministic = deterministic;

    std::vector<vkpt::cpu::BvhAabb> primitive_aabbs;
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
        if (tri.i0 >= m_vertices.size() || tri.i1 >= m_vertices.size() || tri.i2 >= m_vertices.size()) {
          continue;
        }

        const auto& v0 = m_vertices[tri.i0];
        const auto& v1 = m_vertices[tri.i1];
        const auto& v2 = m_vertices[tri.i2];
        vkpt::cpu::BvhAabb aabb{};
        aabb.min[0] = std::min({v0.x, v1.x, v2.x}) - kEpsilon;
        aabb.min[1] = std::min({v0.y, v1.y, v2.y}) - kEpsilon;
        aabb.min[2] = std::min({v0.z, v1.z, v2.z}) - kEpsilon;
        aabb.max[0] = std::max({v0.x, v1.x, v2.x}) + kEpsilon;
        aabb.max[1] = std::max({v0.y, v1.y, v2.y}) + kEpsilon;
        aabb.max[2] = std::max({v0.z, v1.z, v2.z}) + kEpsilon;

        const auto e1 = sub_vec3(v1, v0);
        const auto e2 = sub_vec3(v2, v0);
        m_primitives.push_back({tri,
                                instance.material_index,
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

    m_bvh = m_builder.build(primitive_aabbs, nullptr, deterministic);
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
    std::vector<int32_t> stack;
    stack.reserve(64u);
    stack.push_back(0);

    while (!stack.empty()) {
      const int32_t node_index = stack.back();
      stack.pop_back();
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
        for (int32_t i = 0; i < node.prim_count; ++i) {
          const int32_t ordered_index = node.first_prim + i;
          if (ordered_index < 0 || static_cast<std::size_t>(ordered_index) >= m_bvh.prim_indices.size()) {
            continue;
          }
          const uint32_t primitive_index = m_bvh.prim_indices[static_cast<std::size_t>(ordered_index)];
          if (primitive_index >= m_primitives.size()) {
            continue;
          }
          if (stats) {
            ++stats->triangle_tests;
          }
          const auto& primitive = m_primitives[primitive_index];
          const Vec3 h = cross_vec3(ray.direction, primitive.e2);
          const float det = dot_vec3(primitive.e1, h);
          if (std::fabs(det) < kEpsilon) {
            continue;
          }
          const float inv_det = 1.0f / det;
          const Vec3 s = sub_vec3(ray.origin, primitive.v0);
          const float u = dot_vec3(s, h) * inv_det;
          if (u < 0.0f || u > 1.0f) {
            continue;
          }
          const Vec3 q = cross_vec3(s, primitive.e1);
          const float v = dot_vec3(ray.direction, q) * inv_det;
          if (v < 0.0f || u + v > 1.0f) {
            continue;
          }
          const float t = dot_vec3(primitive.e2, q) * inv_det;
          if (t <= kEpsilon || t >= best_t) {
            continue;
          }
          if (stats) {
            ++stats->triangle_hits;
          }
          best_t = t;
          out.hit = true;
          out.t = t;
          out.position = add_vec3(ray.origin, mul_vec3(ray.direction, t));
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
          stack.push_back(node.right_child);
          stack.push_back(node.left_child);
        } else {
          stack.push_back(node.left_child);
          stack.push_back(node.right_child);
        }
      } else if (hit_left) {
        stack.push_back(node.left_child);
      } else if (hit_right) {
        stack.push_back(node.right_child);
      }
    }

    return out.hit;
  }

  vkpt::pathtracer::RayAcceleratorBuildInfo build_info() const override {
    return m_info;
  }

  void reset() override {
    m_vertices.clear();
    m_primitives.clear();
    m_bvh = {};
    m_info = {};
  }

 private:
  struct Primitive {
    vkpt::pathtracer::RTTriangle triangle{};
    uint32_t material_index = 0;
    uint32_t primitive_index = 0;
    Vec3 v0{};
    Vec3 e1{};
    Vec3 e2{};
    Vec3 normal{};
  };

  std::vector<Vec3> m_vertices;
  std::vector<Primitive> m_primitives;
  vkpt::cpu::ParallelBvhBuilder m_builder;
  vkpt::cpu::BvhBuildResult m_bvh;
  vkpt::pathtracer::RayAcceleratorBuildInfo m_info{};
};

}  // namespace

namespace vkpt::pathtracer {

std::unique_ptr<IRayAccelerator> CreateCpuBvhAccelerator() {
  return std::make_unique<CpuBvhAccelerator>();
}

}  // namespace vkpt::pathtracer
