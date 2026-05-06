#include "pathtracer/SceneConversion.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vkpt::pathtracer {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1e-4f;

uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

uint64_t mix_cache_key(uint64_t key, uint64_t value) {
  return splitmix64(key ^ (value + 0x9e3779b97f4a7c15ULL + (key << 6u) + (key >> 2u)));
}

uint32_t float_bits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint32_t saturating_u32(uint64_t value) {
  return value > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(value);
}

uint64_t build_tessellation_cache_key(
    const vkpt::scene::SceneGeometryDefinition& geometry,
    const vkpt::scene::TransformComponent& transform,
    vkpt::core::StableId material_id) {
  uint64_t key = 0xd2f74407b1ce6e93ULL;
  key = mix_cache_key(key, static_cast<uint64_t>(geometry.id));
  key = mix_cache_key(key, static_cast<uint64_t>(material_id));
  key = mix_cache_key(key, geometry.tessellation.enabled ? 1u : 0u);
  key = mix_cache_key(key, static_cast<uint64_t>(geometry.tessellation.factor));
  key = mix_cache_key(key, geometry.tessellation.gpu_preferred ? 1u : 0u);
  key = mix_cache_key(key, geometry.tessellation.cache_generated_geometry ? 1u : 0u);
  key = mix_cache_key(key, geometry.tessellation.displacement ? 1u : 0u);
  for (const char ch : geometry.tessellation.mode) {
    key = mix_cache_key(key, static_cast<unsigned char>(ch));
  }
  for (const char ch : geometry.tessellation.projection) {
    key = mix_cache_key(key, static_cast<unsigned char>(ch));
  }
  for (const auto& vertex : geometry.vertices) {
    key = mix_cache_key(key, float_bits(vertex.x));
    key = mix_cache_key(key, float_bits(vertex.y));
    key = mix_cache_key(key, float_bits(vertex.z));
  }
  for (const auto index : geometry.indices) {
    key = mix_cache_key(key, index);
  }
  key = mix_cache_key(key, float_bits(transform.translation.x));
  key = mix_cache_key(key, float_bits(transform.translation.y));
  key = mix_cache_key(key, float_bits(transform.translation.z));
  key = mix_cache_key(key, float_bits(transform.rotation.x));
  key = mix_cache_key(key, float_bits(transform.rotation.y));
  key = mix_cache_key(key, float_bits(transform.rotation.z));
  key = mix_cache_key(key, float_bits(transform.rotation.w));
  key = mix_cache_key(key, float_bits(transform.scale.x));
  key = mix_cache_key(key, float_bits(transform.scale.y));
  key = mix_cache_key(key, float_bits(transform.scale.z));
  return key == 0u ? 1u : key;
}

Vec3 operator+(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 operator*(const Vec3& lhs, float rhs) {
  return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

Vec3 operator/(const Vec3& lhs, float rhs) {
  return {lhs.x / rhs, lhs.y / rhs, lhs.z / rhs};
}

float dot(const Vec3& lhs, const Vec3& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 cross(const Vec3& lhs, const Vec3& rhs) {
  return {
      lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.z * rhs.x - lhs.x * rhs.z,
      lhs.x * rhs.y - lhs.y * rhs.x,
  };
}

float length_sq(const Vec3& value) {
  return dot(value, value);
}

Vec3 normalize(const Vec3& value) {
  const float l = std::sqrt(length_sq(value));
  if (l <= kEpsilon) {
    return {0.0f, 1.0f, 0.0f};
  }
  return value / l;
}

Vec3 rotate_quat(const Vec3& value, const vkpt::scene::Quat& rotation) {
  float x = rotation.x;
  float y = rotation.y;
  float z = rotation.z;
  float w = rotation.w;
  const float len = std::sqrt(x * x + y * y + z * z + w * w);
  if (len > 1.0e-6f) {
    const float inv = 1.0f / len;
    x *= inv;
    y *= inv;
    z *= inv;
    w *= inv;
  } else {
    x = 0.0f;
    y = 0.0f;
    z = 0.0f;
    w = 1.0f;
  }
  const Vec3 qv{x, y, z};
  const auto t = cross(qv, value) * 2.0f;
  return value + t * w + cross(qv, t);
}

Vec3 transform_point(const Vec3& point,
                     const Vec3& translation,
                     const vkpt::scene::Quat& rotation,
                     const Vec3& scale) {
  const Vec3 scaled{point.x * scale.x, point.y * scale.y, point.z * scale.z};
  const auto rotated = rotate_quat(scaled, rotation);
  return {rotated.x + translation.x, rotated.y + translation.y, rotated.z + translation.z};
}

Vec3 parse_shape_position(const vkpt::scene::Vec3& pos) {
  return {pos.x, pos.y, pos.z};
}

Vec3 parse_shape_rotation(const vkpt::scene::Quat& rot) {
  return {rot.x, rot.y, rot.z};
}

Vec3 parse_shape_scale(const vkpt::scene::Vec3& scale) {
  return {scale.x, scale.y, scale.z};
}

SdfShape parse_sdf_shape(std::string_view name) {
  if (name == "sphere") return SdfShape::Sphere;
  if (name == "box") return SdfShape::Box;
  if (name == "rounded_box") return SdfShape::RoundedBox;
  if (name == "plane") return SdfShape::Plane;
  if (name == "torus") return SdfShape::Torus;
  if (name == "capsule") return SdfShape::Capsule;
  return SdfShape::Unknown;
}

float clamp01(float v) {
  if (!std::isfinite(v)) {
    return 0.0f;
  }
  return std::min(1.0f, std::max(0.0f, v));
}

bool is_texture_asset_uri(std::string_view uri) {
  const auto dot = uri.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  std::string ext(uri.substr(dot));
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".exr" || ext == ".hdr";
}

std::string normalize_material_id(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if ((uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9')) {
      out.push_back(static_cast<char>(std::tolower(uc)));
    } else if (uc == '_' || uc == '-' || uc == ' ') {
      if (!out.empty() && out.back() != '_') {
        out.push_back('_');
      }
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

bool is_environment_light_type(std::string_view type) {
  const std::string id = normalize_material_id(type);
  return id == "environment" || id == "environment_sky" || id == "environment_hdri" ||
         id == "hdri" || id == "hdri_sky" || id == "open_sky" || id == "sky";
}

bool is_spot_light_type(std::string_view type) {
  const std::string id = normalize_material_id(type);
  return id == "spot" || id == "spot_light" || id == "spotlight";
}

Vec3 environment_color_from_light(const vkpt::scene::LightComponent& light) {
  const float intensity = std::max(0.0f, light.intensity);
  return {light.color.x * intensity, light.color.y * intensity, light.color.z * intensity};
}

Vec3 light_direction_from_component(const vkpt::scene::LightComponent& light,
                                    const vkpt::scene::TransformComponent& transform) {
  Vec3 direction{light.direction.x, light.direction.y, light.direction.z};
  if (length_sq(direction) <= 1.0e-8f) {
    direction = rotate_quat({0.0f, 0.0f, -1.0f}, transform.rotation);
  }
  return normalize(direction);
}

std::pair<float, float> spot_cone_cosines(const vkpt::scene::LightComponent& light) {
  if (!is_spot_light_type(light.type)) {
    return {-1.0f, -1.0f};
  }
  const float innerHalfDegrees = std::clamp(light.beam_angle_degrees * 0.5f, 1.0f, 89.0f);
  const float outerHalfDegrees = std::clamp(
      innerHalfDegrees * (1.0f + std::max(0.0f, light.blend)),
      innerHalfDegrees + 0.25f,
      89.5f);
  return {
      std::cos(innerHalfDegrees * kPi / 180.0f),
      std::cos(outerHalfDegrees * kPi / 180.0f)};
}

uint32_t material_model_from_family(std::string_view family) {
  const std::string id = normalize_material_id(family);
  if (id == "emissive" || id == "environment_emissive" || id == "blackbody_emission" ||
      id == "fire_plasma" || id == "fire_sparkle_emission" || id == "light_emitting_textile" ||
      id == "bokeh_motion_blur_stress") {
    return 1u;
  }
  if (id == "mirror") {
    return 2u;
  }
  if (id == "specular" || id == "glossy" || id == "normal_mapped_pbr" ||
      id == "plastic" || id == "rubber") {
    return 3u;
  }
  if (id == "ggx_rough_conductor" || id == "metallic_pbr" || id == "anisotropic_ggx" ||
      id == "brushed_metal" || id == "ground_metal") {
    return 4u;
  }
  if (id == "ggx_rough_dielectric" || id == "dielectric_glass" || id == "spectral_glass_approx" ||
      id == "frosted_glass" || id == "dirty_glass" || id == "water_fluid_surface" ||
      id == "ice_crystal" || id == "resin" || id == "epoxy" || id == "gemstone" ||
      id == "frosted_acrylic" || id == "translucent_polymer") {
    return 5u;
  }
  if (id == "velvet" || id == "fabric_cloth" || id == "hair_fur_lobes" || id == "pearl_lustre") {
    return 6u;
  }
  if (id == "clearcoat" || id == "car_paint" || id == "paint" || id == "porcelain_ceramic" ||
      id == "wet_surface" || id == "energy_conserving_layered" ||
      id == "thin_film_iridescent" || id == "diffraction_grating" ||
      id == "holographic_coating" || id == "retroreflector" ||
      id == "caustics_inspired_response") {
    return 7u;
  }
  if (id == "toon_surface" || id == "stylized_diffuse" || id == "xray") {
    return 8u;
  }
  if (id == "volumetric_medium" || id == "volumetric_shafts" || id == "smoke" ||
      id == "chromatic_dust") {
    return 9u;
  }
  return 0u;
}

uint32_t material_effect_from_family(std::string_view family) {
  const std::string id = normalize_material_id(family);
  if (id == "velvet" || id == "fabric_cloth" || id == "hair_fur_lobes") return 1u;
  if (id == "procedural_material" || id == "sdf_fractal_material") return 2u;
  if (id == "voronoi_cracks" || id == "charcoal" || id == "cardboard") return 3u;
  if (id == "marble_scattering" || id == "porcelain_ceramic" || id == "ice_crystal") return 4u;
  if (id == "corrosion_oxidation" || id == "rust_progression" || id == "terra_earth" || id == "mud") return 5u;
  if (id == "chromatic_dust" || id == "sand" || id == "stone" || id == "concrete" || id == "plaster") return 6u;
  if (id == "skin" || id == "wax" || id == "subsurface_approx" || id == "paper") return 7u;
  if (id == "thin_film_iridescent" || id == "diffraction_grating" || id == "holographic_coating" ||
      id == "pearl_lustre" || id == "gemstone" || id == "spectral_glass_approx") return 8u;
  if (id == "alpha_mask") return 9u;
  if (id == "blackbody_emission" || id == "fire_plasma" || id == "fire_sparkle_emission") return 10u;
  if (id == "wet_surface" || id == "water_fluid_surface" || id == "dirty_glass") return 11u;
  if (id == "retroreflector" || id == "caustics_inspired_response" || id == "bokeh_motion_blur_stress") return 12u;
  if (id == "normal_mapped_pbr") return 13u;
  if (id == "xray") return 14u;
  if (id == "volumetric_medium" || id == "volumetric_shafts" || id == "smoke") return 15u;
  return 0u;
}

}  // namespace

vkpt::core::Result<RTSceneData> BuildSceneDataFromDocument(const vkpt::scene::SceneDocument& doc) {
  RTSceneData scene;
  scene.camera_position = {0.0f, 1.0f, 4.0f};
  scene.camera_target = {0.0f, 1.0f, 0.0f};
  scene.camera_up = {0.0f, 1.0f, 0.0f};
  scene.camera_fov_deg = 55.0f;
  scene.environment_color = {0.0f, 0.0f, 0.0f};

  std::unordered_map<std::string, uint32_t> textureLookup;
  auto normalize_texture_uri = [](std::string uri) {
    std::replace(uri.begin(), uri.end(), '\\', '/');
    return uri;
  };
  auto add_texture_uri = [&](std::string uri) -> uint32_t {
    if (uri.empty() || !is_texture_asset_uri(uri)) {
      return 0xFFFFFFFFu;
    }
    uri = normalize_texture_uri(std::move(uri));
    if (const auto it = textureLookup.find(uri); it != textureLookup.end()) {
      return it->second;
    }
    const uint32_t index = static_cast<uint32_t>(scene.textures.size());
    textureLookup.emplace(uri, index);
    scene.textures.push_back(std::move(uri));
    return index;
  };

  for (const auto& asset : doc.assets) {
    const auto assetType = normalize_material_id(asset.type);
    if (!asset.uri.empty() &&
        (assetType == "texture" || assetType == "image" || is_texture_asset_uri(asset.uri))) {
      add_texture_uri(asset.uri);
    }
  }
  for (const auto& material : doc.materials) {
    add_texture_uri(material.base_color_texture);
    add_texture_uri(material.normal_texture);
  }

  std::vector<vkpt::scene::SceneMaterialDefinition> materials = doc.materials;
  if (materials.empty()) {
    materials.push_back({});
  }
  std::unordered_map<vkpt::core::StableId, uint32_t> materialLookup;
  for (const auto& material : materials) {
    auto runtimeMaterial = material;
    vkpt::scene::ApplyMaterialFamilyPreset(runtimeMaterial,
                                           vkpt::scene::SceneMaterialPresetPolicy::FillGenericDefaults);
    const uint32_t index = static_cast<uint32_t>(scene.materials.size());
    materialLookup[runtimeMaterial.id] = index;
    RTMaterial outMaterial;
    outMaterial.albedo = {runtimeMaterial.albedo.x, runtimeMaterial.albedo.y, runtimeMaterial.albedo.z};
    outMaterial.emissive = {runtimeMaterial.emission.x * runtimeMaterial.emission_intensity,
                            runtimeMaterial.emission.y * runtimeMaterial.emission_intensity,
                            runtimeMaterial.emission.z * runtimeMaterial.emission_intensity};
    outMaterial.roughness = clamp01(runtimeMaterial.roughness);
    outMaterial.metallic = clamp01(runtimeMaterial.metallic);
    outMaterial.ior = std::max(1.01f, runtimeMaterial.ior);
    outMaterial.transmission = clamp01(runtimeMaterial.transmission);
    outMaterial.clearcoat = clamp01(runtimeMaterial.clearcoat);
    outMaterial.sheen = clamp01(runtimeMaterial.sheen);
    outMaterial.anisotropy = std::min(1.0f, std::max(-1.0f, runtimeMaterial.anisotropy));
    outMaterial.alpha = clamp01(runtimeMaterial.alpha);
    outMaterial.material_model = material_model_from_family(
        runtimeMaterial.family.empty() ? runtimeMaterial.name : runtimeMaterial.family);
    outMaterial.material_effect = material_effect_from_family(
        runtimeMaterial.family.empty() ? runtimeMaterial.name : runtimeMaterial.family);
    outMaterial.material_flags = runtimeMaterial.double_sided ? 1u : 0u;
    outMaterial.base_color_texture_index = add_texture_uri(runtimeMaterial.base_color_texture);
    outMaterial.normal_texture_index = add_texture_uri(runtimeMaterial.normal_texture);
    scene.materials.push_back(outMaterial);
  }

  std::optional<vkpt::scene::SceneWorld> ecsWorld;
  if (auto worldResult = doc.to_world()) {
    ecsWorld = std::move(worldResult.value());
    ecsWorld->recompute_world_transforms();
  }

  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneEntityDefinition*> entityById;
  entityById.reserve(doc.entities.size());
  for (const auto& entity : doc.entities) {
    entityById[entity.id] = &entity;
  }

  auto entity_has_animated_transform_path =
      [&](const vkpt::scene::SceneEntityDefinition& entity) {
    if (!entity.animation.clip.empty()) {
      return true;
    }
    std::unordered_set<vkpt::core::StableId> visited;
    vkpt::core::StableId parent = entity.has_hierarchy ? entity.hierarchy.parent : 0u;
    while (parent != 0u && visited.insert(parent).second) {
      const auto parentIt = entityById.find(parent);
      if (parentIt == entityById.end() || parentIt->second == nullptr) {
        break;
      }
      if (!parentIt->second->animation.clip.empty()) {
        return true;
      }
      parent = parentIt->second->has_hierarchy ? parentIt->second->hierarchy.parent : 0u;
    }
    return false;
  };

  auto resolve_entity_transform = [&](vkpt::core::StableId id,
                                      const vkpt::scene::TransformComponent* local_transform,
                                      bool* has_transform) {
    if (ecsWorld.has_value()) {
      if (const auto* worldTransform = ecsWorld->world_transform(id)) {
        if (has_transform) {
          *has_transform = true;
        }
        vkpt::scene::TransformComponent transform = *worldTransform;
        transform.dirty = false;
        return transform;
      }
    }
    if (local_transform != nullptr) {
      if (has_transform) {
        *has_transform = true;
      }
      return *local_transform;
    }
    if (has_transform) {
      *has_transform = false;
    }
    return vkpt::scene::TransformComponent{};
  };

  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneGeometryDefinition*> geometryById;
  for (const auto& geometry : doc.geometry) {
    geometryById[geometry.id] = &geometry;
  }

  for (const auto& entity : doc.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto itGeom = geometryById.find(entity.mesh.mesh_id);
    if (itGeom == geometryById.end()) {
      continue;
    }
    const auto* geometry = itGeom->second;
    if (geometry->vertices.empty() || geometry->indices.empty() || geometry->indices.size() % 3u != 0u) {
      continue;
    }

    const vkpt::core::StableId materialId = entity.mesh.material_id != 0 ? entity.mesh.material_id : geometry->material_id;
    uint32_t materialIndex = 0;
    if (auto mi = materialLookup.find(materialId); mi != materialLookup.end()) {
      materialIndex = mi->second;
    }

    const auto transform = resolve_entity_transform(
        entity.id, entity.has_transform ? &entity.transform : nullptr, nullptr);
    const auto translation = transform.translation;
    const auto rotation = transform.rotation;
    const auto scale = transform.scale;
    const uint32_t firstVertex = static_cast<uint32_t>(scene.vertices.size());
    const bool hasTexcoords = geometry->texcoords.size() == geometry->vertices.size();
    for (std::size_t vertexIndex = 0; vertexIndex < geometry->vertices.size(); ++vertexIndex) {
      const auto& vertex = geometry->vertices[vertexIndex];
      scene.vertices.push_back(transform_point(
          Vec3{vertex.x, vertex.y, vertex.z},
          Vec3{translation.x, translation.y, translation.z},
          rotation,
          Vec3{scale.x, scale.y, scale.z}));
      if (hasTexcoords) {
        const auto& uv = geometry->texcoords[vertexIndex];
        scene.texcoords.push_back(Vec2{uv.u, uv.v});
      } else {
        scene.texcoords.push_back(Vec2{});
      }
    }

    const uint32_t firstTriangle = static_cast<uint32_t>(scene.indices.size() / 3u);
    for (const auto& index : geometry->indices) {
      scene.indices.push_back(firstVertex + index);
    }
    RTInstance instance;
    instance.entity_id = entity.id;
    instance.geometry_id = static_cast<uint32_t>(entity.mesh.mesh_id);
    instance.first_triangle = firstTriangle;
    instance.triangle_count = static_cast<uint32_t>(geometry->indices.size() / 3u);
    instance.material_index = materialIndex;
    const bool physics_dynamic =
        entity.has_physics_body && entity.physics_body.enabled && entity.physics_body.dynamic;
    const bool animation_dynamic = entity_has_animated_transform_path(entity);
    if (physics_dynamic || animation_dynamic) {
      instance.flags |= kRTInstanceFlagDynamicTransform;
      if (physics_dynamic) {
        instance.flags |= kRTInstanceFlagPhysicsControlled;
      }
      instance.local_first_vertex = static_cast<uint32_t>(scene.local_vertices.size());
      instance.local_vertex_count = static_cast<uint32_t>(geometry->vertices.size());
      for (const auto& vertex : geometry->vertices) {
        scene.local_vertices.push_back(Vec3{vertex.x, vertex.y, vertex.z});
      }
      instance.local_first_index = static_cast<uint32_t>(scene.local_indices.size());
      instance.local_index_count = static_cast<uint32_t>(geometry->indices.size());
      for (const auto index : geometry->indices) {
        scene.local_indices.push_back(index);
      }
    }
    if (entity.has_transform && entity.transform.dirty) {
      instance.flags |= kRTInstanceFlagTransformDirty;
    }
    instance.transform_revision = static_cast<uint32_t>(scene.instances.size() + 1u);
    instance.translation = Vec3{translation.x, translation.y, translation.z};
    instance.rotation = Quat4{rotation.x, rotation.y, rotation.z, rotation.w};
    instance.scale = Vec3{scale.x, scale.y, scale.z};
    scene.instances.push_back(instance);

    const auto& tessellation = geometry->tessellation;
    if (tessellation.enabled && tessellation.mode == "uniform" && tessellation.factor > 1u) {
      const uint64_t factor = tessellation.factor;
      const uint64_t sourceTriangles = geometry->indices.size() / 3u;
      const uint64_t verticesPerTriangle = ((factor + 1u) * (factor + 2u)) / 2u;
      const uint64_t generatedVertices = sourceTriangles * verticesPerTriangle;
      const uint64_t generatedIndices = sourceTriangles * factor * factor * 3u;
      RTTessellationRequest request{};
      request.geometry_id = static_cast<uint32_t>(entity.mesh.mesh_id);
      request.first_triangle = firstTriangle;
      request.source_triangle_count = static_cast<uint32_t>(sourceTriangles);
      request.factor = tessellation.factor;
      request.generated_vertex_count = saturating_u32(generatedVertices);
      request.generated_index_count = saturating_u32(generatedIndices);
      request.cache_key = build_tessellation_cache_key(*geometry, transform, materialId);
      if (tessellation.projection == "sphere") {
        request.projection_mode = 1u;
        request.projection_center = Vec3{translation.x, translation.y, translation.z};
        request.projection_radius = std::max(
            std::max(std::fabs(scale.x), std::fabs(scale.y)),
            std::max(std::fabs(scale.z), 0.001f));
      }
      request.gpu_preferred = tessellation.gpu_preferred;
      request.cache_generated_geometry = tessellation.cache_generated_geometry;
      request.displacement = tessellation.displacement;
      scene.tessellation_requests.push_back(request);
    }
  }

  std::unordered_set<vkpt::core::StableId> ecsSdfIds;
  for (const auto& entity : doc.entities) {
    if (!entity.has_sdf_primitive) {
      continue;
    }
    const auto transform = resolve_entity_transform(
        entity.id, entity.has_transform ? &entity.transform : nullptr, nullptr);
    RTSdfPrimitive out{};
    out.shape = parse_sdf_shape(entity.sdf_primitive.shape);
    out.position = parse_shape_position(transform.translation);
    out.rotation = parse_shape_rotation(transform.rotation);
    out.scale = parse_shape_scale(transform.scale);
    out.radius = std::max(0.01f, entity.sdf_primitive.radius);
    out.param_a = entity.sdf_primitive.param_a;
    out.param_b = entity.sdf_primitive.param_b;
    out.material_index = 0u;
    if (entity.material.material_id != 0) {
      if (auto mi = materialLookup.find(entity.material.material_id); mi != materialLookup.end()) {
        out.material_index = mi->second;
      }
    }
    if (out.shape != SdfShape::Unknown) {
      scene.sdf_primitives.push_back(out);
      ecsSdfIds.insert(entity.id);
    }
  }

  for (const auto& prim : doc.sdf_primitives) {
    if (ecsSdfIds.contains(prim.id)) {
      continue;
    }
    RTSdfPrimitive out{};
    out.shape = parse_sdf_shape(prim.shape);
    out.position = parse_shape_position(prim.transform.translation);
    out.rotation = parse_shape_rotation(prim.transform.rotation);
    out.scale = parse_shape_scale(prim.transform.scale);
    out.radius = std::max(0.01f, prim.primitive.radius);
    out.param_a = prim.primitive.param_a;
    out.param_b = prim.primitive.param_b;
    if (out.shape != SdfShape::Unknown) {
      out.material_index = 0u;
      scene.sdf_primitives.push_back(out);
    }
  }

  auto add_light = [&](vkpt::core::StableId id, const vkpt::scene::LightComponent& light) {
    if (light.intensity <= 0.0f) {
      return;
    }
    if (is_environment_light_type(light.type)) {
      const Vec3 env = environment_color_from_light(light);
      scene.environment_color = {
          scene.environment_color.x + env.x,
          scene.environment_color.y + env.y,
          scene.environment_color.z + env.z};
      return;
    }
    bool hasTransform = false;
    const auto* entity = entityById.contains(id) ? entityById[id] : nullptr;
    const auto transform = resolve_entity_transform(
        id,
        entity != nullptr && entity->has_transform ? &entity->transform : nullptr,
        &hasTransform);
    Vec3 pos{};
    if (hasTransform) {
      pos = {transform.translation.x, transform.translation.y, transform.translation.z};
    }
    if (!hasTransform) {
      pos = {0.0f, 1.8f, 0.0f};
    }
    const auto direction = light_direction_from_component(light, transform);
    const auto [spotInnerCos, spotOuterCos] = spot_cone_cosines(light);
    scene.lights.push_back(RTHitLight{
      pos,
      {light.color.x, light.color.y, light.color.z},
      light.intensity,
      std::max(0.0f, light.radius),
      direction,
      spotInnerCos,
      spotOuterCos});
  };

  std::unordered_set<vkpt::core::StableId> ecsLightIds;
  for (const auto& entity : doc.entities) {
    if (entity.has_light) {
      add_light(entity.id, entity.light);
      ecsLightIds.insert(entity.id);
    }
  }

  for (const auto& light : doc.lights) {
    if (!ecsLightIds.contains(light.id)) {
      add_light(light.id, light.light);
    }
  }

  auto apply_camera = [&](vkpt::core::StableId id,
                          const vkpt::scene::CameraComponent& camera,
                          bool* has_transform) {
    bool cameraTransform = false;
    const auto* entity = entityById.contains(id) ? entityById[id] : nullptr;
    const auto transform = resolve_entity_transform(
        id,
        entity != nullptr && entity->has_transform ? &entity->transform : nullptr,
        &cameraTransform);
    scene.camera_fov_deg = camera.fov;
    scene.camera_focal_length_mm = camera.focal_length_mm;
    scene.camera_sensor_width_mm = camera.sensor_width_mm;
    scene.camera_sensor_height_mm = camera.sensor_height_mm;
    scene.camera_aperture_radius = camera.aperture_radius;
    scene.camera_focus_distance = camera.focus_distance;
    scene.camera_f_stop = camera.f_stop;
    scene.camera_shutter_seconds = camera.shutter_seconds;
    scene.camera_iso = camera.iso;
    scene.camera_exposure_compensation = camera.exposure_compensation;
    scene.camera_white_balance_kelvin = camera.white_balance_kelvin;
    scene.camera_iris_blade_count = camera.iris_blade_count;
    scene.camera_iris_rotation_degrees = camera.iris_rotation_degrees;
    scene.camera_iris_roundness = camera.iris_roundness;
    scene.camera_anamorphic_squeeze = camera.anamorphic_squeeze;
    if (cameraTransform) {
      scene.camera_position = {transform.translation.x, transform.translation.y, transform.translation.z};
      const auto forward = normalize(rotate_quat(Vec3{0.0f, 0.0f, -1.0f}, transform.rotation));
      const auto up = normalize(rotate_quat(Vec3{0.0f, 1.0f, 0.0f}, transform.rotation));
      scene.camera_target = scene.camera_position + forward;
      if (length_sq(up) > kEpsilon * kEpsilon) {
        scene.camera_up = up;
      }
    }
    if (has_transform) {
      *has_transform = cameraTransform;
    }
  };

  bool hasCameraEntity = false;
  bool hasCameraTransform = false;
  for (const auto& entity : doc.entities) {
    if (entity.has_camera) {
      hasCameraEntity = true;
      apply_camera(entity.id, entity.camera, &hasCameraTransform);
      break;
    }
  }

  if (!hasCameraEntity && !doc.cameras.empty()) {
    const auto& camera = doc.cameras.front();
    hasCameraEntity = true;
    apply_camera(camera.id, camera.camera, &hasCameraTransform);
  }

  if (hasCameraEntity && !hasCameraTransform && !scene.vertices.empty()) {
    Vec3 bmin = scene.vertices.front();
    Vec3 bmax = scene.vertices.front();
    for (const auto& v : scene.vertices) {
      bmin.x = std::min(bmin.x, v.x);
      bmin.y = std::min(bmin.y, v.y);
      bmin.z = std::min(bmin.z, v.z);
      bmax.x = std::max(bmax.x, v.x);
      bmax.y = std::max(bmax.y, v.y);
      bmax.z = std::max(bmax.z, v.z);
    }
    const Vec3 center{(bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f, (bmin.z + bmax.z) * 0.5f};
    const Vec3 extent{bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z};
    const float radius = std::max(0.5f, 0.5f * std::max(extent.x, std::max(extent.y, extent.z)));
    scene.camera_target = center;
    scene.camera_position = center + Vec3{0.0f, std::max(0.6f, 0.4f * radius), std::max(2.0f, 2.2f * radius)};

    if (scene.lights.empty()) {
        scene.lights.push_back(RTHitLight{
          center + Vec3{0.0f, std::max(1.0f, 1.2f * radius), 0.0f},
          {6.0f, 6.0f, 6.0f},
          10.0f,
          0.2f});
    }
  }

  if (scene.vertices.empty() || scene.indices.empty()) {
    scene.materials.clear();
    scene.materials.push_back(RTMaterial{{0.75f, 0.75f, 0.75f}, {0.0f, 0.0f, 0.0f}, 1.0f});
    scene.materials.push_back(RTMaterial{{0.7f, 0.2f, 0.2f}, {0.0f, 0.0f, 0.0f}, 1.0f});
    scene.materials.push_back(RTMaterial{{0.2f, 0.7f, 0.2f}, {0.0f, 0.0f, 0.0f}, 1.0f});
    scene.materials.push_back(RTMaterial{{0.95f, 0.95f, 0.95f}, {8.0f, 8.0f, 8.0f}, 1.0f});

    scene.vertices = {
        {-1.0f, 0.0f, -1.0f}, {1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, -1.0f}, {-1.0f, 2.0f, -1.0f},
        {-1.0f, 0.0f, 1.0f},  {1.0f, 0.0f, 1.0f},  {1.0f, 2.0f, 1.0f},  {-1.0f, 2.0f, 1.0f},
    };
    scene.texcoords.assign(scene.vertices.size(), Vec2{});
    scene.indices = {
        0, 1, 2, 0, 2, 3,  // floor
        4, 5, 1, 4, 1, 0,  // floor duplicate for simple enclosure
        3, 2, 6, 3, 6, 7,  // right wall
        0, 3, 7, 0, 7, 4,  // left wall
        1, 5, 6, 1, 6, 2,  // near wall
        4, 0, 3, 4, 3, 7,  // far wall
    };
    RTInstance fallbackInstance{};
    fallbackInstance.geometry_id = 0u;
    fallbackInstance.first_triangle = 0u;
    fallbackInstance.triangle_count = static_cast<uint32_t>(scene.indices.size() / 3u);
    fallbackInstance.material_index = 0u;
    fallbackInstance.translation = {0.0f, 0.0f, 0.0f};
    fallbackInstance.scale = {1.0f, 1.0f, 1.0f};
    scene.instances.push_back(fallbackInstance);
    scene.sdf_primitives.push_back(
        RTSdfPrimitive{SdfShape::Sphere, {0.0f, 1.8f, 0.0f}, {0.35f, 0.35f, 0.35f}, {0.0f, 0.0f, 0.0f}, 3u, 0.35f, 0.0f, 0.0f});
    scene.lights.push_back(RTHitLight{{0.0f, 1.8f, 0.0f}, {6.0f, 6.0f, 6.0f}, 10.0f, 0.2f});
  }

  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                    "traceprobe",
                                    "scene converted to RTSceneData",
                                    {
                                      {"vertices", std::to_string(scene.vertices.size())},
                                      {"indices", std::to_string(scene.indices.size())},
                                      {"texcoords", std::to_string(scene.texcoords.size())},
                                      {"instances", std::to_string(scene.instances.size())},
                                      {"tessellation_requests", std::to_string(scene.tessellation_requests.size())},
                                      {"sdf_primitives", std::to_string(scene.sdf_primitives.size())},
                                      {"materials", std::to_string(scene.materials.size())},
                                      {"textures", std::to_string(scene.textures.size())},
                                      {"lights", std::to_string(scene.lights.size())},
                                      {"camera_pos", std::to_string(scene.camera_position.x) + "," +
                                         std::to_string(scene.camera_position.y) + "," + std::to_string(scene.camera_position.z)},
                                      {"camera_target", std::to_string(scene.camera_target.x) + "," +
                                         std::to_string(scene.camera_target.y) + "," + std::to_string(scene.camera_target.z)}
                                    });

  return vkpt::core::Result<RTSceneData>::ok(std::move(scene));
}

}  // namespace vkpt::pathtracer
