#include "scene/Scene.h"

#include "scene/SceneInternal.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vkpt::scene {

using namespace detail;

vkpt::core::StableId SceneWorld::create_entity(std::string_view name, vkpt::core::StableId stable_hint) {
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
  return id;
}

bool SceneWorld::destroy_entity(vkpt::core::StableId id) {
  const auto it = m_entities.find(id);
  if (it == m_entities.end()) {
    return false;
  }
  if (!it->second.alive) {
    return false;
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
  return true;
}

bool SceneWorld::entity_exists(vkpt::core::StableId id) const {
  const auto it = m_entities.find(id);
  return it != m_entities.end() && it->second.alive;
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

bool SceneWorld::reparent_entity(vkpt::core::StableId child,
                                 vkpt::core::StableId parent,
                                 bool preserve_world_transform) {
  auto* childRecord = get_entity(child);
  if (!childRecord) {
    return false;
  }

  std::optional<TransformComponent> preservedLocalTransform;
  if (preserve_world_transform && childRecord->transform.has_value()) {
    // Convert the current world pose into the new parent's local space before rewiring hierarchy.
    const auto before = compute_world_transform_unchecked(childRecord);
    Mat4 localMatrix = before.world_matrix;
    if (parent != 0) {
      const auto* parentRecord = get_entity(parent);
      if (!parentRecord) {
        return false;
      }
      const auto parentWorld = compute_world_transform_unchecked(parentRecord);
      const auto parentInverse = inverse_affine_matrix(parentWorld.world_matrix);
      if (!parentInverse.has_value()) {
        return false;
      }
      localMatrix = multiply_matrix(parentInverse.value(), before.world_matrix);
    }
    preservedLocalTransform = transform_from_matrix(localMatrix);
  }

  if (!set_hierarchy_parent(child, parent)) {
    return false;
  }
  if (preservedLocalTransform.has_value()) {
    if (auto* updated = get_entity(child)) {
      updated->transform = preservedLocalTransform.value();
      mark_dirty_recursive(child);
    }
  }
  return true;
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
  return destroy_entity(id);
}

bool SceneWorld::set_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component) {
  return add_component(id, kind, component);
}

bool SceneWorld::add_component(vkpt::core::StableId id, ComponentKind kind, const ComponentVariant& component) {
  auto* record = get_entity(id);
  if (!record) {
    return false;
  }
  switch (kind) {
    case ComponentKind::Identity:
      if (const auto* value = std::get_if<IdentityComponent>(&component)) {
        return set_identity(id, *value);
      }
      return false;
    case ComponentKind::Transform:
      if (const auto* value = std::get_if<TransformComponent>(&component)) {
        record->transform = *value;
        mark_dirty_recursive(id);
        return true;
      }
      return false;
    case ComponentKind::Hierarchy:
      if (const auto* value = std::get_if<HierarchyComponent>(&component)) {
        return set_hierarchy_parent(id, value->parent, value->sibling_order);
      }
      return false;
    case ComponentKind::Camera:
      if (const auto* value = std::get_if<CameraComponent>(&component)) {
        record->camera = *value;
        return true;
      }
      return false;
    case ComponentKind::Light:
      if (const auto* value = std::get_if<LightComponent>(&component)) {
        record->light = *value;
        return true;
      }
      return false;
    case ComponentKind::MeshRenderer:
      if (const auto* value = std::get_if<MeshRendererComponent>(&component)) {
        record->mesh_renderer = *value;
        return true;
      }
      return false;
    case ComponentKind::SdfPrimitive:
      if (const auto* value = std::get_if<SdfPrimitiveComponent>(&component)) {
        record->sdf_primitive = *value;
        return true;
      }
      return false;
    case ComponentKind::MaterialOverride:
      if (const auto* value = std::get_if<MaterialOverrideComponent>(&component)) {
        record->material_override = *value;
        return true;
      }
      return false;
    case ComponentKind::PhysicsBody:
      if (const auto* value = std::get_if<PhysicsBodyComponent>(&component)) {
        record->physics_body = *value;
        return true;
      }
      return false;
    case ComponentKind::Script:
      if (const auto* value = std::get_if<ScriptComponent>(&component)) {
        record->script = *value;
        return true;
      }
      return false;
    case ComponentKind::AudioListener:
      if (const auto* value = std::get_if<AudioListenerComponent>(&component)) {
        record->audio_listener = *value;
        return true;
      }
      return false;
    case ComponentKind::AudioEmitter:
      if (const auto* value = std::get_if<AudioEmitterComponent>(&component)) {
        record->audio_emitter = *value;
        return true;
      }
      return false;
    case ComponentKind::UiPanel:
      if (const auto* value = std::get_if<UiPanelComponent>(&component)) {
        record->ui_panel = *value;
        return true;
      }
      return false;
    case ComponentKind::BenchmarkTag:
      if (const auto* value = std::get_if<BenchmarkTagComponent>(&component)) {
        record->benchmark_tag = *value;
        return true;
      }
      return false;
    default:
      return false;
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
      default:
        break;
    }
  }
  return out;
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
  m_entities.clear();
  m_entities_order.clear();
  m_children.clear();
  m_transformAuthority.clear();
  m_worldTransforms.clear();
  m_authority_conflicts.clear();
  m_nextStableId = 1;
  m_nextHandle = 1;
}

}  // namespace vkpt::scene
