#include "pathtracer/PathTracer.h"

#include "cpu/ParallelBvhBuilder.h"
#include "cpu/SimdKernelDispatch.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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

uint32_t crc32_for_byte(uint32_t r) {
  for (int j = 0; j < 8; ++j) {
    r = (r & 1u) ? (0xEDB88320u ^ (r >> 1)) : (r >> 1);
  }
  return r;
}

uint32_t crc32_update(const uint8_t* data, std::size_t size) {
  static uint32_t table[256];
  static bool inited = false;
  if (!inited) {
    for (uint32_t i = 0; i < 256; ++i) {
      table[i] = crc32_for_byte(i);
    }
    inited = true;
  }

  uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
  }
  return crc ^ 0xffffffffu;
}

uint32_t adler32(const std::vector<uint8_t>& bytes) {
  constexpr uint32_t kMod = 65521u;
  uint32_t a = 1u;
  uint32_t b = 0u;
  for (auto byte : bytes) {
    a = (a + byte) % kMod;
    b = (b + a) % kMod;
  }
  return (b << 16) | a;
}

void write_u16_le(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void write_u32_be(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>(value & 0xffu));
}

void append_chunk(std::vector<uint8_t>& out, std::string_view type, const std::vector<uint8_t>& data) {
  write_u32_be(out, static_cast<uint32_t>(data.size()));
  out.insert(out.end(), type.begin(), type.end());
  out.insert(out.end(), data.begin(), data.end());
  std::vector<uint8_t> crcSeed;
  crcSeed.reserve(4 + data.size());
  crcSeed.insert(crcSeed.end(), type.begin(), type.end());
  crcSeed.insert(crcSeed.end(), data.begin(), data.end());
  write_u32_be(out, crc32_update(crcSeed.data(), crcSeed.size()));
}

std::vector<uint8_t> encode_deflate_stored(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  out.reserve(raw.size() * 2);
  out.push_back(0x78);  // ZLIB cmf
  out.push_back(0x01);  // zlib flags (no compression)

  std::size_t offset = 0;
  while (offset < raw.size()) {
    const uint16_t len = static_cast<uint16_t>(std::min(raw.size() - offset, static_cast<std::size_t>(0xffffu)));
    const uint16_t nlen = static_cast<uint16_t>(~len);
    out.push_back(static_cast<uint8_t>((offset + len >= raw.size()) ? 1 : 0));  // BFINAL
    write_u16_le(out, len);
    write_u16_le(out, nlen);
    out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
               raw.begin() + static_cast<std::ptrdiff_t>(offset + len));
    offset += len;
  }

  write_u32_be(out, adler32(raw));
  return out;
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

vkpt::pathtracer::Vec3 rotate_quat(const vkpt::pathtracer::Vec3& value,
                                   const vkpt::scene::Quat& rotation);

vkpt::pathtracer::Vec3 transform_point(const vkpt::pathtracer::Vec3& point,
                                       const vkpt::pathtracer::Vec3& translation,
                                       const vkpt::scene::Quat& rotation,
                                       const vkpt::pathtracer::Vec3& scale) {
  const vkpt::pathtracer::Vec3 scaled{point.x * scale.x, point.y * scale.y, point.z * scale.z};
  const auto rotated = rotate_quat(scaled, rotation);
  return {rotated.x + translation.x, rotated.y + translation.y, rotated.z + translation.z};
}

vkpt::pathtracer::Vec3 rotate_quat(const vkpt::pathtracer::Vec3& value,
                                   const vkpt::scene::Quat& rotation) {
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
  const vkpt::pathtracer::Vec3 qv{x, y, z};
  const auto t = cross(qv, value) * 2.0f;
  return value + t * w + cross(qv, t);
}

vkpt::pathtracer::Vec3 divide_by_scale(const vkpt::pathtracer::Vec3& value, const vkpt::pathtracer::Vec3& scale) {
  const float sx = scale.x != 0.0f ? scale.x : 1.0f;
  const float sy = scale.y != 0.0f ? scale.y : 1.0f;
  const float sz = scale.z != 0.0f ? scale.z : 1.0f;
  return {value.x / sx, value.y / sy, value.z / sz};
}

vkpt::pathtracer::Vec3 parse_shape_position(const vkpt::scene::Vec3& pos) {
  return {pos.x, pos.y, pos.z};
}

vkpt::pathtracer::Vec3 parse_shape_rotation(const vkpt::scene::Quat& rot) {
  return {rot.x, rot.y, rot.z};
}

vkpt::pathtracer::Vec3 parse_shape_scale(const vkpt::scene::Vec3& scale) {
  return {scale.x, scale.y, scale.z};
}

vkpt::pathtracer::SdfShape parse_sdf_shape(std::string_view name) {
  if (name == "sphere") return vkpt::pathtracer::SdfShape::Sphere;
  if (name == "box") return vkpt::pathtracer::SdfShape::Box;
  if (name == "rounded_box") return vkpt::pathtracer::SdfShape::RoundedBox;
  if (name == "plane") return vkpt::pathtracer::SdfShape::Plane;
  if (name == "torus") return vkpt::pathtracer::SdfShape::Torus;
  if (name == "capsule") return vkpt::pathtracer::SdfShape::Capsule;
  return vkpt::pathtracer::SdfShape::Unknown;
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
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".exr" || ext == ".hdr";
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

vkpt::pathtracer::Vec3 environment_color_from_light(const vkpt::scene::LightComponent& light) {
  const float intensity = std::max(0.0f, light.intensity);
  return {light.color.x * intensity, light.color.y * intensity, light.color.z * intensity};
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

struct RayAabbQuery {
  float origin[3]{};
  float inv_direction[3]{};
  bool parallel[3]{};
};

RayAabbQuery make_ray_aabb_query(const vkpt::pathtracer::Ray& ray) {
  RayAabbQuery query{};
  query.origin[0] = ray.origin.x;
  query.origin[1] = ray.origin.y;
  query.origin[2] = ray.origin.z;
  const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
  for (int axis = 0; axis < 3; ++axis) {
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
  for (int axis = 0; axis < 3; ++axis) {
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

        const auto e1 = v1 - v0;
        const auto e2 = v2 - v0;
        m_primitives.push_back({tri,
                                instance.material_index,
                                static_cast<uint32_t>(m_primitives.size()),
                                v0,
                                e1,
                                e2,
                                normalize(cross(e1, e2))});
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

  bool intersect(const vkpt::pathtracer::Ray& ray,
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
          float t = best_t;
          float u = 0.0f;
          float v = 0.0f;
          const vkpt::pathtracer::Vec3 h = cross(ray.direction, primitive.e2);
          const float det = dot(primitive.e1, h);
          if (std::fabs(det) < kEpsilon) {
            continue;
          }
          const float inv_det = 1.0f / det;
          const vkpt::pathtracer::Vec3 s = ray.origin - primitive.v0;
          u = dot(s, h) * inv_det;
          if (u < 0.0f || u > 1.0f) {
            continue;
          }
          const vkpt::pathtracer::Vec3 q = cross(s, primitive.e1);
          v = dot(ray.direction, q) * inv_det;
          if (v < 0.0f || u + v > 1.0f) {
            continue;
          }
          t = dot(primitive.e2, q) * inv_det;
          if (t <= kEpsilon || t >= best_t) {
            continue;
          }
          if (stats) {
            ++stats->triangle_hits;
          }
          best_t = t;
          out.hit = true;
          out.t = t;
          out.position = ray.origin + ray.direction * t;
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
    vkpt::pathtracer::Vec3 v0{};
    vkpt::pathtracer::Vec3 e1{};
    vkpt::pathtracer::Vec3 e2{};
    vkpt::pathtracer::Vec3 normal{};
  };

  std::vector<vkpt::pathtracer::Vec3> m_vertices;
  std::vector<Primitive> m_primitives;
  vkpt::cpu::ParallelBvhBuilder m_builder;
  vkpt::cpu::BvhBuildResult m_bvh;
  vkpt::pathtracer::RayAcceleratorBuildInfo m_info{};
};

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
  out << "\"resolve\":" << SerializeFilmResolveSettings(settings.film.resolve);
  out << "}";
  out << "}";
  return out.str();
}

std::unique_ptr<IRayAccelerator> CreateCpuBvhAccelerator() {
  return std::make_unique<CpuBvhAccelerator>();
}

FilmBuffer::FilmBuffer(uint32_t width, uint32_t height) {
  resize(width, height);
}

void FilmBuffer::resize(uint32_t width, uint32_t height) {
  m_width = width;
  m_height = height;
  m_accumulation.assign(static_cast<std::size_t>(width) * height, Vec3{});
  m_sampleCounts.assign(static_cast<std::size_t>(width) * height, 0);
  m_invalidSamples.assign(static_cast<std::size_t>(width) * height, 0.0f);
}

void FilmBuffer::clear() {
  std::fill(m_accumulation.begin(), m_accumulation.end(), Vec3{});
  std::fill(m_sampleCounts.begin(), m_sampleCounts.end(), 0);
  std::fill(m_invalidSamples.begin(), m_invalidSamples.end(), 0.0f);
}

void FilmBuffer::add_sample(uint32_t x, uint32_t y, const Vec3& color) {
  const std::size_t idx = static_cast<std::size_t>(y) * m_width + x;
  if (idx >= m_accumulation.size()) {
    return;
  }
  if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z)) {
    m_invalidSamples[idx] += 1.0f;
    return;
  }
  m_accumulation[idx] = m_accumulation[idx] + color;
  m_sampleCounts[idx] += 1;
}

void FilmBuffer::import_tile(const FilmBuffer& src, uint32_t start_y, uint32_t end_y) {
  if (m_width != src.m_width || m_height == 0 || src.m_height == 0) {
    return;
  }
  const uint32_t clamped_end = std::min(end_y, std::min(m_height, src.m_height));
  for (uint32_t y = start_y; y < clamped_end; ++y) {
    for (uint32_t x = 0; x < m_width; ++x) {
      const std::size_t dst_idx = static_cast<std::size_t>(y) * m_width + x;
      const std::size_t src_idx = static_cast<std::size_t>(y) * src.m_width + x;
      if (dst_idx < m_accumulation.size() && src_idx < src.m_accumulation.size()) {
        m_accumulation[dst_idx] = src.m_accumulation[src_idx];
        m_sampleCounts[dst_idx] = src.m_sampleCounts[src_idx];
        m_invalidSamples[dst_idx] = src.m_invalidSamples[src_idx];
      }
    }
  }
}

FilmLdr FilmBuffer::resolve_ldr() const {
  return resolve_ldr(m_resolveSettings);
}

FilmLdr FilmBuffer::resolve_ldr(const FilmResolveSettings& settings) const {
  return ApplyFilmResolve(resolve_hdr(), settings);
}

FilmHdr FilmBuffer::resolve_hdr() const {
  FilmHdr out;
  out.width = m_width;
  out.height = m_height;
  out.rgbf.resize(static_cast<std::size_t>(m_width) * m_height * 3, 0.0f);
  for (uint32_t y = 0; y < m_height; ++y) {
    for (uint32_t x = 0; x < m_width; ++x) {
      const auto idx = static_cast<std::size_t>(y) * m_width + x;
      const float invSamples = 1.0f / std::max(1u, m_sampleCounts[idx]);
      const auto base = static_cast<std::size_t>(y) * m_width * 3 + static_cast<std::size_t>(x) * 3;
      out.rgbf[base + 0] = m_accumulation[idx].x * invSamples;
      out.rgbf[base + 1] = m_accumulation[idx].y * invSamples;
      out.rgbf[base + 2] = m_accumulation[idx].z * invSamples;
    }
  }
  return out;
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
                                            float& v) const {
  if (tri.i0 >= m_scene.vertices.size() || tri.i1 >= m_scene.vertices.size() || tri.i2 >= m_scene.vertices.size()) {
    return false;
  }
  atomic_add_u64(m_counters.triangle_tests);
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
                                          Vec3& normal) const {
  atomic_add_u64(m_counters.sdf_tests);

  const float radius = std::max(0.01f, primitive.radius);
  const Vec3 localOrigin =
      divide_by_scale(rotate_euler_inv(ray.origin - primitive.position, primitive.rotation), primitive.scale);
  const Vec3 localDirection = divide_by_scale(rotate_euler_inv(ray.direction, primitive.rotation), primitive.scale);
  const float a = dot(localDirection, localDirection);
  if (a <= kEpsilon * kEpsilon) {
    atomic_add_u64(m_counters.sdf_steps);
    atomic_add_u64(m_counters.sdf_misses);
    return false;
  }

  const float halfB = dot(localOrigin, localDirection);
  const float c = dot(localOrigin, localOrigin) - radius * radius;
  const float discriminant = halfB * halfB - a * c;
  if (discriminant < 0.0f) {
    atomic_add_u64(m_counters.sdf_steps);
    atomic_add_u64(m_counters.sdf_misses);
    return false;
  }

  const float root = std::sqrt(discriminant);
  float candidate = (-halfB - root) / a;
  if (candidate <= kEpsilon) {
    candidate = (-halfB + root) / a;
  }
  if (candidate <= kEpsilon || !std::isfinite(candidate)) {
    atomic_add_u64(m_counters.sdf_steps);
    atomic_add_u64(m_counters.sdf_misses);
    return false;
  }

  t = candidate;
  const Vec3 localHit = localOrigin + localDirection * t;
  const Vec3 localNormal = normalize(localHit);
  const Vec3 normalLocalInvScale = divide_by_scale(localNormal, primitive.scale);
  normal = normalize(rotate_euler(normalLocalInvScale, primitive.rotation));
  atomic_add_u64(m_counters.sdf_steps);
  atomic_add_u64(m_counters.sdf_hits);
  return true;
}

bool ScalarCpuPathTracer::intersect_box(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const {
  atomic_add_u64(m_counters.sdf_tests);
  uint32_t steps = 0u;
  for (uint32_t i = 0; i < kMaxMarchSteps; ++i) {
    ++steps;
    const Vec3 point = ray.origin + ray.direction * t;
    const float d = sdf_distance(primitive, point);
    if (d <= kEpsilon) {
      sdf_normal(primitive, point, normal);
      atomic_add_u64(m_counters.sdf_steps, steps);
      atomic_add_u64(m_counters.sdf_hits);
      return true;
    }
    t += std::max(kMinMarchStep, d);
    if (t > kMaxMarchDistance) {
      atomic_add_u64(m_counters.sdf_steps, steps);
      atomic_add_u64(m_counters.sdf_misses);
      return false;
    }
  }
  atomic_add_u64(m_counters.sdf_steps, steps);
  atomic_add_u64(m_counters.sdf_misses);
  return false;
}

bool ScalarCpuPathTracer::intersect_rounded_box(const RTSdfPrimitive& primitive,
                                               const Ray& ray,
                                               float& t,
                                               Vec3& normal) const {
  return intersect_box(primitive, ray, t, normal);
}

bool ScalarCpuPathTracer::intersect_plane(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const {
  return intersect_box(primitive, ray, t, normal);
}

bool ScalarCpuPathTracer::intersect_torus(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const {
  return intersect_box(primitive, ray, t, normal);
}

bool ScalarCpuPathTracer::intersect_capsule(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const {
  return intersect_box(primitive, ray, t, normal);
}

bool ScalarCpuPathTracer::intersect_scene(const Ray& ray, Hit& out) const {
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
    atomic_add_u64(m_counters.triangle_tests, stats.triangle_tests);
    atomic_add_u64(m_counters.triangle_hits, stats.triangle_hits);
    atomic_add_u64(m_counters.bvh_node_visits, stats.bvh_node_visits);
    atomic_add_u64(m_counters.bvh_leaf_visits, stats.bvh_leaf_visits);
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
        if (!intersect_triangle(tri, ray, t, u, v)) {
          continue;
        }
        atomic_add_u64(m_counters.triangle_hits);
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
        hit = intersect_sphere(primitive, ray, t, normal);
        break;
      case SdfShape::Box:
        hit = intersect_box(primitive, ray, t, normal);
        break;
      case SdfShape::RoundedBox:
        hit = intersect_rounded_box(primitive, ray, t, normal);
        break;
      case SdfShape::Plane:
        hit = intersect_plane(primitive, ray, t, normal);
        break;
      case SdfShape::Torus:
        hit = intersect_torus(primitive, ray, t, normal);
        break;
      case SdfShape::Capsule:
        hit = intersect_capsule(primitive, ray, t, normal);
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

NeeResult ScalarCpuPathTracer::sample_direct_light(const Hit& hit, const Vec3& view_dir, Rng& rng) const {
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

  // Shadow ray — offset by normal to avoid self-intersection.
  const Ray shadow_ray{hit.position + hit.normal * 0.002f, light_dir};
  Hit shadow_hit;
  atomic_add_u64(m_counters.shadow_tests);
  if (intersect_scene(shadow_ray, shadow_hit) && shadow_hit.t < dist - 0.004f) {
    return {};  // occluded
  }

  const auto mat_index = std::min(hit.material_index, static_cast<uint32_t>(m_scene.materials.size() - 1));
  const auto& material = m_scene.materials[mat_index];
  const Vec3 albedo = apply_material_effect(material, hit.position, hit.normal, -light_dir);
  const Vec3 irradiance = light.color * (light.intensity / (dist_sq + kEpsilon));
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
                                Rng& rng) {
  Vec3 radiance{0.0f, 0.0f, 0.0f};
  Vec3 throughput{1.0f, 1.0f, 1.0f};
  Ray ray = input;
  bool previewReflectionEnvironment = false;
  for (uint32_t depth = 0; depth < m_settings.max_depth; ++depth) {
    ++ray_counter;
    Hit hit;
    if (!intersect_scene(ray, hit)) {
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
      const NeeResult nee = sample_direct_light(lightSampleHit, -ray.direction, rng);
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
  std::vector<uint32_t> pixelIndices;
  pixelIndices.reserve(static_cast<std::size_t>(m_settings.width) * (maxY - minY));
  for (uint32_t y = minY; y < maxY; ++y) {
    const uint32_t rowBase = y * m_settings.width;
    for (uint32_t x = 0; x < m_settings.width; ++x) {
      pixelIndices.push_back(rowBase + x);
    }
  }
  if (pixelIndices.empty()) {
    return true;
  }
  return render_sample_pixels(pixelIndices.data(), static_cast<uint32_t>(pixelIndices.size()), sample_index, frame_index);
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

  struct LocalAccum {
    float lum_sum = 0.0f;
    float sample_max = 0.0f;
    std::uint64_t sample_count = 0u;
    std::uint64_t ray_count = 0u;
    uint32_t min_pixel = std::numeric_limits<uint32_t>::max();
    uint32_t max_pixel = 0u;
  };

  auto shade_one = [&](uint32_t pixel, LocalAccum& accum) {
    if (pixel >= m_settings.width * m_settings.height) {
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
    Vec3 sample = trace(ray, sample_index, frame_index, static_cast<uint32_t>(pathId), 0, rayCounter, rng);
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
  };

  auto& jobs = scalar_render_jobs();
  jobs.set_deterministic(m_settings.deterministic);
  const uint32_t threadCount = m_settings.deterministic
      ? 1u
      : std::max(1u, std::min(m_worker_count, pixel_count));
  std::vector<LocalAccum> locals(static_cast<std::size_t>(threadCount));

  if (threadCount == 1u) {
    for (uint32_t i = 0; i < pixel_count; ++i) {
      shade_one(pixel_indices[i], locals[0]);
    }
  } else {
    auto run_worker = [&](uint32_t workerIndex) {
      LocalAccum& local = locals[workerIndex];
      for (uint32_t i = workerIndex; i < pixel_count; i += threadCount) {
        shade_one(pixel_indices[i], local);
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

  float sampleLumSum = 0.0f;
  float sampleMax = 0.0f;
  std::uint64_t sampleCount = 0u;
  std::uint64_t rayCount = 0u;
  uint32_t minPixel = std::numeric_limits<uint32_t>::max();
  uint32_t maxPixel = 0u;
  for (const auto& local : locals) {
    sampleLumSum += local.lum_sum;
    sampleMax = std::max(sampleMax, local.sample_max);
    sampleCount += local.sample_count;
    rayCount += local.ray_count;
    if (local.sample_count > 0u) {
      minPixel = std::min(minPixel, local.min_pixel);
      maxPixel = std::max(maxPixel, local.max_pixel);
    }
  }
  atomic_add_u64(m_counters.samples, sampleCount);
  atomic_add_u64(m_counters.rays, rayCount);

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

vkpt::core::Result<RTSceneData> BuildSceneDataFromDocument(const vkpt::scene::SceneDocument& doc) {
  RTSceneData scene;
  scene.camera_position = {0.0f, 1.0f, 4.0f};
  scene.camera_target = {0.0f, 1.0f, 0.0f};
  scene.camera_up = {0.0f, 1.0f, 0.0f};
  scene.camera_fov_deg = 55.0f;
  scene.environment_color = {0.0f, 0.0f, 0.0f};

  for (const auto& asset : doc.assets) {
    const auto assetType = normalize_material_id(asset.type);
    if (!asset.uri.empty() &&
        (assetType == "texture" || assetType == "image" || is_texture_asset_uri(asset.uri))) {
      scene.textures.push_back(asset.uri);
    }
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
    for (const auto& vertex : geometry->vertices) {
      scene.vertices.push_back(transform_point(
          Vec3{vertex.x, vertex.y, vertex.z},
          Vec3{translation.x, translation.y, translation.z},
          rotation,
          Vec3{scale.x, scale.y, scale.z}));
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
    if (entity.has_physics_body && entity.physics_body.enabled && entity.physics_body.dynamic) {
      instance.flags |= kRTInstanceFlagDynamicTransform | kRTInstanceFlagPhysicsControlled;
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
    scene.lights.push_back(RTHitLight{
      pos,
      {light.color.x, light.color.y, light.color.z},
      light.intensity,
      std::max(0.0f, light.radius)});
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
                                      {"instances", std::to_string(scene.instances.size())},
                                      {"tessellation_requests", std::to_string(scene.tessellation_requests.size())},
                                      {"sdf_primitives", std::to_string(scene.sdf_primitives.size())},
                                      {"materials", std::to_string(scene.materials.size())},
                                      {"lights", std::to_string(scene.lights.size())},
                                      {"camera_pos", std::to_string(scene.camera_position.x) + "," +
                                         std::to_string(scene.camera_position.y) + "," + std::to_string(scene.camera_position.z)},
                                      {"camera_target", std::to_string(scene.camera_target.x) + "," +
                                         std::to_string(scene.camera_target.y) + "," + std::to_string(scene.camera_target.z)}
                                    });

  return vkpt::core::Result<RTSceneData>::ok(std::move(scene));
}

bool SavePngCompat(const std::string& path, const FilmLdr& image, std::string* error) {
  if (image.width == 0 || image.height == 0 || image.rgba8.empty()) {
    if (error) *error = "empty image";
    return false;
  }

  std::vector<uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(image.width) * image.height * 5u);
  for (uint32_t y = 0; y < image.height; ++y) {
    raw.push_back(0);
    const auto row = static_cast<std::size_t>(y) * image.width * 4u;
    for (uint32_t x = 0; x < image.width; ++x) {
      const auto idx = row + static_cast<std::size_t>(x) * 4u;
      raw.push_back(image.rgba8[idx + 0]);
      raw.push_back(image.rgba8[idx + 1]);
      raw.push_back(image.rgba8[idx + 2]);
      raw.push_back(image.rgba8[idx + 3]);
    }
  }
  const std::vector<uint8_t> compressed = encode_deflate_stored(raw);

  std::vector<uint8_t> ihdr;
  write_u32_be(ihdr, image.width);
  write_u32_be(ihdr, image.height);
  ihdr.push_back(8);
  ihdr.push_back(6);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);

  std::vector<uint8_t> png;
  png.reserve(8 + 12 + ihdr.size() + 12 + compressed.size() + 12);
  png.insert(png.end(), {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a});
  append_chunk(png, "IHDR", ihdr);
  append_chunk(png, "IDAT", compressed);
  append_chunk(png, "IEND", {});

  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    if (error) {
      *error = "failed to open output path";
    }
    return false;
  }
  out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(out);
}

bool SaveExrCompat(const std::string& path, const FilmHdr& image, std::string* error) {
  if (image.width == 0 || image.height == 0 || image.rgbf.empty()) {
    if (error) *error = "empty image";
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    if (error) {
      *error = "failed to open output path";
    }
    return false;
  }
  out << "# EXR-compatible placeholder written by vkpt path tracer.\n";
  out << image.width << " " << image.height << "\n";
  for (std::size_t i = 0; i < image.rgbf.size(); i += 3) {
    out << image.rgbf[i] << " " << image.rgbf[i + 1] << " " << image.rgbf[i + 2] << "\n";
  }
  return true;
}

vkpt::core::Result<RTSceneLayoutManifest> BuildRTSceneDataLayoutManifest(
    std::vector<std::string>* diagnostics) {
  if (diagnostics) {
    diagnostics->clear();
  }

  const auto sceneResult = BuildSceneDataFromDocument(vkpt::scene::SceneDocument{});
  if (!sceneResult) {
    if (diagnostics) {
      diagnostics->push_back("failed to construct fallback scene data");
    }
    return vkpt::core::Result<RTSceneLayoutManifest>::error(vkpt::core::ErrorCode::Internal);
  }

  const auto& scene = sceneResult.value();

  auto align_up = [](std::size_t value, std::size_t alignment) {
    if (alignment == 0u) {
      return value;
    }
    const std::size_t mask = alignment - 1u;
    return (value + mask) & ~mask;
  };

  auto append_field = [&](std::vector<GpuLayoutField>& fields,
                        std::size_t& cpu_cursor,
                        std::size_t& gpu_cursor,
                        const std::string& struct_name,
                        const std::string& field,
                        std::size_t element_size,
                        std::size_t element_count,
                        std::size_t alignment) {
    GpuLayoutField out;
    out.struct_name = struct_name;
    out.field = field;
    out.cpu_alignment = alignment;
    out.gpu_alignment = alignment;
    out.cpu_size = element_size * element_count;
    out.gpu_size = element_size * element_count;
    cpu_cursor = align_up(cpu_cursor, std::max<std::size_t>(1u, alignment));
    gpu_cursor = align_up(gpu_cursor, std::max<std::size_t>(1u, alignment));
    out.cpu_offset = cpu_cursor;
    out.gpu_offset = gpu_cursor;
    cpu_cursor += out.cpu_size;
    gpu_cursor += out.gpu_size;
    fields.push_back(std::move(out));
  };

  RTSceneLayoutManifest manifest;
  manifest.schema_version = "1.0";

  std::size_t cpuCursor = 0u;
  std::size_t gpuCursor = 0u;
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_position", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_target", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_up", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_fov_deg", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_focal_length_mm", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_sensor_width_mm", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_sensor_height_mm", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_aperture_radius", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_focus_distance", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_f_stop", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_shutter_seconds", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iso", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_exposure_compensation", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_white_balance_kelvin", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iris_blade_count", sizeof(std::uint32_t), 1u, alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iris_rotation_degrees", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_iris_roundness", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "camera_anamorphic_squeeze", sizeof(float), 1u, alignof(float));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "environment_color", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "materials", sizeof(RTMaterial), scene.materials.size(), alignof(RTMaterial));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "vertices", sizeof(Vec3), scene.vertices.size(), alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "indices", sizeof(std::uint32_t), scene.indices.size(), alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "local_vertices", sizeof(Vec3), scene.local_vertices.size(), alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "local_indices", sizeof(std::uint32_t), scene.local_indices.size(), alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "instances", sizeof(RTInstance), scene.instances.size(), alignof(RTInstance));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "tessellation_requests", sizeof(RTTessellationRequest), scene.tessellation_requests.size(), alignof(RTTessellationRequest));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "sdf_primitives", sizeof(RTSdfPrimitive), scene.sdf_primitives.size(), alignof(RTSdfPrimitive));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "lights", sizeof(RTHitLight), scene.lights.size(), alignof(RTHitLight));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "textures_count", sizeof(std::uint64_t), 1u, alignof(std::uint64_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "texture_names", sizeof(char), 0u, alignof(char));

  manifest.total_cpu_bytes = cpuCursor;
  manifest.total_gpu_bytes = gpuCursor;

  if (diagnostics) {
    diagnostics->push_back("layout manifest built from fallback scene template");
  }
  return vkpt::core::Result<RTSceneLayoutManifest>::ok(std::move(manifest));
}

std::string SerializeRTSceneDataLayoutManifest(const RTSceneLayoutManifest& manifest) {
  std::ostringstream out;
  out << "{";
  out << "\"schema_version\":\"" << manifest.schema_version << "\",";
  out << "\"total_cpu_bytes\":" << manifest.total_cpu_bytes << ",";
  out << "\"total_gpu_bytes\":" << manifest.total_gpu_bytes << ",";
  out << "\"fields\":[";
  for (std::size_t i = 0; i < manifest.fields.size(); ++i) {
    const auto& field = manifest.fields[i];
    if (i > 0u) {
      out << ",";
    }
    out << "{";
    out << "\"struct_name\":\"" << field.struct_name << "\",";
    out << "\"field\":\"" << field.field << "\",";
    out << "\"cpu_offset\":" << field.cpu_offset << ",";
    out << "\"cpu_size\":" << field.cpu_size << ",";
    out << "\"cpu_alignment\":" << field.cpu_alignment << ",";
    out << "\"gpu_offset\":" << field.gpu_offset << ",";
    out << "\"gpu_size\":" << field.gpu_size << ",";
    out << "\"gpu_alignment\":" << field.gpu_alignment;
    out << "}";
  }
  out << "]";
  out << "}";
  return out.str();
}

Vec3 ColorTemperatureToRgb(float kelvin) {
  const float temperature = std::clamp(kelvin, 1000.0f, 40000.0f) / 100.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;

  if (temperature <= 66.0f) {
    r = 1.0f;
    g = std::clamp(0.39008158f * std::log(std::max(1.0f, temperature)) - 0.63184144f, 0.0f, 1.0f);
    b = temperature <= 19.0f
        ? 0.0f
        : std::clamp(0.5432068f * std::log(std::max(1.0f, temperature - 10.0f)) - 1.1962541f, 0.0f, 1.0f);
  } else {
    r = std::clamp(1.2929362f * std::pow(temperature - 60.0f, -0.13320476f), 0.0f, 1.0f);
    g = std::clamp(1.1298909f * std::pow(temperature - 60.0f, -0.075514846f), 0.0f, 1.0f);
    b = 1.0f;
  }

  return {r, g, b};
}

Vec3 WhiteBalanceScale(float kelvin) {
  const Vec3 d65 = ColorTemperatureToRgb(6500.0f);
  const Vec3 target = ColorTemperatureToRgb(kelvin);
  constexpr float kMinWhite = 1.0e-3f;
  return {
      d65.x / std::max(kMinWhite, target.x),
      d65.y / std::max(kMinWhite, target.y),
      d65.z / std::max(kMinWhite, target.z)};
}

FilmResolveSettings CameraAdjustedFilmResolveSettings(const FilmResolveSettings& base,
                                                       const RTSceneData& scene) {
  FilmResolveSettings out = base;
  if (std::isfinite(scene.camera_f_stop) &&
      std::isfinite(scene.camera_shutter_seconds) &&
      std::isfinite(scene.camera_iso) &&
      scene.camera_f_stop > 0.0f &&
      scene.camera_shutter_seconds > 0.0f &&
      scene.camera_iso > 0.0f) {
    constexpr float kReferenceFStop = 2.8f;
    constexpr float kReferenceShutter = 1.0f / 60.0f;
    constexpr float kReferenceIso = 100.0f;
    const float reference = (kReferenceShutter * kReferenceIso) / (kReferenceFStop * kReferenceFStop);
    const float physical = (scene.camera_shutter_seconds * scene.camera_iso) /
        (scene.camera_f_stop * scene.camera_f_stop);
    out.exposure *= std::clamp(physical / std::max(1.0e-6f, reference), 1.0e-6f, 1.0e6f);
  }
  if (std::isfinite(scene.camera_exposure_compensation)) {
    out.exposure *= std::pow(2.0f, std::clamp(scene.camera_exposure_compensation, -32.0f, 32.0f));
  }
  if (std::isfinite(scene.camera_white_balance_kelvin) &&
      scene.camera_white_balance_kelvin >= 1000.0f &&
      scene.camera_white_balance_kelvin <= 40000.0f) {
    out.white_balance_kelvin = scene.camera_white_balance_kelvin;
  }
  return out;
}

FilmLdr ApplyFilmResolve(const FilmHdr& hdr, const FilmResolveSettings& settings) {
  FilmLdr ldr;
  ldr.width = hdr.width;
  ldr.height = hdr.height;
  const std::size_t num_pixels = static_cast<std::size_t>(hdr.width) * hdr.height;
  ldr.rgba8.resize(num_pixels * 4u, 255u);
  if (hdr.rgbf.size() < num_pixels * 3u) {
    std::fill(ldr.rgba8.begin(), ldr.rgba8.end(), 0u);
    for (std::size_t i = 0; i < num_pixels; ++i) {
      ldr.rgba8[i * 4u + 3u] = 255u;
    }
    return ldr;
  }

  const float inv_gamma = 1.0f / std::max(0.01f, settings.gamma);
  const Vec3 white_balance = WhiteBalanceScale(settings.white_balance_kelvin);

  for (std::size_t i = 0; i < num_pixels; ++i) {
    float r = hdr.rgbf[i * 3u + 0u] * settings.exposure * white_balance.x;
    float g = hdr.rgbf[i * 3u + 1u] * settings.exposure * white_balance.y;
    float b = hdr.rgbf[i * 3u + 2u] * settings.exposure * white_balance.z;
    if (!std::isfinite(r)) r = 0.0f;
    if (!std::isfinite(g)) g = 0.0f;
    if (!std::isfinite(b)) b = 0.0f;

    auto tonemap = [&](float x) -> float {
      switch (settings.tone_map) {
        case ToneMapMode::Reinhard:
          return x / (1.0f + x);
        case ToneMapMode::FilmicApprox: {
          // Simplified Uncharted 2 approximation (A=0.15, B=0.50, etc.)
          auto F = [](float v) -> float {
            const float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F_ = 0.30f;
            return ((v * (A * v + C * B) + D * E) / (v * (A * v + B) + D * F_)) - E / F_;
          };
          const float W = 11.2f;
          return F(x) / F(W);
        }
        case ToneMapMode::AcesApprox:
          return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
        default:
          return x;
      }
    };

    r = tonemap(r);
    g = tonemap(g);
    b = tonemap(b);

    if (settings.output_transform == OutputTransformMode::Gamma) {
      r = std::pow(std::max(0.0f, r), inv_gamma);
      g = std::pow(std::max(0.0f, g), inv_gamma);
      b = std::pow(std::max(0.0f, b), inv_gamma);
    }

    if (settings.clamp_output) {
      r = std::min(1.0f, std::max(0.0f, r));
      g = std::min(1.0f, std::max(0.0f, g));
      b = std::min(1.0f, std::max(0.0f, b));
    }

    ldr.rgba8[i * 4u + 0u] = static_cast<uint8_t>(r * 255.0f + 0.5f);
    ldr.rgba8[i * 4u + 1u] = static_cast<uint8_t>(g * 255.0f + 0.5f);
    ldr.rgba8[i * 4u + 2u] = static_cast<uint8_t>(b * 255.0f + 0.5f);
    ldr.rgba8[i * 4u + 3u] = 255u;
  }
  return ldr;
}

std::string SerializeFilmResolveSettings(const FilmResolveSettings& settings) {
  const char* tone_map_str = "linear";
  switch (settings.tone_map) {
    case ToneMapMode::Reinhard:     tone_map_str = "reinhard";      break;
    case ToneMapMode::FilmicApprox: tone_map_str = "filmic_approx"; break;
    case ToneMapMode::AcesApprox:   tone_map_str = "aces_approx";   break;
    default: break;
  }
  const char* output_transform_str = "gamma";
  switch (settings.output_transform) {
    case OutputTransformMode::Linear: output_transform_str = "linear"; break;
    case OutputTransformMode::Gamma:
    default: break;
  }
  std::ostringstream out;
  out << "{";
  out << "\"exposure\":" << settings.exposure << ",";
  out << "\"white_balance_kelvin\":" << settings.white_balance_kelvin << ",";
  out << "\"tone_map\":\"" << tone_map_str << "\",";
  out << "\"output_transform\":\"" << output_transform_str << "\",";
  out << "\"gamma\":" << settings.gamma << ",";
  out << "\"clamp_output\":" << (settings.clamp_output ? "true" : "false");
  out << "}";
  return out.str();
}

}  // namespace vkpt::pathtracer



