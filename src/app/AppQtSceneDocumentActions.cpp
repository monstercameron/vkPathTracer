#ifdef PT_ENABLE_QT

#include "app/AppQtSceneDocumentActions.h"

#include "app/ViewportInteraction.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace vkpt::app {

namespace {

constexpr std::string_view kQtFallbackLightingGroupName = "Fallback Lighting (off by default)";
constexpr std::string_view kQtLegacyFallbackLightingGroupName = "Fallback Lighting";
constexpr std::string_view kQtFallbackKeyLightName = "Fallback Key Light";
constexpr std::string_view kQtFallbackEnvironmentLightName = "Fallback Environment Light";

vkpt::scene::SceneEntityDefinition* FindQtSceneEntityByName(
    vkpt::scene::SceneDocument& document,
    std::string_view name) {
  const auto it = std::find_if(document.entities.begin(),
                               document.entities.end(),
                               [name](const vkpt::scene::SceneEntityDefinition& entity) {
                                 return entity.name == name;
                               });
  return it == document.entities.end() ? nullptr : &*it;
}

vkpt::scene::SceneEntityDefinition* FindQtSceneEntityByNameOrLegacy(
    vkpt::scene::SceneDocument& document,
    std::string_view name,
    std::string_view legacyName) {
  if (auto* entity = FindQtSceneEntityByName(document, name)) {
    return entity;
  }
  if (!legacyName.empty()) {
    if (auto* entity = FindQtSceneEntityByName(document, legacyName)) {
      entity->name = std::string(name);
      return entity;
    }
  }
  return nullptr;
}

vkpt::scene::SceneEntityDefinition& EnsureQtNamedSceneEntity(
    vkpt::scene::SceneDocument& document,
    std::string_view name,
    std::string_view legacyName,
    bool& created) {
  if (auto* entity = FindQtSceneEntityByNameOrLegacy(document, name, legacyName)) {
    created = false;
    return *entity;
  }
  created = true;
  return EnsureQtSceneObjectEntity(document, 0u, name);
}

void EnsureQtSceneEntityParent(vkpt::scene::SceneDocument& document,
                               vkpt::scene::SceneEntityDefinition& entity,
                               vkpt::core::StableId parentId) {
  if (parentId == 0u) {
    return;
  }
  entity.has_hierarchy = true;
  entity.hierarchy.parent = parentId;
  if (entity.hierarchy.sibling_order == 0u) {
    entity.hierarchy.sibling_order = NextQtSceneSiblingOrder(document, parentId);
  }
}

}  // namespace

vkpt::scene::SceneEntityDefinition* FindQtSceneEntity(vkpt::scene::SceneDocument& document,
                                                      vkpt::core::StableId id) {
  const auto it = std::find_if(document.entities.begin(),
                               document.entities.end(),
                               [id](const vkpt::scene::SceneEntityDefinition& entity) {
                                 return entity.id == id;
                               });
  return it == document.entities.end() ? nullptr : &*it;
}

vkpt::scene::SceneSdfPrimitiveDefinition* FindQtSceneSdfPrimitive(vkpt::scene::SceneDocument& document,
                                                                  vkpt::core::StableId id) {
  const auto it = std::find_if(document.sdf_primitives.begin(),
                               document.sdf_primitives.end(),
                               [id](const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
                                 return primitive.id == id;
                               });
  return it == document.sdf_primitives.end() ? nullptr : &*it;
}

const vkpt::scene::SceneTransformEntry* FindQtSceneTransformEntry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.transforms.begin(),
                               document.transforms.end(),
                               [id](const vkpt::scene::SceneTransformEntry& entry) {
                                 return entry.id == id;
                               });
  return it == document.transforms.end() ? nullptr : &*it;
}

vkpt::core::StableId NextQtSceneObjectId(const vkpt::scene::SceneDocument& document) {
  vkpt::core::StableId next = 1u;
  auto observe = [&](vkpt::core::StableId id) {
    if (id >= next) {
      next = id + 1u;
    }
  };
  for (const auto& entity : document.entities) {
    observe(entity.id);
  }
  for (const auto& entry : document.transforms) {
    observe(entry.id);
  }
  for (const auto& camera : document.cameras) {
    observe(camera.id);
  }
  for (const auto& light : document.lights) {
    observe(light.id);
  }
  for (const auto& primitive : document.sdf_primitives) {
    observe(primitive.id);
  }
  return next;
}

void ApplyQtLegacyTransformToEntity(vkpt::scene::SceneDocument& document,
                                    vkpt::scene::SceneEntityDefinition& entity) {
  if (const auto* entry = FindQtSceneTransformEntry(document, entity.id)) {
    if (!entity.has_transform) {
      entity.has_transform = true;
      entity.transform = entry->transform;
    }
    if (entry->parent != 0u && !entity.has_hierarchy) {
      entity.has_hierarchy = true;
      entity.hierarchy.parent = entry->parent;
    }
  }
}

void RemoveQtLegacyTransformEntry(vkpt::scene::SceneDocument& document,
                                  vkpt::core::StableId id) {
  document.transforms.erase(
      std::remove_if(document.transforms.begin(),
                     document.transforms.end(),
                     [id](const vkpt::scene::SceneTransformEntry& entry) {
                       return entry.id == id;
                     }),
      document.transforms.end());
}

vkpt::scene::SceneEntityDefinition& EnsureQtSceneObjectEntity(
    vkpt::scene::SceneDocument& document,
    vkpt::core::StableId requestedId,
    std::string_view fallbackName) {
  const auto id = requestedId != 0u ? requestedId : NextQtSceneObjectId(document);
  if (auto* entity = FindQtSceneEntity(document, id)) {
    if (entity->name.empty()) {
      entity->name = std::string(fallbackName);
    }
    ApplyQtLegacyTransformToEntity(document, *entity);
    return *entity;
  }
  vkpt::scene::SceneEntityDefinition entity{};
  entity.id = id;
  entity.name = std::string(fallbackName);
  ApplyQtLegacyTransformToEntity(document, entity);
  document.entities.push_back(std::move(entity));
  return document.entities.back();
}

vkpt::core::StableId QtSceneEntityParentId(const vkpt::scene::SceneDocument& document,
                                           const vkpt::scene::SceneEntityDefinition& entity) {
  if (entity.has_hierarchy && entity.hierarchy.parent != 0u) {
    return entity.hierarchy.parent;
  }
  if (const auto* entry = FindQtSceneTransformEntry(document, entity.id)) {
    return entry->parent;
  }
  return vkpt::core::StableId{0u};
}

vkpt::core::StableId FindQtSceneRootEntityId(const vkpt::scene::SceneDocument& document) {
  auto lowerName = [](std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return text;
  };
  auto matchesSceneRoot = [&](const vkpt::scene::SceneEntityDefinition& entity) {
    const std::string name = lowerName(entity.name);
    return name == "scene root" || name == "scene_root" || name == "root";
  };
  for (const auto& entity : document.entities) {
    if (QtSceneEntityParentId(document, entity) == 0u && matchesSceneRoot(entity)) {
      return entity.id;
    }
  }
  for (const auto& entity : document.entities) {
    if (matchesSceneRoot(entity)) {
      return entity.id;
    }
  }
  return vkpt::core::StableId{0u};
}

std::uint32_t NextQtSceneSiblingOrder(const vkpt::scene::SceneDocument& document,
                                      vkpt::core::StableId parentId) {
  std::uint32_t order = 0u;
  for (const auto& entity : document.entities) {
    if (QtSceneEntityParentId(document, entity) == parentId) {
      order = std::max(order, entity.hierarchy.sibling_order + 1u);
    }
  }
  return order;
}

bool WouldCreateQtSceneParentCycle(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId childId,
                                   vkpt::core::StableId newParentId) {
  std::unordered_set<vkpt::core::StableId> visited;
  vkpt::core::StableId cursor = newParentId;
  while (cursor != 0u && visited.insert(cursor).second) {
    if (cursor == childId) {
      return true;
    }
    const auto it = std::find_if(document.entities.begin(),
                                 document.entities.end(),
                                 [cursor](const vkpt::scene::SceneEntityDefinition& entity) {
                                   return entity.id == cursor;
                                 });
    cursor = it == document.entities.end() ? 0u : QtSceneEntityParentId(document, *it);
  }
  return false;
}

bool SetQtSceneEntityParentPreserveWorld(vkpt::scene::SceneDocument& document,
                                         vkpt::core::StableId entityId,
                                         vkpt::core::StableId newParentId,
                                         const vkpt::scene::SceneWorld* preWriteWorld,
                                         bool preserveWorld) {
  auto* entity = FindQtSceneEntity(document, entityId);
  if (entity == nullptr || entityId == newParentId ||
      WouldCreateQtSceneParentCycle(document, entityId, newParentId)) {
    return false;
  }
  const auto oldParent = QtSceneEntityParentId(document, *entity);
  if (oldParent == newParentId && entity->has_hierarchy) {
    return false;
  }
  const auto worldTransform = ResolveEntityWorldTransform(*entity, preWriteWorld);
  entity->has_hierarchy = true;
  entity->hierarchy.parent = newParentId;
  entity->hierarchy.sibling_order = NextQtSceneSiblingOrder(document, newParentId);
  RemoveQtLegacyTransformEntry(document, entityId);
  if (preserveWorld) {
    entity->has_transform = true;
    entity->transform = ConvertWorldTransformToDocumentLocal(*entity, preWriteWorld, worldTransform);
    entity->transform.dirty = true;
  } else if (!entity->has_transform) {
    entity->has_transform = true;
    entity->transform = {};
  }
  return true;
}

void PromoteQtSceneObjectCamerasAndLights(vkpt::scene::SceneDocument& document) {
  if (!document.cameras.empty()) {
    const auto legacyCameras = std::move(document.cameras);
    document.cameras.clear();
    for (const auto& camera : legacyCameras) {
      auto& entity = EnsureQtSceneObjectEntity(document, camera.id, "Camera");
      entity.has_camera = true;
      entity.camera = camera.camera;
      RemoveQtLegacyTransformEntry(document, entity.id);
    }
  }
  if (!document.lights.empty()) {
    const auto legacyLights = std::move(document.lights);
    document.lights.clear();
    for (const auto& light : legacyLights) {
      auto& entity = EnsureQtSceneObjectEntity(document, light.id, "Light");
      entity.has_light = true;
      entity.light = light.light;
      RemoveQtLegacyTransformEntry(document, entity.id);
    }
  }
}

void EnsureQtFallbackLightingEntities(vkpt::scene::SceneDocument& document) {
  PromoteQtSceneObjectCamerasAndLights(document);

  bool groupCreated = false;
  auto& group = EnsureQtNamedSceneEntity(document,
                                         kQtFallbackLightingGroupName,
                                         kQtLegacyFallbackLightingGroupName,
                                         groupCreated);
  if (groupCreated) {
    group.visible = false;
  }
  group.has_camera = false;
  group.has_light = false;
  group.has_mesh = false;
  group.has_sdf_primitive = false;
  const auto rootId = FindQtSceneRootEntityId(document);
  EnsureQtSceneEntityParent(document, group, rootId);
  RemoveQtLegacyTransformEntry(document, group.id);

  bool keyCreated = false;
  auto& key = EnsureQtNamedSceneEntity(document, kQtFallbackKeyLightName, {}, keyCreated);
  if (keyCreated) {
    key.visible = true;
    key.has_transform = true;
    key.transform.translation = {0.0f, 3.0f, 3.0f};
    key.transform.dirty = true;
  } else if (!key.has_transform) {
    key.has_transform = true;
    key.transform.translation = {0.0f, 3.0f, 3.0f};
  }
  key.has_light = true;
  key.light.type = "point";
  key.light.color = {1.0f, 0.96f, 0.88f};
  if (key.light.intensity <= 0.0f) {
    key.light.intensity = 10.0f;
  }
  if (key.light.radius <= 0.0f) {
    key.light.radius = 0.2f;
  }
  EnsureQtSceneEntityParent(document, key, group.id);
  RemoveQtLegacyTransformEntry(document, key.id);

  bool environmentCreated = false;
  auto& environment =
      EnsureQtNamedSceneEntity(document, kQtFallbackEnvironmentLightName, {}, environmentCreated);
  if (environmentCreated) {
    environment.visible = true;
  }
  environment.has_light = true;
  environment.light.type = "environment";
  environment.light.color = {0.35f, 0.4f, 0.5f};
  if (environment.light.intensity <= 0.0f) {
    environment.light.intensity = 1.0f;
  }
  EnsureQtSceneEntityParent(document, environment, group.id);
  RemoveQtLegacyTransformEntry(document, environment.id);
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
