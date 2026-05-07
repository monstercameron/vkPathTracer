#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"
#include "scene/SceneTypes.h"

namespace vkpt::scene {

enum class SceneSchemaError {
  Ok,
  ParseFailure,
  ValidationFailure
};

struct SceneTransformEntry {
  vkpt::core::StableId id = 0;
  TransformComponent transform;
  vkpt::core::StableId parent = 0;
};

struct SceneCameraDefinition {
  vkpt::core::StableId id = 0;
  CameraComponent camera;
};

struct SceneLightDefinition {
  vkpt::core::StableId id = 0;
  LightComponent light;
};

struct SceneEntityDefinition {
  vkpt::core::StableId id = 0;
  std::string name;
  bool visible = true;
  bool has_transform = false;
  bool has_camera = false;
  bool has_light = false;
  bool has_mesh = false;
  bool has_hierarchy = false;
  bool has_benchmark_tag = false;
  bool has_sdf_primitive = false;
  bool has_physics_body = false;
  bool has_audio_listener = false;
  bool has_audio_emitter = false;
  bool has_ui_panel = false;
  TransformComponent transform;
  CameraComponent camera;
  LightComponent light;
  MeshRendererComponent mesh;
  SdfPrimitiveComponent sdf_primitive;
  MaterialOverrideComponent material;
  HierarchyComponent hierarchy;
  PhysicsBodyComponent physics_body;
  ScriptComponent script;
  AudioListenerComponent audio_listener;
  AudioEmitterComponent audio_emitter;
  UiPanelComponent ui_panel;
  BenchmarkTagComponent benchmark_tag;
};

struct SceneAssetDefinition {
  vkpt::core::StableId id = 0;
  std::string type;
  std::string uri;
  std::string name;
  vkpt::core::StableId parent = 0;
  std::uint32_t sibling_order = 0;
  bool has_transform = false;
  TransformComponent transform;
};

struct SceneMaterialDefinition {
  vkpt::core::StableId id = 0;
  std::string name;
  std::string family = "diffuse";
  Vec3 albedo{1.0f, 1.0f, 1.0f};
  float roughness = 1.0f;
  float metallic = 0.0f;
  float ior = 1.5f;
  float transmission = 0.0f;
  float clearcoat = 0.0f;
  float sheen = 0.0f;
  float anisotropy = 0.0f;
  float alpha = 1.0f;
  bool double_sided = false;
  std::string base_color_texture;
  std::string normal_texture;
  Vec3 emission{0.0f, 0.0f, 0.0f};
  float emission_intensity = 0.0f;
};

enum class SceneMaterialPresetPolicy : std::uint8_t {
  Override,
  FillGenericDefaults,
};

std::string NormalizeMaterialFamilyId(std::string_view text);
void ApplyMaterialFamilyPreset(SceneMaterialDefinition& material,
                               SceneMaterialPresetPolicy policy = SceneMaterialPresetPolicy::Override);

struct SceneGeometryDefinition {
  vkpt::core::StableId id = 0;
  std::string primitive;
  std::vector<std::string> tags;
  std::vector<Vec3> vertices;
  std::vector<Vec2> texcoords;
  std::vector<std::uint32_t> indices;
  vkpt::core::StableId material_id = 0;

  struct TessellationSettings {
    bool enabled = false;
    std::string mode = "off";
    std::uint32_t factor = 1;
    bool gpu_preferred = true;
    bool cache_generated_geometry = true;
    bool displacement = false;
    std::string projection = "none";
  };

  TessellationSettings tessellation;
};

struct SceneSdfPrimitiveDefinition {
  vkpt::core::StableId id = 0;
  std::string shape;
  TransformComponent transform;
  SdfPrimitiveComponent primitive;
};

struct SceneParticleEmitterDefinition {
  vkpt::core::StableId id = 0;
  std::string name;
  std::string type = "rain";
  bool enabled = true;
  TransformComponent transform;
  Vec3 bounds{4.0f, 4.0f, 4.0f};
  Vec3 velocity{0.0f, -8.0f, 0.0f};
  Vec3 velocity_jitter{0.5f, 1.0f, 0.5f};
  Vec3 wind{0.0f, 0.0f, 0.0f};
  vkpt::core::StableId material_id = 0;
  std::uint32_t count = 128u;
  std::uint32_t seed = 1u;
  float time = 0.0f;
  float lifetime = 1.0f;
  float radius = 0.015f;
  float length = 0.45f;
  float turbulence = 0.25f;
  float gravity_scale = 1.0f;
  float drag = 0.0f;
  float bounce = 0.0f;
  float collision_plane_y = 0.0f;
  float vortex_strength = 0.0f;
};

struct SceneMetadata {
  std::string schema = "1.0";
  std::string scene_name;
  std::string author;
  std::string created;
};

struct SceneBenchmarkMetadata {
  uint32_t frame_target = 0;
  uint32_t warmup_frames = 0;
  bool enabled = false;
};

}  // namespace vkpt::scene
