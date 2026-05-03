#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Logging.h"
#include "core/Types.h"
#include "scene/Scene.h"

namespace vkpt::pathtracer {

struct Vec2 {
  float u = 0.0f;
  float v = 0.0f;
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;

  Vec3& operator+=(const Vec3& other) {
    x += other.x;
    y += other.y;
    z += other.z;
    return *this;
  }
};

struct Ray {
  Vec3 origin{};
  Vec3 direction{};
};

struct RenderSettings {
  uint32_t width = 256;
  uint32_t height = 256;
  uint32_t spp = 8;
  uint32_t max_depth = 6;
  uint64_t seed = 0x12345678ULL;
};

struct RenderStats {
  Vec2 resolution{};
  uint32_t spp = 0;
  uint32_t max_depth = 0;
  uint64_t seed = 0;
};

struct SampleKey {
  vkpt::core::FrameIndex frame_index = 0;
  vkpt::core::StableId pixel_index = 0;
  uint32_t sample_index = 0;
  uint32_t dimension = 0;
  uint32_t path_depth = 0;
  uint64_t path_id = 0;
  uint64_t seed = 0;
};

struct SampleCounters {
  uint64_t samples = 0;
  uint64_t rays = 0;
  uint64_t triangle_tests = 0;
  uint64_t sdf_tests = 0;
  uint64_t triangle_hits = 0;
  uint64_t sdf_hits = 0;
  uint64_t shadow_tests = 0;
};

enum class SdfShape : std::uint8_t {
  Sphere,
  Box,
  RoundedBox,
  Plane,
  Torus,
  Capsule,
  Unknown
};

struct RTMaterial {
  Vec3 albedo{1.0f, 1.0f, 1.0f};
  Vec3 emissive{0.0f, 0.0f, 0.0f};
  float roughness = 1.0f;
  bool is_emissive() const {
    return emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f;
  }
};

struct RTInstance {
  uint32_t geometry_id = 0;
  uint32_t first_triangle = 0;
  uint32_t triangle_count = 0;
  uint32_t material_index = 0;
  Vec3 translation{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
};

struct RTTriangle {
  uint32_t i0 = 0;
  uint32_t i1 = 0;
  uint32_t i2 = 0;
};

struct RTSdfPrimitive {
  SdfShape shape = SdfShape::Unknown;
  Vec3 position{};
  Vec3 scale{1.0f, 1.0f, 1.0f};
  Vec3 rotation{};
  uint32_t material_index = 0;
  float radius = 0.5f;
  float param_a = 0.0f;
  float param_b = 0.0f;
};

struct RTHitLight {
  Vec3 position{};
  Vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 0.0f;
};

struct RTSceneData {
  std::vector<Vec3> vertices;
  std::vector<uint32_t> indices;  // triangle index stream in triples
  std::vector<RTInstance> instances;
  std::vector<RTSdfPrimitive> sdf_primitives;
  std::vector<RTMaterial> materials;
  std::vector<std::string> textures;
  std::vector<RTHitLight> lights;
  Vec3 environment_color{0.0f, 0.0f, 0.0f};
  Vec3 camera_position{};
  Vec3 camera_target{0.0f, 0.0f, -1.0f};
  Vec3 camera_up{0.0f, 1.0f, 0.0f};
  float camera_fov_deg = 60.0f;
};

struct GpuLayoutField {
  std::string struct_name;
  std::string field;
  std::size_t cpu_offset = 0u;
  std::size_t cpu_size = 0u;
  std::size_t cpu_alignment = 0u;
  std::size_t gpu_offset = 0u;
  std::size_t gpu_size = 0u;
  std::size_t gpu_alignment = 0u;
};

struct RTSceneLayoutManifest {
  std::string schema_version = "1.0";
  std::size_t total_cpu_bytes = 0u;
  std::size_t total_gpu_bytes = 0u;
  std::vector<GpuLayoutField> fields;
};

struct FilmLdr {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba8;
};

struct FilmHdr {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> rgbf;
};

class FilmBuffer {
 public:
  FilmBuffer() = default;
  explicit FilmBuffer(uint32_t width, uint32_t height);

  void resize(uint32_t width, uint32_t height);
  void clear();
  void add_sample(uint32_t x, uint32_t y, const Vec3& color);
  // Copy raw accumulation and sample counts from src for rows [start_y, end_y).
  // Overwrites any existing data in that row range.
  void import_tile(const FilmBuffer& src, uint32_t start_y, uint32_t end_y);
  FilmLdr resolve_ldr() const;
  FilmHdr resolve_hdr() const;

  uint32_t width() const { return m_width; }
  uint32_t height() const { return m_height; }
  const std::vector<Vec3>& raw() const { return m_accumulation; }
  const std::vector<uint32_t>& sample_counts() const { return m_sampleCounts; }
  const std::vector<float>& invalid_samples() const { return m_invalidSamples; }

 private:
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  std::vector<Vec3> m_accumulation;
  std::vector<uint32_t> m_sampleCounts;
  std::vector<float> m_invalidSamples;
};

class IPathTracer {
 public:
  virtual ~IPathTracer() = default;

  virtual bool configure(const RenderSettings& settings) = 0;
  virtual bool load_scene_snapshot(const RTSceneData& scene) = 0;
  virtual bool build_or_update_acceleration() = 0;
  virtual bool reset_accumulation() = 0;
  virtual bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) = 0;
  virtual FilmLdr resolve_ldr() const = 0;
  virtual FilmHdr resolve_hdr() const = 0;
  virtual SampleCounters read_counters() const = 0;
  virtual void shutdown() = 0;
};

class ScalarCpuPathTracer final : public IPathTracer {
 public:
  bool configure(const RenderSettings& settings) override;
  bool load_scene_snapshot(const RTSceneData& scene) override;
  bool build_or_update_acceleration() override;
  bool reset_accumulation() override;
  bool render_sample_batch(uint32_t start_y, uint32_t end_y, uint32_t sample_index, uint32_t frame_index) override;
  FilmLdr resolve_ldr() const override { return m_film.resolve_ldr(); }
  FilmHdr resolve_hdr() const override { return m_film.resolve_hdr(); }
  SampleCounters read_counters() const override { return m_counters; }
  const FilmBuffer& film() const { return m_film; }
  void shutdown() override;

 private:
  struct Hit {
    bool hit = false;
    float t = 0.0f;
    Vec3 position{};
    Vec3 normal{};
    uint32_t material_index = 0;
  };

  struct Rng {
    explicit Rng(const SampleKey& key);
    float next01();
    Vec3 unit_sphere();

   private:
    uint64_t m_state = 0;
  };

  Vec3 make_unit(const Vec3& value) const;
  Vec3 sample_hemisphere(Rng& rng, const Vec3& normal) const;
  Vec3 trace(const Ray& ray, uint32_t sample_index, uint32_t frame_index, uint32_t path_id, uint32_t path_depth, uint64_t& ray_counter, Rng& rng);
  bool intersect_triangle(const RTTriangle& tri, const Ray& ray, float& t, float& u, float& v) const;
  bool intersect_box(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const;
  bool intersect_sphere(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const;
  bool intersect_rounded_box(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const;
  bool intersect_plane(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const;
  bool intersect_torus(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const;
  bool intersect_capsule(const RTSdfPrimitive& primitive, const Ray& ray, float& t, Vec3& normal) const;
  bool intersect_scene(const Ray& ray, Hit& out) const;
  Ray camera_rays(uint32_t x,
                  uint32_t y,
                  uint32_t sample_index,
                  uint32_t frame_index,
                  uint32_t path_id,
                  uint64_t& sample_seed);
  static Vec3 evaluate_bsdf(const RTMaterial& material, const Vec3& normal, const Vec3& in_dir, const Vec3& out_dir, float& pdf);

  RenderSettings m_settings;
  RTSceneData m_scene;
  FilmBuffer m_film;
  mutable SampleCounters m_counters{};
  bool m_configured = false;
  bool m_has_scene = false;
  Vec3 m_camera_right{};
  Vec3 m_camera_up{};
  Vec3 m_camera_forward{};
};

vkpt::core::Result<RTSceneData> BuildSceneDataFromDocument(const vkpt::scene::SceneDocument& doc);
bool SavePngCompat(const std::string& path, const FilmLdr& image, std::string* error = nullptr);
bool SaveExrCompat(const std::string& path, const FilmHdr& image, std::string* error = nullptr);
vkpt::core::Result<RTSceneLayoutManifest> BuildRTSceneDataLayoutManifest(std::vector<std::string>* diagnostics = nullptr);
std::string SerializeRTSceneDataLayoutManifest(const RTSceneLayoutManifest& manifest);

}  // namespace vkpt::pathtracer
