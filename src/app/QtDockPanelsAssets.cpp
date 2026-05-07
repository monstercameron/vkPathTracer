#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <utility>

namespace vkpt::app {

QtDockPanelContent BuildQtAssetBrowserDock(const vkpt::scene::SceneDocument& document,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           const vkpt::editor::UiRuntimeState& runtime,
                                           const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "asset_browser", "Asset Browser", true, 720.0f, 360.0f);
  panel.tree_single_column = true;
  panel.tree_stretch = 1;
  panel.property_stretch = 0;
  panel.property_preferred_height = 104;
  const auto sceneFiles = QtDockFindAssetFiles(QtDockFindRepoRelativePath("assets/scenes"), {".json"}, false);
  const auto modelFiles = QtDockFindAssetFiles(QtDockFindRepoRelativePath("assets/models"), {".obj", ".gltf"}, true);

  QtDockAddProperty(panel, "scene", runtime.active_scene.empty() ? "none" : runtime.active_scene);
  QtDockAddProperty(panel, "library", std::to_string(sceneFiles.size()) + " scenes, " +
                                  std::to_string(modelFiles.size()) + " models");
  QtDockAddProperty(panel, "current refs", std::to_string(document.assets.size()));
  QtDockAddProperty(panel, "geometry", std::to_string(document.geometry.size()) + " meshes, " +
                                   std::to_string(scene.textures.size()) + " textures");

  QtDockTreeRow sceneGroup;
  sceneGroup.label = "Scenes (" + std::to_string(sceneFiles.size()) + ")";
  sceneGroup.value = std::to_string(sceneFiles.size()) + " files";
  sceneGroup.icon = "folder";
  for (const auto& path : sceneFiles) {
    const bool selected = QtDockSamePath(QtDockDisplayPath(path), runtime.active_scene);
    sceneGroup.children.push_back(QtDockAssetFileRow("asset.scene.",
                                                     path,
                                                     QtDockReadSceneDisplayName(path),
                                                     "scene",
                                                     selected));
  }
  if (!sceneGroup.children.empty()) {
    QtDockAddTreeRow(panel, std::move(sceneGroup));
  }

  QtDockTreeRow modelGroup;
  modelGroup.label = "Models (" + std::to_string(modelFiles.size()) + ")";
  modelGroup.value = std::to_string(modelFiles.size()) + " files";
  modelGroup.icon = "folder";
  for (const auto& path : modelFiles) {
    const auto ext = QtDockToLower(path.extension().string());
    auto row = QtDockAssetFileRow("asset.model.",
                                  path,
                                  QtDockPrettyStem(path),
                                  "model",
                                  false);
    row.value += "  " + ext;
    row.draggable = true;
    modelGroup.children.push_back(std::move(row));
  }
  if (!modelGroup.children.empty()) {
    QtDockAddTreeRow(panel, std::move(modelGroup));
  }

  QtDockTreeRow currentGroup;
  currentGroup.label = "Current Scene References (" + std::to_string(document.assets.size()) + ")";
  currentGroup.value = std::to_string(document.assets.size()) + " authored assets";
  currentGroup.icon = "folder";
  for (const auto& asset : document.assets) {
    QtDockTreeRow row;
    row.id = "asset.reference." + std::to_string(asset.id);
    row.label = asset.uri.empty() ? "Asset " + std::to_string(asset.id)
                                  : std::filesystem::path(asset.uri).filename().string();
    row.value = "#" + std::to_string(asset.id) + "  " + asset.type + "  " + asset.uri;
    row.icon = QtDockToLower(asset.type).find("model") != std::string::npos ? "model" : "asset";
    currentGroup.children.push_back(std::move(row));
  }
  for (const auto& texture : scene.textures) {
    QtDockTreeRow row;
    row.label = std::filesystem::path(texture).filename().string();
    row.value = texture;
    row.icon = "asset";
    currentGroup.children.push_back(std::move(row));
  }
  if (!currentGroup.children.empty()) {
    QtDockAddTreeRow(panel, std::move(currentGroup));
  }

  if (panel.tree_rows.empty()) {
    QtDockAddRow(panel, "No scenes or models found under assets/");
  }
  return panel;
}

QtDockPanelContent BuildQtTimelineDock(const vkpt::scene::SceneDocument& document,
                                       const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "timeline", "Timeline", false, 560.0f, 220.0f);
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddRow(panel, "Timeline playback disabled; scene transforms remain static.");
  return panel;
}

QtDockPanelContent BuildQtScriptDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockScriptRuntimeState* runtime) {
  auto panel = MakeQtDockPanel(layout, "script_panel", "Scripting", true, 560.0f, 460.0f);
  std::size_t scripted = 0u;
  for (const auto& entity : document.entities) {
    if (!entity.script.script.empty()) {
      ++scripted;
    }
  }
  QtDockAddToggleGroupedProperty(panel,
                                 "script.runtime.enabled",
                                 "Runtime",
                                 "Scripts enabled",
                                 runtime == nullptr || runtime->scripts_enabled);
  QtDockAddToggleGroupedProperty(panel,
                                 "script.runtime.playing",
                                 "Runtime",
                                 "Playing",
                                 runtime != nullptr && runtime->playing);
  QtDockAddButtonGroupedProperty(panel, "script.runtime.play", "Controls", "Play", "Play");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.pause", "Controls", "Pause", "Pause");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.step", "Controls", "Step update", "Step");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.reload", "Controls", "Reload bindings", "Reload");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_load", "Hooks", "on_load", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_fixed_update", "Hooks", "on_fixed_update", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_late_update", "Hooks", "on_late_update", "Fire");

  QtDockAddProperty(panel, "authored script entities", std::to_string(scripted));
  QtDockAddProperty(panel, "runtime status", runtime != nullptr ? runtime->status : "runtime unavailable");
  if (runtime != nullptr) {
    QtDockAddProperty(panel, "lua compiled", QtDockBool(runtime->binding_summary.lua_compiled_in));
    QtDockAddProperty(panel, "execution available", QtDockBool(runtime->binding_summary.execution_available));
    QtDockAddProperty(panel, "bindings", std::to_string(runtime->binding_summary.binding_count));
    QtDockAddProperty(panel, "runnable", std::to_string(runtime->binding_summary.runnable_count));
    QtDockAddProperty(panel, "disabled", std::to_string(runtime->binding_summary.disabled_count));
    QtDockAddProperty(panel,
                      "unsupported language",
                      std::to_string(runtime->binding_summary.unsupported_language_count));
    QtDockAddProperty(panel, "last hook", runtime->last_hook);
    QtDockAddProperty(panel, "last frame", std::to_string(runtime->last_frame));
    QtDockAddProperty(panel, "dispatches", std::to_string(runtime->dispatch_count));
    QtDockAddProperty(panel, "last hook calls", std::to_string(runtime->last_dispatch.hook_call_count));
    QtDockAddProperty(panel, "last skipped", std::to_string(runtime->last_dispatch.skipped_count));
    QtDockAddProperty(panel,
                      "last commands",
                      std::to_string(runtime->last_dispatch.command_count_before) + " -> " +
                          std::to_string(runtime->last_dispatch.command_count_after));
    std::size_t index = 0u;
    for (const auto& binding : runtime->bindings) {
      QtDockAddProperty(panel,
                        "binding " + std::to_string(++index),
                        (binding.enabled ? std::string("on ") : std::string("off ")) +
                            binding.entity_name + " #" + std::to_string(binding.entity) +
                            " " + binding.language + ":" + binding.entry +
                            " " + binding.source);
    }
    const std::size_t diagnostic_count = std::min<std::size_t>(runtime->diagnostics.size(), 6u);
    for (std::size_t i = 0; i < diagnostic_count; ++i) {
      const auto& diagnostic = runtime->diagnostics[runtime->diagnostics.size() - diagnostic_count + i];
      QtDockAddProperty(panel,
                        std::string("diagnostic ") + std::to_string(i + 1u),
                        std::string(vkpt::scripting::to_string(diagnostic.severity)) + " " +
                            std::string(vkpt::scripting::to_string(diagnostic.hook)) +
                            " #" + std::to_string(diagnostic.entity) +
                            " " + diagnostic.message);
    }
  }
  if (scripted == 0u) {
    QtDockAddProperty(panel, "bindings", "No scripts attached");
  }
  return panel;
}

QtDockPanelContent BuildQtPhysicsDock(const vkpt::scene::SceneDocument& document,
                                      const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "physics", "Physics", false, 420.0f, 320.0f);
  const auto engine = vkpt::physics::GetCompiledPhysicsEngineInfo();
  std::size_t authored = 0u;
  std::size_t enabled = 0u;
  std::size_t dynamic = 0u;
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddProperty(panel, "engine", engine.available ? engine.engine_name : std::string("disabled"));
  for (const auto& entity : document.entities) {
    if (entity.has_physics_body) {
      ++authored;
      if (entity.physics_body.enabled) {
        ++enabled;
        if (entity.physics_body.dynamic) {
          ++dynamic;
        }
      }
    }
    std::ostringstream row;
    row << QtEntityDisplayName(entity) << " #" << entity.id
        << " physics=" << QtDockBool(entity.has_physics_body && entity.physics_body.enabled);
    if (entity.has_physics_body) {
      row << " type=" << (entity.physics_body.dynamic ? "dynamic" : entity.physics_body.body_type)
          << " shape=" << entity.physics_body.shape
          << " mass=" << QtDockNumber(entity.physics_body.mass, 2);
      if (entity.physics_body.trigger) {
        row << " trigger";
      }
    }
    QtDockAddRow(panel, row.str());
  }
  QtDockAddProperty(panel, "authored bodies", std::to_string(authored));
  QtDockAddProperty(panel, "enabled bodies", std::to_string(enabled));
  QtDockAddProperty(panel, "dynamic bodies", std::to_string(dynamic));
  if (document.entities.empty()) {
    QtDockAddRow(panel, "No entities in the loaded document");
  }
  QtDockLimitRows(panel, 128u);
  return panel;
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
