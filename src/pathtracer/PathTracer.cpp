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

}  // namespace

namespace vkpt::pathtracer {

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
                                   const std::vector<RTInstanceTransformUpdate>& updates) {
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



