// Avx2PathTracer.cpp
// Path tracing logic mirrors ScalarCpuPathTracer exactly.
// AVX2 is used only for the 8-wide Moller-Trumbore triangle intersection,
// which is the hot inner loop.  Everything else (camera, RNG, shading,
// NEE, bounce) is scalar and matches the reference implementation.

#include "cpu/Avx2PathTracer.h"
#include "cpu/Avx2SimdTypes.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace vkpt::cpu {

namespace {

// ---- math helpers -----------------------------------------------------------

using Vec3 = vkpt::pathtracer::Vec3;

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kInvPi   = 0.31830988618f;
constexpr float kEpsilon = 1e-4f;

inline void store_lanes(__m256 value, float* out) {
  _mm256_storeu_ps(out, value);
}

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 operator-(const Vec3& a)                { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, float s)       { return {a.x*s, a.y*s, a.z*s}; }
inline Vec3 operator*(float s, const Vec3& a)       { return a * s; }
inline Vec3 operator*(const Vec3& a, const Vec3& b) { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
inline Vec3 operator/(const Vec3& a, float s)       { float inv = 1.0f/s; return a*inv; }
// Note: Vec3::operator+= is already a member in PathTracer.h; no free version needed.

inline float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float length_sq(const Vec3& a)           { return dot(a,a); }
inline float length(const Vec3& a)              { return std::sqrt(length_sq(a)); }
inline Vec3  normalize(const Vec3& a) {
  float l = length(a);
  return (l < 1e-12f) ? a : a * (1.0f / l);
}
inline Vec3 preview_reflection_environment(const Vec3& direction) {
  const float t = std::clamp(direction.y * 0.5f + 0.5f, 0.0f, 1.0f);
  const float horizon = std::pow(std::clamp(1.0f - std::fabs(direction.y), 0.0f, 1.0f), 2.0f);
  const Vec3 floor_color{0.08f, 0.075f, 0.065f};
  const Vec3 sky_color{0.34f, 0.42f, 0.58f};
  return floor_color * (1.0f - t) + sky_color * t + Vec3{0.55f, 0.48f, 0.36f} * (horizon * 0.18f);
}
inline float hash01(float x, float y, float z, float seed) {
  const float v = std::sin(x * 12.9898f + y * 78.233f + z * 37.719f + seed * 19.19f) * 43758.5453f;
  return v - std::floor(v);
}
inline Vec3 material_effect_albedo(const vkpt::pathtracer::RTMaterial& material,
                                   const Vec3& position,
                                   const Vec3& normal,
                                   const Vec3& incoming) {
  Vec3 color = material.albedo;
  const float h = hash01(position.x, position.y, position.z, static_cast<float>(material.material_effect));
  switch (material.material_effect) {
    case 1u: {
      const float rim = std::pow(std::max(0.0f, 1.0f - std::fabs(dot(normal, -incoming))), 2.0f);
      color = color * (0.65f + 0.25f * h) + Vec3{0.25f, 0.22f, 0.28f} * (rim * (0.4f + material.sheen));
      break;
    }
    case 2u: {
      const float stripes = 0.5f + 0.5f * std::sin(position.x * 7.0f + position.z * 5.0f + h * 6.0f);
      color = color * (0.45f + 0.55f * stripes);
      break;
    }
    case 3u:
      color = color * (h > 0.72f ? 0.18f : 1.0f);
      break;
    case 4u: {
      const float vein = 0.5f + 0.5f * std::sin((position.x + position.y * 0.4f + position.z * 0.7f) * 9.0f + h * 3.0f);
      color = color * (0.55f + 0.45f * vein) + Vec3{0.18f, 0.20f, 0.23f} * (1.0f - vein);
      break;
    }
    case 5u:
      color = color * (0.55f + 0.35f * h) + Vec3{0.45f, 0.16f, 0.04f} * (0.25f + 0.25f * h);
      break;
    case 6u:
      color = color * (0.65f + 0.35f * h);
      break;
    case 7u:
      color = color * 0.78f + Vec3{1.0f, 0.62f, 0.42f} * 0.16f;
      break;
    case 8u:
      color = color * 0.55f + Vec3{0.35f + 0.45f * h, 0.25f + 0.35f * (1.0f - h), 0.85f} * 0.45f;
      break;
    case 9u: {
      const float check = std::fmod(std::floor(position.x * 4.0f) + std::floor(position.z * 4.0f), 2.0f);
      color = color * (check < 0.5f ? 1.0f : 0.28f);
      break;
    }
    case 10u:
      color = color * 0.45f + Vec3{1.0f, 0.35f + 0.3f * h, 0.08f} * 0.55f;
      break;
    case 11u: {
      const float streak = 0.5f + 0.5f * std::sin(position.y * 18.0f + h * 5.0f);
      color = color * (0.55f + 0.45f * streak);
      break;
    }
    case 12u: {
      const float retro = std::pow(std::max(0.0f, dot(normal, -incoming)), 6.0f);
      color = color + Vec3{0.55f, 0.65f, 0.95f} * retro;
      break;
    }
    case 13u:
      color = color * (0.65f + 0.35f * std::fabs(std::sin(position.x * 10.0f) * std::cos(position.z * 10.0f)));
      break;
    case 14u: {
      const float rim = std::pow(std::max(0.0f, 1.0f - std::fabs(dot(normal, -incoming))), 0.8f);
      color = Vec3{0.15f, 0.75f, 1.0f} * (0.2f + 0.8f * rim);
      break;
    }
    case 15u: {
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
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return { a.y*b.z - a.z*b.y,
           a.z*b.x - a.x*b.z,
           a.x*b.y - a.y*b.x };
}

// ---- RNG (splitmix64; same as scalar tracer) ---------------------------------

inline uint64_t splitmix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(splitmix64(seed)) {}
  float next01() {
    state = splitmix64(state);
    return static_cast<float>((state >> 8) & 0x00ffffffu) * (1.0f / 16777215.0f);
  }
};

// Seed matching scalar tracer: SampleKey mix of pixel_index, sample_index, seed.
inline Rng make_rng(uint32_t x, uint32_t y, uint32_t width,
                    uint32_t sample_index, uint32_t frame_index,
                    uint64_t base_seed) {
  const uint64_t pixel_index = static_cast<uint64_t>(y) * width + x;
  const uint64_t path_id     = pixel_index + sample_index + frame_index;
  const uint64_t mix =
      splitmix64(base_seed ^ splitmix64(frame_index) ^ splitmix64(sample_index)
                 ^ splitmix64(path_id));
  return Rng(splitmix64(mix + splitmix64(pixel_index)));
}

// ---- 8-wide AVX2 Moller-Trumbore --------------------------------------------
// Tests ONE triangle against 8 rays simultaneously.
// Returns a bitmask of lanes that hit, and updates out_t with closer hits.

inline int intersect8(const RaySoA& ray,
                      const Vec3& v0, const Vec3& v1, const Vec3& v2,
                      __m256 current_best_t,   // current closest t per lane
                      __m256& out_t,
                      __m256& out_u,
                      __m256& out_v) {
  const __m256 e1x = _mm256_set1_ps(v1.x - v0.x);
  const __m256 e1y = _mm256_set1_ps(v1.y - v0.y);
  const __m256 e1z = _mm256_set1_ps(v1.z - v0.z);
  const __m256 e2x = _mm256_set1_ps(v2.x - v0.x);
  const __m256 e2y = _mm256_set1_ps(v2.y - v0.y);
  const __m256 e2z = _mm256_set1_ps(v2.z - v0.z);

  // h = cross(ray.d, e2)
  __m256 hx = _mm256_fmsub_ps(ray.dy, e2z, _mm256_mul_ps(ray.dz, e2y));
  __m256 hy = _mm256_fmsub_ps(ray.dz, e2x, _mm256_mul_ps(ray.dx, e2z));
  __m256 hz = _mm256_fmsub_ps(ray.dx, e2y, _mm256_mul_ps(ray.dy, e2x));

  // a = dot(e1, h)
  __m256 a = _mm256_fmadd_ps(e1x, hx,
             _mm256_fmadd_ps(e1y, hy,
             _mm256_mul_ps (e1z, hz)));

  // |a| < epsilon means parallel.
  __m256 abs_a  = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a);
  __m256 valid  = _mm256_cmp_ps(abs_a, _mm256_set1_ps(kEpsilon), _CMP_GE_OQ);
  if (_mm256_movemask_ps(valid) == 0) return 0;

  __m256 inv_a = _mm256_div_ps(_mm256_set1_ps(1.0f), a);

  // s = ray.o - v0
  __m256 sx = _mm256_sub_ps(ray.ox, _mm256_set1_ps(v0.x));
  __m256 sy = _mm256_sub_ps(ray.oy, _mm256_set1_ps(v0.y));
  __m256 sz = _mm256_sub_ps(ray.oz, _mm256_set1_ps(v0.z));

  // u = dot(s, h) * inv_a
  __m256 u = _mm256_fmadd_ps(sx, hx,
             _mm256_fmadd_ps(sy, hy,
             _mm256_mul_ps (sz, hz)));
  u = _mm256_mul_ps(u, inv_a);

  __m256 mask_u = _mm256_and_ps(_mm256_cmp_ps(u, _mm256_setzero_ps(), _CMP_GE_OQ),
                                _mm256_cmp_ps(u, _mm256_set1_ps(1.0f), _CMP_LE_OQ));
  valid = _mm256_and_ps(valid, mask_u);
  if (_mm256_movemask_ps(valid) == 0) return 0;

  // q = cross(s, e1)
  __m256 qx = _mm256_fmsub_ps(sy, e1z, _mm256_mul_ps(sz, e1y));
  __m256 qy = _mm256_fmsub_ps(sz, e1x, _mm256_mul_ps(sx, e1z));
  __m256 qz = _mm256_fmsub_ps(sx, e1y, _mm256_mul_ps(sy, e1x));

  // v = dot(ray.d, q) * inv_a
  __m256 v = _mm256_fmadd_ps(ray.dx, qx,
             _mm256_fmadd_ps(ray.dy, qy,
             _mm256_mul_ps (ray.dz, qz)));
  v = _mm256_mul_ps(v, inv_a);

  __m256 mask_v = _mm256_and_ps(_mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_GE_OQ),
                                _mm256_cmp_ps(_mm256_add_ps(u, v), _mm256_set1_ps(1.0f), _CMP_LE_OQ));
  valid = _mm256_and_ps(valid, mask_v);
  if (_mm256_movemask_ps(valid) == 0) return 0;

  // t = dot(e2, q) * inv_a
  __m256 t = _mm256_fmadd_ps(e2x, qx,
             _mm256_fmadd_ps(e2y, qy,
             _mm256_mul_ps (e2z, qz)));
  t = _mm256_mul_ps(t, inv_a);

  // t > epsilon AND t < current_best_t
  __m256 mask_t = _mm256_and_ps(_mm256_cmp_ps(t, _mm256_set1_ps(kEpsilon), _CMP_GT_OQ),
                                _mm256_cmp_ps(t, current_best_t,             _CMP_LT_OQ));
  valid = _mm256_and_ps(valid, mask_t);
  int hit_mask = _mm256_movemask_ps(valid);
  if (hit_mask == 0) return 0;

  out_t = _mm256_blendv_ps(out_t, t, valid);
  out_u = _mm256_blendv_ps(out_u, u, valid);
  out_v = _mm256_blendv_ps(out_v, v, valid);
  return hit_mask;
}

// ---- Tangent frame ----------------------------------------------------------
inline void build_frame(const Vec3& n, Vec3& tang, Vec3& btan) {
  const Vec3 ref = (std::fabs(n.z) < 0.999f) ? Vec3{0,0,1} : Vec3{0,1,0};
  tang = normalize(cross(ref, n));
  btan = cross(n, tang);
}

// ---- Lambertian hemisphere sample (identical to scalar tracer) --------------
inline Vec3 sample_hemisphere(Rng& rng, const Vec3& normal) {
  const float u1  = rng.next01();
  const float u2  = rng.next01();
  const float r   = std::sqrt(std::max(0.0f, 1.0f - u1));
  const float phi = 2.0f * kPi * u2;
  const Vec3  local{r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, u1))};
  Vec3 tang, btan;
  build_frame(normal, tang, btan);
  return normalize(tang * local.x + btan * local.y + normal * local.z);
}

// ---- Phong lobe sample around a reflection direction ------------------------
inline Vec3 sample_phong_lobe(Rng& rng, const Vec3& refl, float exponent,
                               const Vec3& normal) {
  const float u1   = rng.next01();
  const float u2   = rng.next01();
  const float cosT = std::pow(std::max(0.0f, u1), 1.0f / (exponent + 1.0f));
  const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
  const float phi  = 2.0f * kPi * u2;
  Vec3 tang, btan;
  build_frame(refl, tang, btan);
  Vec3 out = normalize(tang * (sinT * std::cos(phi)) +
                       btan * (sinT * std::sin(phi)) +
                       refl * cosT);
  if (dot(out, normal) <= 0.0f) {
    out = out - 2.0f * dot(out, normal) * normal;
    if (dot(out, normal) <= 0.0f) return sample_hemisphere(rng, normal);
  }
  return out;
}

} // anonymous namespace

// ---- Avx2CpuPathTracer member functions ------------------------------------

bool Avx2CpuPathTracer::configure(const vkpt::pathtracer::RenderSettings& s) {
  settings_ = vkpt::pathtracer::MakeRenderSettings(vkpt::pathtracer::MakePathTraceSettings(s));
  m_film.resize(settings_.width, settings_.height);
  m_film.set_resolve_settings(settings_.film_resolve);
  m_film.clear();
  counters_ = {};
  configured_ = true;
  has_scene_  = false;
  return true;
}

bool Avx2CpuPathTracer::load_scene_snapshot(const vkpt::pathtracer::RTSceneData& scene) {
  scene_      = scene;
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(settings_.film_resolve, scene_));
  if (scene_.materials.empty()) {
    scene_.materials.push_back(vkpt::pathtracer::RTMaterial{});
  }
  has_scene_  = true;
  set_camera_basis();
  return true;
}

bool Avx2CpuPathTracer::build_or_update_acceleration() {
  // Mirror scalar tracer: camera basis from scene data.
  set_camera_basis();
  return true;
}

bool Avx2CpuPathTracer::update_camera(const vkpt::pathtracer::Vec3& pos,
                                      const vkpt::pathtracer::Vec3& target,
                                      const vkpt::pathtracer::Vec3& up,
                                      float fov_deg) {
  if (!configured_ || !has_scene_) {
    return false;
  }
  auto camera = vkpt::pathtracer::ExtractCameraState(scene_);
  camera.position = pos;
  camera.target = target;
  camera.up = up;
  camera.fov_deg = fov_deg;
  return update_camera_state(camera);
}

bool Avx2CpuPathTracer::update_camera_state(const vkpt::pathtracer::RTCameraState& camera) {
  if (!configured_ || !has_scene_) {
    return false;
  }
  vkpt::pathtracer::ApplyCameraState(scene_, camera);
  m_film.set_resolve_settings(
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(settings_.film_resolve, scene_));
  set_camera_basis();
  return true;
}

bool Avx2CpuPathTracer::update_instance_transforms(
    const std::vector<vkpt::pathtracer::RTInstanceTransformUpdate>& updates) {
  if (!configured_ || !has_scene_) {
    return false;
  }
  return vkpt::pathtracer::ApplyInstanceTransformUpdates(scene_, updates);
}

bool Avx2CpuPathTracer::update_scene_delta(
    const vkpt::pathtracer::RTSceneDeltaUpdate& update) {
  if (!configured_ || !has_scene_) {
    return false;
  }
  return vkpt::pathtracer::ApplySceneDeltaUpdate(scene_, update);
}

bool Avx2CpuPathTracer::reset_accumulation() {
  m_film.clear();
  counters_ = {};
  return true;
}

// Camera basis matching ScalarCpuPathTracer::build_or_update_acceleration()
void Avx2CpuPathTracer::set_camera_basis() {
  cam_forward_ = normalize(scene_.camera_target - scene_.camera_position);
  // right = cross(forward, camera_up), same as scalar tracer.
  cam_right_   = normalize(cross(cam_forward_, scene_.camera_up));
  if (length_sq(cam_right_) <= kEpsilon * kEpsilon) {
    cam_right_ = {1.0f, 0.0f, 0.0f};
  }
  // up = cross(right, forward), same as scalar tracer.
  cam_up_ = normalize(cross(cam_right_, cam_forward_));
  if (length_sq(cam_up_) <= kEpsilon * kEpsilon) {
    cam_up_ = {0.0f, 1.0f, 0.0f};
  }
}

// ---- intersect_scene8: test 8 rays vs all triangles, scalar shading ---------
// Returns true for lane `lane` if any triangle was hit.
// Populates hit_{t,pos,normal,mat} for each lane.

struct Hit8 {
  float t   [8];
  Vec3  pos [8];
  Vec3  norm[8];
  uint32_t mat[8];
  bool  hit [8];
};

static void intersect_scene8(const RaySoA& ray8,
                             const vkpt::pathtracer::RTSceneData& scene,
                             Hit8& out,
                             vkpt::pathtracer::SampleCounters* counters = nullptr) {
  // one __m256 per hit quantity
  __m256 best_t = _mm256_set1_ps(std::numeric_limits<float>::infinity());
  __m256 hit_t  = _mm256_set1_ps(std::numeric_limits<float>::infinity());
  __m256 hit_u  = _mm256_setzero_ps();
  __m256 hit_v  = _mm256_setzero_ps();

  // material index stored as float in an __m256
  __m256 hit_mat_f = _mm256_set1_ps(-1.0f);
  // normal stored as three __m256
  __m256 hit_nx = _mm256_setzero_ps();
  __m256 hit_ny = _mm256_setzero_ps();
  __m256 hit_nz = _mm256_setzero_ps();
  int any_hit = 0;

  for (const auto& inst : scene.instances) {
    const float mat_f = static_cast<float>(inst.material_index);
    for (uint32_t ti = 0; ti < inst.triangle_count; ++ti) {
      const uint32_t base = (inst.first_triangle + ti) * 3u;
      if (base + 2u >= scene.indices.size()) continue;
      const uint32_t i0 = scene.indices[base + 0];
      const uint32_t i1 = scene.indices[base + 1];
      const uint32_t i2 = scene.indices[base + 2];
      if (i0 >= scene.vertices.size() ||
          i1 >= scene.vertices.size() ||
          i2 >= scene.vertices.size()) continue;
      if (counters) {
        ++counters->triangle_tests;
      }
      const Vec3& v0 = scene.vertices[i0];
      const Vec3& v1 = scene.vertices[i1];
      const Vec3& v2 = scene.vertices[i2];

      __m256 new_t = hit_t, new_u = hit_u, new_v = hit_v;
      // Pass current best_t so only closer hits are accepted
      int mask = intersect8(ray8, v0, v1, v2, best_t, new_t, new_u, new_v);
      if (!mask) continue;
      if (counters && (mask & 1)) {
        ++counters->triangle_hits;
      }
      any_hit |= mask;

      // For lanes that got a new closer hit, update best_t and normal/mat
      __m256 closer = _mm256_cmp_ps(new_t, hit_t, _CMP_LT_OQ);
      hit_t = new_t;
      hit_u = new_u;
      hit_v = new_v;
      best_t = hit_t; // update culling threshold

      // Compute face normal for this triangle
      const Vec3 e1 = v1 - v0;
      const Vec3 e2 = v2 - v0;
      const Vec3 n  = normalize(cross(e1, e2));
      hit_nx = _mm256_blendv_ps(hit_nx, _mm256_set1_ps(n.x), closer);
      hit_ny = _mm256_blendv_ps(hit_ny, _mm256_set1_ps(n.y), closer);
      hit_nz = _mm256_blendv_ps(hit_nz, _mm256_set1_ps(n.z), closer);
      hit_mat_f = _mm256_blendv_ps(hit_mat_f, _mm256_set1_ps(mat_f), closer);
    }
  }

  // Unpack per-lane
  alignas(32) float pt[8];
  alignas(32) float pnx[8];
  alignas(32) float pny[8];
  alignas(32) float pnz[8];
  alignas(32) float pmat[8];
  alignas(32) float pox[8];
  alignas(32) float poy[8];
  alignas(32) float poz[8];
  alignas(32) float pdx[8];
  alignas(32) float pdy[8];
  alignas(32) float pdz[8];
  store_lanes(hit_t, pt);
  store_lanes(hit_nx, pnx);
  store_lanes(hit_ny, pny);
  store_lanes(hit_nz, pnz);
  store_lanes(hit_mat_f, pmat);
  store_lanes(ray8.ox, pox);
  store_lanes(ray8.oy, poy);
  store_lanes(ray8.oz, poz);
  store_lanes(ray8.dx, pdx);
  store_lanes(ray8.dy, pdy);
  store_lanes(ray8.dz, pdz);

  for (int i = 0; i < 8; ++i) {
    const bool h = (any_hit & (1 << i)) && pt[i] < std::numeric_limits<float>::infinity();
    out.hit[i] = h;
    out.t  [i] = pt[i];
    if (h) {
      out.pos [i] = { pox[i] + pdx[i] * pt[i],
                      poy[i] + pdy[i] * pt[i],
                      poz[i] + pdz[i] * pt[i] };
      out.norm[i] = { pnx[i], pny[i], pnz[i] };
      const uint32_t mi = static_cast<uint32_t>(std::max(0.0f, pmat[i]));
      out.mat [i] = std::min(mi, static_cast<uint32_t>(
                                   scene.materials.empty() ? 0 : scene.materials.size() - 1));
    }
  }
}

// ---- trace one path (mirrors ScalarCpuPathTracer::trace exactly) ------------
// Uses intersect_scene8 to test the current ray vs all geometry using AVX2.
// Returns the radiance for this path.

static Vec3 trace_one(const vkpt::pathtracer::Ray& input_ray,
                      const vkpt::pathtracer::RTSceneData& scene,
                      const Vec3& cam_forward,
                      const Vec3& cam_right,
                      const Vec3& cam_up,
                      const vkpt::pathtracer::RenderSettings& settings,
                      Rng& rng,
                      uint64_t& ray_counter,
                      vkpt::pathtracer::SampleCounters* counters) {
  (void)cam_forward; (void)cam_right; (void)cam_up;

  Vec3 radiance{0.0f, 0.0f, 0.0f};
  Vec3 throughput{1.0f, 1.0f, 1.0f};
  vkpt::pathtracer::Ray ray = input_ray;
  bool preview_reflection_env = false;

  const uint32_t num_mats = static_cast<uint32_t>(scene.materials.size());

  for (uint32_t depth = 0; depth < settings.max_depth; ++depth) {
    ++ray_counter;

    // Build a degenerate 8-lane SoA where all 8 lanes carry the same ray.
    // intersect_scene8 then finds the closest triangle hit for lane 0.
    // (Remaining lanes are identical; we only read lane 0 result.)
    RaySoA ray8{
        _mm256_set1_ps(ray.origin.x),
        _mm256_set1_ps(ray.origin.y),
        _mm256_set1_ps(ray.origin.z),
        _mm256_set1_ps(ray.direction.x),
        _mm256_set1_ps(ray.direction.y),
        _mm256_set1_ps(ray.direction.z)};

    Hit8 hits{};
    intersect_scene8(ray8, scene, hits, counters);

    if (!hits.hit[0]) {
      Vec3 miss_environment = scene.environment_color;
      if (preview_reflection_env &&
          std::max({miss_environment.x, miss_environment.y, miss_environment.z}) <= 1.0e-5f) {
        miss_environment = preview_reflection_environment(ray.direction);
      }
      radiance = radiance + throughput * miss_environment;
      break;
    }

    Vec3 shading_normal = hits.norm[0];
    // Flip normal to face incoming ray (same as scalar tracer)
    if (dot(shading_normal, -ray.direction) < 0.0f) {
      shading_normal = -shading_normal;
    }

    const uint32_t mat_idx = std::min(hits.mat[0], num_mats > 0 ? num_mats - 1 : 0u);
    const auto& material = scene.materials[mat_idx];
    const Vec3 surface_albedo = material_effect_albedo(material, hits.pos[0], shading_normal, ray.direction);

    // Emissive path mirrors scalar: add when NEE is off, at depth zero, or by MIS weight.
    if (!settings.enable_nee || depth == 0) {
      radiance = radiance + throughput * material.emissive;
    } else {
      // MIS-weighted emissive bounce (matches scalar's else-if branch)
      if (material.is_emissive()) {
        const float bsdf_pdf = std::max(0.0f, dot(shading_normal, -ray.direction)) * kInvPi;
        float l_pdf = 0.0f;
        for (const auto& lt : scene.lights) {
          const Vec3 to_lt = lt.position - ray.origin;
          const float dist = length(to_lt);
          if (dist > kEpsilon && dot(normalize(to_lt), ray.direction) > 0.9999f) {
            l_pdf += 1.0f / (static_cast<float>(scene.lights.size()) * (dist*dist + kEpsilon));
          }
        }
        const float a2 = bsdf_pdf * bsdf_pdf;
        const float b2 = l_pdf * l_pdf;
        const float w  = (a2 + b2 > 0.0f) ? a2 / (a2 + b2) : 1.0f;
        radiance = radiance + throughput * material.emissive * w;
      }
    }

    const float roughness = std::clamp(material.roughness, 0.0f, 1.0f);
    const bool is_mirror = (material.material_model == 2u) || (roughness <= 0.001f);
    const bool is_metallic = (material.material_model == 4u) || material.metallic > 0.65f;
    const bool is_transmissive = (material.material_model == 5u) || material.transmission > 0.05f;
    const bool is_clearcoat = (material.material_model == 7u) || material.clearcoat > 0.05f;
    const bool is_diffuse = (roughness >= 0.999f) && !is_mirror && !is_metallic && !is_transmissive;

    // NEE: skip perfect mirrors (delta BSDF).
    if (settings.enable_nee && !scene.lights.empty() && depth + 1 < settings.max_depth) {
      const std::size_t nl  = scene.lights.size();
      const std::size_t li  = static_cast<std::size_t>(rng.next01() * static_cast<float>(nl));
      const auto& lt        = scene.lights[std::min(li, nl - 1)];
      const Vec3  to_light  = lt.position - hits.pos[0];
      const float dist_sq   = length_sq(to_light);
      const float dist      = std::sqrt(dist_sq);
      if (dist > kEpsilon) {
        const Vec3  ldir      = to_light / dist;
        const float cos_theta = dot(shading_normal, ldir);
        if (cos_theta > 0.0f) {
          vkpt::pathtracer::Ray shadow_ray{hits.pos[0] + shading_normal * 0.002f, ldir};
          if (counters) {
            ++counters->shadow_tests;
          }
          RaySoA sr8{
              _mm256_set1_ps(shadow_ray.origin.x),
              _mm256_set1_ps(shadow_ray.origin.y),
              _mm256_set1_ps(shadow_ray.origin.z),
              _mm256_set1_ps(shadow_ray.direction.x),
              _mm256_set1_ps(shadow_ray.direction.y),
              _mm256_set1_ps(shadow_ray.direction.z)};
          Hit8 sh{};
          intersect_scene8(sr8, scene, sh, counters);
          const bool occluded = sh.hit[0] && sh.t[0] < dist - 0.004f;
          if (!occluded) {
            const Vec3 irradiance = lt.color * (lt.intensity / (dist_sq + kEpsilon));
            Vec3 direct{};
            if (!is_mirror && !is_transmissive) {
              direct = direct + surface_albedo * kInvPi * irradiance
                                  * cos_theta * static_cast<float>(nl);
            }
            if (is_mirror || is_metallic || is_clearcoat || is_transmissive || roughness < 0.65f) {
              const Vec3 view_dir = normalize(-ray.direction);
              const Vec3 half_dir = normalize(ldir + view_dir);
              const float effective_roughness = std::max(0.025f, roughness * (is_metallic ? 0.65f : 1.0f));
              const float a2 = effective_roughness * effective_roughness;
              const float spec_power = std::clamp(2.0f / std::max(0.0005f, a2 * a2) - 2.0f, 4.0f, 96.0f);
              const float spec = std::pow(std::max(0.0f, dot(shading_normal, half_dir)), spec_power);
              const float cos_view = std::max(0.0f, dot(shading_normal, view_dir));
              float f0 = (1.0f - material.ior) / (1.0f + material.ior);
              f0 *= f0;
              const float fresnel = f0 + (1.0f - f0) * std::pow(1.0f - cos_view, 5.0f);
              const float spec_strength = std::clamp((1.0f - roughness) * 0.8f +
                                                     material.metallic * 0.45f +
                                                     material.clearcoat * 0.35f +
                                                     (is_mirror ? 0.75f : 0.0f) +
                                                     (is_transmissive ? fresnel : 0.0f),
                                                     0.0f,
                                                     1.0f);
              const Vec3 white{1.0f, 1.0f, 1.0f};
              const Vec3 spec_tint = white * (is_metallic ? 0.15f : 0.88f) +
                                     surface_albedo * (is_metallic ? 0.85f : 0.12f);
              direct = direct + spec_tint * irradiance *
                                  (spec * cos_theta * static_cast<float>(nl) *
                                   std::max(0.15f, spec_strength));
            }
            if (is_transmissive) {
              direct = direct + surface_albedo * irradiance *
                                  (cos_theta * static_cast<float>(nl) *
                                   (0.08f + 0.22f * material.alpha));
            }
            if (settings.enable_mis) {
              const float bsdf_pdf = cos_theta * kInvPi;
              const float l_pdf    = 1.0f / static_cast<float>(nl);
              const float a2 = l_pdf * l_pdf, b2 = bsdf_pdf * bsdf_pdf;
              const float w  = (a2 + b2 > 0.0f) ? a2 / (a2 + b2) : 1.0f;
              radiance = radiance + throughput * direct * w;
            } else {
              radiance = radiance + throughput * direct;
            }
          }
        }
      }
    }

    // ---- BSDF bounce -------------------------------------------------------
    Vec3 out_dir;
    if (is_mirror) {
      out_dir = ray.direction - 2.0f * dot(shading_normal, ray.direction) * shading_normal;
    } else if (is_transmissive) {
      const float cos_theta = std::max(0.0f, dot(shading_normal, -ray.direction));
      float r0 = (1.0f - material.ior) / (1.0f + material.ior);
      r0 *= r0;
      const float fresnel = r0 + (1.0f - r0) * std::pow(1.0f - cos_theta, 5.0f);
      if (rng.next01() < std::min(0.98f, fresnel + material.clearcoat * 0.15f)) {
        out_dir = ray.direction - 2.0f * dot(shading_normal, ray.direction) * shading_normal;
      } else {
        out_dir = sample_phong_lobe(rng,
                                    normalize(ray.direction - 2.0f * dot(shading_normal, ray.direction) * shading_normal),
                                    128.0f,
                                    shading_normal);
      }
    } else if (is_diffuse) {
      out_dir = sample_hemisphere(rng, shading_normal);
    } else {
      const float effective_roughness = std::max(0.025f, roughness * (is_metallic ? 0.75f : 1.0f) * (is_clearcoat ? 0.65f : 1.0f));
      const float a2   = effective_roughness * effective_roughness;
      const float expt = std::max(0.0f, 2.0f / (a2 * a2) - 2.0f);
      const Vec3 refl  = ray.direction - 2.0f * dot(shading_normal, ray.direction) * shading_normal;
      out_dir = sample_phong_lobe(rng, refl, expt, shading_normal);
    }

    if (!is_transmissive && dot(out_dir, shading_normal) <= 0.0f) break;
    Vec3 bounce_weight = material.albedo;
    if (is_metallic || is_mirror) {
      bounce_weight = surface_albedo * (0.65f + 0.35f * material.metallic);
    } else if (is_transmissive) {
      bounce_weight = surface_albedo * (0.25f + 0.55f * material.alpha) + Vec3{0.2f, 0.2f, 0.2f};
    } else {
      bounce_weight = surface_albedo;
    }
    preview_reflection_env = is_mirror || is_metallic || is_transmissive || is_clearcoat || roughness < 0.65f;
    throughput = throughput * bounce_weight;

    if (std::max({throughput.x, throughput.y, throughput.z}) < 0.001f) break;

    const float rr = std::min(settings.russian_roulette_max_survival,
        std::max(settings.russian_roulette_min_survival, std::max({throughput.x, throughput.y, throughput.z})));
    if (depth >= settings.russian_roulette_start_depth && rng.next01() > rr) break;
    throughput = throughput / rr;

    ray.origin    = hits.pos[0] + shading_normal * 0.002f;
    ray.direction = out_dir;
  }

  return radiance;
}

// ---- render_sample_batch: process pixels in 8-wide packets ------------------
// Camera ray generation matches ScalarCpuPathTracer::camera_rays exactly.

bool Avx2CpuPathTracer::render_sample_batch(uint32_t start_y,
                                            uint32_t end_y,
                                            uint32_t sample_index,
                                            uint32_t frame_index) {
  if (!configured_ || !has_scene_) return false;

  const uint32_t max_y = std::min(end_y, settings_.height);
  const uint32_t min_y = std::min(start_y, max_y);
  if (min_y >= max_y) return true;

  const float aspect  = static_cast<float>(settings_.width)
                      / std::max(1.0f, static_cast<float>(settings_.height));
  const float tan_hf  = std::tan(0.5f * (scene_.camera_fov_deg * kPi / 180.0f));
  const float aperture_radius = scene_.camera_aperture_radius > 0.0f
      ? scene_.camera_aperture_radius
      : settings_.camera_aperture_radius;
  const float focus_distance = scene_.camera_focus_distance > 0.0f
      ? scene_.camera_focus_distance
      : settings_.camera_focus_distance;
  const uint32_t iris_blades = std::min(scene_.camera_iris_blade_count, 64u);
  const float iris_roundness = std::clamp(scene_.camera_iris_roundness, 0.0f, 1.0f);
  const float iris_rotation_rad = scene_.camera_iris_rotation_degrees * (kPi / 180.0f);
  const float anamorphic_squeeze = std::isfinite(scene_.camera_anamorphic_squeeze)
      ? std::max(0.01f, scene_.camera_anamorphic_squeeze)
      : 1.0f;
  uint64_t local_rays = 0;

  for (uint32_t y = min_y; y < max_y; ++y) {
    for (uint32_t x_base = 0; x_base < settings_.width; x_base += 8) {
      const uint32_t count = std::min(8u, settings_.width - x_base);

      // Build camera ray lanes for this pixel packet.
      alignas(32) float ox[8];
      alignas(32) float oy[8];
      alignas(32) float oz[8];
      alignas(32) float dx[8];
      alignas(32) float dy[8];
      alignas(32) float dz[8];
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = x_base + i;
        // Per-pixel per-sample RNG for jitter; mirrors scalar camera_rays().
        Rng jitter_rng = make_rng(x, y, settings_.width, sample_index, frame_index,
                                  settings_.seed);
        const float fx = (static_cast<float>(x) + jitter_rng.next01())
                       / static_cast<float>(std::max(1u, settings_.width));
        const float fy = (static_cast<float>(y) + jitter_rng.next01())
                       / static_cast<float>(std::max(1u, settings_.height));
        const float nx = (2.0f * fx - 1.0f) * aspect * tan_hf;
        const float ny = (1.0f - 2.0f * fy) * tan_hf;
        Vec3 dir = normalize(cam_forward_ + cam_right_ * nx + cam_up_ * ny);
        Vec3 origin = scene_.camera_position;
        if (aperture_radius > 0.0f && focus_distance > kEpsilon) {
          const float lens_radius_sample = std::sqrt(jitter_rng.next01());
          const float lens_phi = 2.0f * kPi * jitter_rng.next01();
          float aperture_boundary = 1.0f;
          if (iris_blades >= 3u && iris_roundness < 0.999f) {
            const float sector = 2.0f * kPi / static_cast<float>(iris_blades);
            const float local_phi = lens_phi - iris_rotation_rad;
            const float wrapped = local_phi - sector * std::floor(local_phi / sector);
            const float centered = wrapped > sector * 0.5f ? wrapped - sector : wrapped;
            const float polygon_boundary =
                std::cos(sector * 0.5f) / std::max(0.1f, std::cos(centered));
            aperture_boundary = polygon_boundary * (1.0f - iris_roundness) + iris_roundness;
          }
          const float lens_r = aperture_radius * lens_radius_sample * aperture_boundary;
          const Vec3 lens_offset =
              cam_right_ * (lens_r * std::cos(lens_phi) * anamorphic_squeeze) +
              cam_up_ * (lens_r * std::sin(lens_phi));
          const Vec3 focus_point = scene_.camera_position + dir * focus_distance;
          origin = scene_.camera_position + lens_offset;
          dir = normalize(focus_point - origin);
        }
        ox[i] = origin.x;
        oy[i] = origin.y;
        oz[i] = origin.z;
        dx[i] = dir.x;
        dy[i] = dir.y;
        dz[i] = dir.z;
      }
      // Pad unused lanes
      for (uint32_t i = count; i < 8; ++i) {
        ox[i] = 0.0f;
        oy[i] = 0.0f;
        oz[i] = 0.0f;
        dx[i] = 0.0f;
        dy[i] = 0.0f;
        dz[i] = 1.0f;
      }

      // Trace each pixel individually with correct per-pixel per-sample RNG
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = x_base + i;
        Rng rng = make_rng(x, y, settings_.width, sample_index, frame_index,
                           settings_.seed);
        // Consume camera jitter samples so the path RNG starts at the same dimension.
        rng.next01(); rng.next01();
        if (aperture_radius > 0.0f && focus_distance > kEpsilon) {
          rng.next01(); rng.next01();
        }

        // Build a single-ray SoA for this pixel's path tracing
        vkpt::pathtracer::Ray cam_ray{
          {ox[i], oy[i], oz[i]},
          {dx[i], dy[i], dz[i]}
        };

        uint64_t rc = 0;
        const Vec3 sample = trace_one(cam_ray, scene_,
                                      cam_forward_, cam_right_, cam_up_,
                                      settings_, rng, rc, &counters_);
        local_rays += rc;

        if (std::isfinite(sample.x) && std::isfinite(sample.y) && std::isfinite(sample.z)) {
          m_film.add_sample(x, y, sample);
        }
      }
    }
  }

  std::atomic_ref<uint64_t>(counters_.samples).fetch_add(
      static_cast<uint64_t>((max_y - min_y) * settings_.width),
      std::memory_order_relaxed);
  std::atomic_ref<uint64_t>(counters_.rays).fetch_add(local_rays,
      std::memory_order_relaxed);
  return true;
}

vkpt::pathtracer::SampleCounters Avx2CpuPathTracer::read_counters() const {
  vkpt::pathtracer::SampleCounters out;
  out.samples = std::atomic_ref<const uint64_t>(counters_.samples)
                    .load(std::memory_order_relaxed);
  out.rays    = std::atomic_ref<const uint64_t>(counters_.rays)
                    .load(std::memory_order_relaxed);
  out.triangle_tests = std::atomic_ref<const uint64_t>(counters_.triangle_tests)
                    .load(std::memory_order_relaxed);
  out.triangle_hits = std::atomic_ref<const uint64_t>(counters_.triangle_hits)
                    .load(std::memory_order_relaxed);
  out.shadow_tests = std::atomic_ref<const uint64_t>(counters_.shadow_tests)
                    .load(std::memory_order_relaxed);
  return out;
}

void Avx2CpuPathTracer::shutdown() {
  configured_ = false;
  has_scene_  = false;
  m_film      = vkpt::pathtracer::FilmBuffer{};
  scene_      = vkpt::pathtracer::RTSceneData{};
  counters_   = {};
}

} // namespace vkpt::cpu
