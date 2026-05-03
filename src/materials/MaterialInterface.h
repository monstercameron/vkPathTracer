#pragma once

#include <cmath>
#include <string>
#include <string_view>

namespace vkpt::materials {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct MaterialSample {
  Vec3 direction{};
  Vec3 bsdf{};
  float pdf = 0.0f;
  bool valid = false;
};

class IMaterial {
 public:
  virtual ~IMaterial() = default;
  virtual Vec3 evaluate(const Vec3& wi, const Vec3& wo, const Vec3& normal) const = 0;
  virtual MaterialSample sample(const Vec3& wo, const Vec3& normal, float u1, float u2) const = 0;
  virtual float pdf(const Vec3& wi, const Vec3& wo, const Vec3& normal) const = 0;
  virtual bool is_delta() const = 0;
  virtual bool is_emissive() const = 0;
  virtual bool energy_check() const = 0;
  virtual std::string_view name() const = 0;
};

class DiffuseMaterial final : public IMaterial {
 public:
  explicit DiffuseMaterial(Vec3 albedo = {0.75f, 0.75f, 0.75f}) : m_albedo(albedo) {}
  Vec3 evaluate(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  MaterialSample sample(const Vec3& wo, const Vec3& normal, float u1, float u2) const override;
  float pdf(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  bool is_delta() const override { return false; }
  bool is_emissive() const override { return false; }
  bool energy_check() const override;
  std::string_view name() const override { return "diffuse"; }
 private:
  Vec3 m_albedo;
};

class MirrorMaterial final : public IMaterial {
 public:
  explicit MirrorMaterial(Vec3 tint = {1.0f, 1.0f, 1.0f}) : m_tint(tint) {}
  Vec3 evaluate(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  MaterialSample sample(const Vec3& wo, const Vec3& normal, float u1, float u2) const override;
  float pdf(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  bool is_delta() const override { return true; }
  bool is_emissive() const override { return false; }
  bool energy_check() const override;
  std::string_view name() const override { return "mirror"; }
 private:
  Vec3 m_tint;
};

class GlassMaterial final : public IMaterial {
 public:
  explicit GlassMaterial(float ior = 1.5f, Vec3 tint = {1.0f, 1.0f, 1.0f}) : m_ior(ior), m_tint(tint) {}
  Vec3 evaluate(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  MaterialSample sample(const Vec3& wo, const Vec3& normal, float u1, float u2) const override;
  float pdf(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  bool is_delta() const override { return true; }
  bool is_emissive() const override { return false; }
  bool energy_check() const override;
  std::string_view name() const override { return "glass"; }
 private:
  float m_ior;
  Vec3 m_tint;
};

class EmissiveMaterial final : public IMaterial {
 public:
  explicit EmissiveMaterial(Vec3 emission = {1.0f, 1.0f, 1.0f}) : m_emission(emission) {}
  Vec3 evaluate(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  MaterialSample sample(const Vec3& wo, const Vec3& normal, float u1, float u2) const override;
  float pdf(const Vec3& wi, const Vec3& wo, const Vec3& normal) const override;
  bool is_delta() const override { return false; }
  bool is_emissive() const override { return true; }
  bool energy_check() const override;
  std::string_view name() const override { return "emissive"; }
 private:
  Vec3 m_emission;
};

}  // namespace vkpt::materials
