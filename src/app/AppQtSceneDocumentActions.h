#pragma once

#ifdef PT_ENABLE_QT

#include "core/Types.h"
#include "scene/Scene.h"

#include <cstdint>
#include <string_view>

namespace vkpt::app {

vkpt::scene::SceneEntityDefinition* FindQtSceneEntity(vkpt::scene::SceneDocument& document,
                                                      vkpt::core::StableId id);
vkpt::scene::SceneSdfPrimitiveDefinition* FindQtSceneSdfPrimitive(vkpt::scene::SceneDocument& document,
                                                                  vkpt::core::StableId id);
const vkpt::scene::SceneTransformEntry* FindQtSceneTransformEntry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
vkpt::core::StableId NextQtSceneObjectId(const vkpt::scene::SceneDocument& document);
void ApplyQtLegacyTransformToEntity(vkpt::scene::SceneDocument& document,
                                    vkpt::scene::SceneEntityDefinition& entity);
void RemoveQtLegacyTransformEntry(vkpt::scene::SceneDocument& document,
                                  vkpt::core::StableId id);
vkpt::scene::SceneEntityDefinition& EnsureQtSceneObjectEntity(
    vkpt::scene::SceneDocument& document,
    vkpt::core::StableId requestedId,
    std::string_view fallbackName);
vkpt::core::StableId QtSceneEntityParentId(const vkpt::scene::SceneDocument& document,
                                           const vkpt::scene::SceneEntityDefinition& entity);
vkpt::core::StableId FindQtSceneRootEntityId(const vkpt::scene::SceneDocument& document);
std::uint32_t NextQtSceneSiblingOrder(const vkpt::scene::SceneDocument& document,
                                      vkpt::core::StableId parentId);
bool WouldCreateQtSceneParentCycle(const vkpt::scene::SceneDocument& document,
                                   vkpt::core::StableId childId,
                                   vkpt::core::StableId newParentId);
bool SetQtSceneEntityParentPreserveWorld(vkpt::scene::SceneDocument& document,
                                         vkpt::core::StableId entityId,
                                         vkpt::core::StableId newParentId,
                                         const vkpt::scene::SceneWorld* preWriteWorld,
                                         bool preserveWorld);
void PromoteQtSceneObjectCamerasAndLights(vkpt::scene::SceneDocument& document);
void EnsureQtFallbackLightingEntities(vkpt::scene::SceneDocument& document);

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
