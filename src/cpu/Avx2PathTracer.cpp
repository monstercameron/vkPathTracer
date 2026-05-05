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

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 operator-(const Vec3& a)                { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, float s)       { return {a.x*s, a.y*s, a.z*s}; }
inline Vec3 operator*(float s, const Vec3& a)       { return a * s; }
inline Vec3 operator*(const Vec3& a, const Vec3& b) { return {a.x*b.x, a.y*b.y, a.z*b.z}; }
inline Vec3 operator/(const Vec3& a, float s)       { float inv = 1.0f/s; return a*inv; }
// Note: Vec3::operator+= is already a member in PathTracer.h — no free version needed.

inline float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float length_sq(const Vec3& a)           { return dot(a,a); }
inline float length(const Vec3& a)              { return std::sqrt(length_sq(a)); }
inline Vec3  normalize(const Vec3& a) {
  float l = length(a);
  return (l < 1e-12f) ? a : a * (1.0f / l);
}
inline Vec3 cross(const Vec3& a, const Vec3& b) {
  return { a.y*b.z - a.z*b.y,
           a.z*b.x - a.x*b.z,
           a.x*b.y - a.y*b.x };
}

// ---- RNG (splitmix64 — same as scalar tracer) --------------------------------

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

  // |a| < epsilon → parallel
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
  scene_.camera_position = pos;
  scene_.camera_target = target;
  scene_.camera_up = up;
  scene_.camera_fov_deg = fov_deg;
  set_camera_basis();
  return true;
}

bool Avx2CpuPathTracer::reset_accumulation() {
  m_film.clear();
  counters_ = {};
  return true;
}

// Camera basis matching ScalarCpuPathTracer::build_or_update_acceleration()
void Avx2CpuPathTracer::set_camera_basis() {
  cam_forward_ = normalize(scene_.camera_target - scene_.camera_position);
  // right = cross(forward, camera_up)  — same as scalar tracer
  cam_right_   = normalize(cross(cam_forward_, scene_.camera_up));
  if (length_sq(cam_right_) <= kEpsilon * kEpsilon) {
    cam_right_ = {1.0f, 0.0f, 0.0f};
  }
  // up = cross(right, forward)  — same as scalar tracer
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
  const float* pt   = reinterpret_cast<const float*>(&hit_t);
  const float* pnx  = reinterpret_cast<const float*>(&hit_nx);
  const float* pny  = reinterpret_cast<const float*>(&hit_ny);
  const float* pnz  = reinterpret_cast<const float*>(&hit_nz);
  const float* pmat = reinterpret_cast<const float*>(&hit_mat_f);
  const float* pox  = reinterpret_cast<const float*>(&ray8.ox);
  const float* poy  = reinterpret_cast<const float*>(&ray8.oy);
  const float* poz  = reinterpret_cast<const float*>(&ray8.oz);
  const float* pdx  = reinterpret_cast<const float*>(&ray8.dx);
  const float* pdy  = reinterpret_cast<const float*>(&ray8.dy);
  const float* pdz  = reinterpret_cast<const float*>(&ray8.dz);

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

  const uint32_t num_mats = static_cast<uint32_t>(scene.materials.size());

  for (uint32_t depth = 0; depth < settings.max_depth; ++depth) {
    ++ray_counter;

    // Build a degenerate 8-lane SoA where all 8 lanes carry the same ray.
    // intersect_scene8 then finds the closest triangle hit for lane 0.
    // (Remaining lanes are identical; we only read lane 0 result.)
    RaySoA ray8;
    for (int i = 0; i < 8; ++i) {
      reinterpret_cast<float*>(&ray8.ox)[i] = ray.origin.x;
      reinterpret_cast<float*>(&ray8.oy)[i] = ray.origin.y;
      reinterpret_cast<float*>(&ray8.oz)[i] = ray.origin.z;
      reinterpret_cast<float*>(&ray8.dx)[i] = ray.direction.x;
      reinterpret_cast<float*>(&ray8.dy)[i] = ray.direction.y;
      reinterpret_cast<float*>(&ray8.dz)[i] = ray.direction.z;
    }

    Hit8 hits{};
    intersect_scene8(ray8, scene, hits, counters);

    if (!hits.hit[0]) {
      radiance = radiance + throughput * scene.environment_color;
      break;
    }

    Vec3 shading_normal = hits.norm[0];
    // Flip normal to face incoming ray (same as scalar tracer)
    if (dot(shading_normal, -ray.direction) < 0.0f) {
      shading_normal = -shading_normal;
    }

    const uint32_t mat_idx = std::min(hits.mat[0], num_mats > 0 ? num_mats - 1 : 0u);
    const auto& material = scene.materials[mat_idx];

    // Emissive — mirror scalar: add when NEE off OR depth==0 OR MIS weight
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

    const bool is_mirror  = (material.roughness <= 0.001f);
    const bool is_diffuse = (material.roughness >= 0.999f);

    // NEE — skip for perfect mirrors (delta BSDF)
    if (settings.enable_nee && !is_mirror && !scene.lights.empty() && depth + 1 < settings.max_depth) {
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
          RaySoA sr8;
          for (int i = 0; i < 8; ++i) {
            reinterpret_cast<float*>(&sr8.ox)[i] = shadow_ray.origin.x;
            reinterpret_cast<float*>(&sr8.oy)[i] = shadow_ray.origin.y;
            reinterpret_cast<float*>(&sr8.oz)[i] = shadow_ray.origin.z;
            reinterpret_cast<float*>(&sr8.dx)[i] = shadow_ray.direction.x;
            reinterpret_cast<float*>(&sr8.dy)[i] = shadow_ray.direction.y;
            reinterpret_cast<float*>(&sr8.dz)[i] = shadow_ray.direction.z;
          }
          Hit8 sh{};
          intersect_scene8(sr8, scene, sh, counters);
          const bool occluded = sh.hit[0] && sh.t[0] < dist - 0.004f;
          if (!occluded) {
            const Vec3 irradiance = lt.color * (lt.intensity / (dist_sq + kEpsilon));
            const Vec3 direct     = material.albedo * kInvPi * irradiance
                                    * cos_theta * static_cast<float>(nl);
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
    } else if (is_diffuse) {
      out_dir = sample_hemisphere(rng, shading_normal);
    } else {
      const float a2   = material.roughness * material.roughness;
      const float expt = std::max(0.0f, 2.0f / (a2 * a2) - 2.0f);
      const Vec3 refl  = ray.direction - 2.0f * dot(shading_normal, ray.direction) * shading_normal;
      out_dir = sample_phong_lobe(rng, refl, expt, shading_normal);
    }

    if (dot(out_dir, shading_normal) <= 0.0f) break;
    throughput = throughput * material.albedo;

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
  uint64_t local_rays = 0;

  for (uint32_t y = min_y; y < max_y; ++y) {
    for (uint32_t x_base = 0; x_base < settings_.width; x_base += 8) {
      const uint32_t count = std::min(8u, settings_.width - x_base);

      // Build an 8-lane SoA of camera rays (one per pixel)
      RaySoA ray8;
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = x_base + i;
        // Per-pixel per-sample RNG for jitter — mirrors scalar camera_rays()
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
        if (settings_.camera_aperture_radius > 0.0f && settings_.camera_focus_distance > kEpsilon) {
          const float lens_r = settings_.camera_aperture_radius * std::sqrt(jitter_rng.next01());
          const float lens_phi = 2.0f * kPi * jitter_rng.next01();
          const Vec3 lens_offset =
              cam_right_ * (lens_r * std::cos(lens_phi)) +
              cam_up_ * (lens_r * std::sin(lens_phi));
          const Vec3 focus_point = scene_.camera_position + dir * settings_.camera_focus_distance;
          origin = scene_.camera_position + lens_offset;
          dir = normalize(focus_point - origin);
        }
        reinterpret_cast<float*>(&ray8.ox)[i] = origin.x;
        reinterpret_cast<float*>(&ray8.oy)[i] = origin.y;
        reinterpret_cast<float*>(&ray8.oz)[i] = origin.z;
        reinterpret_cast<float*>(&ray8.dx)[i] = dir.x;
        reinterpret_cast<float*>(&ray8.dy)[i] = dir.y;
        reinterpret_cast<float*>(&ray8.dz)[i] = dir.z;
      }
      // Pad unused lanes
      for (uint32_t i = count; i < 8; ++i) {
        reinterpret_cast<float*>(&ray8.ox)[i] = 0.0f;
        reinterpret_cast<float*>(&ray8.oy)[i] = 0.0f;
        reinterpret_cast<float*>(&ray8.oz)[i] = 0.0f;
        reinterpret_cast<float*>(&ray8.dx)[i] = 0.0f;
        reinterpret_cast<float*>(&ray8.dy)[i] = 0.0f;
        reinterpret_cast<float*>(&ray8.dz)[i] = 1.0f; // safe degenerate
      }

      // Trace each pixel individually with correct per-pixel per-sample RNG
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t x = x_base + i;
        Rng rng = make_rng(x, y, settings_.width, sample_index, frame_index,
                           settings_.seed);
        // Consume camera jitter samples so the path RNG starts at the same dimension.
        rng.next01(); rng.next01();
        if (settings_.camera_aperture_radius > 0.0f && settings_.camera_focus_distance > kEpsilon) {
          rng.next01(); rng.next01();
        }

        // Build a single-ray SoA for this pixel's path tracing
        vkpt::pathtracer::Ray cam_ray{
          { reinterpret_cast<const float*>(&ray8.ox)[i],
            reinterpret_cast<const float*>(&ray8.oy)[i],
            reinterpret_cast<const float*>(&ray8.oz)[i] },
          { reinterpret_cast<const float*>(&ray8.dx)[i],
            reinterpret_cast<const float*>(&ray8.dy)[i],
            reinterpret_cast<const float*>(&ray8.dz)[i] }
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
