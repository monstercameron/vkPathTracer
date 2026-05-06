#include "scene/Scene.h"

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace vkpt::scene {

void WorldCommandBuffer::add_create_entity(std::string_view name,
                                           vkpt::core::StableId stable_hint,
                                           vkpt::core::StableId requested_parent) {
  CreateEntityCommand cmd;
  cmd.name = std::string(name);
  cmd.requested_id = stable_hint;
  cmd.requested_parent = requested_parent;
  m_commands.push_back({CommandType::CreateEntity, cmd});
}

void WorldCommandBuffer::add_destroy_entity(vkpt::core::StableId id) {
  m_commands.push_back({CommandType::DestroyEntity, DestroyEntityCommand{id, false}});
}

void WorldCommandBuffer::add_destroy_subtree(vkpt::core::StableId id) {
  m_commands.push_back({CommandType::DestroyEntity, DestroyEntityCommand{id, true}});
}

void WorldCommandBuffer::add_set_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component) {
  m_commands.push_back({CommandType::SetComponent, SetComponentCommand{id, kind, std::move(component)}});
}

void WorldCommandBuffer::add_add_component(vkpt::core::StableId id, ComponentKind kind, ComponentVariant component) {
  m_commands.push_back({CommandType::AddComponent, AddComponentCommand{id, kind, std::move(component)}});
}

void WorldCommandBuffer::add_remove_component(vkpt::core::StableId id, ComponentKind kind) {
  m_commands.push_back({CommandType::RemoveComponent, RemoveComponentCommand{id, kind}});
}

void WorldCommandBuffer::add_set_transform(vkpt::core::StableId id,
                                           TransformComponent transform,
                                           TransformAuthority authority,
                                           std::string_view writer,
                                           vkpt::core::FrameIndex frame) {
  SetTransformCommand cmd;
  cmd.id = id;
  cmd.transform = transform;
  cmd.authority = authority;
  cmd.writer = std::string(writer);
  cmd.frame = frame;
  m_commands.push_back({CommandType::SetTransform, cmd});
}

void WorldCommandBuffer::add_reparent_entity(vkpt::core::StableId child,
                                             vkpt::core::StableId parent,
                                             bool preserve_world_transform) {
  m_commands.push_back({CommandType::ReparentEntity, ReparentEntityCommand{child, parent, preserve_world_transform}});
}

void WorldCommandBuffer::add_reorder_sibling(vkpt::core::StableId moved,
                                             vkpt::core::StableId sibling_before,
                                             vkpt::core::StableId sibling_after) {
  m_commands.push_back({CommandType::ReorderSibling, ReorderSiblingCommand{moved, sibling_before, sibling_after}});
}

void WorldCommandBuffer::add_assign_material(vkpt::core::StableId id, vkpt::core::StableId material_id) {
  m_commands.push_back({CommandType::AssignMaterial, AssignMaterialCommand{id, material_id}});
}

void WorldCommandBuffer::add_assign_light(vkpt::core::StableId id, const LightComponent& light) {
  m_commands.push_back({CommandType::AssignLight, AssignLightCommand{id, light}});
}

void WorldCommandBuffer::add_assign_camera(vkpt::core::StableId id, const CameraComponent& camera) {
  m_commands.push_back({CommandType::AssignCamera, AssignCameraCommand{id, camera}});
}

vkpt::core::Result<void> WorldCommandBuffer::replay(SceneWorld& world) const {
  for (const auto& command : m_commands) {
    bool ok = std::visit([&](const auto& payload) -> bool {
      using T = std::decay_t<decltype(payload)>;
      if constexpr (std::is_same_v<T, CreateEntityCommand>) {
        const auto created = world.create_entity(payload.name, payload.requested_id);
        return created != 0 && (payload.requested_parent == 0 ||
                                world.set_hierarchy_parent(created, payload.requested_parent));
      } else if constexpr (std::is_same_v<T, DestroyEntityCommand>) {
        return payload.destroy_children ? world.destroy_subtree(payload.id) : world.destroy_entity(payload.id);
      } else if constexpr (std::is_same_v<T, SetComponentCommand>) {
        return world.set_component(payload.id, payload.kind, payload.component);
      } else if constexpr (std::is_same_v<T, AddComponentCommand>) {
        return world.add_component(payload.id, payload.kind, payload.component);
      } else if constexpr (std::is_same_v<T, RemoveComponentCommand>) {
        return world.remove_component(payload.id, payload.kind);
      } else if constexpr (std::is_same_v<T, SetTransformCommand>) {
        return world.set_transform(payload.id, payload.transform, payload.authority, payload.writer, payload.frame);
      } else if constexpr (std::is_same_v<T, ReparentEntityCommand>) {
        return world.reparent_entity(payload.child, payload.parent, payload.preserve_world_transform);
      } else if constexpr (std::is_same_v<T, ReorderSiblingCommand>) {
        return world.reorder_entity(payload.moved, payload.sibling_before, payload.sibling_after);
      } else if constexpr (std::is_same_v<T, AssignMaterialCommand>) {
        return world.assign_material(payload.id, payload.material_id);
      } else if constexpr (std::is_same_v<T, AssignLightCommand>) {
        return world.assign_light(payload.id, payload.light);
      } else if constexpr (std::is_same_v<T, AssignCameraCommand>) {
        return world.assign_camera(payload.id, payload.camera);
      } else {
        return false;
      }
    }, command.payload);
    if (!ok) {
      return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
    }
  }
  return vkpt::core::Result<void>::ok();
}

void WorldCommandBuffer::clear() {
  m_commands.clear();
}

const std::vector<WorldCommandBuffer::Command>& WorldCommandBuffer::commands() const {
  return m_commands;
}

}  // namespace vkpt::scene
