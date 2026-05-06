#include "pathtracer/PathTracer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <utility>

namespace {

constexpr float kEpsilon = 1e-4f;

vkpt::pathtracer::Vec3 operator+(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

vkpt::pathtracer::Vec3 operator*(const vkpt::pathtracer::Vec3& lhs, float rhs) {
  return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

vkpt::pathtracer::Vec3 cross(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {
      lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.z * rhs.x - lhs.x * rhs.z,
      lhs.x * rhs.y - lhs.y * rhs.x,
  };
}

vkpt::pathtracer::Vec3 rotate_quat(const vkpt::pathtracer::Vec3& value,
                                   const vkpt::pathtracer::Quat4& rotation) {
  const float len_sq = rotation.x * rotation.x +
                       rotation.y * rotation.y +
                       rotation.z * rotation.z +
                       rotation.w * rotation.w;
  if (len_sq <= kEpsilon * kEpsilon) {
    return value;
  }
  const float inv_len = 1.0f / std::sqrt(len_sq);
  const vkpt::pathtracer::Vec3 qv{
      rotation.x * inv_len,
      rotation.y * inv_len,
      rotation.z * inv_len};
  const float qw = rotation.w * inv_len;
  const auto t = cross(qv, value) * 2.0f;
  return value + t * qw + cross(qv, t);
}

vkpt::pathtracer::Vec3 transform_instance_vertex(
    const vkpt::pathtracer::Vec3& local,
    const vkpt::pathtracer::RTInstance& instance) {
  const vkpt::pathtracer::Vec3 scaled{
      local.x * instance.scale.x,
      local.y * instance.scale.y,
      local.z * instance.scale.z};
  return rotate_quat(scaled, instance.rotation) + instance.translation;
}

bool same_vec2(const vkpt::pathtracer::Vec2& lhs,
               const vkpt::pathtracer::Vec2& rhs) {
  return lhs.u == rhs.u && lhs.v == rhs.v;
}

bool same_vec3(const vkpt::pathtracer::Vec3& lhs,
               const vkpt::pathtracer::Vec3& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool same_quat(const vkpt::pathtracer::Quat4& lhs,
               const vkpt::pathtracer::Quat4& rhs) {
  return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z && lhs.w == rhs.w;
}

template <typename T, typename Equal>
bool same_vector(const std::vector<T>& lhs, const std::vector<T>& rhs, Equal equal) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!equal(lhs[i], rhs[i])) {
      return false;
    }
  }
  return true;
}

bool same_material(const vkpt::pathtracer::RTMaterial& lhs,
                   const vkpt::pathtracer::RTMaterial& rhs) {
  return same_vec3(lhs.albedo, rhs.albedo) &&
         same_vec3(lhs.emissive, rhs.emissive) &&
         lhs.roughness == rhs.roughness &&
         lhs.metallic == rhs.metallic &&
         lhs.ior == rhs.ior &&
         lhs.transmission == rhs.transmission &&
         lhs.clearcoat == rhs.clearcoat &&
         lhs.sheen == rhs.sheen &&
         lhs.anisotropy == rhs.anisotropy &&
         lhs.alpha == rhs.alpha &&
         lhs.material_model == rhs.material_model &&
         lhs.material_effect == rhs.material_effect &&
         lhs.material_flags == rhs.material_flags &&
         lhs.base_color_texture_index == rhs.base_color_texture_index &&
         lhs.normal_texture_index == rhs.normal_texture_index;
}

bool same_instance(const vkpt::pathtracer::RTInstance& lhs,
                   const vkpt::pathtracer::RTInstance& rhs) {
  return lhs.entity_id == rhs.entity_id &&
         lhs.geometry_id == rhs.geometry_id &&
         lhs.first_triangle == rhs.first_triangle &&
         lhs.triangle_count == rhs.triangle_count &&
         lhs.material_index == rhs.material_index &&
         lhs.flags == rhs.flags &&
         lhs.transform_revision == rhs.transform_revision &&
         lhs.local_first_vertex == rhs.local_first_vertex &&
         lhs.local_vertex_count == rhs.local_vertex_count &&
         lhs.local_first_index == rhs.local_first_index &&
         lhs.local_index_count == rhs.local_index_count &&
         same_vec3(lhs.translation, rhs.translation) &&
         same_quat(lhs.rotation, rhs.rotation) &&
         same_vec3(lhs.scale, rhs.scale);
}

bool same_tessellation_request(const vkpt::pathtracer::RTTessellationRequest& lhs,
                               const vkpt::pathtracer::RTTessellationRequest& rhs) {
  return lhs.geometry_id == rhs.geometry_id &&
         lhs.first_triangle == rhs.first_triangle &&
         lhs.source_triangle_count == rhs.source_triangle_count &&
         lhs.factor == rhs.factor &&
         lhs.generated_vertex_count == rhs.generated_vertex_count &&
         lhs.generated_index_count == rhs.generated_index_count &&
         lhs.cache_key == rhs.cache_key &&
         same_vec3(lhs.projection_center, rhs.projection_center) &&
         lhs.projection_radius == rhs.projection_radius &&
         lhs.projection_mode == rhs.projection_mode &&
         lhs.gpu_preferred == rhs.gpu_preferred &&
         lhs.cache_generated_geometry == rhs.cache_generated_geometry &&
         lhs.displacement == rhs.displacement;
}

bool same_sdf(const vkpt::pathtracer::RTSdfPrimitive& lhs,
              const vkpt::pathtracer::RTSdfPrimitive& rhs) {
  return lhs.shape == rhs.shape &&
         same_vec3(lhs.position, rhs.position) &&
         same_vec3(lhs.scale, rhs.scale) &&
         same_vec3(lhs.rotation, rhs.rotation) &&
         lhs.material_index == rhs.material_index &&
         lhs.radius == rhs.radius &&
         lhs.param_a == rhs.param_a &&
         lhs.param_b == rhs.param_b;
}

bool same_light(const vkpt::pathtracer::RTHitLight& lhs,
                const vkpt::pathtracer::RTHitLight& rhs) {
  return same_vec3(lhs.position, rhs.position) &&
         same_vec3(lhs.color, rhs.color) &&
         lhs.intensity == rhs.intensity &&
         lhs.radius == rhs.radius &&
         same_vec3(lhs.direction, rhs.direction) &&
         lhs.spot_inner_cos == rhs.spot_inner_cos &&
         lhs.spot_outer_cos == rhs.spot_outer_cos;
}

bool same_environment_map(const vkpt::pathtracer::RTSceneData& lhs,
                          const vkpt::pathtracer::RTSceneData& rhs) {
  return lhs.environment_map_width == rhs.environment_map_width &&
         lhs.environment_map_height == rhs.environment_map_height &&
         same_vec3(lhs.environment_map_scale, rhs.environment_map_scale) &&
         same_vector(lhs.environment_map, rhs.environment_map, same_vec3);
}

bool same_camera_scene_fields(const vkpt::pathtracer::RTSceneData& lhs,
                              const vkpt::pathtracer::RTSceneData& rhs) {
  return same_vec3(lhs.camera_position, rhs.camera_position) &&
         same_vec3(lhs.camera_target, rhs.camera_target) &&
         same_vec3(lhs.camera_up, rhs.camera_up) &&
         lhs.camera_fov_deg == rhs.camera_fov_deg &&
         lhs.camera_focal_length_mm == rhs.camera_focal_length_mm &&
         lhs.camera_sensor_width_mm == rhs.camera_sensor_width_mm &&
         lhs.camera_sensor_height_mm == rhs.camera_sensor_height_mm &&
         lhs.camera_aperture_radius == rhs.camera_aperture_radius &&
         lhs.camera_focus_distance == rhs.camera_focus_distance &&
         lhs.camera_f_stop == rhs.camera_f_stop &&
         lhs.camera_shutter_seconds == rhs.camera_shutter_seconds &&
         lhs.camera_iso == rhs.camera_iso &&
         lhs.camera_exposure_compensation == rhs.camera_exposure_compensation &&
         lhs.camera_white_balance_kelvin == rhs.camera_white_balance_kelvin &&
         lhs.camera_iris_blade_count == rhs.camera_iris_blade_count &&
         lhs.camera_iris_rotation_degrees == rhs.camera_iris_rotation_degrees &&
         lhs.camera_iris_roundness == rhs.camera_iris_roundness &&
         lhs.camera_anamorphic_squeeze == rhs.camera_anamorphic_squeeze;
}

}  // namespace

namespace vkpt::pathtracer {

Vec3 SampleSceneEnvironment(const RTSceneData& scene, const Vec3& direction) {
  const auto width = scene.environment_map_width;
  const auto height = scene.environment_map_height;
  if (width == 0u || height == 0u ||
      scene.environment_map.size() < static_cast<std::size_t>(width) * height) {
    return scene.environment_color;
  }

  constexpr float kInvTwoPi = 0.15915494309189533577f;
  constexpr float kInvPi = 0.31830988618379067154f;
  const float len_sq = direction.x * direction.x +
                       direction.y * direction.y +
                       direction.z * direction.z;
  if (len_sq <= 1.0e-12f) {
    return scene.environment_color;
  }
  const float inv_len = 1.0f / std::sqrt(len_sq);
  const Vec3 dir{direction.x * inv_len, direction.y * inv_len, direction.z * inv_len};
  float u = 0.5f + std::atan2(dir.z, dir.x) * kInvTwoPi;
  u -= std::floor(u);
  const float v = std::acos(std::clamp(dir.y, -1.0f, 1.0f)) * kInvPi;

  const float x = u * static_cast<float>(width);
  const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(height - 1u);
  const auto x0 = static_cast<uint32_t>(std::floor(x)) % width;
  const auto x1 = (x0 + 1u) % width;
  const auto y0 = static_cast<uint32_t>(std::floor(y));
  const auto y1 = std::min(y0 + 1u, height - 1u);
  const float tx = x - std::floor(x);
  const float ty = y - std::floor(y);

  const auto texel = [&](uint32_t px, uint32_t py) -> Vec3 {
    return scene.environment_map[static_cast<std::size_t>(py) * width + px];
  };
  const Vec3 c00 = texel(x0, y0);
  const Vec3 c10 = texel(x1, y0);
  const Vec3 c01 = texel(x0, y1);
  const Vec3 c11 = texel(x1, y1);
  const Vec3 top{
      c00.x + (c10.x - c00.x) * tx,
      c00.y + (c10.y - c00.y) * tx,
      c00.z + (c10.z - c00.z) * tx};
  const Vec3 bottom{
      c01.x + (c11.x - c01.x) * tx,
      c01.y + (c11.y - c01.y) * tx,
      c01.z + (c11.z - c01.z) * tx};
  Vec3 color{
      top.x + (bottom.x - top.x) * ty,
      top.y + (bottom.y - top.y) * ty,
      top.z + (bottom.z - top.z) * ty};

  Vec3 scale = scene.environment_map_scale;
  if (std::max({scale.x, scale.y, scale.z}) <= 1.0e-6f) {
    scale = {1.0f, 1.0f, 1.0f};
  }
  color.x *= scale.x;
  color.y *= scale.y;
  color.z *= scale.z;
  return color;
}

PathTraceSettings MakePathTraceSettings(const RenderSettings& settings) {
  PathTraceSettings out;
  out.width = settings.width;
  out.height = settings.height;
  out.spp = settings.spp;
  out.seed = settings.seed;
  out.deterministic = settings.deterministic;
  out.integrator.max_depth = std::max(1u, settings.max_depth);
  out.integrator.enable_nee = settings.enable_nee;
  out.integrator.enable_mis = settings.enable_mis;
  out.integrator.russian_roulette_start_depth = settings.russian_roulette_start_depth;
  out.integrator.russian_roulette_min_survival = settings.russian_roulette_min_survival;
  out.integrator.russian_roulette_max_survival = settings.russian_roulette_max_survival;
  out.camera.aperture_radius = std::max(0.0f, settings.camera_aperture_radius);
  out.camera.focus_distance = std::max(0.0f, settings.camera_focus_distance);
  out.film.resolve = settings.film_resolve;
  out.film.enable_denoiser = settings.enable_denoiser;
  out.film.enable_temporal_aa = settings.enable_temporal_aa;
  return out;
}

RenderSettings MakeRenderSettings(const PathTraceSettings& settings) {
  RenderSettings out;
  out.width = settings.width;
  out.height = settings.height;
  out.spp = settings.spp;
  out.seed = settings.seed;
  out.deterministic = settings.deterministic;
  out.max_depth = std::max(1u, settings.integrator.max_depth);
  out.enable_nee = settings.integrator.enable_nee;
  out.enable_mis = settings.integrator.enable_mis;
  out.russian_roulette_start_depth = settings.integrator.russian_roulette_start_depth;
  out.russian_roulette_min_survival = settings.integrator.russian_roulette_min_survival;
  out.russian_roulette_max_survival = settings.integrator.russian_roulette_max_survival;
  out.camera_aperture_radius = std::max(0.0f, settings.camera.aperture_radius);
  out.camera_focus_distance = std::max(0.0f, settings.camera.focus_distance);
  out.enable_denoiser = settings.film.enable_denoiser;
  out.enable_temporal_aa = settings.film.enable_temporal_aa;
  out.film_resolve = settings.film.resolve;
  return out;
}

RTCameraState ExtractCameraState(const RTSceneData& scene) {
  RTCameraState out;
  out.position = scene.camera_position;
  out.target = scene.camera_target;
  out.up = scene.camera_up;
  out.fov_deg = scene.camera_fov_deg;
  out.focal_length_mm = scene.camera_focal_length_mm;
  out.sensor_width_mm = scene.camera_sensor_width_mm;
  out.sensor_height_mm = scene.camera_sensor_height_mm;
  out.aperture_radius = scene.camera_aperture_radius;
  out.focus_distance = scene.camera_focus_distance;
  out.f_stop = scene.camera_f_stop;
  out.shutter_seconds = scene.camera_shutter_seconds;
  out.iso = scene.camera_iso;
  out.exposure_compensation = scene.camera_exposure_compensation;
  out.white_balance_kelvin = scene.camera_white_balance_kelvin;
  out.iris_blade_count = scene.camera_iris_blade_count;
  out.iris_rotation_degrees = scene.camera_iris_rotation_degrees;
  out.iris_roundness = scene.camera_iris_roundness;
  out.anamorphic_squeeze = scene.camera_anamorphic_squeeze;
  return out;
}

void ApplyCameraState(RTSceneData& scene, const RTCameraState& camera) {
  scene.camera_position = camera.position;
  scene.camera_target = camera.target;
  scene.camera_up = camera.up;
  scene.camera_fov_deg = camera.fov_deg;
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
}

bool ApplyInstanceTransformUpdates(RTSceneData& scene,
                                   const std::vector<RTInstanceTransformUpdate>& updates,
                                   RTInstanceTransformApplyMode mode) {
  bool changed = false;
  for (const auto& update : updates) {
    uint32_t instance_index = update.instance_index;
    if (instance_index >= scene.instances.size() && update.entity_id != 0u) {
      for (std::size_t index = 0; index < scene.instances.size(); ++index) {
        if (scene.instances[index].entity_id == update.entity_id) {
          instance_index = static_cast<uint32_t>(index);
          break;
        }
      }
    }
    if (instance_index >= scene.instances.size()) {
      continue;
    }

    auto& instance = scene.instances[instance_index];
    instance.translation = update.translation;
    instance.rotation = update.rotation;
    instance.scale = update.scale;
    instance.flags |= update.flags;
    if (update.transform_revision != 0u) {
      instance.transform_revision = update.transform_revision;
    }
    changed = true;

    if (mode == RTInstanceTransformApplyMode::MetadataOnly) {
      continue;
    }

    const uint32_t first_scene_index = instance.first_triangle * 3u;
    if (instance.local_vertex_count == 0u ||
        instance.local_first_vertex >= scene.local_vertices.size() ||
        first_scene_index >= scene.indices.size() ||
        instance.local_first_index >= scene.local_indices.size()) {
      continue;
    }

    const uint32_t scene_index = scene.indices[first_scene_index];
    const uint32_t local_index = scene.local_indices[instance.local_first_index];
    if (scene_index < local_index) {
      continue;
    }
    const uint32_t first_scene_vertex = scene_index - local_index;
    const uint64_t local_end =
        static_cast<uint64_t>(instance.local_first_vertex) + instance.local_vertex_count;
    const uint64_t scene_end =
        static_cast<uint64_t>(first_scene_vertex) + instance.local_vertex_count;
    if (local_end > scene.local_vertices.size() || scene_end > scene.vertices.size()) {
      continue;
    }

    for (uint32_t vertex = 0u; vertex < instance.local_vertex_count; ++vertex) {
      scene.vertices[first_scene_vertex + vertex] = transform_instance_vertex(
          scene.local_vertices[instance.local_first_vertex + vertex],
          instance);
    }
  }
  return changed;
}

bool ApplySceneDeltaUpdate(RTSceneData& scene, const RTSceneDeltaUpdate& update) {
  for (const auto& material : update.materials) {
    if (material.material_index >= scene.materials.size()) {
      return false;
    }
  }
  for (const auto& light : update.lights) {
    if (light.light_index >= scene.lights.size()) {
      return false;
    }
  }

  bool changed = false;
  for (const auto& material : update.materials) {
    scene.materials[material.material_index] = material.material;
    changed = true;
  }
  for (const auto& light : update.lights) {
    scene.lights[light.light_index] = light.light;
    changed = true;
  }
  if (update.environment_color_changed) {
    scene.environment_color = update.environment_color;
    changed = true;
  }
  return changed;
}

void MergeSceneDeltaUpdates(RTSceneDeltaUpdate& dst, const RTSceneDeltaUpdate& src) {
  auto mergeMaterial = [&](const RTMaterialUpdate& update) {
    for (auto& existing : dst.materials) {
      if (existing.material_index == update.material_index) {
        existing = update;
        return;
      }
    }
    dst.materials.push_back(update);
  };
  auto mergeLight = [&](const RTLightUpdate& update) {
    for (auto& existing : dst.lights) {
      if (existing.light_index == update.light_index) {
        existing = update;
        return;
      }
    }
    dst.lights.push_back(update);
  };
  for (const auto& material : src.materials) {
    mergeMaterial(material);
  }
  for (const auto& light : src.lights) {
    mergeLight(light);
  }
  if (src.environment_color_changed) {
    dst.environment_color_changed = true;
    dst.environment_color = src.environment_color;
  }
}

std::optional<RTSceneDeltaUpdate> BuildSceneDeltaUpdate(const RTSceneData& before,
                                                        const RTSceneData& after) {
  const bool structural_same =
      same_vector(before.vertices, after.vertices, same_vec3) &&
      same_vector(before.texcoords, after.texcoords, same_vec2) &&
      before.indices == after.indices &&
      same_vector(before.local_vertices, after.local_vertices, same_vec3) &&
      before.local_indices == after.local_indices &&
      same_vector(before.instances, after.instances, same_instance) &&
      same_vector(before.tessellation_requests, after.tessellation_requests, same_tessellation_request) &&
      same_vector(before.sdf_primitives, after.sdf_primitives, same_sdf) &&
      before.textures == after.textures &&
      same_environment_map(before, after) &&
      before.materials.size() == after.materials.size() &&
      before.lights.size() == after.lights.size() &&
      same_camera_scene_fields(before, after);
  if (!structural_same) {
    return std::nullopt;
  }

  RTSceneDeltaUpdate delta;
  for (std::size_t index = 0; index < before.materials.size(); ++index) {
    if (!same_material(before.materials[index], after.materials[index])) {
      delta.materials.push_back(RTMaterialUpdate{
          static_cast<uint32_t>(index),
          after.materials[index]});
    }
  }
  for (std::size_t index = 0; index < before.lights.size(); ++index) {
    if (!same_light(before.lights[index], after.lights[index])) {
      delta.lights.push_back(RTLightUpdate{
          static_cast<uint32_t>(index),
          after.lights[index]});
    }
  }
  if (!same_vec3(before.environment_color, after.environment_color)) {
    delta.environment_color_changed = true;
    delta.environment_color = after.environment_color;
  }
  return delta;
}

std::string SerializePathTraceSettings(const PathTraceSettings& settings) {
  std::ostringstream out;
  out << "{";
  out << "\"width\":" << settings.width << ",";
  out << "\"height\":" << settings.height << ",";
  out << "\"spp\":" << settings.spp << ",";
  out << "\"seed\":" << settings.seed << ",";
  out << "\"deterministic\":" << (settings.deterministic ? "true" : "false") << ",";
  out << "\"integrator\":{";
  out << "\"max_depth\":" << settings.integrator.max_depth << ",";
  out << "\"enable_nee\":" << (settings.integrator.enable_nee ? "true" : "false") << ",";
  out << "\"enable_mis\":" << (settings.integrator.enable_mis ? "true" : "false") << ",";
  out << "\"russian_roulette_start_depth\":" << settings.integrator.russian_roulette_start_depth << ",";
  out << "\"russian_roulette_min_survival\":" << settings.integrator.russian_roulette_min_survival << ",";
  out << "\"russian_roulette_max_survival\":" << settings.integrator.russian_roulette_max_survival;
  out << "},";
  out << "\"camera\":{";
  out << "\"aperture_radius\":" << settings.camera.aperture_radius << ",";
  out << "\"focus_distance\":" << settings.camera.focus_distance;
  out << "},";
  out << "\"film\":{";
  out << "\"enable_denoiser\":" << (settings.film.enable_denoiser ? "true" : "false") << ",";
  out << "\"enable_temporal_aa\":" << (settings.film.enable_temporal_aa ? "true" : "false") << ",";
  out << "\"resolve\":" << SerializeFilmResolveSettings(settings.film.resolve);
  out << "}";
  out << "}";
  return out.str();
}

bool NullPathTracer::configure(const RenderSettings& settings) {
  m_settings = MakeRenderSettings(MakePathTraceSettings(settings));
  m_film.resize(m_settings.width, m_settings.height);
  m_film.set_resolve_settings(m_settings.film_resolve);
  m_film.clear();
  m_counters = {};
  m_configured = true;
  m_has_scene = false;
  m_scene = {};
  return true;
}

bool NullPathTracer::load_scene_snapshot(const RTSceneData& scene) {
  if (!m_configured) {
    return false;
  }
  m_scene = scene;
  m_film.set_resolve_settings(CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_scene));
  m_has_scene = true;
  return true;
}

bool NullPathTracer::build_or_update_acceleration() {
  return m_configured;
}

bool NullPathTracer::reset_accumulation() {
  if (!m_configured) {
    return false;
  }
  m_film.clear();
  m_counters = {};
  return true;
}

bool NullPathTracer::update_camera(const Vec3& pos, const Vec3& target, const Vec3& up, float fov_deg) {
  if (!m_configured) {
    return false;
  }
  auto camera = ExtractCameraState(m_scene);
  camera.position = pos;
  camera.target = target;
  camera.up = up;
  camera.fov_deg = fov_deg;
  return update_camera_state(camera);
}

bool NullPathTracer::update_camera_state(const RTCameraState& camera) {
  if (!m_configured) {
    return false;
  }
  ApplyCameraState(m_scene, camera);
  m_film.set_resolve_settings(CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_scene));
  m_has_scene = true;
  return true;
}

bool NullPathTracer::update_instance_transforms(
    const std::vector<RTInstanceTransformUpdate>& updates) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  return ApplyInstanceTransformUpdates(m_scene, updates);
}

bool NullPathTracer::update_scene_delta(const RTSceneDeltaUpdate& update) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  return ApplySceneDeltaUpdate(m_scene, update);
}

bool NullPathTracer::render_sample_batch(uint32_t start_y,
                                         uint32_t end_y,
                                         uint32_t sample_index,
                                         uint32_t frame_index) {
  (void)start_y;
  (void)end_y;
  (void)sample_index;
  (void)frame_index;
  return m_configured;
}

void NullPathTracer::shutdown() {
  m_settings = {};
  m_scene = {};
  m_film = FilmBuffer{};
  m_counters = {};
  m_configured = false;
  m_has_scene = false;
}

}  // namespace vkpt::pathtracer



