#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "core/Types.h"
#include "scene/FrameLifecycle.h"
#include "scene/RenderExtraction.h"
#include "scene/SceneDocumentSchema.h"
#include "scene/SceneTypes.h"

namespace vkpt::scene {

class SceneDocument;

  enum class WorldSystemPhase : std::uint8_t {
    PreFrame,
    Input,
    ScriptEarly,
    ScriptFixed,
    ScriptLate,
    PhysicsFixed,
  TransformAssembly,
  SceneCommandApply,
  RenderExtract,
  PostFrame,
  Count
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
  /// Last accepted transform writer class for this entity on `frame`.
  TransformAuthority authority = TransformAuthority::Authored;
  vkpt::core::FrameIndex frame = 0;
  std::string writer;
};

/// Records a same-frame transform write conflict and the authority that won arbitration.
struct WorldAuthorityConflict {
  vkpt::core::StableId entity = 0;
  std::string writer_a;
  std::string writer_b;
  TransformAuthority selected = TransformAuthority::Authored;
  vkpt::core::FrameIndex frame = 0;
};

struct SceneWorldStatus {
  std::string name = "scene";
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  vkpt::core::contracts::SubsystemHealth health =
      vkpt::core::contracts::SubsystemHealth::Ok;
  std::uint64_t entity_count = 0u;
  std::uint64_t dirty_entity_count = 0u;
  std::uint64_t transform_cache_count = 0u;
  std::uint64_t authority_conflicts_total = 0u;
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  vkpt::core::FrameIndex determinism_frame_index = 0u;
  std::string determinism_scenario_id;
  std::uint64_t current_flow_id = 0u;
  std::string health_reason = "ok";
  std::string last_error;
};

vkpt::core::health::Report EvaluateSceneWorldHealth(
    const SceneWorldStatus& status);
vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const SceneWorldStatus& status);
std::string FormatSceneWorldStatus(const SceneWorldStatus& status);

class IEcsWorld {
 public:
  virtual ~IEcsWorld() = default;

  // IEcsWorld lifecycle contract:
  //
  // state\method       create mutate/query recompute snapshot/extract clear set_determinism status
  // Uninitialized      ->Ready error/ok     ok        empty-ok          ok    ok              ok
  // Ready              ok      ok           ok        ok                ->Uninit ok           ok
  // Degraded           ok      ok           ok        ok                ->Uninit ok           ok
  // Failed             error   error        error     error             ->Uninit ok           ok
  // ShuttingDown       error   error        error     error             ok    noop            ok
  //
  // SceneWorld currently enters Degraded via status health, not lifecycle, when
  // deterministic authority arbitration records conflicts. Stable entity order,
  // stable writer-name ties, and DeterminismContext frame/seed propagation are
  // part of the scene determinism contract.
  virtual vkpt::core::StableEntityId create_entity(std::string_view name = {}, vkpt::core::StableEntityId stable_hint = 0) = 0;
  virtual vkpt::core::Status destroy_entity(vkpt::core::StableEntityId id) = 0;
  virtual bool entity_exists(vkpt::core::StableEntityId id) const = 0;
  virtual vkpt::core::Status set_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) = 0;
  virtual vkpt::core::Status add_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) = 0;
  virtual bool remove_component(vkpt::core::StableEntityId id, ComponentKind kind) = 0;
  /// Set a transform through authority arbitration. Higher authority wins; equal authority is tie-broken by writer name.
  virtual vkpt::core::Status set_transform(vkpt::core::StableEntityId id, const TransformComponent& transform,
                                           TransformAuthority authority = TransformAuthority::Authored,
                                           std::string_view writer = "scene",
                                           vkpt::core::FrameIndex frame = 0) = 0;
  /// Change hierarchy parent. When requested, local transform is adjusted to preserve current world pose.
  virtual vkpt::core::Status reparent_entity(vkpt::core::StableEntityId child,
                                             vkpt::core::StableEntityId parent,
                                             bool preserve_world_transform = true) = 0;
  virtual bool reorder_entity(vkpt::core::StableEntityId moved,
                              vkpt::core::StableEntityId sibling_before,
                              vkpt::core::StableEntityId sibling_after) = 0;
  virtual const std::vector<vkpt::core::StableEntityId>& all_entities() const = 0;
  virtual std::vector<vkpt::core::StableEntityId> children_of(vkpt::core::StableEntityId parent) const = 0;
  virtual std::vector<vkpt::core::StableEntityId> query(ComponentKind kind) const = 0;
  /// Return transform-dirty entities in stable entity order for partial recomputation.
  virtual std::vector<vkpt::core::StableEntityId> dirty_entities() const = 0;
  /// Recompute cached world transforms for dirty transform hierarchies.
  virtual void recompute_world_transforms() = 0;
  /// Return the cached world transform, or nullptr until recompute_world_transforms has produced one.
  virtual const WorldTransform* world_transform(vkpt::core::StableEntityId id) const = 0;
  virtual void set_determinism(const vkpt::core::DeterminismContext& context) = 0;
  virtual vkpt::core::DeterminismContext determinism_context() const = 0;
  virtual SceneWorldStatus status() const = 0;
  /// Build the scene health probe; hosts can register it with HealthRegistry.
  virtual std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const = 0;
  /// Build a stable snapshot from ECS component state.
  virtual SceneSnapshot build_snapshot() const = 0;
  /// Extract renderer-facing data from ECS state for the given frame.
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
    std::optional<AudioListenerComponent> audio_listener;
    std::optional<AudioEmitterComponent> audio_emitter;
    std::optional<UiPanelComponent> ui_panel;
    std::optional<BenchmarkTagComponent> benchmark_tag;
  };

  vkpt::core::StableEntityId create_entity(std::string_view name = {}, vkpt::core::StableEntityId stable_hint = 0) override;
  vkpt::core::Status destroy_entity(vkpt::core::StableEntityId id) override;
  bool entity_exists(vkpt::core::StableEntityId id) const override;

  vkpt::core::Status set_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) override;
  vkpt::core::Status add_component(vkpt::core::StableEntityId id, ComponentKind kind, const ComponentVariant& component) override;
  bool remove_component(vkpt::core::StableEntityId id, ComponentKind kind) override;

  bool set_identity(vkpt::core::StableEntityId id, const IdentityComponent& component);
  vkpt::core::Status set_transform(vkpt::core::StableEntityId id, const TransformComponent& transform,
                                   TransformAuthority authority = TransformAuthority::Authored,
                                   std::string_view writer = "scene",
                                   vkpt::core::FrameIndex frame = 0) override;
  bool assign_material(vkpt::core::StableEntityId id, vkpt::core::MaterialId material_id);
  bool assign_light(vkpt::core::StableEntityId id, const LightComponent& light);
  bool assign_camera(vkpt::core::StableEntityId id, const CameraComponent& camera);
  bool set_hierarchy_parent(vkpt::core::StableEntityId child,
                            vkpt::core::StableEntityId parent,
                            std::uint32_t sibling_order = UINT32_MAX);
  vkpt::core::Status reparent_entity(vkpt::core::StableEntityId child,
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
  std::vector<vkpt::core::StableEntityId> dirty_entities() const override;

  void recompute_world_transforms() override;
  const WorldTransform* world_transform(vkpt::core::StableEntityId id) const override;
  const std::vector<WorldAuthorityConflict>& authority_conflicts() const;
  void set_determinism(const vkpt::core::DeterminismContext& context) override;
  vkpt::core::DeterminismContext determinism_context() const override;
  SceneWorldStatus status() const override;
  std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const override;

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
  vkpt::core::DeterminismContext m_determinismContext{};

  void mark_dirty_recursive(vkpt::core::StableEntityId id);
  bool is_ancestor(vkpt::core::StableEntityId ancestor, vkpt::core::StableEntityId candidate) const;
  vkpt::core::StableEntityId parent_of(vkpt::core::StableEntityId id) const;
  void normalize_sibling_order(vkpt::core::StableEntityId parent);
  WorldTransform compute_world_transform_unchecked(const EntityRecord* entity) const;
  bool has_authority(vkpt::core::StableEntityId id, TransformAuthority authority, std::string_view writer, vkpt::core::FrameIndex frame) const;
  std::uint32_t kind_mask(ComponentKind kind) const;
  vkpt::core::contracts::ComponentLifecycle lifecycle_state() const noexcept;
};

/// Preferred ECS-facing name. SceneWorld remains as the compatibility spelling.
using EcsWorld = SceneWorld;

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

  /// Flow id propagated with this command batch, normally the sim frame/snapshot generation.
  void set_flow_id(std::uint64_t flow_id);
  std::uint64_t flow_id() const noexcept;
  /// Apply commands in insertion order. Stops at the first rejected command and reports Internal.
  vkpt::core::Result<void> replay(SceneWorld& world) const;
  void clear();

  const std::vector<Command>& commands() const;

 private:
  std::uint64_t m_flowId = 0u;
  std::vector<Command> m_commands;
};

template <typename HealthRegistryT>
void RegisterSceneWorldHealthProbe(SceneWorld& world, HealthRegistryT& registry) {
  registry.register_probe(world.create_health_probe());
}

// ISceneRuntime lifecycle contract:
//
// state\method      world  load_document       snapshot  extract_render_scene  frame_lifecycle  shutdown(dtor)
// Uninitialized     ok     ->Ready             empty     empty                 ok               noop
// Ready             ok     ->Ready (replace)   ok        ok                    ok               ->Uninitialized
// Degraded          ok     ->Ready             ok        ok                    ok               ->Uninitialized
// Failed            ok     error               ok        ok                    ok               ->Uninitialized
// ShuttingDown      ok     illegal             ok        ok                    ok               noop
//
// world(), snapshot(), extract_render_scene(), and frame_lifecycle() are
// state-agnostic Ready-or-better reads. load_document() is the only call that
// drives the lifecycle: it is the explicit transition into Ready and the only
// gate before shutdown.
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

}  // namespace vkpt::scene
