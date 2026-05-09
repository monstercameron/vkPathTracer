#include "scene/Scene.h"

#include "scene/SceneInternal.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vkpt::scene {

using namespace detail;

namespace {

vkpt::core::Status SceneOk(std::string message = {}) {
  return vkpt::core::Status::ok(std::move(message));
}

vkpt::core::Status SceneError(vkpt::core::StatusCode code, std::string message) {
  return vkpt::core::Status::error(code, std::move(message));
}

vkpt::core::Status MissingEntityStatus(std::string_view operation,
                                       vkpt::core::StableId id) {
  VKP_LOG(Warn,
          "scene",
          "operation_failed",
          "operation",
          operation,
          "reason",
          "missing_entity",
          "entity",
          id,
          "flow_id",
          id);
  return SceneError(vkpt::core::StatusCode::InvalidArgument,
                    std::string(operation) + " rejected: entity " +
                        std::to_string(id) + " does not exist");
}

std::string_view ComponentKindName(ComponentKind kind) {
  switch (kind) {
    case ComponentKind::Identity:
      return "Identity";
    case ComponentKind::Transform:
      return "Transform";
    case ComponentKind::Hierarchy:
      return "Hierarchy";
    case ComponentKind::Camera:
      return "Camera";
    case ComponentKind::Light:
      return "Light";
    case ComponentKind::MeshRenderer:
      return "MeshRenderer";
    case ComponentKind::SdfPrimitive:
      return "SDFPrimitive";
    case ComponentKind::MaterialOverride:
      return "MaterialOverride";
    case ComponentKind::PhysicsBody:
      return "PhysicsBody";
    case ComponentKind::Script:
      return "Script";
    case ComponentKind::AudioListener:
      return "AudioListener";
    case ComponentKind::AudioEmitter:
      return "AudioEmitter";
    case ComponentKind::UiPanel:
      return "UiPanel";
    case ComponentKind::BenchmarkTag:
      return "BenchmarkTag";
    case ComponentKind::Skeleton:
      return "Skeleton";
    case ComponentKind::Ragdoll:
      return "Ragdoll";
    case ComponentKind::Count:
      break;
  }
  return "Unknown";
}

vkpt::core::Status ComponentMismatchStatus(ComponentKind kind) {
  return SceneError(vkpt::core::StatusCode::InvalidArgument,
                    "component payload does not match " +
                        std::string(ComponentKindName(kind)));
}

vkpt::core::Status BoolStatus(bool ok,
                              vkpt::core::StatusCode code,
                              std::string message) {
  return ok ? SceneOk(std::move(message)) : SceneError(code, std::move(message));
}

}  // namespace

vkpt::core::StableId SceneWorld::create_entity(std::string_view name, vkpt::core::StableId stable_hint) {
  vkpt::core::contracts::assert_state(
      "SceneWorld::create_entity",
      lifecycle_state(),
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded});
  const bool wasEmpty = m_entities_order.empty();
  auto id = stable_hint == 0 ? m_nextStableId++ : stable_hint;
  if (m_entities.contains(id)) {
    if (stable_hint != 0) {
      return 0;
    }
    while (m_entities.contains(id)) {
      ++id;
    }
    m_nextStableId = id + 1;
  }
  EntityRecord record;
  record.stable_id = id;
  record.runtime_id = m_nextHandle++;
  record.alive = true;
  record.identity.stable_id = id;
  if (!name.empty()) {
    record.identity.name = std::string(name);
  }
  m_entities[id] = std::move(record);
  m_entities_order.push_back(id);
  if (id >= m_nextStableId) {
    m_nextStableId = id + 1;
  }
  if (wasEmpty) {
    VKP_LIFECYCLE_STARTED("scene",
                          "flow_id",
                          id,
                          "entity_count",
                          static_cast<std::uint64_t>(m_entities_order.size()));
  }
  VKP_LIFECYCLE_CONFIG("scene",
                       "flow_id",
                       id,
                       "entity_count",
                       static_cast<std::uint64_t>(m_entities_order.size()));
  return id;
}

vkpt::core::Status SceneWorld::destroy_entity(vkpt::core::StableId id) {
  vkpt::core::contracts::assert_state(
      "SceneWorld::destroy_entity",
      lifecycle_state(),
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded});
  const auto it = m_entities.find(id);
  if (it == m_entities.end()) {
    return MissingEntityStatus("destroy_entity", id);
  }
  if (!it->second.alive) {
    return MissingEntityStatus("destroy_entity", id);
  }
  it->second.alive = false;
  m_entities_order.erase(std::remove(m_entities_order.begin(), m_entities_order.end(), id), m_entities_order.end());
  if (const auto childIt = m_children.find(id); childIt != m_children.end()) {
    for (const auto child : childIt->second) {
      if (auto* childRecord = get_entity(child)) {
        childRecord->hierarchy.reset();
        mark_dirty_recursive(child);
      }
    }
  }
  m_children.erase(id);
  for (auto& parentChildren : m_children) {
    parentChildren.second.erase(std::remove(parentChildren.second.begin(), parentChildren.second.end(), id),
                               parentChildren.second.end());
    normalize_sibling_order(parentChildren.first);
  }
  m_transformAuthority.erase(id);
  m_worldTransforms.erase(id);
  if (m_entities_order.empty()) {
    VKP_LIFECYCLE_STOPPED("scene", "flow_id", id);
  }
  return SceneOk("entity destroyed");
}

bool SceneWorld::entity_exists(vkpt::core::StableId id) const {
  const auto it = m_entities.find(id);
  return it != m_entities.end() && it->second.alive;
}

vkpt::core::contracts::ComponentLifecycle SceneWorld::lifecycle_state() const noexcept {
  return m_entities_order.empty()
      ? vkpt::core::contracts::ComponentLifecycle::Uninitialized
      : vkpt::core::contracts::ComponentLifecycle::Ready;
}

bool SceneWorld::set_identity(vkpt::core::StableId id, const IdentityComponent& component) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->identity = component;
  return true;
}

bool SceneWorld::assign_material(vkpt::core::StableId id, vkpt::core::StableId material_id) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->material_override = MaterialOverrideComponent{material_id};
  return true;
}

bool SceneWorld::assign_light(vkpt::core::StableId id, const LightComponent& light) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->light = light;
  return true;
}

bool SceneWorld::assign_camera(vkpt::core::StableId id, const CameraComponent& camera) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  record->camera = camera;
  return true;
}

bool SceneWorld::set_hierarchy_parent(vkpt::core::StableId child,
                                      vkpt::core::StableId parent,
                                      std::uint32_t sibling_order) {
  auto* childRecord = get_entity(child);
  if (!childRecord) {
    return false;
  }
  if (parent != 0 && !entity_exists(parent)) {
    return false;
  }
  if (child == parent || is_ancestor(child, parent)) {
    return false;
  }
  // Keep sibling storage authoritative and mirror normalized order back to HierarchyComponent.
  const vkpt::core::StableId previous = childRecord->hierarchy ? childRecord->hierarchy->parent : 0;
  if (previous == parent && sibling_order == UINT32_MAX) {
    return true;
  }
  if (previous != 0) {
    auto prev = m_children.find(previous);
    if (prev != m_children.end()) {
      prev->second.erase(std::remove(prev->second.begin(), prev->second.end(), child), prev->second.end());
      normalize_sibling_order(previous);
    }
  }
  if (parent == 0) {
    childRecord->hierarchy.reset();
  } else {
    childRecord->hierarchy = HierarchyComponent{parent, 0};
    auto& list = m_children[parent];
    list.erase(std::remove(list.begin(), list.end(), child), list.end());
    const auto insert_index = sibling_order == UINT32_MAX
        ? list.size()
        : std::min<std::size_t>(list.size(), sibling_order);
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(insert_index), child);
    normalize_sibling_order(parent);
  }
  mark_dirty_recursive(child);
  return true;
}

vkpt::core::Status SceneWorld::reparent_entity(vkpt::core::StableId child,
                                               vkpt::core::StableId parent,
                                               bool preserve_world_transform) {
  vkpt::core::contracts::assert_state(
      "SceneWorld::reparent_entity",
      lifecycle_state(),
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded});
  auto* childRecord = get_entity(child);
  if (!childRecord) {
    return MissingEntityStatus("reparent_entity", child);
  }

  std::optional<TransformComponent> preservedLocalTransform;
  if (preserve_world_transform && childRecord->transform.has_value()) {
    // Convert the current world pose into the new parent's local space before rewiring hierarchy.
    const auto before = compute_world_transform_unchecked(childRecord);
    Mat4 localMatrix = before.world_matrix;
    if (parent != 0) {
      const auto* parentRecord = get_entity(parent);
      if (!parentRecord) {
        return MissingEntityStatus("reparent_entity parent", parent);
      }
      const auto parentWorld = compute_world_transform_unchecked(parentRecord);
      const auto parentInverse = inverse_affine_matrix(parentWorld.world_matrix);
      if (!parentInverse.has_value()) {
        return SceneError(vkpt::core::StatusCode::InvalidArgument,
                          "reparent_entity rejected: parent transform is not invertible");
      }
      localMatrix = multiply_matrix(parentInverse.value(), before.world_matrix);
    }
    preservedLocalTransform = transform_from_matrix(localMatrix);
  }

  if (!set_hierarchy_parent(child, parent)) {
    return SceneError(vkpt::core::StatusCode::InvalidArgument,
                      "reparent_entity rejected: invalid hierarchy relationship");
  }
  if (preservedLocalTransform.has_value()) {
    if (auto* updated = get_entity(child)) {
      updated->transform = preservedLocalTransform.value();
      mark_dirty_recursive(child);
    }
  }
  return SceneOk("entity reparented");
}

bool SceneWorld::reorder_entity(vkpt::core::StableId moved,
                                vkpt::core::StableId sibling_before,
                                vkpt::core::StableId sibling_after) {
  if (!entity_exists(moved) || sibling_before == moved || sibling_after == moved ||
      (sibling_before != 0 && !entity_exists(sibling_before)) ||
      (sibling_after != 0 && !entity_exists(sibling_after))) {
    return false;
  }
  const auto parent = parent_of(moved);
  if ((sibling_before != 0 && parent_of(sibling_before) != parent) ||
      (sibling_after != 0 && parent_of(sibling_after) != parent)) {
    return false;
  }

  auto reorder_in_list = [&](std::vector<vkpt::core::StableId>& list) {
    auto movedIt = std::find(list.begin(), list.end(), moved);
    if (movedIt == list.end()) {
      list.push_back(moved);
      movedIt = std::prev(list.end());
    }
    list.erase(movedIt);
    auto insertIt = list.end();
    if (sibling_before != 0) {
      const auto beforeIt = std::find(list.begin(), list.end(), sibling_before);
      if (beforeIt == list.end()) {
        return false;
      }
      insertIt = std::next(beforeIt);
    } else if (sibling_after != 0) {
      insertIt = std::find(list.begin(), list.end(), sibling_after);
      if (insertIt == list.end()) {
        return false;
      }
    }
    list.insert(insertIt, moved);
    return true;
  };

  if (parent == 0) {
    if (!reorder_in_list(m_entities_order)) {
      return false;
    }
  } else {
    auto& siblings = m_children[parent];
    if (!reorder_in_list(siblings)) {
      return false;
    }
    normalize_sibling_order(parent);
  }
  return true;
}

bool SceneWorld::destroy_subtree(vkpt::core::StableId id) {
  if (!entity_exists(id)) {
    return false;
  }
  auto children = children_of(id);
  for (const auto child : children) {
    if (!destroy_subtree(child)) {
      return false;
    }
  }
  return static_cast<bool>(destroy_entity(id));
}

vkpt::core::Status SceneWorld::set_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component) {
  return add_component(id, kind, component);
}

vkpt::core::Status SceneWorld::add_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component) {
  vkpt::core::contracts::assert_state(
      "SceneWorld::add_component",
      lifecycle_state(),
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded});
  auto* record = get_entity(id);
  if (!record) {
    return MissingEntityStatus("add_component", id);
  }
  switch (kind) {
    case ComponentKind::Identity:
      if (const auto* value = std::get_if<IdentityComponent>(&component)) {
        return BoolStatus(set_identity(id, *value),
                          vkpt::core::StatusCode::InternalError,
                          "identity component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Transform:
      if (const auto* value = std::get_if<TransformComponent>(&component)) {
        record->transform = *value;
        mark_dirty_recursive(id);
        return SceneOk("transform component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Hierarchy:
      if (const auto* value = std::get_if<HierarchyComponent>(&component)) {
        return BoolStatus(set_hierarchy_parent(id, value->parent, value->sibling_order),
                          vkpt::core::StatusCode::InvalidArgument,
                          "hierarchy component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Camera:
      if (const auto* value = std::get_if<CameraComponent>(&component)) {
        record->camera = *value;
        return SceneOk("camera component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Light:
      if (const auto* value = std::get_if<LightComponent>(&component)) {
        record->light = *value;
        return SceneOk("light component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::MeshRenderer:
      if (const auto* value = std::get_if<MeshRendererComponent>(&component)) {
        record->mesh_renderer = *value;
        return SceneOk("mesh renderer component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::SdfPrimitive:
      if (const auto* value = std::get_if<SdfPrimitiveComponent>(&component)) {
        record->sdf_primitive = *value;
        return SceneOk("sdf primitive component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::MaterialOverride:
      if (const auto* value = std::get_if<MaterialOverrideComponent>(&component)) {
        record->material_override = *value;
        return SceneOk("material override component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::PhysicsBody:
      if (const auto* value = std::get_if<PhysicsBodyComponent>(&component)) {
        record->physics_body = *value;
        return SceneOk("physics body component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Script:
      if (const auto* value = std::get_if<ScriptComponent>(&component)) {
        record->script = *value;
        return SceneOk("script component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::AudioListener:
      if (const auto* value = std::get_if<AudioListenerComponent>(&component)) {
        record->audio_listener = *value;
        return SceneOk("audio listener component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::AudioEmitter:
      if (const auto* value = std::get_if<AudioEmitterComponent>(&component)) {
        record->audio_emitter = *value;
        return SceneOk("audio emitter component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::UiPanel:
      if (const auto* value = std::get_if<UiPanelComponent>(&component)) {
        record->ui_panel = *value;
        return SceneOk("ui panel component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::BenchmarkTag:
      if (const auto* value = std::get_if<BenchmarkTagComponent>(&component)) {
        record->benchmark_tag = *value;
        return SceneOk("benchmark tag component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Ragdoll:
      if (const auto* value = std::get_if<RagdollComponent>(&component)) {
        record->ragdoll = *value;
        return SceneOk("ragdoll component set");
      }
      return ComponentMismatchStatus(kind);
    case ComponentKind::Skeleton:
      // Skeleton is not part of ComponentVariant; callers attach via
      // EntityRecord::skeleton directly (see Scene.cpp::to_world). Reject
      // the variant path defensively to catch misuse.
      return SceneError(vkpt::core::StatusCode::Unsupported,
                        "Skeleton component must be attached via EntityRecord::skeleton, not ComponentVariant");
    default:
      return SceneError(vkpt::core::StatusCode::Unsupported,
                        "unsupported component kind");
  }
}

bool SceneWorld::remove_component(vkpt::core::StableId id, ComponentKind kind) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  switch (kind) {
    case ComponentKind::Identity:
      record->identity = IdentityComponent{};
      return true;
    case ComponentKind::Transform:
      record->transform.reset();
      m_worldTransforms.erase(id);
      mark_dirty_recursive(id);
      return true;
    case ComponentKind::Hierarchy:
      if (record->hierarchy && record->hierarchy->parent != 0) {
        auto parentIt = m_children.find(record->hierarchy->parent);
        if (parentIt != m_children.end()) {
          parentIt->second.erase(std::remove(parentIt->second.begin(), parentIt->second.end(), id),
                                 parentIt->second.end());
          normalize_sibling_order(record->hierarchy->parent);
        }
      }
      record->hierarchy.reset();
      mark_dirty_recursive(id);
      return true;
    case ComponentKind::Camera:
      record->camera.reset();
      return true;
    case ComponentKind::Light:
      record->light.reset();
      return true;
    case ComponentKind::MeshRenderer:
      record->mesh_renderer.reset();
      return true;
    case ComponentKind::SdfPrimitive:
      record->sdf_primitive.reset();
      return true;
    case ComponentKind::MaterialOverride:
      record->material_override.reset();
      return true;
    case ComponentKind::PhysicsBody:
      record->physics_body.reset();
      return true;
    case ComponentKind::Script:
      record->script.reset();
      return true;
    case ComponentKind::AudioListener:
      record->audio_listener.reset();
      return true;
    case ComponentKind::AudioEmitter:
      record->audio_emitter.reset();
      return true;
    case ComponentKind::UiPanel:
      record->ui_panel.reset();
      return true;
    case ComponentKind::BenchmarkTag:
      record->benchmark_tag.reset();
      return true;
    case ComponentKind::Ragdoll:
      record->ragdoll.reset();
      return true;
    case ComponentKind::Skeleton:
      record->skeleton.reset();
      record->joint_world_matrices.clear();
      return true;
    default:
      return false;
  }
}

const SceneWorld::EntityRecord* SceneWorld::get_entity(vkpt::core::StableId id) const {
  const auto it = m_entities.find(id);
  if (it == m_entities.end() || !it->second.alive) {
    return nullptr;
  }
  return &it->second;
}

SceneWorld::EntityRecord* SceneWorld::get_entity(vkpt::core::StableId id) {
  auto it = m_entities.find(id);
  if (it == m_entities.end() || !it->second.alive) {
    return nullptr;
  }
  return &it->second;
}

const std::vector<vkpt::core::StableId>& SceneWorld::all_entities() const {
  return m_entities_order;
}

std::vector<vkpt::core::StableId> SceneWorld::children_of(vkpt::core::StableId parent) const {
  std::vector<vkpt::core::StableId> out;
  if (parent == 0) {
    out.reserve(m_entities_order.size());
    for (const auto id : m_entities_order) {
      const auto* entity = get_entity(id);
      if (entity && (!entity->hierarchy.has_value() || entity->hierarchy->parent == 0)) {
        out.push_back(id);
      }
    }
    return out;
  }

  const auto it = m_children.find(parent);
  if (it == m_children.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const auto child : it->second) {
    if (entity_exists(child)) {
      out.push_back(child);
    }
  }
  return out;
}

std::vector<vkpt::core::StableId> SceneWorld::query(ComponentKind kind) const {
  std::vector<vkpt::core::StableId> out;
  out.reserve(m_entities_order.size());
  for (const auto id : m_entities_order) {
    const auto* entity = get_entity(id);
    if (!entity) {
      continue;
    }
    switch (kind) {
      case ComponentKind::Identity:
        out.push_back(id);
        break;
      case ComponentKind::Transform:
        if (entity->transform.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Hierarchy:
        if (entity->hierarchy.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Camera:
        if (entity->camera.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Light:
        if (entity->light.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::MeshRenderer:
        if (entity->mesh_renderer.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::SdfPrimitive:
        if (entity->sdf_primitive.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::MaterialOverride:
        if (entity->material_override.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::PhysicsBody:
        if (entity->physics_body.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Script:
        if (entity->script.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::AudioListener:
        if (entity->audio_listener.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::AudioEmitter:
        if (entity->audio_emitter.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::UiPanel:
        if (entity->ui_panel.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::BenchmarkTag:
        if (entity->benchmark_tag.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Skeleton:
        if (entity->skeleton.has_value()) {
          out.push_back(id);
        }
        break;
      case ComponentKind::Ragdoll:
        if (entity->ragdoll.has_value()) {
          out.push_back(id);
        }
        break;
      default:
        break;
    }
  }
  return out;
}

std::vector<vkpt::core::StableId> SceneWorld::dirty_entities() const {
  std::vector<vkpt::core::StableId> out;
  out.reserve(m_entities_order.size());
  for (const auto id : m_entities_order) {
    const auto* entity = get_entity(id);
    if (entity != nullptr && entity->transform.has_value() && entity->transform->dirty) {
      out.push_back(id);
    }
  }
  return out;
}

void SceneWorld::set_determinism(const vkpt::core::DeterminismContext& context) {
  vkpt::core::contracts::assert_state(
      "SceneWorld::set_determinism",
      lifecycle_state(),
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded});
  const auto previous = m_determinismContext;
  m_determinismContext = context;
  vkpt::core::EmitDeterminismChangedIfNeeded("scene", previous, m_determinismContext);
}

vkpt::core::DeterminismContext SceneWorld::determinism_context() const {
  return m_determinismContext;
}

SceneWorldStatus SceneWorld::status() const {
  SceneWorldStatus out;
  out.entity_count = static_cast<std::uint64_t>(m_entities_order.size());
  out.dirty_entity_count = static_cast<std::uint64_t>(dirty_entities().size());
  out.transform_cache_count = static_cast<std::uint64_t>(m_worldTransforms.size());
  out.authority_conflicts_total =
      static_cast<std::uint64_t>(m_authority_conflicts.size());
  out.lifecycle = out.entity_count == 0u
      ? vkpt::core::contracts::ComponentLifecycle::Uninitialized
      : vkpt::core::contracts::ComponentLifecycle::Ready;
  out.deterministic = m_determinismContext.enabled;
  out.determinism_base_seed = m_determinismContext.base_seed;
  out.determinism_frame_index = m_determinismContext.frame_index;
  out.determinism_scenario_id = m_determinismContext.scenario_id;
  out.current_flow_id = m_determinismContext.frame_index;
  if (out.authority_conflicts_total != 0u) {
    out.health = vkpt::core::contracts::SubsystemHealth::Degraded;
    out.health_reason = "authority_conflicts";
  }
  return out;
}

vkpt::core::health::Report EvaluateSceneWorldHealth(
    const SceneWorldStatus& status) {
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;

  if (!status.last_error.empty()) {
    return Report{Status::Failed, status.last_error};
  }
  if (status.authority_conflicts_total != 0u) {
    return Report{
        Status::Degraded,
        "authority_conflicts=" + std::to_string(status.authority_conflicts_total)};
  }
  return Report{Status::Ok,
                status.health_reason.empty() ? "ok" : status.health_reason};
}

std::shared_ptr<vkpt::core::health::IHealthProbe>
SceneWorld::create_health_probe() const {
  class SceneWorldHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit SceneWorldHealthProbe(const SceneWorld* world) : m_world(world) {}

    std::string name() const override { return "scene"; }

    vkpt::core::health::Report check() override {
      if (m_world == nullptr) {
        return {vkpt::core::health::Status::Failed, "scene world unavailable"};
      }
      return EvaluateSceneWorldHealth(m_world->status());
    }

   private:
    const SceneWorld* m_world = nullptr;
  };

  return std::make_shared<SceneWorldHealthProbe>(this);
}

vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const SceneWorldStatus& status) {
  auto out = vkpt::core::contracts::MakeSubsystemStatus(status.name,
                                                       status.health);
  out.last_error = status.last_error;
  out.set_custom("lifecycle",
                 std::string(vkpt::core::contracts::ComponentLifecycleName(
                     status.lifecycle)));
  out.set_custom("health_reason", status.health_reason);
  out.set_custom("entity_count", std::to_string(status.entity_count));
  out.set_custom("dirty_entity_count",
                 std::to_string(status.dirty_entity_count));
  out.set_custom("transform_cache_count",
                 std::to_string(status.transform_cache_count));
  out.set_custom("authority_conflicts_total",
                 std::to_string(status.authority_conflicts_total));
  out.set_custom("deterministic", status.deterministic ? "true" : "false");
  out.set_custom("determinism_base_seed",
                 std::to_string(status.determinism_base_seed));
  out.set_custom("determinism_frame_index",
                 std::to_string(status.determinism_frame_index));
  out.set_custom("determinism_scenario_id", status.determinism_scenario_id);
  out.set_custom("current_flow_id", std::to_string(status.current_flow_id));
  return out;
}

std::string FormatSceneWorldStatus(const SceneWorldStatus& status) {
  std::ostringstream out;
  out << "scene status: "
      << vkpt::core::contracts::SubsystemHealthName(status.health)
      << "\n  lifecycle: "
      << vkpt::core::contracts::ComponentLifecycleName(status.lifecycle)
      << "\n  entities: " << status.entity_count
      << "\n  dirty_entities: " << status.dirty_entity_count
      << "\n  transform_cache: " << status.transform_cache_count
      << "\n  authority_conflicts: " << status.authority_conflicts_total
      << "\n  deterministic: " << (status.deterministic ? "true" : "false")
      << "\n  current_flow_id: " << status.current_flow_id;
  if (!status.last_error.empty()) {
    out << "\n  last_error: " << status.last_error;
  }
  return out.str();
}

void SceneWorld::mark_dirty_recursive(vkpt::core::StableId id) {
  auto* entity = get_entity(id);
  if (!entity) {
    return;
  }
  if (entity->transform.has_value()) {
    entity->transform->dirty = true;
  }
  // Cached world transforms are invalid for the full subtree once a local transform or parent changes.
  m_worldTransforms.erase(id);
  const auto it = m_children.find(id);
  if (it == m_children.end()) {
    return;
  }
  for (const auto child : it->second) {
    mark_dirty_recursive(child);
  }
}

bool SceneWorld::is_ancestor(vkpt::core::StableId ancestor, vkpt::core::StableId candidate) const {
  if (ancestor == 0 || candidate == 0) {
    return false;
  }
  auto it = m_entities.find(candidate);
  while (it != m_entities.end() && it->second.hierarchy.has_value()) {
    const auto parent = it->second.hierarchy->parent;
    if (parent == ancestor) {
      return true;
    }
    if (parent == 0) {
      break;
    }
    it = m_entities.find(parent);
  }
  return false;
}

vkpt::core::StableId SceneWorld::parent_of(vkpt::core::StableId id) const {
  const auto* entity = get_entity(id);
  if (!entity || !entity->hierarchy.has_value()) {
    return 0;
  }
  return entity->hierarchy->parent;
}

void SceneWorld::normalize_sibling_order(vkpt::core::StableId parent) {
  auto it = m_children.find(parent);
  if (it == m_children.end()) {
    return;
  }
  auto& children = it->second;
  children.erase(std::remove_if(children.begin(), children.end(),
                                [&](vkpt::core::StableId child) {
                                  return !entity_exists(child) || parent_of(child) != parent;
                                }),
                 children.end());
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (auto* child = get_entity(children[i]); child && child->hierarchy.has_value()) {
      child->hierarchy->sibling_order = static_cast<std::uint32_t>(i);
    }
  }
}

void SceneWorld::clear() {
  vkpt::core::contracts::assert_state(
      "SceneWorld::clear",
      lifecycle_state(),
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded,
       vkpt::core::contracts::ComponentLifecycle::ShuttingDown});
  const bool hadEntities = !m_entities_order.empty();
  const auto lastFlowId = hadEntities ? m_entities_order.back() : 0u;
  m_entities.clear();
  m_entities_order.clear();
  m_children.clear();
  m_transformAuthority.clear();
  m_worldTransforms.clear();
  m_authority_conflicts.clear();
  m_nextStableId = 1;
  m_nextHandle = 1;
  if (hadEntities) {
    VKP_LIFECYCLE_STOPPED("scene", "flow_id", lastFlowId);
  }
}

}  // namespace vkpt::scene
