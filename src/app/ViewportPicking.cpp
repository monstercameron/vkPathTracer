#include "app/ViewportInteractionInternal.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkpt::app {

ViewportPickable::Triangle
MakeViewportTriangle(const vkpt::pathtracer::Vec3 &v0,
                     const vkpt::pathtracer::Vec3 &v1,
                     const vkpt::pathtracer::Vec3 &v2) {
  ViewportPickable::Triangle triangle{};
  triangle.v0 = v0;
  triangle.v1 = v1;
  triangle.v2 = v2;
  triangle.bounds_min = {std::min({v0.x, v1.x, v2.x}),
                         std::min({v0.y, v1.y, v2.y}),
                         std::min({v0.z, v1.z, v2.z})};
  triangle.bounds_max = {std::max({v0.x, v1.x, v2.x}),
                         std::max({v0.y, v1.y, v2.y}),
                         std::max({v0.z, v1.z, v2.z})};
  triangle.bounds_valid = true;
  return triangle;
}

std::string PickableLabel(std::string_view name, vkpt::core::StableId id) {
  if (!name.empty()) {
    return std::string(name);
  }
  return "entity " + std::to_string(id);
}

void AddSdfPickable(std::vector<ViewportPickable> &pickables,
                    vkpt::core::StableId id, std::string label,
                    std::string_view shape,
                    const vkpt::scene::TransformComponent &transform,
                    const vkpt::scene::SdfPrimitiveComponent &primitive) {
  const auto center = ToPtVec3(transform.translation);
  const auto scale = ToPtVec3(transform.scale);
  const float radius = std::max(0.05f, primitive.radius);
  vkpt::pathtracer::Vec3 extent{
      std::max(0.05f, std::fabs(scale.x) * radius),
      std::max(0.05f, std::fabs(scale.y) * radius),
      std::max(0.05f, std::fabs(scale.z) * radius),
  };
  if (shape == "box" || shape == "rounded_box") {
    extent = {
        std::max(0.05f, std::fabs(scale.x)),
        std::max(0.05f, std::fabs(scale.y)),
        std::max(0.05f, std::fabs(scale.z)),
    };
  } else if (shape == "torus") {
    const float major = std::max(0.05f, primitive.param_a);
    const float minor = std::max(0.02f, radius);
    const float torusExtent = major + minor;
    extent = {
        std::max(0.05f, std::fabs(scale.x) * torusExtent),
        std::max(0.05f, std::fabs(scale.y) * minor),
        std::max(0.05f, std::fabs(scale.z) * torusExtent),
    };
  } else if (shape == "capsule") {
    const float halfHeight = std::max(0.0f, primitive.param_a);
    extent = {
        std::max(0.05f, std::fabs(scale.x) * radius),
        std::max(0.05f, std::fabs(scale.y) * (halfHeight + radius)),
        std::max(0.05f, std::fabs(scale.z) * radius),
    };
  } else if (shape == "plane") {
    return;
  }

  vkpt::editor::Bounds bounds{};
  ExpandBounds(bounds, PtSub(center, extent));
  ExpandBounds(bounds, PtAdd(center, extent));
  if (bounds.valid) {
    ViewportPickable pickable{};
    pickable.entity_id = id;
    pickable.bounds = bounds;
    pickable.label = std::move(label);
    pickables.push_back(std::move(pickable));
  }
}

void AddSdfPickable(std::vector<ViewportPickable> &pickables,
                    const vkpt::scene::SceneSdfPrimitiveDefinition &primitive) {
  const std::string shape =
      primitive.shape.empty()
          ? (primitive.primitive.shape.empty() ? std::string("sphere")
                                               : primitive.primitive.shape)
          : primitive.shape;
  AddSdfPickable(pickables, primitive.id, "sdf " + std::to_string(primitive.id),
                 shape, primitive.transform, primitive.primitive);
}

std::vector<ViewportPickable>
BuildViewportPickables(const vkpt::scene::SceneDocument &document,
                       const vkpt::pathtracer::RTSceneData &scene) {
  std::vector<ViewportPickable> pickables;
  pickables.reserve(document.entities.size() +
                    document.sdf_primitives.size() +
                    scene.instances.size() +
                    scene.sdf_primitives.size());
  const auto worldSnapshot = BuildSceneWorldSnapshot(document);
  const auto *world = worldSnapshot ? &worldSnapshot.value() : nullptr;
  std::unordered_map<vkpt::core::StableId,
                     const vkpt::scene::SceneGeometryDefinition *>
      geometryById;
  geometryById.reserve(document.geometry.size());
  for (const auto &geometry : document.geometry) {
    geometryById[geometry.id] = &geometry;
  }

  struct MeshPickableRef {
    vkpt::core::StableId entity_id = 0;
    vkpt::core::StableId mesh_id = 0;
    std::string label;
  };
  std::vector<MeshPickableRef> meshRefs;
  meshRefs.reserve(document.entities.size());
  // RT instances are emitted in document mesh order, so keep the same compact
  // entity list before pairing them with backend geometry below.
  for (const auto &entity : document.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto geometryIt = geometryById.find(entity.mesh.mesh_id);
    if (geometryIt == geometryById.end()) {
      continue;
    }
    const auto *geometry = geometryIt->second;
    if (geometry == nullptr || geometry->vertices.empty() ||
        geometry->indices.empty()) {
      continue;
    }
    meshRefs.push_back({entity.id, entity.mesh.mesh_id,
                        PickableLabel(entity.name, entity.id)});
  }

  auto appendEntitySdfPickables = [&]() {
    for (const auto &entity : document.entities) {
      if (!entity.has_sdf_primitive) {
        continue;
      }
      const auto transform = ResolveEntityWorldTransform(entity, world);
      const std::string shape = entity.sdf_primitive.shape.empty()
                                    ? std::string("sphere")
                                    : entity.sdf_primitive.shape;
      AddSdfPickable(pickables, entity.id,
                     PickableLabel(entity.name, entity.id), shape, transform,
                     entity.sdf_primitive);
    }
  };

  // Prefer RT-scene geometry when it is available: it reflects tessellation,
  // dynamic transform flags, and backend-visible triangle ranges. The later
  // document pass is a fallback for partial or failed RT conversion paths.
  if (!meshRefs.empty() && !scene.instances.empty()) {
    const std::size_t count = std::min(meshRefs.size(), scene.instances.size());
    for (std::size_t instanceIndex = 0; instanceIndex < count;
         ++instanceIndex) {
      const auto &instance = scene.instances[instanceIndex];
      const auto &meshRef = meshRefs[instanceIndex];
      if (instance.has_flag(
              vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
        // Dynamic RT instances keep local mesh vertices in the document, so
        // rebuild pick triangles from the current instance transform instead
        // of using the flattened RT vertex buffer.
        const auto geometryIt = geometryById.find(meshRef.mesh_id);
        if (geometryIt == geometryById.end() || geometryIt->second == nullptr) {
          continue;
        }
        const auto *geometry = geometryIt->second;
        const auto transform = TransformFromRtInstance(instance);
        vkpt::editor::Bounds bounds{};
        for (const auto &vertex : geometry->vertices) {
          ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
        }
        if (!bounds.valid) {
          continue;
        }
        ViewportPickable pickable{};
        pickable.entity_id = meshRef.entity_id;
        pickable.bounds = bounds;
        pickable.label = meshRef.label;
        pickable.require_triangle_hit = true;
        pickable.triangles.reserve(geometry->indices.size() / 3u);
        for (std::size_t index = 0; index + 2u < geometry->indices.size();
             index += 3u) {
          const auto i0 = geometry->indices[index + 0u];
          const auto i1 = geometry->indices[index + 1u];
          const auto i2 = geometry->indices[index + 2u];
          if (i0 >= geometry->vertices.size() ||
              i1 >= geometry->vertices.size() ||
              i2 >= geometry->vertices.size()) {
            continue;
          }
          pickable.triangles.push_back(MakeViewportTriangle(
              TransformPointForPreview(geometry->vertices[i0], transform),
              TransformPointForPreview(geometry->vertices[i1], transform),
              TransformPointForPreview(geometry->vertices[i2], transform)));
        }
        if (!pickable.triangles.empty()) {
          pickables.push_back(std::move(pickable));
        }
        continue;
      }
      vkpt::editor::Bounds bounds{};
      for (uint32_t triangle = 0; triangle < instance.triangle_count;
           ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        for (uint32_t corner = 0u; corner < 3u; ++corner) {
          const uint32_t vertexIndex = scene.indices[base + corner];
          if (vertexIndex < scene.vertices.size()) {
            ExpandBounds(bounds, scene.vertices[vertexIndex]);
          }
        }
      }
      if (!bounds.valid) {
        continue;
      }

      ViewportPickable pickable{};
      pickable.entity_id = meshRef.entity_id;
      pickable.bounds = bounds;
      pickable.label = meshRef.label;
      pickable.require_triangle_hit = true;
      pickable.triangles.reserve(instance.triangle_count);
      for (uint32_t triangle = 0; triangle < instance.triangle_count;
           ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        const uint32_t i0 = scene.indices[base + 0u];
        const uint32_t i1 = scene.indices[base + 1u];
        const uint32_t i2 = scene.indices[base + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() ||
            i2 >= scene.vertices.size()) {
          continue;
        }
        pickable.triangles.push_back(MakeViewportTriangle(
            scene.vertices[i0], scene.vertices[i1], scene.vertices[i2]));
      }
      if (!pickable.triangles.empty()) {
        pickables.push_back(std::move(pickable));
      }
    }

    appendEntitySdfPickables();
    for (const auto &primitive : document.sdf_primitives) {
      AddSdfPickable(pickables, primitive);
    }
    if (!pickables.empty()) {
      return pickables;
    }
  }

  for (const auto &entity : document.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto geometryIt = geometryById.find(entity.mesh.mesh_id);
    if (geometryIt == geometryById.end()) {
      continue;
    }
    const auto *geometry = geometryIt->second;
    if (geometry == nullptr || geometry->vertices.empty()) {
      continue;
    }
    const auto transform = ResolveEntityWorldTransform(entity, world);
    vkpt::editor::Bounds bounds{};
    for (const auto &vertex : geometry->vertices) {
      ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
    }
    if (bounds.valid) {
      ViewportPickable pickable{};
      pickable.entity_id = entity.id;
      pickable.bounds = bounds;
      pickable.label = PickableLabel(entity.name, entity.id);
      pickable.require_triangle_hit = true;
      pickable.triangles.reserve(geometry->indices.size() / 3u);
      for (std::size_t index = 0; index + 2u < geometry->indices.size();
           index += 3u) {
        const auto i0 = geometry->indices[index + 0u];
        const auto i1 = geometry->indices[index + 1u];
        const auto i2 = geometry->indices[index + 2u];
        if (i0 >= geometry->vertices.size() ||
            i1 >= geometry->vertices.size() ||
            i2 >= geometry->vertices.size()) {
          continue;
        }
        pickable.triangles.push_back(MakeViewportTriangle(
            TransformPointForPreview(geometry->vertices[i0], transform),
            TransformPointForPreview(geometry->vertices[i1], transform),
            TransformPointForPreview(geometry->vertices[i2], transform)));
      }
      if (pickable.triangles.empty()) {
        continue;
      }
      pickables.push_back(std::move(pickable));
    }
  }

  appendEntitySdfPickables();
  for (const auto &primitive : document.sdf_primitives) {
    AddSdfPickable(pickables, primitive);
  }

  if (!pickables.empty()) {
    return pickables;
  }

  // Last resort for generated scenes without document entity ids: expose
  // stable synthetic ids so viewport selection and labels still work.
  for (std::size_t instanceIndex = 0; instanceIndex < scene.instances.size();
       ++instanceIndex) {
    const auto &instance = scene.instances[instanceIndex];
    vkpt::editor::Bounds bounds{};
    for (uint32_t triangle = 0; triangle < instance.triangle_count;
         ++triangle) {
      const uint32_t base = (instance.first_triangle + triangle) * 3u;
      if (base + 2u >= scene.indices.size()) {
        continue;
      }
      for (uint32_t corner = 0u; corner < 3u; ++corner) {
        const uint32_t vertexIndex = scene.indices[base + corner];
        if (vertexIndex < scene.vertices.size()) {
          ExpandBounds(bounds, scene.vertices[vertexIndex]);
        }
      }
    }
    if (bounds.valid) {
      const auto id = static_cast<vkpt::core::StableId>(instanceIndex + 1u);
      ViewportPickable pickable{};
      pickable.entity_id = id;
      pickable.bounds = bounds;
      pickable.label = "instance " + std::to_string(id);
      pickable.require_triangle_hit = true;
      pickable.triangles.reserve(instance.triangle_count);
      for (uint32_t triangle = 0; triangle < instance.triangle_count;
           ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        const uint32_t i0 = scene.indices[base + 0u];
        const uint32_t i1 = scene.indices[base + 1u];
        const uint32_t i2 = scene.indices[base + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() ||
            i2 >= scene.vertices.size()) {
          continue;
        }
        pickable.triangles.push_back(MakeViewportTriangle(
            scene.vertices[i0], scene.vertices[i1], scene.vertices[i2]));
      }
      if (pickable.triangles.empty()) {
        continue;
      }
      pickables.push_back(std::move(pickable));
    }
  }

  for (std::size_t primitiveIndex = 0;
       primitiveIndex < scene.sdf_primitives.size(); ++primitiveIndex) {
    const auto &primitive = scene.sdf_primitives[primitiveIndex];
    const float radius = std::max(0.05f, primitive.radius);
    const vkpt::pathtracer::Vec3 extent{
        std::max(0.05f, std::fabs(primitive.scale.x) * radius),
        std::max(0.05f, std::fabs(primitive.scale.y) * radius),
        std::max(0.05f, std::fabs(primitive.scale.z) * radius),
    };
    vkpt::editor::Bounds bounds{};
    ExpandBounds(bounds, PtSub(primitive.position, extent));
    ExpandBounds(bounds, PtAdd(primitive.position, extent));
    if (bounds.valid) {
      const auto id = static_cast<vkpt::core::StableId>(pickables.size() + 1u);
      ViewportPickable pickable{};
      pickable.entity_id = id;
      pickable.bounds = bounds;
      pickable.label = "sdf " + std::to_string(id);
      pickables.push_back(std::move(pickable));
    }
  }

  return pickables;
}

ViewportRay BuildViewportRay(const ViewportCameraPose &camera, float x, float y,
                             float width, float height, float renderAspect) {
  const float safeWidth = std::max(1.0f, width);
  const float safeHeight = std::max(1.0f, height);
  const float safeAspect = std::max(0.01f, renderAspect);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right =
      PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float tanHalfFov =
      std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));
  const float nx =
      ((x + 0.5f) / safeWidth * 2.0f - 1.0f) * safeAspect * tanHalfFov;
  const float ny = (1.0f - (y + 0.5f) / safeHeight * 2.0f) * tanHalfFov;
  return {camera.position,
          PtNormalize(PtAdd(PtAdd(forward, PtMul(right, nx)), PtMul(up, ny)))};
}

bool IntersectBounds(const ViewportRay &ray, const vkpt::editor::Bounds &bounds,
                     float &t_near) {
  if (!bounds.valid) {
    return false;
  }
  const float minValues[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
  const float maxValues[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
  const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  const float direction[3] = {ray.direction.x, ray.direction.y,
                              ray.direction.z};
  float tMin = 1.0e-4f;
  float tMax = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(direction[axis]) <= 1.0e-6f) {
      if (origin[axis] < minValues[axis] || origin[axis] > maxValues[axis]) {
        return false;
      }
      continue;
    }
    const float invD = 1.0f / direction[axis];
    float t0 = (minValues[axis] - origin[axis]) * invD;
    float t1 = (maxValues[axis] - origin[axis]) * invD;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax) {
      return false;
    }
  }
  t_near = tMin;
  return true;
}

bool IntersectFrontFacingTriangle(const ViewportRay &ray,
                                  const ViewportPickable::Triangle &triangle,
                                  float maxDistance, float &t_out) {
  constexpr float kEpsilon = 1.0e-6f;
  const auto edge1 = PtSub(triangle.v1, triangle.v0);
  const auto edge2 = PtSub(triangle.v2, triangle.v0);
  const auto pvec = PtCross(ray.direction, edge2);
  const float det = PtDot(edge1, pvec);
  if (det <= kEpsilon) {
    return false;
  }

  const float invDet = 1.0f / det;
  const auto tvec = PtSub(ray.origin, triangle.v0);
  const float u = PtDot(tvec, pvec) * invDet;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const auto qvec = PtCross(tvec, edge1);
  const float v = PtDot(ray.direction, qvec) * invDet;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float t = PtDot(edge2, qvec) * invDet;
  if (t <= kEpsilon || t >= maxDistance) {
    return false;
  }
  t_out = t;
  return true;
}

bool IntersectTriangleDoubleSided(const ViewportRay &ray,
                                  const ViewportPickable::Triangle &triangle,
                                  float maxDistance, float &t_out,
                                  vkpt::pathtracer::Vec3 &normal_out) {
  constexpr float kEpsilon = 1.0e-6f;
  const auto edge1 = PtSub(triangle.v1, triangle.v0);
  const auto edge2 = PtSub(triangle.v2, triangle.v0);
  const auto pvec = PtCross(ray.direction, edge2);
  const float det = PtDot(edge1, pvec);
  if (std::fabs(det) <= kEpsilon) {
    return false;
  }

  const float invDet = 1.0f / det;
  const auto tvec = PtSub(ray.origin, triangle.v0);
  const float u = PtDot(tvec, pvec) * invDet;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const auto qvec = PtCross(tvec, edge1);
  const float v = PtDot(ray.direction, qvec) * invDet;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float t = PtDot(edge2, qvec) * invDet;
  if (t <= kEpsilon || t >= maxDistance) {
    return false;
  }

  auto normal = PtNormalize(PtCross(edge1, edge2), {0.0f, 1.0f, 0.0f});
  if (PtDot(normal, ray.direction) > 0.0f) {
    normal = PtMul(normal, -1.0f);
  }
  t_out = t;
  normal_out = normal;
  return true;
}

bool BoundsOverlapsAabb(const vkpt::editor::Bounds &bounds,
                        const vkpt::pathtracer::Vec3 &queryMin,
                        const vkpt::pathtracer::Vec3 &queryMax) {
  if (!bounds.valid) {
    return true;
  }
  return bounds.max.x >= queryMin.x && bounds.min.x <= queryMax.x &&
         bounds.max.y >= queryMin.y && bounds.min.y <= queryMax.y &&
         bounds.max.z >= queryMin.z && bounds.min.z <= queryMax.z;
}

bool TriangleOverlapsAabb(const ViewportPickable::Triangle &triangle,
                          const vkpt::pathtracer::Vec3 &queryMin,
                          const vkpt::pathtracer::Vec3 &queryMax) {
  if (triangle.bounds_valid) {
    return triangle.bounds_max.x >= queryMin.x &&
           triangle.bounds_min.x <= queryMax.x &&
           triangle.bounds_max.y >= queryMin.y &&
           triangle.bounds_min.y <= queryMax.y &&
           triangle.bounds_max.z >= queryMin.z &&
           triangle.bounds_min.z <= queryMax.z;
  }
  const float minX = std::min({triangle.v0.x, triangle.v1.x, triangle.v2.x});
  const float minY = std::min({triangle.v0.y, triangle.v1.y, triangle.v2.y});
  const float minZ = std::min({triangle.v0.z, triangle.v1.z, triangle.v2.z});
  const float maxX = std::max({triangle.v0.x, triangle.v1.x, triangle.v2.x});
  const float maxY = std::max({triangle.v0.y, triangle.v1.y, triangle.v2.y});
  const float maxZ = std::max({triangle.v0.z, triangle.v1.z, triangle.v2.z});
  return maxX >= queryMin.x && minX <= queryMax.x && maxY >= queryMin.y &&
         minY <= queryMax.y && maxZ >= queryMin.z && minZ <= queryMax.z;
}

bool IntersectPickableForSelection(const ViewportRay &ray,
                                   const ViewportPickable &pickable,
                                   float maxDistance, float &t_out) {
  float boundsDistance = 0.0f;
  if (!IntersectBounds(ray, pickable.bounds, boundsDistance) ||
      boundsDistance >= maxDistance) {
    return false;
  }

  if (pickable.triangles.empty()) {
    if (pickable.require_triangle_hit) {
      return false;
    }
    t_out = boundsDistance;
    return true;
  }

  // Bounds choose candidates cheaply, but mesh pickables need triangle hits so
  // a large AABB cannot select through empty space.
  bool hit = false;
  float bestTriangleDistance = maxDistance;
  for (const auto &triangle : pickable.triangles) {
    float triangleDistance = 0.0f;
    if (!IntersectFrontFacingTriangle(ray, triangle, bestTriangleDistance,
                                      triangleDistance)) {
      continue;
    }
    bestTriangleDistance = triangleDistance;
    hit = true;
  }
  if (!hit) {
    return false;
  }
  t_out = bestTriangleDistance;
  return true;
}

std::optional<ViewportPickResult>
PickViewportObject(const std::vector<ViewportPickable> &pickables,
                   const ViewportCameraPose &camera, float x, float y,
                   float width, float height, float renderAspect) {
  const auto ray = BuildViewportRay(camera, x, y, width, height, renderAspect);
  std::optional<ViewportPickResult> best;
  for (const auto &pickable : pickables) {
    const float maxDistance =
        best ? best->distance : std::numeric_limits<float>::infinity();
    float distance = 0.0f;
    if (!IntersectPickableForSelection(ray, pickable, maxDistance, distance)) {
      continue;
    }
    if (!best || distance < best->distance) {
      best = ViewportPickResult{pickable.entity_id, pickable.bounds,
                                pickable.label, distance};
    }
  }
  return best;
}

} // namespace vkpt::app

#endif
