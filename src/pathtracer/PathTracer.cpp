#include "pathtracer/PathTracer.h"

#include "cpu/SimdKernelDispatch.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <utility>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvPi = 0.31830988618f;
constexpr float kEpsilon = 1e-4f;
constexpr float kMinMarchStep = 1.0e-3f;
constexpr uint32_t kMaxMarchSteps = 192u;
constexpr float kMaxMarchDistance = 10000.0f;

void atomic_add_u64(uint64_t& value, uint64_t delta = 1u) {
  std::atomic_ref<uint64_t> ref(value);
  ref.fetch_add(delta, std::memory_order_relaxed);
}

uint64_t atomic_load_u64(const uint64_t& value) {
  std::atomic_ref<const uint64_t> ref(value);
  return ref.load(std::memory_order_relaxed);
}

uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

vkpt::jobs::JobSystem& scalar_render_jobs() {
  static vkpt::jobs::JobSystem jobs(0u);
  return jobs;
}

vkpt::pathtracer::Vec3 operator+(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

vkpt::pathtracer::Vec3 operator-(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

vkpt::pathtracer::Vec3 operator-(const vkpt::pathtracer::Vec3& value) {
  return {-value.x, -value.y, -value.z};
}

vkpt::pathtracer::Vec3 operator*(const vkpt::pathtracer::Vec3& lhs, float rhs) {
  return {lhs.x * rhs, lhs.y * rhs, lhs.z * rhs};
}

vkpt::pathtracer::Vec3 operator*(float lhs, const vkpt::pathtracer::Vec3& rhs) {
  return rhs * lhs;
}

vkpt::pathtracer::Vec3 operator*(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z};
}

vkpt::pathtracer::Vec3 operator/(const vkpt::pathtracer::Vec3& lhs, float rhs) {
  return {lhs.x / rhs, lhs.y / rhs, lhs.z / rhs};
}

float dot(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

vkpt::pathtracer::Vec3 cross(const vkpt::pathtracer::Vec3& lhs, const vkpt::pathtracer::Vec3& rhs) {
  return {
      lhs.y * rhs.z - lhs.z * rhs.y,
      lhs.z * rhs.x - lhs.x * rhs.z,
      lhs.x * rhs.y - lhs.y * rhs.x,
  };
}

float length_sq(const vkpt::pathtracer::Vec3& value) {
  return dot(value, value);
}

float length(const vkpt::pathtracer::Vec3& value) {
  return std::sqrt(length_sq(value));
}

float luminance(const vkpt::pathtracer::Vec3& value) {
  return 0.2126f * value.x + 0.7152f * value.y + 0.0722f * value.z;
}

vkpt::pathtracer::Vec3 preview_reflection_environment(const vkpt::pathtracer::Vec3& direction) {
  const float t = std::clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);
  const float horizon = std::pow(std::clamp(1.0f - std::fabs(direction.y), 0.0f, 1.0f), 2.0f);
  const vkpt::pathtracer::Vec3 floorColor{0.08f, 0.075f, 0.065f};
  const vkpt::pathtracer::Vec3 skyColor{0.34f, 0.42f, 0.58f};
  return floorColor * (1.0f - t) + skyColor * t +
         vkpt::pathtracer::Vec3{0.55f, 0.48f, 0.36f} * (horizon * 0.18f);
}

vkpt::pathtracer::Vec3 normalize(const vkpt::pathtracer::Vec3& value) {
  const float l = length(value);
  if (l <= kEpsilon) {
    return {0.0f, 1.0f, 0.0f};
  }
  return value / l;
}

vkpt::pathtracer::Vec3 rotate_x(const vkpt::pathtracer::Vec3& value, float angle) {
  const float s = std::sin(angle);
  const float c = std::cos(angle);
  return {value.x, c * value.y - s * value.z, s * value.y + c * value.z};
}

vkpt::pathtracer::Vec3 rotate_y(const vkpt::pathtracer::Vec3& value, float angle) {
  const float s = std::sin(angle);
  const float c = std::cos(angle);
  return {c * value.x + s * value.z, value.y, -s * value.x + c * value.z};
}

vkpt::pathtracer::Vec3 rotate_z(const vkpt::pathtracer::Vec3& value, float angle) {
  const float s = std::sin(angle);
  const float c = std::cos(angle);
  return {c * value.x - s * value.y, s * value.x + c * value.y, value.z};
}

vkpt::pathtracer::Vec3 rotate_euler_inv(const vkpt::pathtracer::Vec3& value, const vkpt::pathtracer::Vec3& rotation) {
  return rotate_z(rotate_y(rotate_x(value, -rotation.x), -rotation.y), -rotation.z);
}

vkpt::pathtracer::Vec3 rotate_euler(const vkpt::pathtracer::Vec3& value, const vkpt::pathtracer::Vec3& rotation) {
  return rotate_x(rotate_y(rotate_z(value, rotation.z), rotation.y), rotation.x);
}

vkpt::pathtracer::Vec3 divide_by_scale(const vkpt::pathtracer::Vec3& value, const vkpt::pathtracer::Vec3& scale) {
  const float sx = scale.x != 0.0f ? scale.x : 1.0f;
  const float sy = scale.y != 0.0f ? scale.y : 1.0f;
  const float sz = scale.z != 0.0f ? scale.z : 1.0f;
  return {value.x / sx, value.y / sy, value.z / sz};
}

float clamp01(float v) {
  if (!std::isfinite(v)) {
    return 0.0f;
  }
  return std::min(1.0f, std::max(0.0f, v));
}

float spot_attenuation(const vkpt::pathtracer::RTHitLight& light,
                       const vkpt::pathtracer::Vec3& from_light_dir) {
  if (light.spot_inner_cos <= -0.999f) {
    return 1.0f;
  }
  const float cone = dot(normalize(from_light_dir), normalize(light.direction));
  if (cone <= light.spot_outer_cos) {
    return 0.0f;
  }
  if (cone >= light.spot_inner_cos || light.spot_inner_cos <= light.spot_outer_cos) {
    return 1.0f;
  }
  const float t = std::clamp(
      (cone - light.spot_outer_cos) / (light.spot_inner_cos - light.spot_outer_cos),
      0.0f,
      1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float hash01(float x, float y, float z, float seed) {
  const float v = std::sin(x * 12.9898f + y * 78.233f + z * 37.719f + seed * 19.19f) * 43758.5453f;
  return v - std::floor(v);
}

vkpt::pathtracer::Vec3 apply_material_effect(const vkpt::pathtracer::RTMaterial& material,
                                             const vkpt::pathtracer::Vec3& position,
                                             const vkpt::pathtracer::Vec3& normal,
                                             const vkpt::pathtracer::Vec3& incoming) {
  using vkpt::pathtracer::Vec3;
  Vec3 color = material.albedo;
  const float h = hash01(position.x, position.y, position.z, static_cast<float>(material.material_effect));
  switch (material.material_effect) {
    case 1u: {  // cloth/velvet fibers
      const float rim = std::pow(std::max(0.0f, 1.0f - std::fabs(dot(normal, -incoming))), 2.0f);
      color = color * (0.65f + 0.25f * h) + Vec3{0.25f, 0.22f, 0.28f} * (rim * (0.4f + material.sheen));
      break;
    }
    case 2u: {  // procedural cells
      const float stripes = 0.5f + 0.5f * std::sin(position.x * 7.0f + position.z * 5.0f + h * 6.0f);
      color = color * (0.45f + 0.55f * stripes);
      break;
    }
    case 3u: {  // cracks/charcoal/cardboard
      const float crack = h > 0.72f ? 0.18f : 1.0f;
      color = color * crack;
      break;
    }
    case 4u: {  // marble/ice veins
      const float vein = 0.5f + 0.5f * std::sin((position.x + position.y * 0.4f + position.z * 0.7f) * 9.0f + h * 3.0f);
      color = color * (0.55f + 0.45f * vein) + Vec3{0.18f, 0.20f, 0.23f} * (1.0f - vein);
      break;
    }
    case 5u: {  // rust/earth oxidation
      color = color * (0.55f + 0.35f * h) + Vec3{0.45f, 0.16f, 0.04f} * (0.25f + 0.25f * h);
      break;
    }
    case 6u: {  // granular dust/stone/sand
      color = color * (0.65f + 0.35f * h);
      break;
    }
    case 7u: {  // skin/wax/paper subsurface hint
      color = color * 0.78f + Vec3{1.0f, 0.62f, 0.42f} * 0.16f;
      break;
    }
    case 8u: {  // thin film / holographic spectral tint
      color = color * 0.55f + Vec3{0.35f + 0.45f * h, 0.25f + 0.35f * (1.0f - h), 0.85f} * 0.45f;
      break;
    }
    case 9u: {  // alpha mask represented as a visible check pattern
      const float check = std::fmod(std::floor(position.x * 4.0f) + std::floor(position.z * 4.0f), 2.0f);
      color = color * (check < 0.5f ? 1.0f : 0.28f);
      break;
    }
    case 10u: {  // hot emission families keep a brighter warm surface
      color = color * 0.45f + Vec3{1.0f, 0.35f + 0.3f * h, 0.08f} * 0.55f;
      break;
    }
    case 11u: {  // wet/dirty streaks
      const float streak = 0.5f + 0.5f * std::sin(position.y * 18.0f + h * 5.0f);
      color = color * (0.55f + 0.45f * streak);
      break;
    }
    case 12u: {  // retro/caustic sparkle
      const float retro = std::pow(std::max(0.0f, dot(normal, -incoming)), 6.0f);
      color = color + Vec3{0.55f, 0.65f, 0.95f} * retro;
      break;
    }
    case 13u: {  // normal-map placeholder: color shows tangent-like waves
      color = color * (0.65f + 0.35f * std::fabs(std::sin(position.x * 10.0f) * std::cos(position.z * 10.0f)));
      break;
    }
    case 14u: {  // x-ray silhouette
      const float rim = std::pow(std::max(0.0f, 1.0f - std::fabs(dot(normal, -incoming))), 0.8f);
      color = Vec3{0.15f, 0.75f, 1.0f} * (0.2f + 0.8f * rim);
      break;
    }
    case 15u: {  // volumetric/smoke proxy: soft layered density bands
      const float bands = 0.5f + 0.5f * std::sin(position.y * 11.0f + position.x * 3.0f + h * 7.0f);
      const float rim = std::pow(std::max(0.0f, 1.0f - std::fabs(dot(normal, -incoming))), 1.5f);
      color = color * (0.25f + 0.45f * bands) + Vec3{0.48f, 0.56f, 0.68f} * (0.18f + 0.32f * rim);
      break;
    }
    default:
      break;
  }
  return {std::min(1.5f, std::max(0.0f, color.x)),
          std::min(1.5f, std::max(0.0f, color.y)),
          std::min(1.5f, std::max(0.0f, color.z))};
}

float schlick_fresnel(float cosTheta, float ior) {
  const float safeIor = std::max(1.01f, ior);
  float r0 = (1.0f - safeIor) / (1.0f + safeIor);
  r0 *= r0;
  const float m = 1.0f - std::min(1.0f, std::max(0.0f, cosTheta));
  return r0 + (1.0f - r0) * m * m * m * m * m;
}

vkpt::pathtracer::Vec3 refract_dir(const vkpt::pathtracer::Vec3& incoming,
                                   const vkpt::pathtracer::Vec3& normal,
                                   float eta,
                                   bool& tir) {
  const float cosi = std::min(1.0f, std::max(-1.0f, dot(incoming, normal)));
  const float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
  if (k < 0.0f) {
    tir = true;
    return {};
  }
  tir = false;
  return normalize(incoming * eta - normal * (eta * cosi + std::sqrt(k)));
}

float radians(float deg) {
  return deg * (kPi / 180.0f);
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

ScalarCpuPathTracer::Rng::Rng(const SampleKey& key) {
  const uint64_t mix =
      splitmix64(key.seed ^ splitmix64(key.frame_index) ^ splitmix64(key.sample_index) ^ splitmix64(key.path_id) ^
                 splitmix64(key.dimension) ^ splitmix64(key.path_depth));
  m_state = splitmix64(mix + splitmix64(key.pixel_index));
}

float ScalarCpuPathTracer::Rng::next01() {
  m_state = splitmix64(m_state);
  return static_cast<float>((m_state >> 8) & 0x00ffffffu) * (1.0f / 16777215.0f);
}

Vec3 ScalarCpuPathTracer::Rng::unit_sphere() {
  const float z = 2.0f * next01() - 1.0f;
  const float a = 2.0f * kPi * next01();
  const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
  return {r * std::cos(a), r * std::sin(a), z};
}

Vec3 ScalarCpuPathTracer::make_unit(const Vec3& value) const {
  return normalize(value);
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
  m_scene.camera_position = pos;
  m_scene.camera_target = target;
  m_scene.camera_up = up;
  m_scene.camera_fov_deg = fov_deg;
  return true;
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

ScalarCpuPathTracer::~ScalarCpuPathTracer() = default;

bool ScalarCpuPathTracer::set_accelerator(IRayAccelerator* accelerator) {
  m_external_accelerator = accelerator;
  m_accel_info = accelerator ? accelerator->build_info() : RayAcceleratorBuildInfo{};
  return true;
}

bool ScalarCpuPathTracer::configure(const RenderSettings& settings) {
  m_trace_settings = MakePathTraceSettings(settings);
  m_settings = MakeRenderSettings(m_trace_settings);
  m_film = FilmBuffer{m_settings.width, m_settings.height};
  m_film.set_resolve_settings(m_settings.film_resolve);
  m_film.clear();
  m_counters = {};
  m_worker_count = std::max<std::uint32_t>(
      1u,
      static_cast<std::uint32_t>(scalar_render_jobs().worker_count()));
  const auto features = vkpt::cpu::QueryCpuFeatures();
  m_simd_dispatch = vkpt::cpu::BuildSimdDispatchInfo(features);
  const auto kernel_info = vkpt::cpu::SelectSimdKernel(features).info();
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                    "pathtracer",
                                    "cpu tracer dispatch configured",
                                    {
                                      {"workers", std::to_string(m_worker_count)},
                                      {"simd_preferred", vkpt::cpu::ToString(m_simd_dispatch.preferred)},
                                      {"simd_kernel", std::string(kernel_info.name)},
                                      {"simd_lane_width", std::to_string(kernel_info.lane_width)},
                                      {"simd_available", std::to_string(m_simd_dispatch.available.size())}
                                    });
  m_configured = true;
  m_has_scene = false;
  m_scene = RTSceneData{};
  m_accelerator = CreateCpuBvhAccelerator();
  m_external_accelerator = nullptr;
  m_accel_info = {};
  return true;
}

bool ScalarCpuPathTracer::load_scene_snapshot(const RTSceneData& scene) {
  if (!m_configured) {
    return false;
  }
  m_scene = scene;
  m_film.set_resolve_settings(CameraAdjustedFilmResolveSettings(m_settings.film_resolve, m_scene));
  if (m_scene.materials.empty()) {
    m_scene.materials.push_back(RTMaterial{});
  }
  m_has_scene = true;
  return true;
}

bool ScalarCpuPathTracer::build_or_update_acceleration() {
  if (!m_has_scene) {
    return false;
  }
  if (!m_accelerator) {
    m_accelerator = CreateCpuBvhAccelerator();
  }
  IRayAccelerator* accelerator = m_external_accelerator ? m_external_accelerator : m_accelerator.get();
  if (accelerator == nullptr || !accelerator->build(m_scene, m_settings.deterministic)) {
    return false;
  }
  m_accel_info = accelerator->build_info();
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                    "pathtracer",
                                    "cpu bvh built",
                                    {
                                      {"primitives", std::to_string(m_accel_info.primitive_count)},
                                      {"nodes", std::to_string(m_accel_info.node_count)},
                                      {"leaves", std::to_string(m_accel_info.leaf_count)},
                                      {"build_ms", std::to_string(m_accel_info.build_ms)},
                                      {"deterministic", m_accel_info.deterministic ? "true" : "false"}
                                    });
  m_camera_forward = normalize(m_scene.camera_target - m_scene.camera_position);
  m_camera_right = normalize(cross(m_camera_forward, m_scene.camera_up));
  if (length_sq(m_camera_right) <= kEpsilon * kEpsilon) {
    m_camera_right = {1.0f, 0.0f, 0.0f};
  }
  m_camera_up = normalize(cross(m_camera_right, m_camera_forward));
  if (length_sq(m_camera_up) <= kEpsilon * kEpsilon) {
    m_camera_up = {0.0f, 1.0f, 0.0f};
  }
  return true;
}

bool ScalarCpuPathTracer::update_camera(const Vec3& pos, const Vec3& target, const Vec3& up, float fov_deg) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  m_scene.camera_position = pos;
  m_scene.camera_target = target;
  m_scene.camera_up = up;
  m_scene.camera_fov_deg = fov_deg;
  m_camera_forward = normalize(m_scene.camera_target - m_scene.camera_position);
  m_camera_right = normalize(cross(m_camera_forward, m_scene.camera_up));
  if (length_sq(m_camera_right) <= kEpsilon * kEpsilon) {
    m_camera_right = {1.0f, 0.0f, 0.0f};
  }
  m_camera_up = normalize(cross(m_camera_right, m_camera_forward));
  if (length_sq(m_camera_up) <= kEpsilon * kEpsilon) {
    m_camera_up = {0.0f, 1.0f, 0.0f};
  }
  return true;
}

bool ScalarCpuPathTracer::reset_accumulation() {
  if (!m_configured) {
    return false;
  }
  m_film.clear();
  m_counters = {};
  return true;
}

bool ScalarCpuPathTracer::intersect_triangle(const RTTriangle& tri,
                                            const Ray& ray,
                                            float& t,
                                            float& u,
                                            float& v,
                                            SampleCounters& counters) const {
  if (tri.i0 >= m_scene.vertices.size() || tri.i1 >= m_scene.vertices.size() || tri.i2 >= m_scene.vertices.size()) {
    return false;
  }
  ++counters.triangle_tests;
  const Vec3 p0 = m_scene.vertices[tri.i0];
  const Vec3 p1 = m_scene.vertices[tri.i1];
  const Vec3 p2 = m_scene.vertices[tri.i2];
  const Vec3 e1 = p1 - p0;
  const Vec3 e2 = p2 - p0;
  const Vec3 h = cross(ray.direction, e2);
  const float det = dot(e1, h);
  if (std::fabs(det) < kEpsilon) {
    return false;
  }
  const float invDet = 1.0f / det;
  const Vec3 s = ray.origin - p0;
  u = dot(s, h) * invDet;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }
  const Vec3 q = cross(s, e1);
  v = dot(ray.direction, q) * invDet;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }
  t = dot(e2, q) * invDet;
  return t > kEpsilon;
}

float sdf_distance(const RTSdfPrimitive& primitive, const Vec3& worldPoint) {
  const Vec3 local = divide_by_scale(rotate_euler_inv(worldPoint - primitive.position, primitive.rotation), primitive.scale);
  if (primitive.shape == SdfShape::Sphere) {
    return length(local) - primitive.radius;
  }
  if (primitive.shape == SdfShape::Box) {
    const Vec3 q = {std::fabs(local.x) - primitive.scale.x, std::fabs(local.y) - primitive.scale.y, std::fabs(local.z) - primitive.scale.z};
    const Vec3 maxq = {std::max(0.0f, q.x), std::max(0.0f, q.y), std::max(0.0f, q.z)};
    return length(maxq) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
  }
  if (primitive.shape == SdfShape::RoundedBox) {
    const Vec3 r = {primitive.param_a, primitive.param_a, primitive.param_a};
    const Vec3 q = {std::fabs(local.x) - primitive.scale.x + r.x, std::fabs(local.y) - primitive.scale.y + r.y,
                    std::fabs(local.z) - primitive.scale.z + r.z};
    const Vec3 maxq = {std::max(0.0f, q.x), std::max(0.0f, q.y), std::max(0.0f, q.z)};
    return length(maxq) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f) - primitive.param_a;
  }
  if (primitive.shape == SdfShape::Plane) {
    return local.y + primitive.param_b;
  }
  if (primitive.shape == SdfShape::Torus) {
    const float a = std::max(0.05f, primitive.param_a);
    const float b = std::max(0.02f, primitive.radius);
    const float qx = length(Vec3{local.x, 0.0f, local.z}) - a;
    return length(Vec3{qx, local.y, 0.0f}) - b;
  }
  if (primitive.shape == SdfShape::Capsule) {
    const Vec3 a = {0.0f, -primitive.param_a, 0.0f};
    const Vec3 b = {0.0f, primitive.param_a, 0.0f};
    const Vec3 ab = b - a;
    const float t = dot(local - a, ab) / std::max(kEpsilon, dot(ab, ab));
    const float c = std::max(0.0f, std::min(1.0f, t));
    const Vec3 closest = a + ab * c;
    return length(local - closest) - primitive.radius;
  }
  return length(local) - primitive.radius;
}

void sdf_normal(const RTSdfPrimitive& primitive, const Vec3& worldPoint, Vec3& out) {
  const float e = 0.0005f;
  const float dx = sdf_distance(primitive, worldPoint + Vec3{e, 0.0f, 0.0f}) - sdf_distance(primitive, worldPoint - Vec3{e, 0.0f, 0.0f});
  const float dy = sdf_distance(primitive, worldPoint + Vec3{0.0f, e, 0.0f}) - sdf_distance(primitive, worldPoint - Vec3{0.0f, e, 0.0f});
  const float dz = sdf_distance(primitive, worldPoint + Vec3{0.0f, 0.0f, e}) - sdf_distance(primitive, worldPoint - Vec3{0.0f, 0.0f, e});
  out = normalize(Vec3{dx, dy, dz} * 0.5f);
}

bool ScalarCpuPathTracer::intersect_sphere(const RTSdfPrimitive& primitive,
                                          const Ray& ray,
                                          float& t,
                                          Vec3& normal,
                                          SampleCounters& counters) const {
  ++counters.sdf_tests;

  const float radius = std::max(0.01f, primitive.radius);
  const Vec3 localOrigin =
      divide_by_scale(rotate_euler_inv(ray.origin - primitive.position, primitive.rotation), primitive.scale);
  const Vec3 localDirection = divide_by_scale(rotate_euler_inv(ray.direction, primitive.rotation), primitive.scale);
  const float a = dot(localDirection, localDirection);
  if (a <= kEpsilon * kEpsilon) {
    ++counters.sdf_steps;
    ++counters.sdf_misses;
    return false;
  }

  const float halfB = dot(localOrigin, localDirection);
  const float c = dot(localOrigin, localOrigin) - radius * radius;
  const float discriminant = halfB * halfB - a * c;
  if (discriminant < 0.0f) {
    ++counters.sdf_steps;
    ++counters.sdf_misses;
    return false;
  }

  const float root = std::sqrt(discriminant);
  float candidate = (-halfB - root) / a;
  if (candidate <= kEpsilon) {
    candidate = (-halfB + root) / a;
  }
  if (candidate <= kEpsilon || !std::isfinite(candidate)) {
    ++counters.sdf_steps;
    ++counters.sdf_misses;
    return false;
  }

  t = candidate;
  const Vec3 localHit = localOrigin + localDirection * t;
  const Vec3 localNormal = normalize(localHit);
  const Vec3 normalLocalInvScale = divide_by_scale(localNormal, primitive.scale);
  normal = normalize(rotate_euler(normalLocalInvScale, primitive.rotation));
  ++counters.sdf_steps;
  ++counters.sdf_hits;
  return true;
}

bool ScalarCpuPathTracer::intersect_box(const RTSdfPrimitive& primitive,
                                        const Ray& ray,
                                        float& t,
                                        Vec3& normal,
                                        SampleCounters& counters) const {
  ++counters.sdf_tests;
  uint32_t steps = 0u;
  for (uint32_t i = 0; i < kMaxMarchSteps; ++i) {
    ++steps;
    const Vec3 point = ray.origin + ray.direction * t;
    const float d = sdf_distance(primitive, point);
    if (d <= kEpsilon) {
      sdf_normal(primitive, point, normal);
      counters.sdf_steps += steps;
      ++counters.sdf_hits;
      return true;
    }
    t += std::max(kMinMarchStep, d);
    if (t > kMaxMarchDistance) {
      counters.sdf_steps += steps;
      ++counters.sdf_misses;
      return false;
    }
  }
  counters.sdf_steps += steps;
  ++counters.sdf_misses;
  return false;
}

bool ScalarCpuPathTracer::intersect_rounded_box(const RTSdfPrimitive& primitive,
                                               const Ray& ray,
                                               float& t,
                                               Vec3& normal,
                                               SampleCounters& counters) const {
  return intersect_box(primitive, ray, t, normal, counters);
}

bool ScalarCpuPathTracer::intersect_plane(const RTSdfPrimitive& primitive,
                                          const Ray& ray,
                                          float& t,
                                          Vec3& normal,
                                          SampleCounters& counters) const {
  return intersect_box(primitive, ray, t, normal, counters);
}

bool ScalarCpuPathTracer::intersect_torus(const RTSdfPrimitive& primitive,
                                          const Ray& ray,
                                          float& t,
                                          Vec3& normal,
                                          SampleCounters& counters) const {
  return intersect_box(primitive, ray, t, normal, counters);
}

bool ScalarCpuPathTracer::intersect_capsule(const RTSdfPrimitive& primitive,
                                            const Ray& ray,
                                            float& t,
                                            Vec3& normal,
                                            SampleCounters& counters) const {
  return intersect_box(primitive, ray, t, normal, counters);
}

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

Vec3 ScalarCpuPathTracer::evaluate_bsdf(const RTMaterial& material,
                                        const Vec3& normal,
                                        const Vec3& in_direction,
                                        const Vec3& out_direction,
                                        float& pdf) {
  (void)in_direction;
  const float cosTheta = std::max(0.0f, dot(normal, out_direction));
  pdf = cosTheta * kInvPi;
  if (pdf <= 0.0f) {
    return {0.0f, 0.0f, 0.0f};
  }
  return material.albedo * (1.0f * kInvPi);
}

Vec3 ScalarCpuPathTracer::sample_hemisphere(Rng& rng, const Vec3& normal) const {
  const float u1 = rng.next01();
  const float u2 = rng.next01();
  const float r = std::sqrt(std::max(0.0f, 1.0f - u1));
  const float phi = 2.0f * kPi * u2;
  const Vec3 local{r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, u1))};
  Vec3 tangent = cross((std::fabs(normal.z) < 0.999f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f}, normal);
  tangent = normalize(tangent);
  Vec3 bitangent = cross(normal, tangent);
  return make_unit(tangent * local.x + bitangent * local.y + normal * local.z);
}

// Phong lobe sampling.  Samples a direction around the perfect-mirror
// reflection direction.  exponent controls the spread:
//   exponent -> inf  : perfect mirror
//   exponent   = 0   : cosine-weighted hemisphere (diffuse)
// pdf (not used explicitly) = (n+1)/(2*pi) * cos^n(theta)
// Weight: throughput *= albedo (energy-conserving approx).
Vec3 ScalarCpuPathTracer::sample_phong_lobe(Rng& rng, const Vec3& refl_dir,
                                            float exponent, const Vec3& normal) const {
  const float u1  = rng.next01();
  const float u2  = rng.next01();
  const float cosT = std::pow(std::max(0.0f, u1), 1.0f / (exponent + 1.0f));
  const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
  const float phi  = 2.0f * kPi * u2;
  // Build tangent frame around refl_dir
  const Vec3 ref_t = (std::fabs(refl_dir.z) < 0.999f)
                       ? Vec3{0.0f, 0.0f, 1.0f}
                       : Vec3{0.0f, 1.0f, 0.0f};
  Vec3 tang = normalize(cross(ref_t, refl_dir));
  Vec3 btan = cross(refl_dir, tang);
  Vec3 out = normalize(tang * (sinT * std::cos(phi)) +
                       btan * (sinT * std::sin(phi)) +
                       refl_dir * cosT);
  // If sample ends up below the shading hemisphere, clamp onto it
  if (dot(out, normal) <= 0.0f) {
    out = out - 2.0f * dot(out, normal) * normal;
    if (dot(out, normal) <= 0.0f) {
      return sample_hemisphere(rng, normal);  // fallback
    }
  }
  return out;
}

NeeResult ScalarCpuPathTracer::sample_direct_light(const Hit& hit,
                                                   const Vec3& view_dir,
                                                   Rng& rng,
                                                   SampleCounters& counters) const {
  if (m_scene.lights.empty()) {
    return {};
  }
  const std::size_t num_lights = m_scene.lights.size();
  const auto light_idx = static_cast<std::size_t>(rng.next01() * static_cast<float>(num_lights));
  const std::size_t clamped_idx = std::min(light_idx, num_lights - 1u);
  const RTHitLight& light = m_scene.lights[clamped_idx];

  Vec3 sampled_light_pos = light.position;
  if (light.radius > 1.0e-5f) {
    const float uz = 1.0f - 2.0f * rng.next01();
    const float upr = std::sqrt(std::max(0.0f, 1.0f - uz * uz));
    const float phi = 6.28318530718f * rng.next01();
    sampled_light_pos += Vec3{upr * std::cos(phi), uz, upr * std::sin(phi)} * light.radius;
  }

  const Vec3 to_light = sampled_light_pos - hit.position;
  const float dist_sq = length_sq(to_light);
  const float dist = std::sqrt(dist_sq);
  if (dist < kEpsilon) {
    return {};
  }
  const Vec3 light_dir = to_light / dist;
  const float cos_theta = dot(hit.normal, light_dir);
  if (cos_theta <= 0.0f) {
    return {};
  }
  const float spotFactor = spot_attenuation(light, -light_dir);
  if (spotFactor <= 0.0f) {
    return {};
  }

  // Shadow ray — offset by normal to avoid self-intersection.
  const Ray shadow_ray{hit.position + hit.normal * 0.002f, light_dir};
  Hit shadow_hit;
  ++counters.shadow_tests;
  if (intersect_scene(shadow_ray, shadow_hit, counters) && shadow_hit.t < dist - 0.004f) {
    return {};  // occluded
  }

  const auto mat_index = std::min(hit.material_index, static_cast<uint32_t>(m_scene.materials.size() - 1));
  const auto& material = m_scene.materials[mat_index];
  const Vec3 albedo = apply_material_effect(material, hit.position, hit.normal, -light_dir);
  const Vec3 irradiance = light.color * ((light.intensity * spotFactor) / (dist_sq + kEpsilon));
  const float roughness = clamp01(material.roughness);
  const bool isMirror = material.material_model == 2u || roughness <= 0.001f;
  const bool isMetallic = material.material_model == 4u || material.metallic > 0.65f;
  const bool isTransmissive = material.material_model == 5u || material.transmission > 0.05f;
  const bool isClearcoat = material.material_model == 7u || material.clearcoat > 0.05f;

  Vec3 direct{};
  if (!isMirror && !isTransmissive) {
    direct += albedo * kInvPi * irradiance * cos_theta * static_cast<float>(num_lights);
  }
  if (isMirror || isMetallic || isClearcoat || isTransmissive || roughness < 0.65f) {
    const Vec3 half_dir = normalize(light_dir + normalize(view_dir));
    const float effectiveRoughness = std::max(0.025f, roughness * (isMetallic ? 0.65f : 1.0f));
    const float a2 = effectiveRoughness * effectiveRoughness;
    const float specPower = std::clamp(2.0f / std::max(0.0005f, a2 * a2) - 2.0f, 4.0f, 96.0f);
    const float spec = std::pow(std::max(0.0f, dot(hit.normal, half_dir)), specPower);
    const float cosView = std::max(0.0f, dot(hit.normal, normalize(view_dir)));
    float f0 = (1.0f - material.ior) / (1.0f + material.ior);
    f0 *= f0;
    const float fresnel = f0 + (1.0f - f0) * std::pow(1.0f - cosView, 5.0f);
    const float specStrength = clamp01((1.0f - roughness) * 0.8f +
                                       material.metallic * 0.45f +
                                       material.clearcoat * 0.35f +
                                       (isMirror ? 0.75f : 0.0f) +
                                       (isTransmissive ? fresnel : 0.0f));
    const Vec3 white{1.0f, 1.0f, 1.0f};
    const Vec3 specTint = white * (isMetallic ? 0.15f : 0.88f) +
                          albedo * (isMetallic ? 0.85f : 0.12f);
    direct += specTint * irradiance * (spec * cos_theta * static_cast<float>(num_lights) *
                                       std::max(0.15f, specStrength));
  }
  if (isTransmissive) {
    direct += albedo * irradiance * (cos_theta * static_cast<float>(num_lights) *
                                     (0.08f + 0.22f * material.alpha));
  }

  return NeeResult{direct, true};
}

float ScalarCpuPathTracer::light_pdf_for_direction(const Vec3& position, const Vec3& direction) const {
  if (m_scene.lights.empty()) {
    return 0.0f;
  }
  // Uniform light selection; solid-angle PDF approximation.
  const float inv_n = 1.0f / static_cast<float>(m_scene.lights.size());
  float total_pdf = 0.0f;
  for (const auto& light : m_scene.lights) {
    const Vec3 to_light = light.position - position;
    const float dist = length(to_light);
    if (dist < kEpsilon) continue;
    const Vec3 dir = to_light / dist;
    if (dot(dir, direction) > 0.9999f) {
      // Approximate: treat light as point — pdf proportional to 1/dist^2.
      total_pdf += inv_n / (dist * dist + kEpsilon);
    }
  }
  return total_pdf;
}

float MisWeight(float pdf_a, float pdf_b) {
  const float a2 = pdf_a * pdf_a;
  const float b2 = pdf_b * pdf_b;
  const float denom = a2 + b2;
  if (denom <= 0.0f) return 1.0f;
  return a2 / denom;
}

Vec3 ScalarCpuPathTracer::trace(const Ray& input,
                                uint32_t sample_index,
                                uint32_t frame_index,
                                uint32_t path_id,
                                uint32_t path_depth,
                                uint64_t& ray_counter,
                                SampleCounters& counters,
                                Rng& rng) {
  Vec3 radiance{0.0f, 0.0f, 0.0f};
  Vec3 throughput{1.0f, 1.0f, 1.0f};
  Ray ray = input;
  bool previewReflectionEnvironment = false;
  for (uint32_t depth = 0; depth < m_settings.max_depth; ++depth) {
    ++ray_counter;
    Hit hit;
    if (!intersect_scene(ray, hit, counters)) {
      Vec3 missEnvironment = m_scene.environment_color;
      if (previewReflectionEnvironment && std::max({missEnvironment.x, missEnvironment.y, missEnvironment.z}) <= 1.0e-5f) {
        missEnvironment = preview_reflection_environment(ray.direction);
      }
      radiance += throughput * missEnvironment;
      break;
    }

    Vec3 shadingNormal = hit.normal;
    if (dot(shadingNormal, -ray.direction) < 0.0f) {
      shadingNormal = -shadingNormal;
    }

    const auto index = std::min(hit.material_index, static_cast<uint32_t>(m_scene.materials.size() - 1));
    const auto& material = m_scene.materials[index];
    const Vec3 materialAlbedo = apply_material_effect(material, hit.position, shadingNormal, ray.direction);
    const float roughness = clamp01(material.roughness);
    const bool isEmissiveModel = material.material_model == 1u || material.is_emissive();
    const bool isMirror = material.material_model == 2u || roughness <= 0.001f;
    const bool isMetallic = material.material_model == 4u || material.metallic > 0.65f;
    const bool isTransmissive = material.material_model == 5u || material.transmission > 0.05f;
    const bool isDiffuse = roughness >= 0.999f && !isMirror && !isMetallic && !isTransmissive;
    const bool isSheen = material.material_model == 6u;
    const bool isClearcoat = material.material_model == 7u || material.clearcoat > 0.05f;
    const bool isToon = material.material_model == 8u;

    // Emissive: only add if NEE is off, or this is the first bounce, or MIS weights it.
    if (!m_settings.enable_nee || depth == 0) {
      radiance += throughput * material.emissive;
    } else if (m_settings.enable_mis && isEmissiveModel) {
      const float bsdf_pdf = std::max(0.0f, dot(shadingNormal, -ray.direction)) * kInvPi;
      const float l_pdf = light_pdf_for_direction(ray.origin, ray.direction);
      const float w = MisWeight(bsdf_pdf, l_pdf);
      radiance += throughput * material.emissive * w;
    }

    // NEE direct light sampling — skip for perfect mirrors (delta BSDF)
    if (m_settings.enable_nee && depth < m_settings.max_depth - 1u) {
      Hit lightSampleHit = hit;
      lightSampleHit.normal = shadingNormal;
      const NeeResult nee = sample_direct_light(lightSampleHit, -ray.direction, rng, counters);
      if (nee.valid) {
        if (m_settings.enable_mis) {
          const Vec3 to_approx_light = (m_scene.lights.empty()) ? Vec3{} :
              (m_scene.lights[0].position - hit.position);
          const Vec3 l_dir = normalize(to_approx_light);
          const float cos_theta = std::max(0.0f, dot(shadingNormal, l_dir));
          const float bsdf_pdf = cos_theta * kInvPi;
          const float l_pdf = (m_scene.lights.empty()) ? 1.0f :
              (1.0f / static_cast<float>(m_scene.lights.size()));
          const float w = MisWeight(l_pdf, bsdf_pdf);
          radiance += throughput * nee.radiance * w;
        } else {
          radiance += throughput * nee.radiance;
        }
      }
    }

    // ---- BSDF bounce -------------------------------------------------------
    Vec3 outDir;
    if (isMirror) {
      // Perfect specular reflection: r = d - 2*(d·n)*n
      outDir = ray.direction - 2.0f * dot(shadingNormal, ray.direction) * shadingNormal;
    } else if (isTransmissive) {
      const float cosTheta = std::max(0.0f, dot(shadingNormal, -ray.direction));
      const float fresnel = schlick_fresnel(cosTheta, material.ior);
      if (rng.next01() < std::min(0.98f, fresnel + material.clearcoat * 0.15f)) {
        outDir = ray.direction - 2.0f * dot(shadingNormal, ray.direction) * shadingNormal;
      } else {
        bool tir = false;
        outDir = refract_dir(ray.direction, shadingNormal, 1.0f / std::max(1.01f, material.ior), tir);
        if (tir || length_sq(outDir) <= 1.0e-8f) {
          outDir = ray.direction - 2.0f * dot(shadingNormal, ray.direction) * shadingNormal;
        }
      }
    } else if (isDiffuse || isToon) {
      outDir = sample_hemisphere(rng, shadingNormal);
    } else {
      // Glossy: Phong lobe around mirror direction
      // exponent = 2/alpha^4 - 2, alpha = roughness^2  (GGX-inspired mapping)
      const float effectiveRoughness = std::max(0.025f, roughness * (isMetallic ? 0.75f : 1.0f) * (isClearcoat ? 0.65f : 1.0f));
      const float a2   = effectiveRoughness * effectiveRoughness;
      const float expt = std::max(0.0f, 2.0f / (a2 * a2) - 2.0f);
      const Vec3 refl = ray.direction - 2.0f * dot(shadingNormal, ray.direction) * shadingNormal;
      outDir = sample_phong_lobe(rng, refl, expt, shadingNormal);
      if (isSheen && rng.next01() < material.sheen * 0.35f) {
        outDir = sample_hemisphere(rng, shadingNormal);
      }
    }

    if (!isTransmissive && dot(outDir, shadingNormal) <= 0.0f) break;
    // Throughput weight = albedo for all three cases (energy-conserving approx)
    Vec3 bounceWeight = materialAlbedo;
    if (isMetallic || isMirror) {
      bounceWeight = materialAlbedo * (0.65f + 0.35f * material.metallic);
    } else if (isTransmissive) {
      bounceWeight = materialAlbedo * (0.25f + 0.55f * material.alpha) + Vec3{0.2f, 0.2f, 0.2f};
    }
    if (isToon) {
      bounceWeight = bounceWeight * (dot(outDir, shadingNormal) > 0.55f ? 1.0f : 0.45f);
    }
    previewReflectionEnvironment = isMirror || isMetallic || isTransmissive || isClearcoat || roughness < 0.65f;
    throughput = throughput * bounceWeight;

    if (std::max(throughput.x, std::max(throughput.y, throughput.z)) < 0.001f) {
      break;
    }
    const float rr = std::min(m_trace_settings.integrator.russian_roulette_max_survival,
        std::max(m_trace_settings.integrator.russian_roulette_min_survival,
        std::max(throughput.x, std::max(throughput.y, throughput.z))));
    if (depth >= m_trace_settings.integrator.russian_roulette_start_depth && rng.next01() > rr) {
      break;
    }
    throughput = throughput / rr;
    ray.origin = hit.position + shadingNormal * 0.002f;
    ray.direction = outDir;
  }
  (void)sample_index;
  (void)frame_index;
  (void)path_id;
  (void)path_depth;
  return radiance;
}

Ray ScalarCpuPathTracer::camera_rays(uint32_t x,
                                     uint32_t y,
                                     uint32_t sample_index,
                                     uint32_t frame_index,
                                     uint32_t path_id,
                                     uint64_t& sample_seed) {
  SampleKey key{};
  key.pixel_index = static_cast<vkpt::core::StableId>(y) * m_settings.width + x;
  key.sample_index = sample_index;
  key.frame_index = frame_index;
  key.path_id = path_id;
  key.seed = m_settings.seed;
  key.dimension = 0;
  key.path_depth = 0;
  Rng rng(key);

  const float fx = (static_cast<float>(x) + rng.next01()) / std::max(1u, m_settings.width);
  const float fy = (static_cast<float>(y) + rng.next01()) / std::max(1u, m_settings.height);
  sample_seed ^= static_cast<uint64_t>(rng.next01() * 0xffffu);
  const float aspect = static_cast<float>(m_settings.width) / std::max(1.0f, static_cast<float>(m_settings.height));
  const float tanHalfFov = std::tan(0.5f * radians(m_scene.camera_fov_deg));
  const float nx = (2.0f * fx - 1.0f) * aspect * tanHalfFov;
  const float ny = (1.0f - 2.0f * fy) * tanHalfFov;
  const Vec3 dir = normalize(m_camera_forward + m_camera_right * nx + m_camera_up * ny);
  const float aperture_radius = m_scene.camera_aperture_radius > 0.0f
      ? m_scene.camera_aperture_radius
      : m_settings.camera_aperture_radius;
  const float focus_distance = m_scene.camera_focus_distance > 0.0f
      ? m_scene.camera_focus_distance
      : m_settings.camera_focus_distance;
  if (aperture_radius > 0.0f && focus_distance > kEpsilon) {
    const float lens_radius_sample = std::sqrt(rng.next01());
    const float lens_phi = 2.0f * kPi * rng.next01();
    float aperture_boundary = 1.0f;
    const uint32_t iris_blades = std::min(m_scene.camera_iris_blade_count, 64u);
    const float iris_roundness = clamp01(m_scene.camera_iris_roundness);
    if (iris_blades >= 3u && iris_roundness < 0.999f) {
      const float sector = 2.0f * kPi / static_cast<float>(iris_blades);
      const float local_phi = lens_phi - radians(m_scene.camera_iris_rotation_degrees);
      const float wrapped = local_phi - sector * std::floor(local_phi / sector);
      const float centered = wrapped > sector * 0.5f ? wrapped - sector : wrapped;
      const float polygon_boundary =
          std::cos(sector * 0.5f) / std::max(0.1f, std::cos(centered));
      aperture_boundary = polygon_boundary * (1.0f - iris_roundness) + iris_roundness;
    }
    const float lens_r = aperture_radius * lens_radius_sample * aperture_boundary;
    const float anamorphic_squeeze =
        std::isfinite(m_scene.camera_anamorphic_squeeze)
            ? std::max(0.01f, m_scene.camera_anamorphic_squeeze)
            : 1.0f;
    const Vec3 lens_offset =
        m_camera_right * (lens_r * std::cos(lens_phi) * anamorphic_squeeze) +
        m_camera_up * (lens_r * std::sin(lens_phi));
    const Vec3 focus_point = m_scene.camera_position + dir * focus_distance;
    const Vec3 origin = m_scene.camera_position + lens_offset;
    return Ray{origin, normalize(focus_point - origin)};
  }
  return Ray{m_scene.camera_position, dir};
}

bool ScalarCpuPathTracer::render_sample_batch(uint32_t start_y,
                                             uint32_t end_y,
                                             uint32_t sample_index,
                                             uint32_t frame_index) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  const uint32_t maxY = std::min(end_y, m_settings.height);
  const uint32_t minY = std::min(start_y, maxY);
  const uint64_t pixelCount64 = static_cast<uint64_t>(m_settings.width) * (maxY - minY);
  if (pixelCount64 == 0u) {
    return true;
  }
  if (pixelCount64 > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  const uint64_t firstPixel64 = static_cast<uint64_t>(minY) * m_settings.width;
  if (firstPixel64 > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  return render_sample_contiguous_pixels(static_cast<uint32_t>(firstPixel64),
                                         static_cast<uint32_t>(pixelCount64),
                                         sample_index,
                                         frame_index);
}

void ScalarCpuPathTracer::shade_pixel(uint32_t pixel,
                                      uint32_t sample_index,
                                      uint32_t frame_index,
                                      RenderBatchAccum& accum) {
  if (m_settings.width == 0u || m_settings.height == 0u) {
    return;
  }
  const uint64_t totalPixels = static_cast<uint64_t>(m_settings.width) * m_settings.height;
  if (static_cast<uint64_t>(pixel) >= totalPixels) {
    return;
  }

  accum.min_pixel = std::min(accum.min_pixel, pixel);
  accum.max_pixel = std::max(accum.max_pixel, pixel);

  const uint32_t x = pixel % m_settings.width;
  const uint32_t y = pixel / m_settings.width;

  uint64_t seed = 0;
  const uint64_t sampleSeed = (static_cast<uint64_t>(y) << 32) | x;
  const uint64_t pathId = sampleSeed + static_cast<uint64_t>(sample_index) + static_cast<uint64_t>(frame_index);
  const Ray ray = camera_rays(x, y, sample_index, frame_index, static_cast<uint32_t>(pathId), seed);
  SampleKey key{};
  key.pixel_index = sampleSeed;
  key.sample_index = sample_index;
  key.frame_index = frame_index;
  key.path_id = pathId;
  key.seed = m_settings.seed + sampleSeed + seed;
  key.path_depth = 0;
  Rng rng(key);
  uint64_t rayCounter = 0;
  Vec3 sample = trace(ray,
                      sample_index,
                      frame_index,
                      static_cast<uint32_t>(pathId),
                      0,
                      rayCounter,
                      accum.counters,
                      rng);
  if (!std::isfinite(sample.x) || !std::isfinite(sample.y) || !std::isfinite(sample.z)) {
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning,
                                      "pathtracer",
                                      "non-finite sample",
                                      {{"pixel", std::to_string(sampleSeed)}, {"sample", std::to_string(sample_index)}});
    return;
  }

  const float lum = luminance(sample);
  accum.lum_sum += lum;
  accum.sample_max = std::max(accum.sample_max, std::max(sample.x, std::max(sample.y, sample.z)));
  ++accum.sample_count;
  accum.ray_count += rayCounter;
  m_film.add_sample(x, y, sample);
}

bool ScalarCpuPathTracer::render_sample_contiguous_pixels(uint32_t first_pixel,
                                                          uint32_t pixel_count,
                                                          uint32_t sample_index,
                                                          uint32_t frame_index) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  if (pixel_count == 0u) {
    return true;
  }

  auto& jobs = scalar_render_jobs();
  jobs.set_deterministic(m_settings.deterministic);
  const uint32_t threadCount = m_settings.deterministic
      ? 1u
      : std::max(1u, std::min(m_worker_count, pixel_count));
  std::vector<RenderBatchAccum> locals(static_cast<std::size_t>(threadCount));

  if (threadCount == 1u) {
    for (uint32_t i = 0; i < pixel_count; ++i) {
      shade_pixel(first_pixel + i, sample_index, frame_index, locals[0]);
    }
  } else {
    auto run_worker = [&](uint32_t workerIndex) {
      RenderBatchAccum& local = locals[workerIndex];
      const uint64_t begin = (static_cast<uint64_t>(pixel_count) * workerIndex) / threadCount;
      const uint64_t end = (static_cast<uint64_t>(pixel_count) * (workerIndex + 1u)) / threadCount;
      for (uint64_t i = begin; i < end; ++i) {
        shade_pixel(first_pixel + static_cast<uint32_t>(i), sample_index, frame_index, local);
      }
    };

    std::vector<vkpt::core::JobHandle> handles;
    handles.reserve(threadCount);
    for (uint32_t t = 0u; t < threadCount; ++t) {
      handles.push_back(jobs.submit_job([&, t]() {
        run_worker(t);
      }));
    }
    if (!jobs.wait_group(handles)) {
      return false;
    }
  }

  return finish_render_batch(locals, sample_index, frame_index);
}

bool ScalarCpuPathTracer::render_sample_pixels(const uint32_t* pixel_indices,
                                               uint32_t pixel_count,
                                               uint32_t sample_index,
                                               uint32_t frame_index) {
  if (!m_configured || !m_has_scene) {
    return false;
  }
  if (pixel_indices == nullptr || pixel_count == 0u) {
    return true;
  }

  auto& jobs = scalar_render_jobs();
  jobs.set_deterministic(m_settings.deterministic);
  const uint32_t threadCount = m_settings.deterministic
      ? 1u
      : std::max(1u, std::min(m_worker_count, pixel_count));
  std::vector<RenderBatchAccum> locals(static_cast<std::size_t>(threadCount));

  if (threadCount == 1u) {
    for (uint32_t i = 0; i < pixel_count; ++i) {
      shade_pixel(pixel_indices[i], sample_index, frame_index, locals[0]);
    }
  } else {
    auto run_worker = [&](uint32_t workerIndex) {
      RenderBatchAccum& local = locals[workerIndex];
      for (uint32_t i = workerIndex; i < pixel_count; i += threadCount) {
        shade_pixel(pixel_indices[i], sample_index, frame_index, local);
      }
    };

    std::vector<vkpt::core::JobHandle> handles;
    handles.reserve(threadCount);
    for (uint32_t t = 0u; t < threadCount; ++t) {
      handles.push_back(jobs.submit_job([&, t]() {
        run_worker(t);
      }));
    }
    if (!jobs.wait_group(handles)) {
      return false;
    }
  }

  return finish_render_batch(locals, sample_index, frame_index);
}

bool ScalarCpuPathTracer::finish_render_batch(const std::vector<RenderBatchAccum>& locals,
                                              uint32_t sample_index,
                                              uint32_t frame_index) {
  float sampleLumSum = 0.0f;
  float sampleMax = 0.0f;
  std::uint64_t sampleCount = 0u;
  std::uint64_t rayCount = 0u;
  uint32_t minPixel = std::numeric_limits<uint32_t>::max();
  uint32_t maxPixel = 0u;
  SampleCounters counters{};
  for (const auto& local : locals) {
    sampleLumSum += local.lum_sum;
    sampleMax = std::max(sampleMax, local.sample_max);
    sampleCount += local.sample_count;
    rayCount += local.ray_count;
    counters.triangle_tests += local.counters.triangle_tests;
    counters.sdf_tests += local.counters.sdf_tests;
    counters.sdf_steps += local.counters.sdf_steps;
    counters.triangle_hits += local.counters.triangle_hits;
    counters.sdf_hits += local.counters.sdf_hits;
    counters.sdf_misses += local.counters.sdf_misses;
    counters.bvh_node_visits += local.counters.bvh_node_visits;
    counters.bvh_leaf_visits += local.counters.bvh_leaf_visits;
    counters.shadow_tests += local.counters.shadow_tests;
    if (local.sample_count > 0u) {
      minPixel = std::min(minPixel, local.min_pixel);
      maxPixel = std::max(maxPixel, local.max_pixel);
    }
  }
  atomic_add_u64(m_counters.samples, sampleCount);
  atomic_add_u64(m_counters.rays, rayCount);
  atomic_add_u64(m_counters.triangle_tests, counters.triangle_tests);
  atomic_add_u64(m_counters.sdf_tests, counters.sdf_tests);
  atomic_add_u64(m_counters.sdf_steps, counters.sdf_steps);
  atomic_add_u64(m_counters.triangle_hits, counters.triangle_hits);
  atomic_add_u64(m_counters.sdf_hits, counters.sdf_hits);
  atomic_add_u64(m_counters.sdf_misses, counters.sdf_misses);
  atomic_add_u64(m_counters.bvh_node_visits, counters.bvh_node_visits);
  atomic_add_u64(m_counters.bvh_leaf_visits, counters.bvh_leaf_visits);
  atomic_add_u64(m_counters.shadow_tests, counters.shadow_tests);

  if (sampleCount > 0u && (sample_index == 0u || ((sample_index + 1u) % 4u) == 0u || (sample_index + 1u) == m_settings.spp)) {
    const float avgLum = sampleCount == 0u ? 0.0f : (sampleLumSum / static_cast<float>(sampleCount));
    vkpt::log::Logger::instance().log(vkpt::log::Severity::Info,
                                      "traceprobe",
                                      "render_sample_pixels",
                                      {
                                        {"frame", std::to_string(frame_index)},
                                        {"sample_index", std::to_string(sample_index)},
                                        {"pixels", std::to_string(sampleCount)},
                                        {"pixel_span", std::to_string(minPixel) + "-" + std::to_string(maxPixel)},
                                        {"avg_lum", std::to_string(avgLum)},
                                        {"sample_max", std::to_string(sampleMax)},
                                        {"samples", std::to_string(sampleCount)},
                                        {"sdf_steps", std::to_string(atomic_load_u64(m_counters.sdf_steps))},
                                        {"bvh_node_visits", std::to_string(atomic_load_u64(m_counters.bvh_node_visits))}
                                      });
  }
  return true;
}

SampleCounters ScalarCpuPathTracer::read_counters() const {
  SampleCounters out;
  out.samples = atomic_load_u64(m_counters.samples);
  out.rays = atomic_load_u64(m_counters.rays);
  out.triangle_tests = atomic_load_u64(m_counters.triangle_tests);
  out.sdf_tests = atomic_load_u64(m_counters.sdf_tests);
  out.sdf_steps = atomic_load_u64(m_counters.sdf_steps);
  out.triangle_hits = atomic_load_u64(m_counters.triangle_hits);
  out.sdf_hits = atomic_load_u64(m_counters.sdf_hits);
  out.sdf_misses = atomic_load_u64(m_counters.sdf_misses);
  out.bvh_node_visits = atomic_load_u64(m_counters.bvh_node_visits);
  out.bvh_leaf_visits = atomic_load_u64(m_counters.bvh_leaf_visits);
  out.shadow_tests = atomic_load_u64(m_counters.shadow_tests);
  return out;
}

void ScalarCpuPathTracer::shutdown() {
  m_configured = false;
  m_has_scene = false;
  m_film = FilmBuffer{};
  m_scene = RTSceneData{};
  m_accelerator.reset();
  m_external_accelerator = nullptr;
  m_accel_info = {};
}

}  // namespace vkpt::pathtracer



