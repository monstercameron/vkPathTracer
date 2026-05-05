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

enum class FrameStage : std::uint8_t {
  FrameBegin,
  Input,
  CommandCollection,
  FixedUpdate,
  VariableUpdate,
  TransformAssembly,
  SceneMutationApply,
  RenderPreparation,
  RenderSubmit,
  PresentOrExport,
  FrameEnd,
  Count
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
  std::uint32_t sibling_order = 0;
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
  bool has_hierarchy = false;
  bool has_benchmark_tag = false;
  bool has_sdf_primitive = false;
  bool has_physics_body = false;
  TransformComponent transform;
  CameraComponent camera;
  LightComponent light;
  MeshRendererComponent mesh;
  SdfPrimitiveComponent sdf_primitive;
  MaterialOverrideComponent material;
  HierarchyComponent hierarchy;
  PhysicsBodyComponent physics_body;
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

struct RenderSceneProxy {
  vkpt::core::Hash256 scene_hash{};
  vkpt::core::FrameIndex frame = 0;

  struct Renderable {
    vkpt::core::StableEntityId entity_id = 0;
    vkpt::core::AssetId geometry_id = 0;
    vkpt::core::MaterialId material_id = 0;
    Mat4 world_matrix{};
    Vec3 translation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
  };

  struct Light {
    vkpt::core::StableEntityId entity_id = 0;
    std::string type;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float radius = 0.0f;
    Mat4 world_matrix{};
    Vec3 position{};
  };

  struct Material {
    vkpt::core::MaterialId id = 0;
    Vec3 albedo{1.0f, 1.0f, 1.0f};
    float roughness = 1.0f;
    Vec3 emission{0.0f, 0.0f, 0.0f};
    float emission_intensity = 0.0f;
  };

  struct Camera {
    vkpt::core::StableEntityId entity_id = 0;
    float fov = 60.0f;
    float near_plane = 0.1f;
    float far_plane = 1000.0f;
    Mat4 world_matrix{};
    Vec3 position{};
  };

  std::vector<Renderable> renderables;
  std::vector<Light> lights;
  std::vector<Material> materials;
  std::optional<Camera> camera;
  SceneBenchmarkMetadata benchmark{};

  bool empty() const {
    return renderables.empty() && lights.empty() && materials.empty() && !camera.has_value();
  }
};

struct FrameContext {
  vkpt::core::FrameIndex frame = 0;
  double delta_seconds = 0.0;
  FrameStage stage = FrameStage::FrameBegin;
  bool deterministic = false;
};

struct FrameStageTiming {
  vkpt::core::FrameIndex frame = 0;
  FrameStage stage = FrameStage::FrameBegin;
  uint64_t start_ns = 0u;
  uint64_t end_ns = 0u;
  uint64_t duration_ns() const { return end_ns >= start_ns ? end_ns - start_ns : 0u; }
};

class FrameLifecycleController {
 public:
  const FrameContext& context() const;
  const std::vector<FrameStageTiming>& timings() const;

  void begin_frame(vkpt::core::FrameIndex frame, double delta_seconds, bool deterministic = false);
  void begin_stage(FrameStage stage);
  void end_stage(FrameStage stage);
  void end_frame();
  void clear_history();

 private:
  FrameContext m_context{};
  std::vector<FrameStageTiming> m_timings;
  uint64_t m_stageStartNs = 0u;
  bool m_stageOpen = false;
};

class IEcsWorld {
 public:
  virtual ~IEcsWorld() = default;

  virtual vkpt::core::StableEntityId create_entity(std::string_view name = {}, vkpt::core::StableEntityId stable_hint = 0) = 0;
  virtual bool destroy_entity(vkpt::core::StableEntityId id) = 0;
  virtual bool entity_exists(vkpt::core::StableEntityId id) const = 0;
  virtual bool set_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) = 0;
  virtual bool add_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) = 0;
  virtual bool remove_component(vkpt::core::StableEntityId id, ComponentKind kind) = 0;
  virtual bool set_transform(vkpt::core::StableEntityId id, const TransformComponent& transform,
                             TransformAuthority authority = TransformAuthority::Authored,
                             std::string_view writer = "scene",
                             vkpt::core::FrameIndex frame = 0) = 0;
  virtual bool reparent_entity(vkpt::core::StableEntityId child,
                               vkpt::core::StableEntityId parent,
                               bool preserve_world_transform = true) = 0;
  virtual bool reorder_entity(vkpt::core::StableEntityId moved,
                              vkpt::core::StableEntityId sibling_before,
                              vkpt::core::StableEntityId sibling_after) = 0;
  virtual const std::vector<vkpt::core::StableEntityId>& all_entities() const = 0;
  virtual std::vector<vkpt::core::StableEntityId> children_of(vkpt::core::StableEntityId parent) const = 0;
  virtual std::vector<vkpt::core::StableEntityId> query(ComponentKind kind) const = 0;
  virtual void recompute_world_transforms() = 0;
  virtual const WorldTransform* world_transform(vkpt::core::StableEntityId id) const = 0;
  virtual SceneSnapshot build_snapshot() const = 0;
  virtual RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const = 0;
};

class SceneWorld : public IEcsWorld {
 public:
  struct EntityRecord {
    vkpt::core::StableEntityId stable_id = 0;
    vkpt::core::EntityHandle runtime_id = 0;
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

  vkpt::core::StableEntityId create_entity(std::string_view name = {}, vkpt::core::StableEntityId stable_hint = 0) override;
  bool destroy_entity(vkpt::core::StableEntityId id) override;
  bool entity_exists(vkpt::core::StableEntityId id) const override;

  bool set_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) override;
  bool add_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) override;
  bool remove_component(vkpt::core::StableEntityId id, ComponentKind kind) override;

  bool set_identity(vkpt::core::StableEntityId id, const IdentityComponent& component);
  bool set_transform(vkpt::core::StableEntityId id, const TransformComponent& transform,
                     TransformAuthority authority = TransformAuthority::Authored,
                     std::string_view writer = "scene",
                     vkpt::core::FrameIndex frame = 0) override;
  bool assign_material(vkpt::core::StableEntityId id, vkpt::core::MaterialId material_id);
  bool assign_light(vkpt::core::StableEntityId id, const LightComponent& light);
  bool assign_camera(vkpt::core::StableEntityId id, const CameraComponent& camera);
  bool set_hierarchy_parent(vkpt::core::StableEntityId child,
                            vkpt::core::StableEntityId parent,
                            std::uint32_t sibling_order = UINT32_MAX);
  bool reparent_entity(vkpt::core::StableEntityId child,
                       vkpt::core::StableEntityId parent,
                       bool preserve_world_transform = true) override;
  bool reorder_entity(vkpt::core::StableEntityId moved,
                      vkpt::core::StableEntityId sibling_before,
                      vkpt::core::StableEntityId sibling_after) override;
  bool destroy_subtree(vkpt::core::StableEntityId id);

  const EntityRecord* get_entity(vkpt::core::StableEntityId id) const;
  EntityRecord* get_entity(vkpt::core::StableEntityId id);
  const std::vector<vkpt::core::StableEntityId>& all_entities() const override;
  std::vector<vkpt::core::StableEntityId> children_of(vkpt::core::StableEntityId parent) const override;
  std::vector<vkpt::core::StableEntityId> query(ComponentKind kind) const override;

  void recompute_world_transforms() override;
  const WorldTransform* world_transform(vkpt::core::StableEntityId id) const override;
  const std::vector<WorldAuthorityConflict>& authority_conflicts() const;

  SceneSnapshot build_snapshot() const override;
  RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const override;

  void clear();

 private:
  vkpt::core::StableEntityId m_nextStableId = 1;
  vkpt::core::EntityHandle m_nextHandle = 1;
  std::vector<vkpt::core::StableEntityId> m_entities_order;
  std::unordered_map<vkpt::core::StableEntityId, EntityRecord> m_entities;
  std::unordered_map<vkpt::core::StableEntityId, std::vector<vkpt::core::StableEntityId>> m_children;
  std::unordered_map<vkpt::core::StableEntityId, TransformAuthorityState> m_transformAuthority;
  std::unordered_map<vkpt::core::StableEntityId, WorldTransform> m_worldTransforms;
  std::vector<WorldAuthorityConflict> m_authority_conflicts;

  void mark_dirty_recursive(vkpt::core::StableEntityId id);
  bool is_ancestor(vkpt::core::StableEntityId ancestor, vkpt::core::StableEntityId candidate) const;
  vkpt::core::StableEntityId parent_of(vkpt::core::StableEntityId id) const;
  void normalize_sibling_order(vkpt::core::StableEntityId parent);
  WorldTransform compute_world_transform_unchecked(const EntityRecord* entity) const;
  bool has_authority(vkpt::core::StableEntityId id, TransformAuthority authority, std::string_view writer, vkpt::core::FrameIndex frame) const;
  std::uint32_t kind_mask(ComponentKind kind) const;
};

class WorldCommandBuffer {
 public:
  struct CreateEntityCommand {
    vkpt::core::StableId requested_id = 0;
    vkpt::core::StableId requested_parent = 0;
    std::string name;
  };

  struct DestroyEntityCommand {
    vkpt::core::StableId id = 0;
    bool destroy_children = false;
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

  struct ReparentEntityCommand {
    vkpt::core::StableId child = 0;
    vkpt::core::StableId parent = 0;
    bool preserve_world_transform = true;
  };

  struct ReorderSiblingCommand {
    vkpt::core::StableId moved = 0;
    vkpt::core::StableId sibling_before = 0;
    vkpt::core::StableId sibling_after = 0;
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
    ReparentEntity,
    ReorderSibling,
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
        ReparentEntityCommand,
        ReorderSiblingCommand,
        AssignMaterialCommand,
        AssignLightCommand,
        AssignCameraCommand> payload;
  };

  void add_create_entity(std::string_view name = {},
                         vkpt::core::StableId stable_hint = 0,
                         vkpt::core::StableId requested_parent = 0);
  void add_destroy_entity(vkpt::core::StableId id);
  void add_destroy_subtree(vkpt::core::StableId id);
  void add_set_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component);
  void add_add_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component);
  void add_remove_component(vkpt::core::StableId id, ComponentKind kind);
  void add_set_transform(vkpt::core::StableId id, TransformComponent transform,
                        TransformAuthority authority,
                        std::string_view writer,
                        vkpt::core::FrameIndex frame);
  void add_reparent_entity(vkpt::core::StableId child,
                           vkpt::core::StableId parent,
                           bool preserve_world_transform = true);
  void add_reorder_sibling(vkpt::core::StableId moved,
                           vkpt::core::StableId sibling_before,
                           vkpt::core::StableId sibling_after);
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
  RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const;
};

class ISceneLoader {
 public:
  virtual ~ISceneLoader() = default;
  virtual vkpt::core::Result<SceneDocument> load_document_from_text(std::string_view text) = 0;
  virtual vkpt::core::Result<SceneDocument> load_document_from_file(std::string_view path) = 0;
};

class JsonSceneLoader final : public ISceneLoader {
 public:
  vkpt::core::Result<SceneDocument> load_document_from_text(std::string_view text) override;
  vkpt::core::Result<SceneDocument> load_document_from_file(std::string_view path) override;
};

class ISceneRuntime {
 public:
  virtual ~ISceneRuntime() = default;
  virtual IEcsWorld& world() = 0;
  virtual const IEcsWorld& world() const = 0;
  virtual vkpt::core::Result<void> load_document(const SceneDocument& document) = 0;
  virtual SceneSnapshot snapshot() const = 0;
  virtual RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const = 0;
  virtual FrameLifecycleController& frame_lifecycle() = 0;
  virtual const FrameLifecycleController& frame_lifecycle() const = 0;
};

class SceneRuntime final : public ISceneRuntime {
 public:
  SceneRuntime() = default;
  explicit SceneRuntime(SceneWorld world);

  IEcsWorld& world() override;
  const IEcsWorld& world() const override;
  SceneWorld& scene_world();
  const SceneWorld& scene_world() const;

  vkpt::core::Result<void> load_document(const SceneDocument& document) override;
  SceneSnapshot snapshot() const override;
  RenderSceneProxy extract_render_scene(vkpt::core::FrameIndex frame = 0) const override;
  FrameLifecycleController& frame_lifecycle() override;
  const FrameLifecycleController& frame_lifecycle() const override;

 private:
  SceneWorld m_world;
  FrameLifecycleController m_frameLifecycle;
};

std::string_view to_string(ComponentKind kind);
std::string_view to_string(TransformAuthority authority);
std::string_view to_string(FrameStage stage);

}  // namespace vkpt::scene
