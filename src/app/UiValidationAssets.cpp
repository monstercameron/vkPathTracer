#include "app/UiValidationInternal.h"

#include "assets/SceneAssetLoader.h"
#include "scene/Scene.h"

namespace vkpt::app {

void RunUiAssetDropSmokeChecks(const UiSmokeCheckFn& check_true) {
  using namespace vkpt::editor;
  const auto valid_texture_drop = ValidateAssetDrop("C:/assets/brick.PNG", "texture_slot");
  check_true("valid texture drop extension support", valid_texture_drop.extension_supported);
  check_true("valid texture drop accepted", valid_texture_drop.accepted);
  check_true("valid texture drop type", valid_texture_drop.asset_type == "texture");
  vkpt::scene::SceneDocument importDoc;
  importDoc.metadata.schema = "1.0";
  vkpt::scene::SceneMaterialDefinition importMaterial;
  importMaterial.id = 1;
  importMaterial.name = "default";
  importDoc.materials.push_back(importMaterial);
  vkpt::scene::TransformComponent importPlacement;
  importPlacement.translation = {1.0f, 2.0f, 3.0f};
  importPlacement.scale = {1.0f, 1.0f, 1.0f};
  importPlacement.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  importPlacement.dirty = true;
  vkpt::assets::SceneAssetExpansionStats importStats{};
  const bool modelImported = vkpt::assets::ImportSceneModelAsset(importDoc,
                                                                 "assets/scenes/cornell_native.json",
                                                                 "assets/models/lisa/lisa.obj",
                                                                 importPlacement,
                                                                 &importStats,
                                                                 nullptr);
  check_true("asset browser model import spawns root entity",
             modelImported &&
             importStats.imported_models == 1u &&
             importStats.imported_root_entity != 0u &&
             !importDoc.geometry.empty());
  vkpt::scene::SceneDocument emptyMaterialImportDoc;
  emptyMaterialImportDoc.metadata.schema = "1.0";
  vkpt::assets::SceneAssetExpansionStats emptyMaterialImportStats{};
  const bool emptyMaterialModelImported = vkpt::assets::ImportSceneModelAsset(emptyMaterialImportDoc,
                                                                             "assets/scenes/cornell_native.json",
                                                                             "assets/models/lisa/lisa.obj",
                                                                             importPlacement,
                                                                             &emptyMaterialImportStats,
                                                                             nullptr);
  check_true("asset browser model import creates fallback material",
             emptyMaterialModelImported &&
             emptyMaterialImportStats.imported_models == 1u &&
             !emptyMaterialImportDoc.materials.empty() &&
             !emptyMaterialImportDoc.geometry.empty());

  const auto invalid_texture_drop = ValidateAssetDrop("C:/assets/readme.md", "texture_slot");
  check_true("invalid texture drop rejected", !invalid_texture_drop.accepted);
  check_true("invalid texture drop unsupported", !invalid_texture_drop.extension_supported);

  const auto unknown_slot_drop = ValidateAssetDrop("C:/assets/model.obj", "invalid_slot");
  check_true("unknown slot drop rejected", !unknown_slot_drop.accepted);
  check_true("unknown slot drop still supported", unknown_slot_drop.extension_supported);

  const auto empty_path_drop = ValidateAssetDrop("", "material_slot");
  check_true("empty path drop rejected", !empty_path_drop.accepted);
  check_true("empty path drop reports reason", !empty_path_drop.reason.empty());

}

}  // namespace vkpt::app
