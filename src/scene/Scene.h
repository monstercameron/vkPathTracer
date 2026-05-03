#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "core/Logging.h"
#include "core/Types.h"

namespace vkpt::scene {

enum class WorldSystemPhase : std::uint8_t {
  PreFrame,
  Input,
  ScriptEarly,
  AnimationSample,
  PhysicsFixed,
  TransformAssembly,
  SceneCommandApply,
  RenderExtract,
  PostFrame,
  Count
};

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
  Animation,
  BenchmarkTag,
  Count
};

enum class TransformAuthority : std::uint8_t {
  BenchmarkFrozen,
  PhysicsControlled,
  AnimationControlled,
  ScriptControlled,
  EditorControlled,
  Authored,
  Count
};

enum class SceneSchemaError {
  Ok,
  ParseFailure,
  ValidationFailure
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
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
};

struct CameraComponent {
  float fov = 60.0f;
  float near_plane = 0.1f;
  float far_plane = 1000.0f;
};

struct LightComponent {
  std::string type = "point";
  Vec3 color{1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  float radius = 0.0f;
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
  float mass = 1.0f;
  bool dynamic = false;
};

struct ScriptComponent {
  std::string script;
};

struct AnimationComponent {
  std::string clip;
  bool looping = true;
};

struct BenchmarkTagComponent {
  bool enabled = true;
};

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
    AnimationComponent,
    BenchmarkTagComponent>;

struct JsonValue;

class JsonParser {
 public:
  [[nodiscard]] static std::optional<JsonValue> parse(std::string_view text);
  [[nodiscard]] static std::string stringify(const JsonValue& value);
};

struct JsonValue {
  enum class Kind : std::uint8_t { Null, Boolean, Number, String, Array, Object };

  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<JsonValue> array;
  std::unordered_map<std::string, JsonValue> object;
};

struct WorldSystemSpec {
  std::string name;
  WorldSystemPhase phase = WorldSystemPhase::PreFrame;
  std::uint32_t readMask = 0;
  std::uint32_t writeMask = 0;
};

struct WorldSystemConflict {
  std::string system_a;
  std::string system_b;
  ComponentKind component = ComponentKind::Identity;
  WorldSystemPhase phase = WorldSystemPhase::PreFrame;
};

class WorldSystemScheduler {
 public:
  explicit WorldSystemScheduler(std::vector<WorldSystemPhase> phaseOrder = {});

  bool register_system(WorldSystemSpec spec);
  std::vector<std::string> validate() const;
  const std::vector<WorldSystemPhase>& phase_order() const;
  const std::vector<WorldSystemSpec>& systems() const;

  std::vector<WorldSystemConflict> conflicts() const;

 private:
  std::vector<WorldSystemPhase> m_phaseOrder;
  std::vector<WorldSystemSpec> m_systems;
  std::vector<WorldSystemConflict> m_conflicts;
};

struct TransformAuthorityState {
  TransformAuthority authority = TransformAuthority::Authored;
  vkpt::core::FrameIndex frame = 0;
  std::string writer;
};

struct WorldAuthorityConflict {
  vkpt::core::StableId entity = 0;
  std::string writer_a;
  std::string writer_b;
  TransformAuthority selected = TransformAuthority::Authored;
  vkpt::core::FrameIndex frame = 0;
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
  bool has_transform = false;
  bool has_camera = false;
  bool has_light = false;
  bool has_mesh = false;
  TransformComponent transform;
  CameraComponent camera;
  LightComponent light;
  MeshRendererComponent mesh;
  MaterialOverrideComponent material;
  HierarchyComponent hierarchy;
  AnimationComponent animation;
  ScriptComponent script;
  BenchmarkTagComponent benchmark_tag;
};

struct SceneAssetDefinition {
  vkpt::core::StableId id = 0;
  std::string type;
  std::string uri;
};

struct SceneMaterialDefinition {
  vkpt::core::StableId id = 0;
  std::string name;
  Vec3 albedo{1.0f, 1.0f, 1.0f};
  float roughness = 1.0f;
  Vec3 emission{0.0f, 0.0f, 0.0f};
  float emission_intensity = 0.0f;
};

struct SceneGeometryDefinition {
  vkpt::core::StableId id = 0;
  std::string primitive;
  std::vector<std::string> tags;
  std::vector<Vec3> vertices;
  std::vector<std::uint32_t> indices;
  vkpt::core::StableId material_id = 0;
};

struct SceneSdfPrimitiveDefinition {
  vkpt::core::StableId id = 0;
  std::string shape;
  TransformComponent transform;
  SdfPrimitiveComponent primitive;
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

struct SceneSnapshot {
  vkpt::core::Hash256 scene_hash{};
  std::vector<std::string> asset_refs;
  std::vector<vkpt::core::StableId> entity_ids;

  struct RenderableObject {
    vkpt::core::StableId entity_id = 0;
    vkpt::core::StableId mesh_id = 0;
    vkpt::core::StableId material_id = 0;
    TransformComponent transform;
  };

  struct LightObject {
    vkpt::core::StableId entity_id = 0;
    LightComponent light;
    TransformComponent transform;
  };

  struct MaterialObject {
    vkpt::core::StableId id = 0;
    SceneMaterialDefinition material;
  };

  std::vector<RenderableObject> renderables;
  std::vector<LightObject> lights;
  std::vector<MaterialObject> materials;
  std::optional<SceneCameraDefinition> camera;
  SceneBenchmarkMetadata benchmark{};
};

class SceneWorld {
 public:
  struct EntityRecord {
    vkpt::core::StableId stable_id = 0;
    vkpt::core::RuntimeHandle runtime_id = 0;
    bool alive = false;
    IdentityComponent identity;
    std::optional<TransformComponent> transform;
    std::optional<HierarchyComponent> hierarchy;
    std::optional<CameraComponent> camera;
    std::optional<LightComponent> light;
    std::optional<MeshRendererComponent> mesh_renderer;
    std::optional<SdfPrimitiveComponent> sdf_primitive;
    std::optional<MaterialOverrideComponent> material_override;
    std::optional<PhysicsBodyComponent> physics_body;
    std::optional<ScriptComponent> script;
    std::optional<AnimationComponent> animation;
    std::optional<BenchmarkTagComponent> benchmark_tag;
  };

  vkpt::core::StableId create_entity(std::string_view name = {}, vkpt::core::StableId stable_hint = 0);
  bool destroy_entity(vkpt::core::StableId id);
  bool entity_exists(vkpt::core::StableId id) const;

  bool set_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component);
  bool add_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component);
  bool remove_component(vkpt::core::StableId id, ComponentKind kind);

  bool set_identity(vkpt::core::StableId id, const IdentityComponent& component);
  bool set_transform(vkpt::core::StableId id, const TransformComponent& transform,
                     TransformAuthority authority = TransformAuthority::Authored,
                     std::string_view writer = "scene",
                     vkpt::core::FrameIndex frame = 0);
  bool assign_material(vkpt::core::StableId id, vkpt::core::StableId material_id);
  bool assign_light(vkpt::core::StableId id, const LightComponent& light);
  bool assign_camera(vkpt::core::StableId id, const CameraComponent& camera);
  bool set_hierarchy_parent(vkpt::core::StableId child, vkpt::core::StableId parent);

  const EntityRecord* get_entity(vkpt::core::StableId id) const;
  EntityRecord* get_entity(vkpt::core::StableId id);
  const std::vector<vkpt::core::StableId>& all_entities() const;
  std::vector<vkpt::core::StableId> query(ComponentKind kind) const;

  void recompute_world_transforms();
  const WorldTransform* world_transform(vkpt::core::StableId id) const;
  const std::vector<WorldAuthorityConflict>& authority_conflicts() const;

  SceneSnapshot build_snapshot() const;

  void clear();

 private:
  vkpt::core::StableId m_nextStableId = 1;
  vkpt::core::RuntimeHandle m_nextHandle = 1;
  std::vector<vkpt::core::StableId> m_entities_order;
  std::unordered_map<vkpt::core::StableId, EntityRecord> m_entities;
  std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>> m_children;
  std::unordered_map<vkpt::core::StableId, TransformAuthorityState> m_transformAuthority;
  std::unordered_map<vkpt::core::StableId, WorldTransform> m_worldTransforms;
  std::vector<WorldAuthorityConflict> m_authority_conflicts;

  void mark_dirty_recursive(vkpt::core::StableId id);
  bool is_ancestor(vkpt::core::StableId ancestor, vkpt::core::StableId candidate) const;
  WorldTransform compute_world_transform_unchecked(const EntityRecord* entity) const;
  bool has_authority(vkpt::core::StableId id, TransformAuthority authority, std::string_view writer, vkpt::core::FrameIndex frame) const;
  std::uint32_t kind_mask(ComponentKind kind) const;
};

class WorldCommandBuffer {
 public:
  struct CreateEntityCommand {
    vkpt::core::StableId requested_id = 0;
    std::string name;
  };

  struct DestroyEntityCommand {
    vkpt::core::StableId id = 0;
  };

  struct SetComponentCommand {
    vkpt::core::StableId id = 0;
    ComponentKind kind = ComponentKind::Identity;
    ComponentVariant component;
  };

  struct AddComponentCommand {
    vkpt::core::StableId id = 0;
    ComponentKind kind = ComponentKind::Identity;
    ComponentVariant component;
  };

  struct RemoveComponentCommand {
    vkpt::core::StableId id = 0;
    ComponentKind kind = ComponentKind::Identity;
  };

  struct SetTransformCommand {
    vkpt::core::StableId id = 0;
    TransformComponent transform;
    TransformAuthority authority = TransformAuthority::Authored;
    std::string writer;
    vkpt::core::FrameIndex frame = 0;
  };

  struct AssignMaterialCommand {
    vkpt::core::StableId id = 0;
    vkpt::core::StableId material_id = 0;
  };

  struct AssignLightCommand {
    vkpt::core::StableId id = 0;
    LightComponent light;
  };

  struct AssignCameraCommand {
    vkpt::core::StableId id = 0;
    CameraComponent camera;
  };

  enum class CommandType {
    CreateEntity,
    DestroyEntity,
    SetComponent,
    AddComponent,
    RemoveComponent,
    SetTransform,
    AssignMaterial,
    AssignLight,
    AssignCamera,
  };

  struct Command {
    CommandType type = CommandType::CreateEntity;
    std::variant<
        CreateEntityCommand,
        DestroyEntityCommand,
        SetComponentCommand,
        AddComponentCommand,
        RemoveComponentCommand,
        SetTransformCommand,
        AssignMaterialCommand,
        AssignLightCommand,
        AssignCameraCommand> payload;
  };

  void add_create_entity(std::string_view name = {}, vkpt::core::StableId stable_hint = 0);
  void add_destroy_entity(vkpt::core::StableId id);
  void add_set_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component);
  void add_add_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component);
  void add_remove_component(vkpt::core::StableId id, ComponentKind kind);
  void add_set_transform(vkpt::core::StableId id, TransformComponent transform,
                        TransformAuthority authority,
                        std::string_view writer,
                        vkpt::core::FrameIndex frame);
  void add_assign_material(vkpt::core::StableId id, vkpt::core::StableId material_id);
  void add_assign_light(vkpt::core::StableId id, const LightComponent& light);
  void add_assign_camera(vkpt::core::StableId id, const CameraComponent& camera);

  vkpt::core::Result<void> replay(SceneWorld& world) const;
  void clear();

  const std::vector<Command>& commands() const;

 private:
  std::vector<Command> m_commands;
};

class SceneDocument {
 public:
  SceneMetadata metadata;
  std::vector<SceneAssetDefinition> assets;
  std::vector<SceneMaterialDefinition> materials;
  std::vector<SceneGeometryDefinition> geometry;
  std::vector<SceneSdfPrimitiveDefinition> sdf_primitives;
  std::vector<SceneEntityDefinition> entities;
  std::vector<SceneTransformEntry> transforms;
  std::vector<SceneCameraDefinition> cameras;
  std::vector<SceneLightDefinition> lights;
  SceneBenchmarkMetadata benchmark;

  SceneSchemaError parse_result = SceneSchemaError::Ok;
  std::string parse_error;

  static vkpt::core::Result<SceneDocument> load_from_text(std::string_view text);
  static vkpt::core::Result<SceneDocument> load_from_file(std::string_view path);

  bool validate(std::vector<std::string>* issues = nullptr) const;
  bool has_section(std::string_view name) const;
  vkpt::core::Result<SceneWorld> to_world() const;
  vkpt::core::Result<void> apply_to_world(SceneWorld& world) const;

  std::string to_json(bool pretty = false) const;
  std::string export_hash_hex() const;
  SceneSnapshot snapshot() const;
};

std::string_view to_string(ComponentKind kind);
std::string_view to_string(TransformAuthority authority);

}  // namespace vkpt::scene
