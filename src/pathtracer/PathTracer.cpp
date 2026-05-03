#include "pathtracer/PathTracer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvPi = 0.31830988618f;
constexpr float kEpsilon = 1e-4f;
constexpr float kMinMarchStep = 1.0e-3f;
constexpr uint32_t kMaxMarchSteps = 192u;
constexpr float kMaxMarchDistance = 10000.0f;

uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
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

void write_u16_be(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>(value & 0xffu));
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

void write_u32_le(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
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

uint8_t to_byte(float linear) {
  float clamped = std::min(1.0f, std::max(0.0f, linear));
  return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
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

vkpt::pathtracer::Vec3 normalize(const vkpt::pathtracer::Vec3& value) {
  const float l = length(value);
  if (l <= kEpsilon) {
    return {0.0f, 1.0f, 0.0f};
  }
  return value / l;
}

vkpt::pathtracer::Vec3 clamp01(const vkpt::pathtracer::Vec3& value) {
  const auto c = [](float v) { return std::min(1.0f, std::max(0.0f, v)); };
  return {c(value.x), c(value.y), c(value.z)};
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

vkpt::pathtracer::Vec3 rotate_euler(const vkpt::pathtracer::Vec3& value, const vkpt::pathtracer::Vec3& rotation) {
  return rotate_x(rotate_y(rotate_z(value, rotation.z), rotation.y), rotation.x);
}

vkpt::pathtracer::Vec3 rotate_euler_inv(const vkpt::pathtracer::Vec3& value, const vkpt::pathtracer::Vec3& rotation) {
  return rotate_z(rotate_y(rotate_x(value, -rotation.x), -rotation.y), -rotation.z);
}

vkpt::pathtracer::Vec3 transform_point(const vkpt::pathtracer::Vec3& point,
                                       const vkpt::pathtracer::Vec3& translation,
                                       const vkpt::pathtracer::Vec3& scale) {
  return {point.x * scale.x + translation.x, point.y * scale.y + translation.y, point.z * scale.z + translation.z};
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

vkpt::pathtracer::Vec3 parse_shape_rotation(const vkpt::scene::Vec3& rot) {
  return {rot.x, rot.y, rot.z};
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

float radians(float deg) {
  return deg * (kPi / 180.0f);
}

}  // namespace

namespace vkpt::pathtracer {

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

FilmLdr FilmBuffer::resolve_ldr() const {
  FilmLdr out;
  out.width = m_width;
  out.height = m_height;
  out.rgba8.assign(static_cast<std::size_t>(m_width) * m_height * 4, 0u);

  for (uint32_t y = 0; y < m_height; ++y) {
    for (uint32_t x = 0; x < m_width; ++x) {
      const auto idx = static_cast<std::size_t>(y) * m_width + x;
      const float invSamples = 1.0f / std::max(1u, m_sampleCounts[idx]);
      const Vec3 linear = clamp01({m_accumulation[idx].x * invSamples,
                                  m_accumulation[idx].y * invSamples,
                                  m_accumulation[idx].z * invSamples});
      const auto base = static_cast<std::size_t>(y) * m_width * 4 + static_cast<std::size_t>(x) * 4;
      out.rgba8[base + 0] = to_byte(std::pow(linear.x, 1.0f / 2.2f));
      out.rgba8[base + 1] = to_byte(std::pow(linear.y, 1.0f / 2.2f));
      out.rgba8[base + 2] = to_byte(std::pow(linear.z, 1.0f / 2.2f));
      out.rgba8[base + 3] = 255;
    }
  }
  return out;
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

bool ScalarCpuPathTracer::configure(const RenderSettings& settings) {
  m_settings = settings;
  m_film = FilmBuffer{settings.width, settings.height};
  m_film.clear();
  m_counters = {};
  m_configured = true;
  m_has_scene = false;
  m_scene = RTSceneData{};
  return true;
}

bool ScalarCpuPathTracer::load_scene_snapshot(const RTSceneData& scene) {
  if (!m_configured) {
    return false;
  }
  m_scene = scene;
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
  ++m_counters.triangle_tests;
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
  return intersect_box(primitive, ray, t, normal);
}

bool ScalarCpuPathTracer::intersect_box(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const {
  for (uint32_t i = 0; i < kMaxMarchSteps; ++i) {
    ++m_counters.sdf_tests;
    const Vec3 point = ray.origin + ray.direction * t;
    const float d = sdf_distance(primitive, point);
    if (d <= kEpsilon) {
      sdf_normal(primitive, point, normal);
      ++m_counters.sdf_hits;
      return true;
    }
    t += std::max(kMinMarchStep, d);
    if (t > kMaxMarchDistance) {
      return false;
    }
  }
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
        ++m_counters.triangle_hits;
        continue;
      }
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
  for (uint32_t depth = 0; depth < m_settings.max_depth; ++depth) {
    ++ray_counter;
    Hit hit;
    if (!intersect_scene(ray, hit)) {
      radiance += throughput * m_scene.environment_color;
      break;
    }

    const auto index = std::min(hit.material_index, static_cast<uint32_t>(m_scene.materials.size() - 1));
    const auto& material = m_scene.materials[index];
    radiance += throughput * material.emissive;

    const Vec3 inDir = -ray.direction;
    const Vec3 outDir = sample_hemisphere(rng, hit.normal);
    float pdf = 0.0f;
    const Vec3 bsdf = evaluate_bsdf(material, hit.normal, inDir, outDir, pdf);
    if (pdf <= 0.0f) {
      break;
    }
    const float cosTheta = std::max(0.0f, dot(hit.normal, outDir));
    throughput = throughput * bsdf * (cosTheta / pdf);

    if (std::max(throughput.x, std::max(throughput.y, throughput.z)) < 0.001f) {
      break;
    }
    const float rr = std::min(0.99f, std::max(0.1f, std::max(throughput.x, std::max(throughput.y, throughput.z))));
    if (depth >= 3 && rng.next01() > rr) {
      break;
    }
    throughput = throughput / rr;
    ray.origin = hit.position + hit.normal * 0.002f;
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
  for (uint32_t y = start_y; y < maxY; ++y) {
    for (uint32_t x = 0; x < m_settings.width; ++x) {
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
        vkpt::log::Logger::instance().log(vkpt::log::Severity::Warning, "pathtracer", "non-finite sample", {
          {"pixel", std::to_string(sampleSeed)},
          {"sample", std::to_string(sample_index)}
        });
        continue;
      }
      m_film.add_sample(x, y, sample);
      m_counters.samples += 1;
      m_counters.rays += rayCounter;
    }
  }
  return true;
}

void ScalarCpuPathTracer::shutdown() {
  m_configured = false;
  m_has_scene = false;
  m_film = FilmBuffer{};
  m_scene = RTSceneData{};
}

vkpt::core::Result<RTSceneData> BuildSceneDataFromDocument(const vkpt::scene::SceneDocument& doc) {
  RTSceneData scene;
  scene.camera_position = {0.0f, 1.0f, 4.0f};
  scene.camera_target = {0.0f, 1.0f, 0.0f};
  scene.camera_up = {0.0f, 1.0f, 0.0f};
  scene.camera_fov_deg = 55.0f;
  scene.environment_color = {0.0f, 0.0f, 0.0f};

  for (const auto& asset : doc.assets) {
    if (!asset.uri.empty()) {
      scene.textures.push_back(asset.uri);
    }
  }

  std::vector<vkpt::scene::SceneMaterialDefinition> materials = doc.materials;
  if (materials.empty()) {
    materials.push_back({});
  }
  std::unordered_map<vkpt::core::StableId, uint32_t> materialLookup;
  for (const auto& material : materials) {
    const uint32_t index = static_cast<uint32_t>(scene.materials.size());
    materialLookup[material.id] = index;
    RTMaterial outMaterial;
    outMaterial.albedo = {material.albedo.x, material.albedo.y, material.albedo.z};
    outMaterial.emissive = {material.emission.x * material.emission_intensity, material.emission.y * material.emission_intensity,
                            material.emission.z * material.emission_intensity};
    outMaterial.roughness = material.roughness;
    scene.materials.push_back(outMaterial);
  }

  std::unordered_map<vkpt::core::StableId, vkpt::scene::TransformComponent> entityTransforms;
  for (const auto& entity : doc.entities) {
    if (entity.has_transform) {
      entityTransforms[entity.id] = entity.transform;
    }
  }

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

    const uint32_t materialId = entity.mesh.material_id != 0 ? entity.mesh.material_id : geometry->material_id;
    uint32_t materialIndex = 0;
    if (auto mi = materialLookup.find(materialId); mi != materialLookup.end()) {
      materialIndex = mi->second;
    }

    const auto translation = entity.has_transform ? entity.transform.translation : vkpt::scene::TransformComponent{}.translation;
    const auto scale = entity.has_transform ? entity.transform.scale : vkpt::scene::TransformComponent{}.scale;
    const uint32_t firstVertex = static_cast<uint32_t>(scene.vertices.size());
    for (const auto& vertex : geometry->vertices) {
      scene.vertices.push_back(transform_point(
          Vec3{vertex.x, vertex.y, vertex.z}, Vec3{translation.x, translation.y, translation.z}, Vec3{scale.x, scale.y, scale.z}));
    }

    const uint32_t firstTriangle = static_cast<uint32_t>(scene.indices.size() / 3u);
    for (const auto& index : geometry->indices) {
      scene.indices.push_back(firstVertex + index);
    }
    RTInstance instance;
    instance.geometry_id = static_cast<uint32_t>(entity.mesh.mesh_id);
    instance.first_triangle = firstTriangle;
    instance.triangle_count = static_cast<uint32_t>(geometry->indices.size() / 3u);
    instance.material_index = materialIndex;
    instance.translation = Vec3{translation.x, translation.y, translation.z};
    instance.scale = Vec3{scale.x, scale.y, scale.z};
    scene.instances.push_back(instance);
  }

  for (const auto& prim : doc.sdf_primitives) {
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

  for (const auto& light : doc.lights) {
    if (light.light.intensity <= 0.0f) {
      continue;
    }
    Vec3 pos{};
    if (auto it = entityTransforms.find(light.id); it != entityTransforms.end()) {
      pos = {it->second.translation.x, it->second.translation.y, it->second.translation.z};
    }
    scene.lights.push_back(RTHitLight{pos, {light.light.color.x, light.light.color.y, light.light.color.z}, light.light.intensity});
  }

  for (const auto& entity : doc.entities) {
    if (entity.has_camera) {
      const auto& transform = entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
      scene.camera_position = {transform.translation.x, transform.translation.y, transform.translation.z};
      scene.camera_fov_deg = entity.camera.fov;
      scene.camera_target = scene.camera_position + Vec3{0.0f, 0.0f, -1.0f};
      break;
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
    scene.instances.push_back({0, 0, static_cast<uint32_t>(scene.indices.size() / 3u), 0u, {0.0f, 0.0f, 0.0f},
                              {1.0f, 1.0f, 1.0f}});
    scene.sdf_primitives.push_back(
        RTSdfPrimitive{SdfShape::Sphere, {0.0f, 1.8f, 0.0f}, {0.35f, 0.35f, 0.35f}, {0.0f, 0.0f, 0.0f}, 3u, 0.35f, 0.0f, 0.0f});
    scene.lights.push_back(RTHitLight{{0.0f, 1.8f, 0.0f}, {6.0f, 6.0f, 6.0f}, 10.0f});
  }

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
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "environment_color", sizeof(Vec3), 1u, alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "materials", sizeof(RTMaterial), scene.materials.size(), alignof(RTMaterial));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "vertices", sizeof(Vec3), scene.vertices.size(), alignof(Vec3));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "indices", sizeof(std::uint32_t), scene.indices.size(), alignof(std::uint32_t));
  append_field(manifest.fields, cpuCursor, gpuCursor, "RTSceneData", "instances", sizeof(RTInstance), scene.instances.size(), alignof(RTInstance));
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

}  // namespace vkpt::pathtracer



