#include "materials/MaterialInterface.h"

#include <algorithm>
#include <cmath>

namespace vkpt::materials {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kInvPi = 0.31830988618f;

inline float vec3_dot(const Vec3& a, const Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float vec3_length(const Vec3& v) {
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline Vec3 vec3_normalize(const Vec3& v) {
  const float l = vec3_length(v);
  if (l < 1e-7f) return {0.0f, 1.0f, 0.0f};
  return {v.x / l, v.y / l, v.z / l};
}

inline Vec3 vec3_reflect(const Vec3& v, const Vec3& n) {
  const float d = 2.0f * vec3_dot(v, n);
  return {v.x - d * n.x, v.y - d * n.y, v.z - d * n.z};
}

inline Vec3 vec3_cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Build an ONB from a normal.
inline void make_onb(const Vec3& n, Vec3& t, Vec3& bt) {
  const Vec3 up = (std::fabs(n.z) < 0.999f) ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
  t = vec3_normalize(vec3_cross(up, n));
  bt = vec3_cross(n, t);
}

// Cosine-weighted hemisphere sample in world space.
inline Vec3 cosine_hemisphere_sample(const Vec3& normal, float u1, float u2) {
  const float r = std::sqrt(std::max(0.0f, 1.0f - u1));
  const float phi = 2.0f * kPi * u2;
  const Vec3 local{r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, u1))};
  Vec3 t, bt;
  make_onb(normal, t, bt);
  return vec3_normalize({
      t.x * local.x + bt.x * local.y + normal.x * local.z,
      t.y * local.x + bt.y * local.y + normal.y * local.z,
      t.z * local.x + bt.z * local.y + normal.z * local.z,
  });
}

inline float schlick(float cos_theta, float ior) {
  float r0 = (1.0f - ior) / (1.0f + ior);
  r0 = r0 * r0;
  return r0 + (1.0f - r0) * std::pow(1.0f - cos_theta, 5.0f);
}

inline Vec3 refract(const Vec3& wi, const Vec3& n, float eta, bool& total_internal) {
  const float cos_i = vec3_dot(wi, n);
  const float sin2_t = eta * eta * (1.0f - cos_i * cos_i);
  if (sin2_t >= 1.0f) {
    total_internal = true;
    return {};
  }
  total_internal = false;
  const float cos_t = std::sqrt(1.0f - sin2_t);
  return vec3_normalize({
      eta * (-wi.x) + (eta * cos_i - cos_t) * n.x,
      eta * (-wi.y) + (eta * cos_i - cos_t) * n.y,
      eta * (-wi.z) + (eta * cos_i - cos_t) * n.z,
  });
}

}  // namespace

// ----- DiffuseMaterial -------------------------------------------------------

Vec3 DiffuseMaterial::evaluate(const Vec3& wi, const Vec3& /*wo*/, const Vec3& normal) const {
  const float cos_theta = std::max(0.0f, vec3_dot(normal, wi));
  return {m_albedo.x * kInvPi * cos_theta,
          m_albedo.y * kInvPi * cos_theta,
          m_albedo.z * kInvPi * cos_theta};
}

MaterialSample DiffuseMaterial::sample(const Vec3& /*wo*/, const Vec3& normal, float u1, float u2) const {
  MaterialSample s;
  s.direction = cosine_hemisphere_sample(normal, u1, u2);
  const float cos_theta = std::max(0.0f, vec3_dot(normal, s.direction));
  s.pdf = cos_theta * kInvPi;
  if (s.pdf <= 0.0f) return s;
  s.bsdf = {m_albedo.x * kInvPi, m_albedo.y * kInvPi, m_albedo.z * kInvPi};
  s.valid = true;
  return s;
}

float DiffuseMaterial::pdf(const Vec3& wi, const Vec3& /*wo*/, const Vec3& normal) const {
  return std::max(0.0f, vec3_dot(normal, wi)) * kInvPi;
}

bool DiffuseMaterial::energy_check() const {
  return std::max({m_albedo.x, m_albedo.y, m_albedo.z}) <= 1.0f;
}

// ----- MirrorMaterial --------------------------------------------------------

Vec3 MirrorMaterial::evaluate(const Vec3& /*wi*/, const Vec3& /*wo*/, const Vec3& /*normal*/) const {
  return {0.0f, 0.0f, 0.0f};  // delta distribution
}

MaterialSample MirrorMaterial::sample(const Vec3& wo, const Vec3& normal, float /*u1*/, float /*u2*/) const {
  MaterialSample s;
  s.direction = vec3_reflect(wo, normal);
  s.bsdf = m_tint;
  s.pdf = 1.0f;
  s.valid = true;
  return s;
}

float MirrorMaterial::pdf(const Vec3& /*wi*/, const Vec3& /*wo*/, const Vec3& /*normal*/) const {
  return 0.0f;  // delta
}

bool MirrorMaterial::energy_check() const {
  return std::max({m_tint.x, m_tint.y, m_tint.z}) <= 1.0f;
}

// ----- GlassMaterial ---------------------------------------------------------

Vec3 GlassMaterial::evaluate(const Vec3& /*wi*/, const Vec3& /*wo*/, const Vec3& /*normal*/) const {
  return {0.0f, 0.0f, 0.0f};
}

MaterialSample GlassMaterial::sample(const Vec3& wo, const Vec3& normal, float u1, float /*u2*/) const {
  MaterialSample s;
  const float cos_i = std::max(0.0f, vec3_dot(wo, normal));
  const float fresnel = schlick(cos_i, m_ior);

  if (u1 < fresnel) {
    // Reflect.
    s.direction = vec3_reflect(wo, normal);
  } else {
    // Refract.
    const float eta = cos_i > 0.0f ? (1.0f / m_ior) : m_ior;
    const Vec3 n_used = cos_i > 0.0f ? normal : Vec3{-normal.x, -normal.y, -normal.z};
    bool tir = false;
    s.direction = refract(wo, n_used, eta, tir);
    if (tir) {
      s.direction = vec3_reflect(wo, normal);
    }
  }
  s.bsdf = m_tint;
  s.pdf = 1.0f;
  s.valid = true;
  return s;
}

float GlassMaterial::pdf(const Vec3& /*wi*/, const Vec3& /*wo*/, const Vec3& /*normal*/) const {
  return 0.0f;
}

bool GlassMaterial::energy_check() const {
  return std::max({m_tint.x, m_tint.y, m_tint.z}) <= 1.0f;
}

// ----- EmissiveMaterial ------------------------------------------------------

Vec3 EmissiveMaterial::evaluate(const Vec3& /*wi*/, const Vec3& /*wo*/, const Vec3& /*normal*/) const {
  return {0.0f, 0.0f, 0.0f};
}

MaterialSample EmissiveMaterial::sample(const Vec3& /*wo*/, const Vec3& /*normal*/, float /*u1*/, float /*u2*/) const {
  return {};  // not sampled as a BSDF
}

float EmissiveMaterial::pdf(const Vec3& /*wi*/, const Vec3& /*wo*/, const Vec3& /*normal*/) const {
  return 0.0f;
}

bool EmissiveMaterial::energy_check() const {
  return true;  // emission is not bounded by energy conservation the same way
}

}  // namespace vkpt::materials
