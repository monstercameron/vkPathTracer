#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/Types.h"

namespace vkpt::animation {
struct Skeleton;
}  // namespace vkpt::animation

namespace vkpt::scene {

enum class ComponentKind : std::uint8_t {
  Identity,
  Transform,
  Hierarchy,
  Camera,
  Light,
  MeshRenderer,
  SdfPrimitive,
  MaterialOverride,
  PhysicsBody,
  Script,
  AudioListener,
  AudioEmitter,
  UiPanel,
  BenchmarkTag,
  Skeleton,
  Count
};

enum class TransformAuthority : std::uint8_t {
  BenchmarkFrozen,
  PhysicsControlled,
  ScriptControlled,
  EditorControlled,
  Authored,
  Count
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Vec2 {
  float u = 0.0f;
  float v = 0.0f;
};

struct Quat {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
};

struct Mat4 {
  std::array<float, 16> values{};
};

struct IdentityComponent {
  vkpt::core::StableId stable_id = 0;
  std::string name;
};

struct TransformComponent {
  Vec3 translation{0.0f, 0.0f, 0.0f};
  Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
  Vec3 scale{1.0f, 1.0f, 1.0f};
  bool dirty = true;
};

struct WorldTransform : TransformComponent {
  Mat4 world_matrix{};
};

struct HierarchyComponent {
  vkpt::core::StableId parent = 0;
  std::uint32_t sibling_order = 0;
};

struct CameraComponent {
  float fov = 60.0f;
  float near_plane = 0.1f;
  float far_plane = 1000.0f;
  float focal_length_mm = 35.0f;
  float sensor_width_mm = 36.0f;
  float sensor_height_mm = 24.0f;
  float aperture_radius = 0.0f;
  float focus_distance = 0.0f;
  float f_stop = 0.0f;
  float shutter_seconds = 0.0166666675f;
  float iso = 100.0f;
  float exposure_compensation = 0.0f;
  float white_balance_kelvin = 6500.0f;
  std::uint32_t iris_blade_count = 0u;
  float iris_rotation_degrees = 0.0f;
  float iris_roundness = 1.0f;
  float anamorphic_squeeze = 1.0f;
};

struct LightComponent {
  std::string type = "point";
  Vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float radius = 0.0f;
  Vec3 direction{0.0f, -1.0f, 0.0f};
  float beam_angle_degrees = 35.0f;
  float blend = 0.35f;
};

struct MeshRendererComponent {
  vkpt::core::StableId mesh_id = 0;
  vkpt::core::StableId material_id = 0;
};

struct SdfPrimitiveComponent {
  std::string shape = "sphere";
  float radius = 1.0f;
  float param_a = 0.0f;
  float param_b = 0.0f;
};

struct MaterialOverrideComponent {
  vkpt::core::StableId material_id = 0;
};

struct PhysicsBodyComponent {
  bool enabled = true;
  float mass = 1.0f;
  bool dynamic = false;
  std::string body_type = "static";
  std::string shape = "box";
  float friction = 0.5f;
  float restitution = 0.0f;
  float gravity_scale = 1.0f;
  bool trigger = false;
  bool allow_sleeping = true;
  bool continuous_collision = false;
};

struct ScriptComponent {
  std::string script;
  std::string language = "lua";
  std::string entry = "default";
  std::string module_id = "default";
  bool enabled = true;
  bool reload_on_save = true;
  std::unordered_map<std::string, std::string> params;
};

struct AudioListenerComponent {
  bool enabled = true;
  bool primary = true;
};

struct AudioEmitterComponent {
  std::string event;
  std::string bus = "sfx";
  bool enabled = true;
  bool autoplay = false;
  bool loop = false;
  bool spatial = true;
  float volume = 1.0f;
  float pitch = 1.0f;
  float min_distance = 1.0f;
  float max_distance = 24.0f;
};

struct UiPanelComponent {
  std::string panel_id = "script.panel";
  std::string title;
  std::string anchor = "top_left";
  bool enabled = true;
  bool visible = true;
  float x = 16.0f;
  float y = 16.0f;
  float width = 320.0f;
  float height = 0.0f;
  float opacity = 0.84f;
  float font_size = 13.0f;
  Vec3 background{0.04f, 0.055f, 0.07f};
  Vec3 foreground{0.92f, 0.96f, 1.0f};
  Vec3 accent{0.16f, 0.72f, 0.95f};
  std::vector<std::string> lines;
};

struct BenchmarkTagComponent {
  bool enabled = true;
};

// Phase 1 ANI01: alias only — full Skeleton definition lives in
// animation/Skeleton.h, which scene callers should include directly when they
// need to inspect joints. Kept out of the ComponentVariant to avoid pulling
// the animation header into every scene translation unit.
using SkeletonComponent = vkpt::animation::Skeleton;

using ComponentVariant = std::variant<
    IdentityComponent,
    TransformComponent,
    HierarchyComponent,
    CameraComponent,
    LightComponent,
    MeshRendererComponent,
    SdfPrimitiveComponent,
    MaterialOverrideComponent,
    PhysicsBodyComponent,
    ScriptComponent,
    AudioListenerComponent,
    AudioEmitterComponent,
    UiPanelComponent,
    BenchmarkTagComponent>;

std::string_view to_string(ComponentKind kind);
std::string_view to_string(TransformAuthority authority);

}  // namespace vkpt::scene
