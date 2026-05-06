#include "pathtracer/PathTracer.h"

#include <cmath>
#include <limits>

namespace {

vkpt::pathtracer::Vec3 operator+(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) { return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z}; }
vkpt::pathtracer::Vec3 operator-(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) { return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z}; }
vkpt::pathtracer::Vec3 operator*(const vkpt::pathtracer::Vec3& lhs, float rhs) { return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs}; }
vkpt::pathtracer::Vec3 operator/(const vkpt::pathtracer::Vec3& lhs, float rhs) { return {lhs.x / rhs, lhs.y / rhs, lhs.z / rhs}; }

float dot(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) { return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z; }
vkpt::pathtracer::Vec3 cross(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x};
}

float length(const vkpt::pathtracer::Vec3& value) { return std::sqrt(dot(value, value)); }
vkpt::pathtracer::Vec3 normalize(const vkpt::pathtracer::Vec3& value) {
  const float l = length(value);
  return l <= 1.0e-4f ? vkpt::pathtracer::Vec3{0.0f, 1.0f, 0.0f} : value / l;
}

}  // namespace

namespace vkpt::pathtracer {

bool ScalarCpuPathTracer::intersect_scene(const Ray& ray, Hit& out, SampleCounters& counters) const {
  out = {};
  float bestT = std::numeric_limits<float>::infinity();
  const IRayAccelerator* accelerator = m_external_accelerator ? m_external_accelerator : m_accelerator.get();
  const bool use_accelerator = accelerator && m_accel_info.built && m_accel_info.primitive_count > 0u;

  if (use_accelerator) {
    RayQueryHit accel_hit{};
    RayQueryStats stats{};
    if (accelerator->intersect(ray, accel_hit, &stats) && accel_hit.hit) {
      out.hit = true;
      out.t = accel_hit.t;
      out.position = accel_hit.position;
      out.normal = accel_hit.normal;
      out.material_index = accel_hit.material_index;
      bestT = accel_hit.t;
    }
    counters.triangle_tests += stats.triangle_tests;
    counters.triangle_hits += stats.triangle_hits;
    counters.bvh_node_visits += stats.bvh_node_visits;
    counters.bvh_leaf_visits += stats.bvh_leaf_visits;
  } else {
    for (const auto& instance : m_scene.instances) {
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t baseTri = (instance.first_triangle + triangle) * 3;
        if (baseTri + 2 >= m_scene.indices.size()) {
          continue;
        }
        const RTTriangle tri{
            m_scene.indices[baseTri + 0],
            m_scene.indices[baseTri + 1],
            m_scene.indices[baseTri + 2],
        };
        float t = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        if (!intersect_triangle(tri, ray, t, u, v, counters)) {
          continue;
        }
        ++counters.triangle_hits;
        if (t >= bestT) {
          continue;
        }
        if (tri.i0 >= m_scene.vertices.size() || tri.i1 >= m_scene.vertices.size() || tri.i2 >= m_scene.vertices.size()) {
          continue;
        }
        const Vec3 e1 = m_scene.vertices[tri.i1] - m_scene.vertices[tri.i0];
        const Vec3 e2 = m_scene.vertices[tri.i2] - m_scene.vertices[tri.i0];
        out.hit = true;
        out.t = t;
        out.position = ray.origin + ray.direction * t;
        out.normal = make_unit(cross(e1, e2));
        out.material_index = instance.material_index;
        bestT = t;
      }
    }
  }

  for (const auto& primitive : m_scene.sdf_primitives) {
    float t = 0.0f;
    Vec3 normal{};
    bool hit = false;
    switch (primitive.shape) {
      case SdfShape::Sphere:
        hit = intersect_sphere(primitive, ray, t, normal, counters);
        break;
      case SdfShape::Box:
        hit = intersect_box(primitive, ray, t, normal, counters);
        break;
      case SdfShape::RoundedBox:
        hit = intersect_rounded_box(primitive, ray, t, normal, counters);
        break;
      case SdfShape::Plane:
        hit = intersect_plane(primitive, ray, t, normal, counters);
        break;
      case SdfShape::Torus:
        hit = intersect_torus(primitive, ray, t, normal, counters);
        break;
      case SdfShape::Capsule:
        hit = intersect_capsule(primitive, ray, t, normal, counters);
        break;
      default:
        break;
    }
    if (!hit || t >= bestT) {
      continue;
    }
    bestT = t;
    out.hit = true;
    out.t = t;
    out.position = ray.origin + ray.direction * t;
    out.normal = normalize(normal);
    out.material_index = primitive.material_index;
  }

  return out.hit;
}


}  // namespace vkpt::pathtracer
