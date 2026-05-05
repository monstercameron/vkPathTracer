#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Vec3 {
  float x;
  float y;
  float z;
};

struct Ray {
  Vec3 origin;
  Vec3 direction;
};

struct Triangle {
  Vec3 v0;
  Vec3 v1;
  Vec3 v2;
};

struct TriangleCached {
  Vec3 v0;
  Vec3 e1;
  Vec3 e2;
};

struct Aabb {
  Vec3 mn;
  Vec3 mx;
};

struct RayWithInv {
  Ray ray;
  Vec3 inv_dir;
};

struct EdgeResult {
  std::string id;
  std::string from;
  std::string to;
  std::string domain;
  std::string metric;
  std::string decision;
  double baseline_ms = 0.0;
  double candidate_ms = 0.0;
  double speedup = 0.0;
  double checksum_baseline = 0.0;
  double checksum_candidate = 0.0;
  double max_abs_error = 0.0;
  bool accuracy_pass = true;
};

volatile double g_sink = 0.0;

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
  return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x,
  };
}

float clamp01(float v) {
  return std::min(1.0f, std::max(0.0f, v));
}

uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

float random01(uint64_t& state) {
  state = splitmix64(state);
  return static_cast<float>((state >> 8) & 0x00ffffffu) * (1.0f / 16777215.0f);
}

uint32_t pcg32(uint32_t v) {
  const uint32_t state = v * 747796405u + 2891336453u;
  const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

float pcg32_float(uint32_t& state) {
  state = pcg32(state);
  return static_cast<float>(state) * (1.0f / 4294967296.0f);
}

float fast_inv_sqrt(float value) {
  if (value <= 0.0f) {
    return 0.0f;
  }
  float x2 = value * 0.5f;
  float y = value;
  uint32_t bits = 0;
  std::memcpy(&bits, &y, sizeof(bits));
  bits = 0x5f3759dfu - (bits >> 1u);
  std::memcpy(&y, &bits, sizeof(y));
  y = y * (1.5f - (x2 * y * y));
  y = y * (1.5f - (x2 * y * y));
  return y;
}

Vec3 normalize_sqrt_div(Vec3 value) {
  const float len_sq = dot(value, value);
  const float len = std::sqrt(len_sq);
  if (len <= 1.0e-4f) {
    return {0.0f, 1.0f, 0.0f};
  }
  const float inv = 1.0f / len;
  return value * inv;
}

Vec3 normalize_rsqrt_nr(Vec3 value) {
  const float len_sq = dot(value, value);
  if (len_sq <= 1.0e-8f) {
    return {0.0f, 1.0f, 0.0f};
  }
  return value * fast_inv_sqrt(len_sq);
}

float fresnel_pow5(float cos_theta, float ior) {
  const float safe_ior = std::max(1.01f, ior);
  float r0 = (1.0f - safe_ior) / (1.0f + safe_ior);
  r0 *= r0;
  const float m = 1.0f - clamp01(cos_theta);
  return r0 + (1.0f - r0) * std::pow(m, 5.0f);
}

float fresnel_mul5(float cos_theta, float ior) {
  const float safe_ior = std::max(1.01f, ior);
  float r0 = (1.0f - safe_ior) / (1.0f + safe_ior);
  r0 *= r0;
  const float m = 1.0f - clamp01(cos_theta);
  const float m2 = m * m;
  return r0 + (1.0f - r0) * m2 * m2 * m;
}

bool intersect_triangle_baseline(const Triangle& tri, const Ray& ray, float& out_t) {
  constexpr float kEps = 1.0e-6f;
  const Vec3 e1 = tri.v1 - tri.v0;
  const Vec3 e2 = tri.v2 - tri.v0;
  const Vec3 h = cross(ray.direction, e2);
  const float det = dot(e1, h);
  if (std::fabs(det) < kEps) {
    return false;
  }
  const float inv_det = 1.0f / det;
  const Vec3 s = ray.origin - tri.v0;
  const float u = dot(s, h) * inv_det;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }
  const Vec3 q = cross(s, e1);
  const float v = dot(ray.direction, q) * inv_det;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }
  const float t = dot(e2, q) * inv_det;
  if (t <= kEps) {
    return false;
  }
  out_t = t;
  return true;
}

bool intersect_triangle_cached(const TriangleCached& tri, const Ray& ray, float& out_t) {
  constexpr float kEps = 1.0e-6f;
  const Vec3 h = cross(ray.direction, tri.e2);
  const float det = dot(tri.e1, h);
  if (std::fabs(det) < kEps) {
    return false;
  }
  const float inv_det = 1.0f / det;
  const Vec3 s = ray.origin - tri.v0;
  const float u = dot(s, h) * inv_det;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }
  const Vec3 q = cross(s, tri.e1);
  const float v = dot(ray.direction, q) * inv_det;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }
  const float t = dot(tri.e2, q) * inv_det;
  if (t <= kEps) {
    return false;
  }
  out_t = t;
  return true;
}

bool intersect_aabb_divide(const Aabb& aabb, const Ray& ray, float max_t, float& out_t) {
  float t_min = 1.0e-4f;
  float t_max = max_t;
  const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
  const float mn[3] = {aabb.mn.x, aabb.mn.y, aabb.mn.z};
  const float mx[3] = {aabb.mx.x, aabb.mx.y, aabb.mx.z};
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(direction[axis]) <= 1.0e-6f) {
      if (origin[axis] < mn[axis] || origin[axis] > mx[axis]) {
        return false;
      }
      continue;
    }
    const float inv_d = 1.0f / direction[axis];
    float t0 = (mn[axis] - origin[axis]) * inv_d;
    float t1 = (mx[axis] - origin[axis]) * inv_d;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    t_min = std::max(t_min, t0);
    t_max = std::min(t_max, t1);
    if (t_min > t_max) {
      return false;
    }
  }
  out_t = t_min;
  return t_max > 1.0e-4f;
}

bool intersect_aabb_inv_dir(const Aabb& aabb, const RayWithInv& ray, float max_t, float& out_t) {
  float t_min = 1.0e-4f;
  float t_max = max_t;
  const float origin[3] = {ray.ray.origin.x, ray.ray.origin.y, ray.ray.origin.z};
  const float inv_dir[3] = {ray.inv_dir.x, ray.inv_dir.y, ray.inv_dir.z};
  const float mn[3] = {aabb.mn.x, aabb.mn.y, aabb.mn.z};
  const float mx[3] = {aabb.mx.x, aabb.mx.y, aabb.mx.z};
  for (int axis = 0; axis < 3; ++axis) {
    float t0 = (mn[axis] - origin[axis]) * inv_dir[axis];
    float t1 = (mx[axis] - origin[axis]) * inv_dir[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    t_min = std::max(t_min, t0);
    t_max = std::min(t_max, t1);
    if (t_min > t_max) {
      return false;
    }
  }
  out_t = t_min;
  return t_max > 1.0e-4f;
}

std::vector<Vec3> make_vectors(std::size_t count) {
  std::vector<Vec3> values;
  values.reserve(count);
  uint64_t state = 0x123456789abcdef0ULL;
  for (std::size_t i = 0; i < count; ++i) {
    values.push_back({
        random01(state) * 2.0f - 1.0f,
        random01(state) * 2.0f - 1.0f,
        random01(state) * 2.0f - 1.0f,
    });
  }
  return values;
}

std::vector<Triangle> make_triangles(std::size_t count) {
  std::vector<Triangle> values;
  values.reserve(count);
  uint64_t state = 0x456789abcdef0123ULL;
  for (std::size_t i = 0; i < count; ++i) {
    const Vec3 v0{random01(state) * 8.0f - 4.0f, random01(state) * 8.0f - 4.0f,
                  random01(state) * 4.0f - 8.0f};
    const Vec3 e1{random01(state) * 0.5f + 0.01f, random01(state) * 0.5f,
                  random01(state) * 0.5f};
    const Vec3 e2{random01(state) * 0.5f, random01(state) * 0.5f + 0.01f,
                  random01(state) * 0.5f};
    values.push_back({v0, v0 + e1, v0 + e2});
  }
  return values;
}

std::vector<TriangleCached> cache_triangles(const std::vector<Triangle>& triangles) {
  std::vector<TriangleCached> cached;
  cached.reserve(triangles.size());
  for (const auto& tri : triangles) {
    cached.push_back({tri.v0, tri.v1 - tri.v0, tri.v2 - tri.v0});
  }
  return cached;
}

std::vector<Ray> make_rays(std::size_t count) {
  std::vector<Ray> values;
  values.reserve(count);
  uint64_t state = 0xfedcba9876543210ULL;
  for (std::size_t i = 0; i < count; ++i) {
    const Vec3 origin{random01(state) * 6.0f - 3.0f, random01(state) * 6.0f - 3.0f,
                      random01(state) * 2.0f + 1.0f};
    Vec3 direction{random01(state) * 0.6f - 0.3f, random01(state) * 0.6f - 0.3f, -1.0f};
    direction = normalize_sqrt_div(direction);
    values.push_back({origin, direction});
  }
  return values;
}

std::vector<RayWithInv> make_rays_with_inv(const std::vector<Ray>& rays) {
  std::vector<RayWithInv> out;
  out.reserve(rays.size());
  for (const auto& ray : rays) {
    auto safe_inv = [](float v) {
      return std::fabs(v) <= 1.0e-9f ? std::copysign(std::numeric_limits<float>::infinity(), v) : 1.0f / v;
    };
    out.push_back({ray, {safe_inv(ray.direction.x), safe_inv(ray.direction.y), safe_inv(ray.direction.z)}});
  }
  return out;
}

std::vector<Aabb> make_aabbs(std::size_t count) {
  std::vector<Aabb> values;
  values.reserve(count);
  uint64_t state = 0x0ddc0ffee1234567ULL;
  for (std::size_t i = 0; i < count; ++i) {
    Vec3 c{random01(state) * 8.0f - 4.0f, random01(state) * 8.0f - 4.0f,
           random01(state) * 8.0f - 6.0f};
    Vec3 e{random01(state) * 0.35f + 0.05f, random01(state) * 0.35f + 0.05f,
           random01(state) * 0.35f + 0.05f};
    values.push_back({c - e, c + e});
  }
  return values;
}

template <typename Fn>
double time_ms(Fn&& fn, int repeats = 1) {
  double best = std::numeric_limits<double>::infinity();
  for (int r = 0; r < repeats; ++r) {
    const auto start = std::chrono::steady_clock::now();
    const double checksum = fn();
    const auto end = std::chrono::steady_clock::now();
    g_sink += checksum * 1.0e-30;
    const double ms = std::chrono::duration<double, std::milli>(end - start).count();
    best = std::min(best, ms);
  }
  return best;
}

std::string escape_json(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    if (ch == '"') {
      out += "\\\"";
    } else if (ch == '\\') {
      out += "\\\\";
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string serialize_json(const std::vector<EdgeResult>& rows) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "{\n";
  out << "  \"schema\":\"ptopt_math_microbench.v1\",\n";
  out << "  \"sink\":" << g_sink << ",\n";
  out << "  \"edges\":[\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    out << "    {";
    out << "\"id\":\"" << escape_json(row.id) << "\",";
    out << "\"from\":\"" << escape_json(row.from) << "\",";
    out << "\"to\":\"" << escape_json(row.to) << "\",";
    out << "\"domain\":\"" << escape_json(row.domain) << "\",";
    out << "\"metric\":\"" << escape_json(row.metric) << "\",";
    out << "\"decision\":\"" << escape_json(row.decision) << "\",";
    out << "\"baseline_ms\":" << row.baseline_ms << ",";
    out << "\"candidate_ms\":" << row.candidate_ms << ",";
    out << "\"speedup\":" << row.speedup << ",";
    out << "\"checksum_baseline\":" << row.checksum_baseline << ",";
    out << "\"checksum_candidate\":" << row.checksum_candidate << ",";
    out << "\"max_abs_error\":" << row.max_abs_error << ",";
    out << "\"accuracy_pass\":" << (row.accuracy_pass ? "true" : "false");
    out << "}";
    if (i + 1 < rows.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

void finalize(EdgeResult& row) {
  row.speedup = row.candidate_ms > 0.0 ? row.baseline_ms / row.candidate_ms : 0.0;
  if (!row.accuracy_pass) {
    row.decision = "reject_accuracy";
  } else if (row.speedup > 1.02) {
    row.decision = "take_candidate";
  } else {
    row.decision = "keep_baseline";
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path output = "microbench_results.json";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--output" && i + 1 < argc) {
      output = argv[++i];
    }
  }

  constexpr std::size_t kVecCount = 1u << 20u;
  constexpr std::size_t kTriCount = 4096u;
  constexpr std::size_t kRayCount = 4096u;
  constexpr std::size_t kPairs = 1u << 21u;

  const auto vectors = make_vectors(kVecCount);
  const auto triangles = make_triangles(kTriCount);
  const auto cached_triangles = cache_triangles(triangles);
  const auto rays = make_rays(kRayCount);
  const auto rays_with_inv = make_rays_with_inv(rays);
  const auto aabbs = make_aabbs(kTriCount);

  std::vector<EdgeResult> rows;

  {
    EdgeResult row;
    row.id = "edge_cpu_normalize_rsqrt";
    row.from = "cpu.normalize.sqrt_div";
    row.to = "cpu.normalize.fast_rsqrt_nr";
    row.domain = "cpu_math";
    row.metric = "normalize vectors";
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    double max_error = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      for (const auto& v : vectors) {
        const auto n = normalize_sqrt_div(v);
        checksum += n.x * 0.25 + n.y * 0.5 + n.z;
      }
      checksum_a = checksum;
      return checksum;
    }, 3);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < vectors.size(); ++i) {
        const auto n = normalize_rsqrt_nr(vectors[i]);
        const auto ref = normalize_sqrt_div(vectors[i]);
        max_error = std::max(max_error, static_cast<double>(std::fabs(n.x - ref.x)));
        max_error = std::max(max_error, static_cast<double>(std::fabs(n.y - ref.y)));
        max_error = std::max(max_error, static_cast<double>(std::fabs(n.z - ref.z)));
        checksum += n.x * 0.25 + n.y * 0.5 + n.z;
      }
      checksum_b = checksum;
      return checksum;
    }, 3);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = max_error;
    row.accuracy_pass = max_error <= 1.0e-4;
    finalize(row);
    rows.push_back(row);
  }

  {
    EdgeResult row;
    row.id = "edge_cpu_fresnel_pow5";
    row.from = "cpu.fresnel.std_pow5";
    row.to = "cpu.fresnel.mul_pow5";
    row.domain = "cpu_math";
    row.metric = "Schlick Fresnel pow5";
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    double max_error = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < vectors.size(); ++i) {
        checksum += fresnel_pow5(std::fabs(vectors[i].x), 1.01f + std::fabs(vectors[i].y) * 2.0f);
      }
      checksum_a = checksum;
      return checksum;
    }, 3);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < vectors.size(); ++i) {
        const float cos_theta = std::fabs(vectors[i].x);
        const float ior = 1.01f + std::fabs(vectors[i].y) * 2.0f;
        const float a = fresnel_pow5(cos_theta, ior);
        const float b = fresnel_mul5(cos_theta, ior);
        max_error = std::max(max_error, static_cast<double>(std::fabs(a - b)));
        checksum += b;
      }
      checksum_b = checksum;
      return checksum;
    }, 3);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = max_error;
    row.accuracy_pass = max_error <= 1.0e-6;
    finalize(row);
    rows.push_back(row);
  }

  {
    EdgeResult row;
    row.id = "edge_cpu_material_pow2";
    row.from = "cpu.material.pow2";
    row.to = "cpu.material.mul_pow2";
    row.domain = "cpu_math";
    row.metric = "small integer material power";
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    double max_error = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      for (const auto& v : vectors) {
        const float x = clamp01(1.0f - std::fabs(v.x));
        checksum += std::pow(x, 2.0f);
      }
      checksum_a = checksum;
      return checksum;
    }, 3);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      for (const auto& v : vectors) {
        const float x = clamp01(1.0f - std::fabs(v.x));
        const float a = std::pow(x, 2.0f);
        const float b = x * x;
        max_error = std::max(max_error, static_cast<double>(std::fabs(a - b)));
        checksum += b;
      }
      checksum_b = checksum;
      return checksum;
    }, 3);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = max_error;
    row.accuracy_pass = max_error <= 1.0e-7;
    finalize(row);
    rows.push_back(row);
  }

  {
    EdgeResult row;
    row.id = "edge_cpu_triangle_cached_edges";
    row.from = "cpu.triangle.compute_edges";
    row.to = "cpu.triangle.cached_edges";
    row.domain = "cpu_math";
    row.metric = "Moller-Trumbore cached edges";
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < kPairs; ++i) {
        float t = 0.0f;
        if (intersect_triangle_baseline(triangles[i & (kTriCount - 1u)], rays[(i * 17u) & (kRayCount - 1u)], t)) {
          checksum += t;
        }
      }
      checksum_a = checksum;
      return checksum;
    }, 2);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < kPairs; ++i) {
        float t = 0.0f;
        if (intersect_triangle_cached(cached_triangles[i & (kTriCount - 1u)], rays[(i * 17u) & (kRayCount - 1u)], t)) {
          checksum += t;
        }
      }
      checksum_b = checksum;
      return checksum;
    }, 2);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = std::fabs(checksum_a - checksum_b);
    row.accuracy_pass = row.max_abs_error <= 1.0e-4;
    finalize(row);
    rows.push_back(row);
  }

  {
    EdgeResult row;
    row.id = "edge_cpu_aabb_inv_dir";
    row.from = "cpu.aabb.divide_per_axis";
    row.to = "cpu.aabb.precompute_inv_dir";
    row.domain = "cpu_scaffolding";
    row.metric = "AABB slab inv direction";
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < kPairs; ++i) {
        float t = 0.0f;
        if (intersect_aabb_divide(aabbs[i & (kTriCount - 1u)], rays[(i * 13u) & (kRayCount - 1u)], 1.0e20f, t)) {
          checksum += t;
        }
      }
      checksum_a = checksum;
      return checksum;
    }, 2);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < kPairs; ++i) {
        float t = 0.0f;
        if (intersect_aabb_inv_dir(aabbs[i & (kTriCount - 1u)], rays_with_inv[(i * 13u) & (kRayCount - 1u)], 1.0e20f, t)) {
          checksum += t;
        }
      }
      checksum_b = checksum;
      return checksum;
    }, 2);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = std::fabs(checksum_a - checksum_b);
    row.accuracy_pass = row.max_abs_error <= 1.0e-3;
    finalize(row);
    rows.push_back(row);
  }

  {
    EdgeResult row;
    row.id = "edge_cpu_camera_basis";
    row.from = "cpu.camera.basis_per_ray";
    row.to = "cpu.camera.precomputed_basis";
    row.domain = "cpu_scaffolding";
    row.metric = "camera basis reuse";
    const Vec3 camera_position{0.0f, 1.0f, 3.0f};
    const Vec3 camera_target{0.0f, 1.0f, 0.0f};
    const Vec3 camera_up{0.0f, 1.0f, 0.0f};
    auto make_dir = [](Vec3 forward, Vec3 right, Vec3 up, float nx, float ny) {
      return normalize_sqrt_div(forward + right * nx + up * ny);
    };
    const Vec3 forward = normalize_sqrt_div(camera_target - camera_position);
    const Vec3 right = normalize_sqrt_div(cross(forward, camera_up));
    const Vec3 up = normalize_sqrt_div(cross(right, forward));
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < kVecCount; ++i) {
        const Vec3 f = normalize_sqrt_div(camera_target - camera_position);
        const Vec3 r = normalize_sqrt_div(cross(f, camera_up));
        const Vec3 u = normalize_sqrt_div(cross(r, f));
        const auto d = make_dir(f, r, u, vectors[i].x, vectors[i].y);
        checksum += d.x + d.y * 0.5 + d.z * 0.25;
      }
      checksum_a = checksum;
      return checksum;
    }, 3);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      for (std::size_t i = 0; i < kVecCount; ++i) {
        const auto d = make_dir(forward, right, up, vectors[i].x, vectors[i].y);
        checksum += d.x + d.y * 0.5 + d.z * 0.25;
      }
      checksum_b = checksum;
      return checksum;
    }, 3);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = std::fabs(checksum_a - checksum_b);
    row.accuracy_pass = row.max_abs_error <= 1.0e-4;
    finalize(row);
    rows.push_back(row);
  }

  {
    EdgeResult row;
    row.id = "edge_cpu_rng_pcg32";
    row.from = "cpu.rng.splitmix64";
    row.to = "cpu.rng.pcg32_contract_candidate";
    row.domain = "cpu_scaffolding";
    row.metric = "RNG next01 throughput";
    double checksum_a = 0.0;
    double checksum_b = 0.0;
    row.baseline_ms = time_ms([&]() {
      double checksum = 0.0;
      uint64_t state = 0xabcddcba12344321ULL;
      for (std::size_t i = 0; i < kVecCount * 8u; ++i) {
        checksum += random01(state);
      }
      checksum_a = checksum;
      return checksum;
    }, 3);
    row.candidate_ms = time_ms([&]() {
      double checksum = 0.0;
      uint32_t state = 0x12344321u;
      for (std::size_t i = 0; i < kVecCount * 8u; ++i) {
        checksum += pcg32_float(state);
      }
      checksum_b = checksum;
      return checksum;
    }, 3);
    row.checksum_baseline = checksum_a;
    row.checksum_candidate = checksum_b;
    row.max_abs_error = std::fabs((checksum_a / static_cast<double>(kVecCount * 8u)) -
                                  (checksum_b / static_cast<double>(kVecCount * 8u)));
    row.accuracy_pass = row.max_abs_error <= 2.0e-3;
    finalize(row);
    if (row.decision == "take_candidate") {
      row.decision = "candidate_requires_rng_contract_update";
    }
    rows.push_back(row);
  }

  std::filesystem::create_directories(output.parent_path());
  std::ofstream file(output);
  if (!file.is_open()) {
    std::cerr << "failed to open output: " << output.string() << "\n";
    return 2;
  }
  file << serialize_json(rows);
  std::cout << "math microbench results: " << output.string() << "\n";
  for (const auto& row : rows) {
    std::cout << row.id << " speedup=" << std::fixed << std::setprecision(3) << row.speedup
              << " decision=" << row.decision << "\n";
  }
  return 0;
}

