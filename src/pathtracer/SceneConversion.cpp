#include "pathtracer/SceneConversion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/Logging.h"

namespace vkpt::pathtracer {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1e-4f;

void log_scene_conversion_warning(std::string_view message,
                                  vkpt::core::StableId entity_id,
                                  vkpt::core::StableId geometry_id) noexcept {
  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        "scene-conversion",
        message,
        {{"entity_id", std::to_string(entity_id)},
         {"geometry_id", std::to_string(geometry_id)}});
  } catch (...) {
  }
}

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

Vec3 operator-(const Vec3& lhs, const Vec3& rhs) {
  return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
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

float random01(uint64_t seed, uint32_t stream) {
  constexpr float kInv24Bit = 1.0f / 16777216.0f;
  return static_cast<float>((splitmix64(seed + static_cast<uint64_t>(stream)) >> 40u) & 0x00ffffffu) *
         kInv24Bit;
}

float random_signed(uint64_t seed, uint32_t stream) {
  return random01(seed, stream) * 2.0f - 1.0f;
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
         id == "environmenthdri" || id == "hdri" || id == "hdri_sky" ||
         id == "open_sky" || id == "sky";
}

bool is_rain_particle_type(std::string_view type) {
  return normalize_material_id(type) == "rain";
}

bool is_smoke_particle_type(std::string_view type) {
  return normalize_material_id(type) == "smoke";
}

bool is_spot_light_type(std::string_view type) {
  const std::string id = normalize_material_id(type);
  return id == "spot" || id == "spot_light" || id == "spotlight";
}

bool is_environment_map_asset_type(std::string_view type) {
  const std::string id = normalize_material_id(type);
  return id == "environment" || id == "environment_sky" || id == "environment_hdri" ||
         id == "environmenthdri" || id == "hdri" || id == "hdri_sky" ||
         id == "open_sky" || id == "sky";
}

bool is_hdr_asset_uri(std::string_view uri) {
  const auto dot = uri.find_last_of('.');
  if (dot == std::string_view::npos) {
    return false;
  }
  std::string ext(uri.substr(dot));
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".hdr";
}

struct RadianceHdrImage {
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<Vec3> pixels;
};

Vec3 decode_rgbe(const std::array<unsigned char, 4>& rgbe) {
  if (rgbe[3] == 0u) {
    return {};
  }
  const float scale = std::ldexp(1.0f, static_cast<int>(rgbe[3]) - (128 + 8));
  return {
      static_cast<float>(rgbe[0]) * scale,
      static_cast<float>(rgbe[1]) * scale,
      static_cast<float>(rgbe[2]) * scale};
}

bool parse_radiance_resolution(std::string_view line,
                               uint32_t& width,
                               uint32_t& height,
                               bool& flip_x,
                               bool& flip_y) {
  std::istringstream input{std::string(line)};
  std::string axis_a;
  std::string axis_b;
  int value_a = 0;
  int value_b = 0;
  if (!(input >> axis_a >> value_a >> axis_b >> value_b)) {
    return false;
  }
  auto consume = [&](const std::string& axis, int value) {
    if (axis.size() != 2u || value <= 0) {
      return false;
    }
    const char sign = axis[0];
    const char dimension = static_cast<char>(std::toupper(static_cast<unsigned char>(axis[1])));
    if (dimension == 'X') {
      width = static_cast<uint32_t>(value);
      flip_x = sign == '-';
      return true;
    }
    if (dimension == 'Y') {
      height = static_cast<uint32_t>(value);
      flip_y = sign == '+';
      return true;
    }
    return false;
  };
  return consume(axis_a, value_a) && consume(axis_b, value_b) &&
         width > 0u && height > 0u;
}

std::optional<RadianceHdrImage> load_radiance_hdr(std::string_view uri) {
  std::ifstream file{std::filesystem::path(std::string(uri)), std::ios::binary};
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::string line;
  bool foundFormat = false;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("FORMAT=", 0u) == 0u &&
        line.find("32-bit_rle_rgbe") != std::string::npos) {
      foundFormat = true;
    }
    if (line.empty()) {
      break;
    }
  }
  if (!foundFormat || !std::getline(file, line)) {
    return std::nullopt;
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  uint32_t width = 0u;
  uint32_t height = 0u;
  bool flipX = false;
  bool flipY = false;
  if (!parse_radiance_resolution(line, width, height, flipX, flipY)) {
    return std::nullopt;
  }
  constexpr uint64_t kMaxEnvironmentPixels = 8192ull * 4096ull;
  const uint64_t pixelCount = static_cast<uint64_t>(width) * height;
  if (pixelCount == 0u || pixelCount > kMaxEnvironmentPixels) {
    return std::nullopt;
  }

  RadianceHdrImage image;
  image.width = width;
  image.height = height;
  image.pixels.resize(static_cast<std::size_t>(pixelCount));

  auto store_pixel = [&](uint32_t x, uint32_t y, const std::array<unsigned char, 4>& rgbe) {
    const uint32_t dstX = flipX ? (width - 1u - x) : x;
    const uint32_t dstY = flipY ? (height - 1u - y) : y;
    image.pixels[static_cast<std::size_t>(dstY) * width + dstX] = decode_rgbe(rgbe);
  };

  std::vector<unsigned char> scanline(static_cast<std::size_t>(width) * 4u);
  for (uint32_t y = 0u; y < height; ++y) {
    std::array<unsigned char, 4> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), 4)) {
      return std::nullopt;
    }
    const bool rle = width >= 8u && width <= 32767u &&
                     header[0] == 2u && header[1] == 2u &&
                     ((static_cast<uint32_t>(header[2]) << 8u) | header[3]) == width;
    if (!rle) {
      store_pixel(0u, y, header);
      for (uint32_t x = 1u; x < width; ++x) {
        std::array<unsigned char, 4> rgbe{};
        if (!file.read(reinterpret_cast<char*>(rgbe.data()), 4)) {
          return std::nullopt;
        }
        store_pixel(x, y, rgbe);
      }
      continue;
    }

    for (uint32_t channel = 0u; channel < 4u; ++channel) {
      uint32_t x = 0u;
      while (x < width) {
        unsigned char count = 0u;
        if (!file.read(reinterpret_cast<char*>(&count), 1)) {
          return std::nullopt;
        }
        if (count > 128u) {
          const uint32_t run = static_cast<uint32_t>(count - 128u);
          unsigned char value = 0u;
          if (run == 0u || x + run > width ||
              !file.read(reinterpret_cast<char*>(&value), 1)) {
            return std::nullopt;
          }
          std::fill_n(scanline.begin() + static_cast<std::ptrdiff_t>(channel * width + x),
                      run,
                      value);
          x += run;
        } else {
          const uint32_t run = static_cast<uint32_t>(count);
          if (run == 0u || x + run > width ||
              !file.read(reinterpret_cast<char*>(
                             scanline.data() + static_cast<std::size_t>(channel) * width + x),
                         run)) {
            return std::nullopt;
          }
          x += run;
        }
      }
    }

    for (uint32_t x = 0u; x < width; ++x) {
      store_pixel(x,
                  y,
                  {scanline[x],
                   scanline[static_cast<std::size_t>(width) + x],
                   scanline[static_cast<std::size_t>(width) * 2u + x],
                   scanline[static_cast<std::size_t>(width) * 3u + x]});
    }
  }

  return image;
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
  // Compact model IDs are the CPU/GPU bridge for broad BSDF class selection.
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
  // Effect IDs preserve specialized material intent even when scalar RTMaterial fields are shared.
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
  if (id == "parquet" || id == "oak") return 17u;
  if (id == "walnut" || id == "mahogany") return 18u;
  if (id == "sandalwood" || id == "pine" || id == "teak" || id == "cedar") return 19u;
  if (id == "wood") return 16u;
  return 0u;
}

}  // namespace

vkpt::core::Result<RTSceneData> BuildSceneDataFromDocument(const vkpt::scene::SceneDocument& doc) {
  return BuildSceneDataFromDocumentAtFrame(doc, SceneParticleAnimationState{});
}

vkpt::core::Result<RTSceneData> BuildSceneDataFromDocumentAtFrame(
    const vkpt::scene::SceneDocument& doc,
    const SceneParticleAnimationState& animation) {
  RTSceneData scene;
  scene.camera_position = {0.0f, 1.0f, 4.0f};
  scene.camera_target = {0.0f, 1.0f, 0.0f};
  scene.camera_up = {0.0f, 1.0f, 0.0f};
  scene.camera_fov_deg = 55.0f;
  scene.environment_color = {0.0f, 0.0f, 0.0f};

  std::unordered_map<std::string, uint32_t> textureLookup;
  textureLookup.reserve(doc.assets.size() + doc.materials.size() * 2u);
  scene.textures.reserve(doc.assets.size() + doc.materials.size() * 2u);
  scene.materials.reserve(std::max<std::size_t>(1u, doc.materials.size()));
  scene.instances.reserve(doc.entities.size() + doc.particle_emitters.size());
  scene.lights.reserve(doc.entities.size() + doc.lights.size());
  scene.sdf_primitives.reserve(doc.entities.size() + doc.sdf_primitives.size() + doc.particle_emitters.size() * 16u);
  scene.tessellation_requests.reserve(doc.entities.size());
  std::size_t estimated_vertices = 0u;
  std::size_t estimated_indices = 0u;
  for (const auto& geometry : doc.geometry) {
    estimated_vertices += geometry.vertices.size();
    estimated_indices += geometry.indices.size();
  }
  for (const auto& emitter : doc.particle_emitters) {
    if (emitter.enabled && is_rain_particle_type(emitter.type)) {
      estimated_vertices += static_cast<std::size_t>(emitter.count) * 12u;
      estimated_indices += static_cast<std::size_t>(emitter.count) * 24u;
    } else if (emitter.enabled && is_smoke_particle_type(emitter.type)) {
      estimated_vertices += static_cast<std::size_t>(emitter.count) * 12u;
      estimated_indices += static_cast<std::size_t>(emitter.count) * 24u;
    }
  }
  scene.vertices.reserve(estimated_vertices);
  scene.texcoords.reserve(estimated_vertices);
  scene.indices.reserve(estimated_indices);
  auto normalize_texture_uri = [](std::string uri) {
    std::replace(uri.begin(), uri.end(), '\\', '/');
    return uri;
  };
  auto add_texture_uri = [&](std::string uri) -> uint32_t {
    // Texture indices are packed densely; 0xFFFFFFFF means no bound texture.
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
        (assetType == "texture" || assetType == "image" ||
         (!is_environment_map_asset_type(asset.type) && is_texture_asset_uri(asset.uri)))) {
      add_texture_uri(asset.uri);
    }
    if (scene.environment_map.empty() && !asset.uri.empty() &&
        is_environment_map_asset_type(asset.type) && is_hdr_asset_uri(asset.uri)) {
      if (auto hdr = load_radiance_hdr(asset.uri)) {
        scene.environment_map_width = hdr->width;
        scene.environment_map_height = hdr->height;
        scene.environment_map = std::move(hdr->pixels);
      } else {
        vkpt::log::Logger::instance().log(
            vkpt::log::Severity::Warning,
            "scene-conversion",
            "environment HDRI could not be decoded",
            {{"uri", asset.uri}});
      }
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
  materialLookup.reserve(materials.size());
  for (const auto& material : materials) {
    auto runtimeMaterial = material;
    vkpt::scene::ApplyMaterialFamilyPreset(runtimeMaterial,
                                           vkpt::scene::SceneMaterialPresetPolicy::FillGenericDefaults);
    const uint32_t index = static_cast<uint32_t>(scene.materials.size());
    materialLookup[runtimeMaterial.id] = index;
    // Scene material fields are normalized into RTMaterial's fixed descriptor layout.
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
  std::unordered_map<vkpt::core::StableId, bool> visiblePathCache;
  visiblePathCache.reserve(doc.entities.size());
  auto entity_visible_path =
      [&](const vkpt::scene::SceneEntityDefinition& entity) {
    if (const auto cached = visiblePathCache.find(entity.id);
        cached != visiblePathCache.end()) {
      return cached->second;
    }
    bool visible = true;
    const auto* current = &entity;
    std::unordered_set<vkpt::core::StableId> visited;
    visited.reserve(8u);
    for (std::size_t depth = 0u;
         current != nullptr && depth <= doc.entities.size() &&
         visited.insert(current->id).second;
         ++depth) {
      if (!current->visible) {
        visible = false;
        break;
      }
      const vkpt::core::StableId parent = current->hierarchy.parent;
      if (parent == 0u) {
        break;
      }
      const auto parentIt = entityById.find(parent);
      current = parentIt == entityById.end() ? nullptr : parentIt->second;
    }
    visiblePathCache.emplace(entity.id, visible);
    return visible;
  };

  std::unordered_map<vkpt::core::StableId, bool> animatedPathCache;
  std::unordered_map<vkpt::core::StableId, bool> scriptedPathCache;
  animatedPathCache.reserve(doc.entities.size());
  scriptedPathCache.reserve(doc.entities.size());

  auto entity_has_animated_transform_path =
      [&](const vkpt::scene::SceneEntityDefinition& entity) {
    if (const auto cached = animatedPathCache.find(entity.id); cached != animatedPathCache.end()) {
      return cached->second;
    }
    bool animated = false;
    const auto* current = &entity;
    for (std::size_t depth = 0u; current != nullptr && depth <= doc.entities.size(); ++depth) {
      if (const auto cached = animatedPathCache.find(current->id); cached != animatedPathCache.end()) {
        animated = cached->second;
        break;
      }
      if (!current->animation.clip.empty()) {
        animated = true;
        break;
      }
      const vkpt::core::StableId parent = current->has_hierarchy ? current->hierarchy.parent : 0u;
      if (parent == 0u) {
        break;
      }
      const auto parentIt = entityById.find(parent);
      current = (parentIt != entityById.end()) ? parentIt->second : nullptr;
    }
    animatedPathCache.emplace(entity.id, animated);
    return animated;
  };

  auto entity_has_scripted_transform_path =
      [&](const vkpt::scene::SceneEntityDefinition& entity) {
    if (const auto cached = scriptedPathCache.find(entity.id); cached != scriptedPathCache.end()) {
      return cached->second;
    }
    bool scripted = false;
    const auto* current = &entity;
    for (std::size_t depth = 0u; current != nullptr && depth <= doc.entities.size(); ++depth) {
      if (const auto cached = scriptedPathCache.find(current->id); cached != scriptedPathCache.end()) {
        scripted = cached->second;
        break;
      }
      if (current->script.enabled && !current->script.script.empty()) {
        scripted = true;
        break;
      }
      const vkpt::core::StableId parent = current->has_hierarchy ? current->hierarchy.parent : 0u;
      if (parent == 0u) {
        break;
      }
      const auto parentIt = entityById.find(parent);
      current = (parentIt != entityById.end()) ? parentIt->second : nullptr;
    }
    scriptedPathCache.emplace(entity.id, scripted);
    return scripted;
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
  geometryById.reserve(doc.geometry.size());
  for (const auto& geometry : doc.geometry) {
    geometryById[geometry.id] = &geometry;
  }
  auto add_capacity = [](std::size_t& target, std::size_t amount) {
    const std::size_t maxValue = std::numeric_limits<std::size_t>::max();
    target = amount > maxValue - target ? maxValue : target + amount;
  };
  auto reserve_capacity = [](auto& values, std::size_t capacity) {
    if (capacity != std::numeric_limits<std::size_t>::max() && values.capacity() < capacity) {
      values.reserve(capacity);
    }
  };

  std::size_t meshVertexCapacity = 0u;
  std::size_t meshIndexCapacity = 0u;
  std::size_t dynamicVertexCapacity = 0u;
  std::size_t dynamicIndexCapacity = 0u;
  for (const auto& entity : doc.entities) {
    if (!entity.has_mesh || !entity_visible_path(entity)) {
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
    add_capacity(meshVertexCapacity, geometry->vertices.size());
    add_capacity(meshIndexCapacity, geometry->indices.size());
    const bool physicsDynamic =
        entity.has_physics_body && entity.physics_body.enabled && entity.physics_body.dynamic;
    if (physicsDynamic ||
        entity_has_animated_transform_path(entity) ||
        entity_has_scripted_transform_path(entity)) {
      add_capacity(dynamicVertexCapacity, geometry->vertices.size());
      add_capacity(dynamicIndexCapacity, geometry->indices.size());
    }
  }
  reserve_capacity(scene.vertices, meshVertexCapacity);
  reserve_capacity(scene.texcoords, meshVertexCapacity);
  reserve_capacity(scene.indices, meshIndexCapacity);
  reserve_capacity(scene.local_vertices, dynamicVertexCapacity);
  reserve_capacity(scene.local_indices, dynamicIndexCapacity);

  for (const auto& entity : doc.entities) {
    if (!entity.has_mesh || !entity_visible_path(entity)) {
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
    const bool physics_dynamic =
        entity.has_physics_body && entity.physics_body.enabled && entity.physics_body.dynamic;
    const bool animation_dynamic = entity_has_animated_transform_path(entity);
    const bool scripted_dynamic = entity_has_scripted_transform_path(entity);
    const bool dynamic_transform =
        physics_dynamic || animation_dynamic || scripted_dynamic;
    // Dynamic instances keep local geometry alongside world-space triangles for later refits.
    if (scene.vertices.size() >
        static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) -
            geometry->vertices.size()) {
      log_scene_conversion_warning(
          "skipped mesh instance because vertex index range exceeds uint32",
          entity.id,
          entity.mesh.mesh_id);
      continue;
    }
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

    if ((scene.indices.size() / 3u) >
        static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
      log_scene_conversion_warning(
          "skipped mesh instance because triangle range exceeds uint32",
          entity.id,
          entity.mesh.mesh_id);
      continue;
    }
    const uint32_t firstTriangle = static_cast<uint32_t>(scene.indices.size() / 3u);
    std::vector<uint32_t> validLocalIndices;
    if (dynamic_transform) {
      validLocalIndices.reserve(geometry->indices.size());
    }
    std::size_t validIndexCount = 0u;
    std::size_t skippedInvalidTriangles = 0u;
    for (std::size_t index = 0u; index + 2u < geometry->indices.size(); index += 3u) {
      const uint32_t i0 = geometry->indices[index + 0u];
      const uint32_t i1 = geometry->indices[index + 1u];
      const uint32_t i2 = geometry->indices[index + 2u];
      if (i0 >= geometry->vertices.size() ||
          i1 >= geometry->vertices.size() ||
          i2 >= geometry->vertices.size()) {
        ++skippedInvalidTriangles;
        continue;
      }
      validIndexCount += 3u;
      if (dynamic_transform) {
        validLocalIndices.push_back(i0);
        validLocalIndices.push_back(i1);
        validLocalIndices.push_back(i2);
      }
      scene.indices.push_back(firstVertex + i0);
      scene.indices.push_back(firstVertex + i1);
      scene.indices.push_back(firstVertex + i2);
    }
    if (skippedInvalidTriangles != 0u) {
      log_scene_conversion_warning(
          "skipped mesh triangles with out-of-range indices",
          entity.id,
          entity.mesh.mesh_id);
    }
    const uint32_t validTriangleCount =
        static_cast<uint32_t>(validIndexCount / 3u);
    if (validTriangleCount == 0u) {
      log_scene_conversion_warning(
          "skipped mesh instance with no valid triangles",
          entity.id,
          entity.mesh.mesh_id);
      continue;
    }
    RTInstance instance;
    instance.entity_id = entity.id;
    instance.geometry_id = static_cast<uint32_t>(entity.mesh.mesh_id);
    instance.first_triangle = firstTriangle;
    instance.triangle_count = validTriangleCount;
    instance.material_index = materialIndex;
    if (dynamic_transform) {
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
      instance.local_index_count = static_cast<uint32_t>(validIndexCount);
      for (const auto index : validLocalIndices) {
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
      const uint64_t sourceTriangles = validTriangleCount;
      const uint64_t verticesPerTriangle = ((factor + 1u) * (factor + 2u)) / 2u;
      const uint64_t generatedVertices = sourceTriangles * verticesPerTriangle;
      const uint64_t generatedIndices = sourceTriangles * factor * factor * 3u;
      RTTessellationRequest request{};
      request.geometry_id = static_cast<uint32_t>(entity.mesh.mesh_id);
      request.first_triangle = firstTriangle;
      request.source_triangle_count = saturating_u32(sourceTriangles);
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
  ecsSdfIds.reserve(doc.entities.size());
  for (const auto& entity : doc.entities) {
    if (!entity.has_sdf_primitive) {
      continue;
    }
    ecsSdfIds.insert(entity.id);
    if (!entity_visible_path(entity)) {
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

  auto material_index_for_id = [&](vkpt::core::StableId materialId) {
    if (materialId != 0) {
      if (auto mi = materialLookup.find(materialId); mi != materialLookup.end()) {
        return mi->second;
      }
    }
    return 0u;
  };

  auto make_particle_material_variant = [&](uint32_t baseMaterialIndex,
                                            float brightness,
                                            float emissionBoost,
                                            float alphaScale) {
    RTMaterial material = scene.materials.empty()
                              ? RTMaterial{}
                              : scene.materials[std::min<std::size_t>(baseMaterialIndex, scene.materials.size() - 1u)];
    material.albedo = {
        std::min(1.5f, material.albedo.x * brightness),
        std::min(1.5f, material.albedo.y * brightness),
        std::min(1.5f, material.albedo.z * brightness)};
    material.emissive = {
        material.emissive.x + material.albedo.x * emissionBoost,
        material.emissive.y + material.albedo.y * emissionBoost,
        material.emissive.z + material.albedo.z * emissionBoost};
    material.alpha = clamp01(material.alpha * alphaScale);
    material.material_flags |= 1u;
    scene.materials.push_back(material);
    return static_cast<uint32_t>(scene.materials.size() - 1u);
  };

  auto emitter_world_position = [&](const vkpt::scene::SceneParticleEmitterDefinition& emitter,
                                    const Vec3& local) {
    return transform_point(
        local,
        Vec3{emitter.transform.translation.x, emitter.transform.translation.y, emitter.transform.translation.z},
        emitter.transform.rotation,
        Vec3{emitter.transform.scale.x, emitter.transform.scale.y, emitter.transform.scale.z});
  };

  struct ParticlePhysicsSample {
    Vec3 position{};
    Vec3 velocity{};
  };

  auto simulate_emitter_particle = [&](const vkpt::scene::SceneParticleEmitterDefinition& emitter,
                                       const Vec3& basePosition,
                                       const Vec3& velocityJitter,
                                       uint64_t seed,
                                       float age) {
    const Vec3 baseVelocity{
        emitter.velocity.x + velocityJitter.x,
        emitter.velocity.y + velocityJitter.y,
        emitter.velocity.z + velocityJitter.z};
    const Vec3 acceleration{
        emitter.wind.x,
        emitter.wind.y - 9.80665f * emitter.gravity_scale,
        emitter.wind.z};
    const float dragDamping = std::exp(-std::max(0.0f, emitter.drag) * age);
    Vec3 position = basePosition + baseVelocity * (age * dragDamping) +
                    acceleration * (0.5f * age * age * dragDamping);
    Vec3 velocity = (baseVelocity + acceleration * age) * dragDamping;

    if (std::fabs(emitter.vortex_strength) > 1.0e-6f) {
      const float radiusScale = std::sqrt(std::max(0.0f, position.x * position.x + position.z * position.z));
      const float angle = emitter.vortex_strength * age * (0.35f + 0.65f * random01(seed, 31u));
      const float c = std::cos(angle);
      const float s = std::sin(angle);
      const Vec3 before = position;
      position.x = before.x * c - before.z * s;
      position.z = before.x * s + before.z * c;
      velocity.x += -position.z * emitter.vortex_strength * 0.1f * radiusScale;
      velocity.z += position.x * emitter.vortex_strength * 0.1f * radiusScale;
    }

    if (emitter.bounce > 0.0f && position.y < emitter.collision_plane_y) {
      position.y = emitter.collision_plane_y + (emitter.collision_plane_y - position.y) * emitter.bounce;
      if (velocity.y < 0.0f) {
        velocity.y = -velocity.y * emitter.bounce;
      }
      velocity.x *= 1.0f - 0.25f * emitter.bounce;
      velocity.z *= 1.0f - 0.25f * emitter.bounce;
    }
    return ParticlePhysicsSample{position, velocity};
  };

  auto emitter_lifetime = [](const vkpt::scene::SceneParticleEmitterDefinition& emitter) {
    return std::max(0.001f, emitter.lifetime);
  };

  auto emitter_phase = [&](const vkpt::scene::SceneParticleEmitterDefinition& emitter,
                           uint64_t seed,
                           uint32_t stream) {
    const float lifetime = emitter_lifetime(emitter);
    const float animatedTime = emitter.time + (animation.advance_emitters ? animation.seconds : 0.0f);
    return std::fmod(random01(seed, stream) + animatedTime / lifetime, 1.0f);
  };

  auto append_rain_emitter = [&](const vkpt::scene::SceneParticleEmitterDefinition& emitter) {
    if (!emitter.enabled || emitter.count == 0u) {
      return;
    }
    if (scene.vertices.size() >
        static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) -
            static_cast<std::size_t>(emitter.count) * 12u) {
      log_scene_conversion_warning("skipped rain emitter because vertex range exceeds uint32",
                                   emitter.id,
                                   0u);
      return;
    }
    const uint32_t baseMaterialIndex = material_index_for_id(emitter.material_id);
    constexpr uint32_t kRainBuckets = 4u;
    std::array<uint32_t, kRainBuckets> materialBuckets{
        make_particle_material_variant(baseMaterialIndex, 0.75f, 0.00f, 0.80f),
        make_particle_material_variant(baseMaterialIndex, 0.95f, 0.01f, 0.95f),
        make_particle_material_variant(baseMaterialIndex, 1.18f, 0.025f, 1.05f),
        make_particle_material_variant(baseMaterialIndex, 1.42f, 0.05f, 1.15f)};
    const float halfRadius = std::max(0.001f, emitter.radius);
    const float streakLength = std::max(0.002f, emitter.length);
    const Vec3 bounds{
        std::max(0.001f, emitter.bounds.x),
        std::max(0.001f, emitter.bounds.y),
        std::max(0.001f, emitter.bounds.z)};
    for (uint32_t bucket = 0u; bucket < kRainBuckets; ++bucket) {
      const uint32_t firstTriangle = static_cast<uint32_t>(scene.indices.size() / 3u);
      const uint32_t firstVertex = static_cast<uint32_t>(scene.vertices.size());
      uint32_t bucketDroplets = 0u;
      for (uint32_t i = 0u; i < emitter.count; ++i) {
        const uint64_t seed = (static_cast<uint64_t>(emitter.seed) << 32u) ^
                              static_cast<uint64_t>(emitter.id) ^
                              static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
        const uint32_t particleBucket = static_cast<uint32_t>(random01(seed, 23u) * kRainBuckets) % kRainBuckets;
        if (particleBucket != bucket) {
          continue;
        }
      const float phase = emitter_phase(emitter, seed, 11u);
      const Vec3 local{
          random_signed(seed, 1u) * bounds.x,
          (0.5f - phase) * 2.0f * bounds.y,
          random_signed(seed, 3u) * bounds.z};
      const Vec3 jitter{
          random_signed(seed, 5u) * emitter.velocity_jitter.x,
          random_signed(seed, 6u) * emitter.velocity_jitter.y,
          random_signed(seed, 7u) * emitter.velocity_jitter.z};
      const auto physics = simulate_emitter_particle(
          emitter,
          local,
          jitter,
          seed,
          phase * emitter_lifetime(emitter));
      const Vec3 particleDirection = normalize(
          length_sq(physics.velocity) <= kEpsilon ? Vec3{0.0f, -1.0f, 0.0f} : physics.velocity);
      const Vec3 center = emitter_world_position(emitter, physics.position);
      Vec3 viewDirection = normalize(scene.camera_position - center);
      if (length_sq(viewDirection) <= kEpsilon) {
        viewDirection = {0.0f, 0.0f, 1.0f};
      }
      Vec3 basisA = normalize(cross(particleDirection, viewDirection));
      if (length_sq(basisA) <= kEpsilon) {
        basisA = normalize(cross(particleDirection, Vec3{1.0f, 0.0f, 0.0f}));
      }
      const Vec3 basisB = normalize(cross(viewDirection, basisA));
      const float widthJitter = 0.72f + random01(seed, 29u) * 0.75f;
      const Vec3 a = basisA * (halfRadius * widthJitter);
      const Vec3 b = basisB * (halfRadius * widthJitter);
      const Vec3 tip = center + particleDirection * (streakLength * 0.48f);
      const Vec3 neck = center + particleDirection * (streakLength * 0.15f);
      const Vec3 belly = center + particleDirection * (-streakLength * 0.28f);
      const Vec3 tail = center + particleDirection * (-streakLength * 0.58f);
      const uint32_t v = static_cast<uint32_t>(scene.vertices.size());
      scene.vertices.push_back(tip);
      scene.vertices.push_back(neck + a * -0.42f);
      scene.vertices.push_back(neck + a * 0.42f);
      scene.vertices.push_back(belly + a * -1.35f);
      scene.vertices.push_back(belly + a * 1.35f);
      scene.vertices.push_back(tail);
      scene.vertices.push_back(tip);
      scene.vertices.push_back(neck + b * -0.42f);
      scene.vertices.push_back(neck + b * 0.42f);
      scene.vertices.push_back(belly + b * -1.35f);
      scene.vertices.push_back(belly + b * 1.35f);
      scene.vertices.push_back(tail);
      for (uint32_t uv = 0u; uv < 12u; ++uv) {
        scene.texcoords.push_back(Vec2{});
      }
      const uint32_t quadIndices[] = {
          v + 0u, v + 1u, v + 2u,
          v + 1u, v + 3u, v + 4u,
          v + 1u, v + 4u, v + 2u,
          v + 3u, v + 5u, v + 4u,
          v + 6u, v + 7u, v + 8u,
          v + 7u, v + 9u, v + 10u,
          v + 7u, v + 10u, v + 8u,
          v + 9u, v + 11u, v + 10u};
      scene.indices.insert(scene.indices.end(), std::begin(quadIndices), std::end(quadIndices));
        ++bucketDroplets;
      }
      if (bucketDroplets == 0u) {
        continue;
      }
      RTInstance instance;
      instance.entity_id = emitter.id + bucket;
      instance.geometry_id = static_cast<uint32_t>((emitter.id + bucket) & 0xffffffffu);
      instance.first_triangle = firstTriangle;
      instance.triangle_count = static_cast<uint32_t>((scene.indices.size() / 3u) - firstTriangle);
      instance.material_index = materialBuckets[bucket];
      instance.transform_revision = static_cast<uint32_t>(scene.instances.size() + 1u);
      instance.translation = {emitter.transform.translation.x, emitter.transform.translation.y, emitter.transform.translation.z};
      instance.rotation = {emitter.transform.rotation.x, emitter.transform.rotation.y, emitter.transform.rotation.z, emitter.transform.rotation.w};
      instance.scale = {emitter.transform.scale.x, emitter.transform.scale.y, emitter.transform.scale.z};
      scene.instances.push_back(instance);
      (void)firstVertex;
    }
  };

  auto append_smoke_emitter = [&](const vkpt::scene::SceneParticleEmitterDefinition& emitter) {
    if (!emitter.enabled || emitter.count == 0u) {
      return;
    }
    const uint32_t materialIndex = make_particle_material_variant(
        material_index_for_id(emitter.material_id), 1.65f, 0.22f, 0.34f);
    scene.materials[materialIndex].material_effect = 0u;
    scene.materials[materialIndex].material_model = 0u;
    scene.materials[materialIndex].emissive = {
        std::max(scene.materials[materialIndex].emissive.x, 0.18f),
        std::max(scene.materials[materialIndex].emissive.y, 0.18f),
        std::max(scene.materials[materialIndex].emissive.z, 0.18f)};
    const uint32_t firstTriangle = static_cast<uint32_t>(scene.indices.size() / 3u);
    const Vec3 bounds{
        std::max(0.001f, emitter.bounds.x),
        std::max(0.001f, emitter.bounds.y),
        std::max(0.001f, emitter.bounds.z)};
    uint32_t sheetCount = 0u;
    for (uint32_t i = 0u; i < emitter.count; ++i) {
      const uint64_t seed = (static_cast<uint64_t>(emitter.seed) << 32u) ^
                            static_cast<uint64_t>(emitter.id) ^
                            static_cast<uint64_t>(i) * 0xbf58476d1ce4e5b9ULL;
      const float phase = emitter_phase(emitter, seed, 17u);
      const Vec3 baseLocal{
          random_signed(seed, 1u) * bounds.x,
          random_signed(seed, 2u) * bounds.y,
          random_signed(seed, 3u) * bounds.z};
      const Vec3 jitter{
          random_signed(seed, 5u) * emitter.velocity_jitter.x,
          random_signed(seed, 6u) * emitter.velocity_jitter.y,
          random_signed(seed, 7u) * emitter.velocity_jitter.z};
      auto physics = simulate_emitter_particle(
          emitter,
          baseLocal,
          jitter,
          seed,
          phase * emitter_lifetime(emitter));
      Vec3 local = physics.position;
      local.x += random_signed(seed, 8u) * emitter.turbulence * phase;
      local.z += random_signed(seed, 9u) * emitter.turbulence * phase;

      const Vec3 center = emitter_world_position(emitter, local);
      const float growth = 0.65f + 0.85f * phase;
      const float wobble = 0.75f + random01(seed, 13u) * 0.65f;
      const float plumeHeight = std::max(0.04f, emitter.radius * (2.2f + 3.0f * phase) * wobble);
      const float plumeWidth = std::max(0.03f, emitter.radius * (0.9f + 1.9f * phase) * wobble);
      Vec3 viewDirection = normalize(scene.camera_position - center);
      if (length_sq(viewDirection) <= kEpsilon) {
        viewDirection = {0.0f, 0.0f, 1.0f};
      }
      const Vec3 up{0.0f, 1.0f, 0.0f};
      Vec3 axisA = normalize(cross(up, viewDirection));
      if (length_sq(axisA) <= kEpsilon) {
        axisA = {1.0f, 0.0f, 0.0f};
      }
      const float angle = random01(seed, 41u) * 0.7f + emitter.vortex_strength * phase * 0.25f;
      axisA = normalize(axisA * std::cos(angle) + up * (std::sin(angle) * 0.18f));
      const Vec3 axisB = normalize(cross(viewDirection, axisA));
      const Vec3 bottom = center - up * (plumeHeight * 0.35f);
      const Vec3 mid = center + up * (plumeHeight * 0.10f);
      const Vec3 top = center + up * (plumeHeight * 0.65f);
      const uint32_t v = static_cast<uint32_t>(scene.vertices.size());
      scene.vertices.push_back(bottom + axisA * (-plumeWidth * 0.28f));
      scene.vertices.push_back(bottom + axisA * (plumeWidth * 0.28f));
      scene.vertices.push_back(mid + axisA * (plumeWidth * 0.95f));
      scene.vertices.push_back(mid + axisA * (-plumeWidth * 0.95f));
      scene.vertices.push_back(top + axisA * (-plumeWidth * 0.42f));
      scene.vertices.push_back(top + axisA * (plumeWidth * 0.42f));
      scene.vertices.push_back(bottom + axisB * (-plumeWidth * 0.22f));
      scene.vertices.push_back(bottom + axisB * (plumeWidth * 0.22f));
      scene.vertices.push_back(mid + axisB * (plumeWidth * 0.82f));
      scene.vertices.push_back(mid + axisB * (-plumeWidth * 0.82f));
      scene.vertices.push_back(top + axisB * (-plumeWidth * 0.36f));
      scene.vertices.push_back(top + axisB * (plumeWidth * 0.36f));
      for (uint32_t uv = 0u; uv < 12u; ++uv) {
        scene.texcoords.push_back(Vec2{});
      }
      const uint32_t plumeIndices[] = {
          v + 0u, v + 1u, v + 2u, v + 0u, v + 2u, v + 3u,
          v + 3u, v + 2u, v + 5u, v + 3u, v + 5u, v + 4u,
          v + 6u, v + 7u, v + 8u, v + 6u, v + 8u, v + 9u,
          v + 9u, v + 8u, v + 11u, v + 9u, v + 11u, v + 10u};
      scene.indices.insert(scene.indices.end(), std::begin(plumeIndices), std::end(plumeIndices));
      ++sheetCount;

      (void)growth;
    }
    if (sheetCount != 0u) {
      RTInstance instance;
      instance.entity_id = emitter.id;
      instance.geometry_id = static_cast<uint32_t>(emitter.id & 0xffffffffu);
      instance.first_triangle = firstTriangle;
      instance.triangle_count = static_cast<uint32_t>((scene.indices.size() / 3u) - firstTriangle);
      instance.material_index = materialIndex;
      instance.transform_revision = static_cast<uint32_t>(scene.instances.size() + 1u);
      instance.translation = {emitter.transform.translation.x, emitter.transform.translation.y, emitter.transform.translation.z};
      instance.rotation = {emitter.transform.rotation.x, emitter.transform.rotation.y, emitter.transform.rotation.z, emitter.transform.rotation.w};
      instance.scale = {emitter.transform.scale.x, emitter.transform.scale.y, emitter.transform.scale.z};
      scene.instances.push_back(instance);
    }
  };

  auto add_light = [&](vkpt::core::StableId id,
                       const vkpt::scene::LightComponent& light,
                       bool enabled) {
    const float effectiveIntensity = enabled ? std::max(0.0f, light.intensity) : 0.0f;
    if (is_environment_light_type(light.type)) {
      if (effectiveIntensity <= 0.0f) {
        return;
      }
      const Vec3 env = environment_color_from_light(light);
      scene.environment_color = {
          scene.environment_color.x + env.x,
          scene.environment_color.y + env.y,
          scene.environment_color.z + env.z};
      scene.environment_map_scale = {
          scene.environment_map_scale.x + env.x,
          scene.environment_map_scale.y + env.y,
          scene.environment_map_scale.z + env.z};
      return;
    }
    bool hasTransform = false;
    const auto entityIt = entityById.find(id);
    const auto* entity = entityIt != entityById.end() ? entityIt->second : nullptr;
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
    scene.lights.back().intensity = effectiveIntensity;
  };

  std::unordered_set<vkpt::core::StableId> ecsLightIds;
  ecsLightIds.reserve(doc.entities.size());
  for (const auto& entity : doc.entities) {
    if (!entity.has_light) {
      continue;
    }
    ecsLightIds.insert(entity.id);
    add_light(entity.id, entity.light, entity_visible_path(entity));
  }

  for (const auto& light : doc.lights) {
    if (!ecsLightIds.contains(light.id)) {
      add_light(light.id, light.light, true);
    }
  }

  if (!scene.environment_map.empty() &&
      std::max({scene.environment_map_scale.x,
                scene.environment_map_scale.y,
                scene.environment_map_scale.z}) <= 1.0e-6f) {
    scene.environment_map_scale = {1.0f, 1.0f, 1.0f};
  }

  auto apply_camera = [&](vkpt::core::StableId id,
                          const vkpt::scene::CameraComponent& camera,
                          bool* has_transform) {
    bool cameraTransform = false;
    const auto entityIt = entityById.find(id);
    const auto* entity = entityIt != entityById.end() ? entityIt->second : nullptr;
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
    if (entity.has_camera && entity_visible_path(entity)) {
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

  }

  for (const auto& emitter : doc.particle_emitters) {
    if (is_rain_particle_type(emitter.type)) {
      append_rain_emitter(emitter);
    } else if (is_smoke_particle_type(emitter.type)) {
      append_smoke_emitter(emitter);
    }
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
