#include "pathtracer/RayAccelerator.h"

#include "cpu/BvhTriangleIntersector.h"
#include "cpu/CpuFeatures.h"
#include "cpu/ParallelBvhBuilder.h"

#include <algorithm>
#include <array>
#include <chrono>
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

Vec3 rotate_quat(const Vec3& value,
                 const vkpt::pathtracer::Quat4& rotation) {
  const float len_sq = rotation.x * rotation.x +
                       rotation.y * rotation.y +
                       rotation.z * rotation.z +
                       rotation.w * rotation.w;
  if (len_sq <= kEpsilon * kEpsilon) {
    return value;
  }
  const float inv_len = 1.0f / std::sqrt(len_sq);
  const Vec3 qv{
      rotation.x * inv_len,
      rotation.y * inv_len,
      rotation.z * inv_len};
  const float qw = rotation.w * inv_len;
  const auto t = mul_vec3(cross_vec3(qv, value), 2.0f);
  return add_vec3(add_vec3(value, mul_vec3(t, qw)), cross_vec3(qv, t));
}

Vec3 transform_instance_vertex(const Vec3& local,
                               const vkpt::pathtracer::RTInstance& instance) {
  const Vec3 scaled{
      local.x * instance.scale.x,
      local.y * instance.scale.y,
      local.z * instance.scale.z};
  return add_vec3(rotate_quat(scaled, instance.rotation), instance.translation);
}

Vec3 normalize_vec3(const Vec3& value) {
  const float len_sq = dot_vec3(value, value);
  if (len_sq <= kEpsilon * kEpsilon) {
    return {0.0f, 1.0f, 0.0f};
  }
  return mul_vec3(value, 1.0f / std::sqrt(len_sq));
}

vkpt::cpu::BvhAabb make_triangle_aabb(const Vec3& v0,
                                      const Vec3& v1,
                                      const Vec3& v2) {
  vkpt::cpu::BvhAabb aabb{};
  aabb.min[0] = std::min({v0.x, v1.x, v2.x}) - kEpsilon;
  aabb.min[1] = std::min({v0.y, v1.y, v2.y}) - kEpsilon;
  aabb.min[2] = std::min({v0.z, v1.z, v2.z}) - kEpsilon;
  aabb.max[0] = std::max({v0.x, v1.x, v2.x}) + kEpsilon;
  aabb.max[1] = std::max({v0.y, v1.y, v2.y}) + kEpsilon;
  aabb.max[2] = std::max({v0.z, v1.z, v2.z}) + kEpsilon;
  return aabb;
}

vkpt::cpu::BvhAabb union_aabb(const vkpt::cpu::BvhAabb& lhs,
                              const vkpt::cpu::BvhAabb& rhs) {
  vkpt::cpu::BvhAabb out{};
  for (std::size_t axis = 0; axis < 3u; ++axis) {
    out.min[axis] = std::min(lhs.min[axis], rhs.min[axis]);
    out.max[axis] = std::max(lhs.max[axis], rhs.max[axis]);
  }
  return out;
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

  bool build(const vkpt::pathtracer::PathTracerSceneSnapshot& scene, bool deterministic) override {
    reset();
    m_info.deterministic = deterministic;
    const auto& vertices = scene.vertices;
    m_instance_ranges.resize(scene.instances.size());

    std::size_t triangle_capacity = 0u;
    for (const auto& instance : scene.instances) {
      triangle_capacity += instance.triangle_count;
    }
    m_primitive_aabbs.reserve(triangle_capacity);
    m_primitives.reserve(triangle_capacity);
    // Flatten render instances into one primitive array. The BVH stores only
    // primitive indices; material and edge data stay in m_primitives so leaves
    // can execute Moller-Trumbore tests without chasing scene instance ranges.
    for (std::size_t instance_index = 0u; instance_index < scene.instances.size(); ++instance_index) {
      const auto& instance = scene.instances[instance_index];
      const std::uint32_t first_primitive = static_cast<std::uint32_t>(m_primitives.size());
      const bool use_local_dynamic_geometry =
          (instance.flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) != 0u &&
          instance.local_vertex_count > 0u &&
          instance.local_index_count > 0u;
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        Vec3 v0{};
        Vec3 v1{};
        Vec3 v2{};
        bool triangle_loaded = false;
        if (use_local_dynamic_geometry) {
          const std::uint32_t local_base = instance.local_first_index + triangle * 3u;
          const std::uint32_t local_end = instance.local_first_vertex + instance.local_vertex_count;
          if (local_base + 2u < scene.local_indices.size()) {
            const std::uint32_t li0 = scene.local_indices[local_base + 0u];
            const std::uint32_t li1 = scene.local_indices[local_base + 1u];
            const std::uint32_t li2 = scene.local_indices[local_base + 2u];
            if (instance.local_first_vertex + li0 < local_end &&
                instance.local_first_vertex + li1 < local_end &&
                instance.local_first_vertex + li2 < local_end &&
                instance.local_first_vertex + li0 < scene.local_vertices.size() &&
                instance.local_first_vertex + li1 < scene.local_vertices.size() &&
                instance.local_first_vertex + li2 < scene.local_vertices.size()) {
              v0 = transform_instance_vertex(
                  scene.local_vertices[instance.local_first_vertex + li0], instance);
              v1 = transform_instance_vertex(
                  scene.local_vertices[instance.local_first_vertex + li1], instance);
              v2 = transform_instance_vertex(
                  scene.local_vertices[instance.local_first_vertex + li2], instance);
              triangle_loaded = true;
            }
          }
        }
        if (!triangle_loaded) {
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
          v0 = vertices[tri.i0];
          v1 = vertices[tri.i1];
          v2 = vertices[tri.i2];
        }
        const auto aabb = make_triangle_aabb(v0, v1, v2);

        const auto e1 = sub_vec3(v1, v0);
        const auto e2 = sub_vec3(v2, v0);
        m_primitives.push_back({instance.material_index,
                                static_cast<uint32_t>(m_primitives.size()),
                                static_cast<uint32_t>(instance_index),
                                v0,
                                e1,
                                e2,
                                normalize_vec3(cross_vec3(e1, e2))});
        m_primitive_aabbs.push_back(aabb);
      }
      m_instance_ranges[instance_index] = {
          first_primitive,
          static_cast<std::uint32_t>(m_primitives.size() - first_primitive)};
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
    m_bvh = m_builder.build(m_primitive_aabbs, nullptr, deterministic, leaf_threshold);
    const auto stats = m_builder.last_stats();
    m_info.built = true;
    m_info.node_count = stats.node_count;
    m_info.leaf_count = stats.leaf_count;
    m_info.build_ms = stats.build_ms;
    return !m_bvh.nodes.empty();
  }

  vkpt::pathtracer::InstanceTransformPlan plan_instance_transform_update(
      const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& /*options*/) const override {
    if (!m_info.built || m_bvh.nodes.empty()) {
      return {
          vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
          static_cast<std::uint32_t>(updates.size()),
          0u,
          "CPU BVH is not built"};
    }

    std::uint32_t matched = 0u;
    for (const auto& update : updates) {
      std::uint32_t instance_index = 0u;
      if (!resolve_instance_index(scene, update, instance_index)) {
        return {
            vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
            static_cast<std::uint32_t>(updates.size()),
            matched,
            "CPU BVH transform update references an unknown instance"};
      }
      const auto& instance = scene.instances[instance_index];
      if ((instance.flags & vkpt::pathtracer::kRTInstanceFlagDynamicTransform) == 0u ||
          instance.local_vertex_count == 0u ||
          instance.local_index_count == 0u ||
          instance_index >= m_instance_ranges.size()) {
        return {
            vkpt::pathtracer::InstanceTransformUpdateStatus::BlockedNeedsFullStaticAccelRebuild,
            static_cast<std::uint32_t>(updates.size()),
            matched,
            "CPU BVH transform update needs dynamic local geometry metadata"};
      }
      ++matched;
    }

    return {
        vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate,
        static_cast<std::uint32_t>(updates.size()),
        matched,
        "CPU BVH can refit dynamic instance transforms"};
  }

  vkpt::pathtracer::InstanceTransformUpdateResult apply_instance_transform_update(
      const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
      std::span<const vkpt::pathtracer::RTInstanceTransformUpdate> updates,
      const vkpt::pathtracer::InstanceTransformUpdateOptions& options) override {
    const auto plan = plan_instance_transform_update(scene, updates, options);
    if (!plan.can_apply_without_full_fallback()) {
      return {
          plan.status,
          plan.requested_count,
          0u,
          0.0,
          0.0,
          0.0,
          0.0,
          plan.message};
    }

    const auto start = std::chrono::steady_clock::now();
    std::uint32_t applied = 0u;
    for (const auto& update : updates) {
      std::uint32_t instance_index = 0u;
      if (!resolve_instance_index(scene, update, instance_index) ||
          !refit_instance_primitives(scene, instance_index, update)) {
        return {
            vkpt::pathtracer::InstanceTransformUpdateStatus::Failed,
            static_cast<std::uint32_t>(updates.size()),
            applied,
            0.0,
            0.0,
            0.0,
            0.0,
            "CPU BVH failed while refitting instance primitives"};
      }
      ++applied;
    }
    if (!m_bvh.nodes.empty()) {
      (void)refit_node(0);
    }
    const auto end = std::chrono::steady_clock::now();
    const double refit_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
    return {
        vkpt::pathtracer::InstanceTransformUpdateStatus::AppliedDynamicAccelUpdate,
        static_cast<std::uint32_t>(updates.size()),
        applied,
        0.0,
        0.0,
        refit_ms,
        0.0,
        "CPU BVH refit dynamic instance transforms"};
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
    m_primitive_aabbs.clear();
    m_instance_ranges.clear();
    m_bvh = {};
    m_info = {};
  }

 private:
  struct PrimitiveRange {
    std::uint32_t first = 0u;
    std::uint32_t count = 0u;
  };

  struct Primitive {
    uint32_t material_index = 0;
    uint32_t primitive_index = 0;
    uint32_t instance_index = 0;
    Vec3 v0{};
    Vec3 e1{};
    Vec3 e2{};
    Vec3 normal{};
  };

  static bool resolve_instance_index(
      const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
      const vkpt::pathtracer::RTInstanceTransformUpdate& update,
      std::uint32_t& out) {
    std::uint32_t instance_index = update.instance_index;
    if (instance_index >= scene.instances.size() && update.entity_id != 0u) {
      for (std::size_t index = 0u; index < scene.instances.size(); ++index) {
        if (scene.instances[index].entity_id == update.entity_id) {
          instance_index = static_cast<std::uint32_t>(index);
          break;
        }
      }
    }
    if (instance_index >= scene.instances.size()) {
      return false;
    }
    out = instance_index;
    return true;
  }

  bool refit_instance_primitives(
      const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
      std::uint32_t instance_index,
      const vkpt::pathtracer::RTInstanceTransformUpdate& update) {
    if (instance_index >= scene.instances.size() ||
        instance_index >= m_instance_ranges.size()) {
      return false;
    }

    auto instance = scene.instances[instance_index];
    instance.translation = update.translation;
    instance.rotation = update.rotation;
    instance.scale = update.scale;
    instance.flags |= update.flags;
    if (update.transform_revision != 0u) {
      instance.transform_revision = update.transform_revision;
    }

    const auto range = m_instance_ranges[instance_index];
    if (range.count != instance.triangle_count ||
        range.first + range.count > m_primitives.size() ||
        range.first + range.count > m_primitive_aabbs.size()) {
      return false;
    }

    for (std::uint32_t triangle = 0u; triangle < instance.triangle_count; ++triangle) {
      const std::uint32_t local_base = instance.local_first_index + triangle * 3u;
      if (local_base + 2u >= scene.local_indices.size()) {
        return false;
      }
      const std::uint32_t li0 = scene.local_indices[local_base + 0u];
      const std::uint32_t li1 = scene.local_indices[local_base + 1u];
      const std::uint32_t li2 = scene.local_indices[local_base + 2u];
      const std::uint32_t local_end = instance.local_first_vertex + instance.local_vertex_count;
      if (instance.local_first_vertex + li0 >= local_end ||
          instance.local_first_vertex + li1 >= local_end ||
          instance.local_first_vertex + li2 >= local_end ||
          instance.local_first_vertex + li0 >= scene.local_vertices.size() ||
          instance.local_first_vertex + li1 >= scene.local_vertices.size() ||
          instance.local_first_vertex + li2 >= scene.local_vertices.size()) {
        return false;
      }

      const auto v0 = transform_instance_vertex(
          scene.local_vertices[instance.local_first_vertex + li0], instance);
      const auto v1 = transform_instance_vertex(
          scene.local_vertices[instance.local_first_vertex + li1], instance);
      const auto v2 = transform_instance_vertex(
          scene.local_vertices[instance.local_first_vertex + li2], instance);
      const auto e1 = sub_vec3(v1, v0);
      const auto e2 = sub_vec3(v2, v0);
      const auto primitive_index = range.first + triangle;
      auto& primitive = m_primitives[primitive_index];
      primitive.material_index = instance.material_index;
      primitive.instance_index = instance_index;
      primitive.v0 = v0;
      primitive.e1 = e1;
      primitive.e2 = e2;
      primitive.normal = normalize_vec3(cross_vec3(e1, e2));
      m_primitive_aabbs[primitive_index] = make_triangle_aabb(v0, v1, v2);
    }
    return true;
  }

  vkpt::cpu::BvhAabb refit_node(std::int32_t node_index) {
    auto& node = m_bvh.nodes[static_cast<std::size_t>(node_index)];
    if (node.is_leaf()) {
      auto aabb = m_primitive_aabbs[
          m_bvh.prim_indices[static_cast<std::size_t>(node.first_prim)]];
      for (std::int32_t i = 1; i < node.prim_count; ++i) {
        const auto primitive_index = m_bvh.prim_indices[
            static_cast<std::size_t>(node.first_prim + i)];
        aabb = union_aabb(aabb, m_primitive_aabbs[primitive_index]);
      }
      node.aabb = aabb;
      return aabb;
    }
    const auto left = refit_node(node.left_child);
    const auto right = refit_node(node.right_child);
    node.aabb = union_aabb(left, right);
    return node.aabb;
  }

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
  std::vector<vkpt::cpu::BvhAabb> m_primitive_aabbs;
  std::vector<PrimitiveRange> m_instance_ranges;
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
