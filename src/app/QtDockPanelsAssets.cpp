#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkpt::app {
namespace {

bool QtDockLooksBool(std::string_view value) {
  const auto lower = QtDockToLower(std::string(value));
  return lower == "true" || lower == "false" || lower == "1" || lower == "0";
}

bool QtDockLooksNumber(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  char* end = nullptr;
  const std::string text(value);
  (void)std::strtod(text.c_str(), &end);
  return end != text.c_str() && end != nullptr && *end == '\0';
}

std::optional<double> QtDockParseNumber(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const std::string text(value);
  const double parsed = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

bool QtDockLooksInteger(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  char* end = nullptr;
  const std::string text(value);
  (void)std::strtoll(text.c_str(), &end, 10);
  return end != text.c_str() && end != nullptr && *end == '\0';
}

bool QtDockLooksQuotedString(std::string_view value) {
  return value.size() >= 2u && value.front() == '"' && value.back() == '"';
}

bool QtDockLooksList(std::string_view value) {
  return value.size() >= 2u && value.front() == '{' && value.back() == '}';
}

std::string QtDockUnquoteString(std::string_view value) {
  if (!QtDockLooksQuotedString(value)) {
    return std::string(value);
  }
  return std::string(value.substr(1u, value.size() - 2u));
}

std::string QtDockFormatListValue(std::string_view value) {
  if (!QtDockLooksList(value)) {
    return std::string(value);
  }
  auto cleanItem = [](std::string item) {
    item = QtTrim(item);
    const auto equals = item.find('=');
    if (equals != std::string::npos) {
      const auto key = QtTrim(std::string_view(item).substr(0u, equals));
      const auto listIndex = !key.empty() &&
          std::all_of(key.begin(), key.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
          });
      if (listIndex) {
        return QtDockUnquoteString(QtTrim(std::string_view(item).substr(equals + 1u)));
      }
    }
    return QtDockUnquoteString(item);
  };
  std::string body(value.substr(1u, value.size() - 2u));
  std::string out;
  std::string current;
  bool inString = false;
  for (char ch : body) {
    if (ch == '"') {
      inString = !inString;
    }
    if (ch == ',' && !inString) {
      if (!current.empty()) {
        if (!out.empty()) {
          out.push_back('\n');
        }
        out += cleanItem(current);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) {
    if (!out.empty()) {
      out.push_back('\n');
    }
    out += cleanItem(current);
  }
  return out.empty() ? "{}" : out;
}

bool QtDockScriptValueWantsSlider(std::string_view name,
                                  std::string_view value) {
  if (!QtDockLooksNumber(value) || QtDockLooksInteger(value)) {
    return false;
  }
  const auto lower = QtDockToLower(std::string(name));
  return lower.find("min") != std::string::npos ||
         lower.find("max") != std::string::npos ||
         lower.find("range") != std::string::npos ||
         lower.find("volume") != std::string::npos ||
         lower.find("gain") != std::string::npos ||
         lower.find("sensitivity") != std::string::npos ||
         lower.find("pitch") != std::string::npos ||
         lower.find("yaw") != std::string::npos ||
         lower.find("distance") != std::string::npos ||
         lower.find("speed") != std::string::npos ||
         lower.find("height") != std::string::npos ||
         lower.find("radius") != std::string::npos ||
         lower.find("intensity") != std::string::npos ||
         lower.find("time") != std::string::npos ||
         lower.find("scale") != std::string::npos ||
         lower.find("fov") != std::string::npos;
}

struct QtDockNumericRange {
  double minimum = 0.0;
  double maximum = 1.0;
  double step = 0.01;
  double default_value = 0.0;
};

QtDockNumericRange QtDockScriptValueRange(std::string_view name, double value) {
  const auto lower = QtDockToLower(std::string(name));
  QtDockNumericRange range;
  range.default_value = value;
  if (lower.find("volume") != std::string::npos ||
      lower.find("gain") != std::string::npos ||
      (value >= 0.0 && value <= 1.0)) {
    range.minimum = 0.0;
    range.maximum = 1.0;
    range.step = 0.001;
  } else if (lower.find("pitch") != std::string::npos ||
             lower.find("yaw") != std::string::npos) {
    range.minimum = -3.142;
    range.maximum = 3.142;
    range.step = 0.001;
  } else if (lower.find("fov") != std::string::npos) {
    range.minimum = 1.0;
    range.maximum = 179.0;
    range.step = 0.1;
  } else if (lower.find("sensitivity") != std::string::npos) {
    range.minimum = 0.0;
    range.maximum = std::max(0.01, std::abs(value) * 4.0);
    range.step = 0.0001;
  } else {
    const double span = std::max(1.0, std::abs(value) * 2.0);
    range.minimum = value < 0.0 ? -span : 0.0;
    range.maximum = value < 0.0 ? span : span;
    range.step = span >= 100.0 ? 1.0 : 0.01;
  }
  return range;
}

void QtDockAddListGroupedProperty(QtDockPanelContent& panel,
                                  std::string id,
                                  std::string_view group,
                                  std::string_view label,
                                  std::string value,
                                  bool enabled) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "list";
  property.editable = true;
  property.enabled = enabled;
  panel.properties.push_back(std::move(property));
}

void QtDockAddTypedScriptValueProperty(QtDockPanelContent& panel,
                                       std::string id,
                                       std::string_view group,
                                       std::string_view label,
                                       std::string value,
                                       bool enabled,
                                       bool live_value = false) {
  if (QtDockLooksBool(value)) {
    QtDockAddToggleGroupedProperty(panel, std::move(id), group, label, QtDockToLower(value) == "true" || value == "1");
  } else if (QtDockLooksList(value)) {
    QtDockAddListGroupedProperty(panel, std::move(id), group, label, QtDockFormatListValue(value), enabled);
    return;
  } else if (const auto parsed = QtDockParseNumber(value)) {
    if (QtDockScriptValueWantsSlider(label, value)) {
      const auto range = QtDockScriptValueRange(label, *parsed);
      QtDockAddSliderGroupedProperty(panel,
                                     std::move(id),
                                     group,
                                     label,
                                     *parsed,
                                     range.minimum,
                                     range.maximum,
                                     range.step,
                                     range.default_value);
      if (live_value && !panel.properties.empty()) {
        panel.properties.back().has_default = false;
        panel.properties.back().default_value = 0.0;
      }
    } else {
      QtDockAddEditableGroupedProperty(panel, std::move(id), group, label, std::move(value));
      panel.properties.back().editor = "number";
    }
  } else {
    QtDockAddTextGroupedProperty(panel,
                                 std::move(id),
                                 group,
                                 label,
                                 QtDockUnquoteString(value));
  }
  if (!panel.properties.empty()) {
    panel.properties.back().enabled = enabled;
  }
}

void QtDockAddCommandProperty(QtDockPanelContent& panel,
                              std::string id,
                              std::string_view group,
                              std::string_view label,
                              std::string value,
                              bool enabled = true) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "button";
  property.editable = true;
  property.enabled = enabled;
  panel.properties.push_back(std::move(property));
}

void QtDockAddReadOnlyGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editable = false;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

std::string QtDockScriptRuntimeMode(const QtDockScriptRuntimeState* runtime) {
  if (runtime == nullptr) {
    return "unavailable";
  }
  if (!runtime->mode.empty()) {
    return runtime->mode;
  }
  return runtime->playing ? "play" : "edit";
}

bool QtDockScriptViewportInputForwarding(const QtDockScriptRuntimeState* runtime) {
  return runtime != nullptr &&
         (runtime->viewport_input_forwarding || runtime->playing);
}

bool QtDockScriptRuntimeScriptsRunning(const QtDockScriptRuntimeState* runtime) {
  const std::string mode = QtDockScriptRuntimeMode(runtime);
  return runtime != nullptr &&
         (runtime->playing || mode == "play" || mode == "live_edit" || mode == "live");
}

void QtDockAddScriptParamProperty(QtDockPanelContent& panel,
                                  vkpt::core::StableId entity_id,
                                  std::string_view name,
                                  std::string value,
                                  const vkpt::scripting::ScriptEditorParam* metadata = nullptr) {
  const std::string id = "entity." + std::to_string(entity_id) + ".script.param." + std::string(name);
  const std::string label = metadata != nullptr && !metadata->label.empty()
      ? metadata->label
      : std::string(name);
  if (metadata != nullptr) {
    const auto type = QtDockToLower(metadata->type);
    if (type == "bool") {
      QtDockAddToggleGroupedProperty(panel,
                                     id,
                                     "Params",
                                     label,
                                     QtDockToLower(value) == "true" || value == "1");
      return;
    }
    if (type == "number") {
      if (const auto parsed = QtDockParseNumber(value)) {
        if (metadata->has_minimum || metadata->has_maximum || metadata->has_step) {
          const double minimum = metadata->has_minimum ? metadata->minimum : std::min(0.0, *parsed);
          const double maximum = metadata->has_maximum ? metadata->maximum : std::max(1.0, *parsed);
          const double step = metadata->has_step ? metadata->step : 0.01;
          const double default_value =
              metadata->default_value.empty()
                  ? *parsed
                  : QtDockParseNumber(metadata->default_value).value_or(*parsed);
          QtDockAddSliderGroupedProperty(panel,
                                         id,
                                         "Params",
                                         label,
                                         *parsed,
                                         minimum,
                                         maximum,
                                         step,
                                         default_value);
        } else {
          QtDockAddEditableGroupedProperty(panel, id, "Params", label, std::move(value));
          panel.properties.back().editor = "number";
        }
        return;
      }
      QtDockAddEditableGroupedProperty(panel, id, "Params", label, std::move(value));
      panel.properties.back().editor = "number";
      return;
    }
  }
  QtDockAddTypedScriptValueProperty(panel, id, "Params", label, std::move(value), true);
}

std::string QtDockScriptParamMetadataKey(vkpt::core::StableId entity_id, std::string_view name) {
  return std::to_string(entity_id) + "|" + std::string(name);
}

void QtDockAddRuntimeVariableProperty(QtDockPanelContent& panel,
                                      const vkpt::scripting::ScriptVariableSnapshot& variable) {
  const std::string label =
      variable.name.empty() ? std::string("unnamed") : variable.name;
  const std::string id = "script.runtime.variable." + std::to_string(variable.entity) + "." +
                         variable.scope + "." + label;
  QtDockAddTypedScriptValueProperty(panel,
                                    id,
                                    "#" + std::to_string(variable.entity) + " " + variable.scope,
                                    label,
                                    variable.value,
                                    variable.editable,
                                    true);
}

}  // namespace

QtDockPanelContent BuildQtAssetBrowserDock(const vkpt::scene::SceneDocument& document,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           const vkpt::editor::UiRuntimeState& runtime,
                                           const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "asset_browser", "Asset Browser", true, 720.0f, 360.0f);
  panel.tree_single_column = true;
  panel.tree_stretch = 1;
  panel.property_stretch = 0;
  panel.property_preferred_height = 104;
  auto appendFiles = [](std::vector<std::filesystem::path>& target,
                        const std::vector<std::filesystem::path>& source) {
    target.insert(target.end(), source.begin(), source.end());
    std::sort(target.begin(), target.end(), [](const auto& lhs, const auto& rhs) {
      return QtDockPathString(lhs) < QtDockPathString(rhs);
    });
    target.erase(std::unique(target.begin(), target.end()), target.end());
  };
  std::vector<std::filesystem::path> sceneFiles =
      QtDockFindAssetFiles(QtDockFindRepoRelativePath("assets/scenes"), {".json"}, false);
  appendFiles(sceneFiles,
              QtDockFindAssetFiles(QtDockFindRepoRelativePath("game/scenes"), {".json"}, false));
  std::vector<std::filesystem::path> modelFiles =
      QtDockFindAssetFiles(QtDockFindRepoRelativePath("assets/models"), {".obj", ".gltf"}, true);
  appendFiles(modelFiles,
              QtDockFindAssetFiles(QtDockFindRepoRelativePath("game/models/lods"), {".gltf"}, true));

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
    QtDockAddRow(panel, "No scenes or models found under assets/ or game/");
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
                                     const vkpt::editor::SelectionState& selection,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockScriptRuntimeState* runtime) {
  auto panel = MakeQtDockPanel(layout, "script_panel", "Scripting", true, 560.0f, 460.0f);
  std::size_t scripted = 0u;
  const auto selectedEntity = QtPrimarySelectionId(selection);
  const auto* selectedScriptEntity = FindQtSceneEntity(document, selectedEntity);
  const bool selectedHasScript = selectedScriptEntity != nullptr &&
                                 !selectedScriptEntity->script.script.empty();
  for (const auto& entity : document.entities) {
    if (!entity.script.script.empty()) {
      ++scripted;
    }
  }
  QtDockAddToggleGroupedProperty(panel,
                                 "script.runtime.enabled",
                                 "Runtime",
                                 "Script execution",
                                 runtime == nullptr || runtime->scripts_enabled);
  QtDockAddToggleGroupedProperty(panel,
                                 "script.runtime.playing",
                                 "Runtime",
                                 "Game mode scripts (F1/F2)",
                                 QtDockScriptRuntimeScriptsRunning(runtime));
  const std::string runtimeMode = QtDockScriptRuntimeMode(runtime);
  const bool liveEditMode = runtimeMode == "live_edit" || runtimeMode == "live";
  const bool scriptsRunning = liveEditMode || runtimeMode == "play";
  QtDockAddCommandProperty(panel, "script.runtime.run_live", "Live/Play", "Run Live", "Run Live", !liveEditMode);
  QtDockAddCommandProperty(panel, "script.runtime.play", "Live/Play", "Play", "Play", runtimeMode != "play");
  QtDockAddCommandProperty(panel, "script.runtime.stop", "Live/Play", "Stop", "Stop", scriptsRunning);
  QtDockAddCommandProperty(panel,
                           "script.runtime.send_viewport_input",
                           "Live/Play",
                           "Lock Mouse / Input",
                           runtime != nullptr && runtime->viewport_input_forwarding ? "Unlock" : "Lock",
                           liveEditMode);
  QtDockAddButtonGroupedProperty(panel, "script.runtime.pause", "Controls", "Stop runtime", "Stop");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.step", "Controls", "Step update", "Step");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.reload", "Controls", "Reload bindings", "Reload");
  QtDockAddCommandProperty(panel,
                           "script.selection.attach",
                           "Controls",
                           "Attach to selection",
                           "Attach",
                           selectedScriptEntity != nullptr);
  QtDockAddCommandProperty(panel,
                           "script.selection.detach",
                           "Controls",
                           "Detach from selection",
                           "Detach",
                           selectedHasScript);
  QtDockAddCommandProperty(panel, "script.actions.new_lua_script", "Controls", "New Lua script", "New");
  QtDockAddCommandProperty(panel, "script.actions.open_folder", "Controls", "Open script folder", "Open");
  QtDockAddCommandProperty(panel,
                           "script.scene_init.create",
                           "Scene Init",
                           "Create scene init",
                           "Create",
                           true);
  QtDockAddCommandProperty(panel,
                           "script.scene_init.open",
                           "Scene Init",
                           "Open scene init",
                           "Open",
                           document.has_scene_script && !document.scene_script.script.empty());
  QtDockAddCommandProperty(panel,
                           "script.scene_init.bake_default_fps",
                           "Scene Init",
                           "Bake default FPS",
                           "Bake",
                           runtime != nullptr && runtime->bootstrap_fallback_injected);
  QtDockAddCommandProperty(panel,
                           "script.scene_init.disable_fallback",
                           "Scene Init",
                           "Disable fallback",
                           "Disable",
                           true);
  QtDockAddCommandProperty(panel,
                           "script.selection.open",
                           "Controls",
                           "Open selected script",
                           "Open",
                           selectedHasScript);
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_load", "Hooks", "on_load", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_spawn", "Hooks", "on_spawn", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_enable", "Hooks", "on_enable", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_update", "Hooks", "on_update", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_fixed_update", "Hooks", "on_fixed_update", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_late_update", "Hooks", "on_late_update", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_disable", "Hooks", "on_disable", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_destroy", "Hooks", "on_destroy", "Fire");
  QtDockAddButtonGroupedProperty(panel, "script.runtime.dispatch_on_unload", "Hooks", "on_unload", "Fire");

  QtDockAddProperty(panel, "authored script entities", std::to_string(scripted));
  QtDockAddReadOnlyGroupedProperty(panel,
                                   "script.runtime.mode",
                                   "Runtime",
                                   "runtime mode",
                                   runtimeMode);
  QtDockAddReadOnlyGroupedProperty(panel,
                                   "script.runtime.status",
                                   "Runtime",
                                   "runtime status",
                                   runtime != nullptr ? runtime->status : "runtime unavailable");
  QtDockAddReadOnlyGroupedProperty(panel,
                                   "script.runtime.viewport_input",
                                   "Runtime",
                                   "viewport input",
                                   QtDockBool(QtDockScriptViewportInputForwarding(runtime)));
  if (runtime != nullptr) {
    QtDockAddReadOnlyGroupedProperty(panel,
                                     "script.bootstrap.policy",
                                     "Bootstrap",
                                     "policy",
                                     runtime->bootstrap_policy);
    QtDockAddReadOnlyGroupedProperty(panel,
                                     "script.bootstrap.operation",
                                     "Bootstrap",
                                     "operation",
                                     runtime->bootstrap_operation);
    QtDockAddReadOnlyGroupedProperty(panel,
                                     "script.bootstrap.target",
                                     "Bootstrap",
                                     "target",
                                     runtime->bootstrap_target.empty() ? "none" : runtime->bootstrap_target);
    QtDockAddReadOnlyGroupedProperty(panel,
                                     "script.bootstrap.status",
                                     "Bootstrap",
                                     "status",
                                     runtime->bootstrap_status.empty() ? "not resolved" : runtime->bootstrap_status);
    QtDockAddReadOnlyGroupedProperty(panel,
                                     "script.bootstrap.scene_init",
                                     "Bootstrap",
                                     "scene init",
                                     QtDockBool(runtime->bootstrap_scene_init_injected));
    QtDockAddReadOnlyGroupedProperty(panel,
                                     "script.bootstrap.fallback",
                                     "Bootstrap",
                                     "fallback",
                                     QtDockBool(runtime->bootstrap_fallback_injected));
  }
  if (selectedEntity != 0u) {
    QtDockAddProperty(panel, "selected entity", std::to_string(selectedEntity));
  }
  const auto scriptFiles = QtDockFindAssetFiles(QtDockFindRepoRelativePath("assets/scripts"), {".lua"}, false);
  if (!scriptFiles.empty()) {
    QtDockTreeRow scriptGroup;
    scriptGroup.label = "Available Lua Scripts (" + std::to_string(scriptFiles.size()) + ")";
    scriptGroup.value = std::to_string(scriptFiles.size()) + " files";
    scriptGroup.icon = "folder";
    for (const auto& path : scriptFiles) {
      auto row = QtDockAssetFileRow("script.asset.",
                                    path,
                                    QtDockPrettyStem(path),
                                    "script",
                                    false);
      row.activatable = true;
      row.value = QtDockDisplayPath(path);
      scriptGroup.children.push_back(std::move(row));
    }
    QtDockAddTreeRow(panel, std::move(scriptGroup));
  }
  std::unordered_map<std::string, const vkpt::scripting::ScriptEditorParam*> editorParamMetadata;
  if (runtime != nullptr) {
    for (const auto& binding : runtime->bindings) {
      for (const auto& param : binding.editor_params) {
        editorParamMetadata[QtDockScriptParamMetadataKey(binding.entity, param.name)] = &param;
      }
    }
  }
  for (const auto& entity : document.entities) {
    if (entity.script.script.empty() && entity.script.params.empty()) {
      continue;
    }
    const std::string prefix = "entity." + std::to_string(entity.id) + ".script.";
    const std::string label = QtEntityDisplayName(entity) + " #" + std::to_string(entity.id);
    QtDockAddTextGroupedProperty(panel, prefix + "path", label, "Script path", entity.script.script);
    QtDockAddDropdownGroupedProperty(panel, prefix + "language", label, "Language", entity.script.language, {"lua"});
    QtDockAddTextGroupedProperty(panel, prefix + "entry", label, "Entry point", entity.script.entry);
    QtDockAddTextGroupedProperty(panel, prefix + "module_id", label, "Module ID", entity.script.module_id);
    QtDockAddToggleGroupedProperty(panel, prefix + "enabled", label, "Enabled", entity.script.enabled);
    QtDockAddToggleGroupedProperty(panel, prefix + "reload_on_save", label, "Reload on save", entity.script.reload_on_save);
    for (const auto& [name, value] : entity.script.params) {
      const auto metadata = editorParamMetadata.find(QtDockScriptParamMetadataKey(entity.id, name));
      QtDockAddScriptParamProperty(panel,
                                   entity.id,
                                   name,
                                   value,
                                   metadata == editorParamMetadata.end() ? nullptr : metadata->second);
    }
  }
  if (runtime != nullptr) {
    for (const auto& binding : runtime->bindings) {
      const auto* entity = FindQtSceneEntity(document, binding.entity);
      if (entity == nullptr) {
        continue;
      }
      for (const auto& param : binding.editor_params) {
        if (entity->script.params.find(param.name) != entity->script.params.end()) {
          continue;
        }
        const auto value = binding.params.find(param.name);
        QtDockAddScriptParamProperty(panel,
                                     binding.entity,
                                     param.name,
                                     value == binding.params.end() ? param.default_value : value->second,
                                     &param);
      }
    }
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
    QtDockAddProperty(panel, "audio backend", runtime->audio.backend_name);
    QtDockAddProperty(panel, "audio voices", std::to_string(runtime->audio.active_voices));
    QtDockAddButtonGroupedProperty(panel, "script.audio.play_scene", "Scene Audio", "Play scripted audio", "Play");
    QtDockAddButtonGroupedProperty(panel, "script.audio.pause_scene", "Scene Audio", "Pause scripted audio", "Pause");
    for (const auto& bus : runtime->audio.buses) {
      if (bus.name == "debug") {
        continue;
      }
      QtDockAddSliderGroupedProperty(panel,
                                     "script.audio.volume." + bus.name,
                                     "Scene Audio",
                                     bus.name + " volume",
                                     bus.volume,
                                     0.0,
                                     2.0,
                                     0.01,
                                     1.0);
      QtDockAddToggleGroupedProperty(panel,
                                     "script.audio.muted." + bus.name,
                                     "Scene Audio",
                                     bus.name + " muted",
                                     bus.muted);
    }
    std::size_t index = 0u;
    for (const auto& binding : runtime->bindings) {
      QtDockAddProperty(panel,
                        "binding " + std::to_string(++index),
                        (binding.enabled ? std::string("on ") : std::string("off ")) +
                            binding.entity_name + " #" + std::to_string(binding.entity) +
                            " " + binding.language + ":" + binding.entry +
                            " " + binding.source);
      if (!binding.params.empty()) {
        QtDockAddProperty(panel,
                          "params " + std::to_string(index),
                          std::to_string(binding.params.size()) + " authored");
      }
    }
    const std::size_t runtime_state_count = std::min<std::size_t>(runtime->runtime_states.size(), 8u);
    for (std::size_t i = 0; i < runtime_state_count; ++i) {
      const auto& state = runtime->runtime_states[i];
      std::string value = "#" + std::to_string(state.entity) +
                          " " + std::string(vkpt::scripting::to_string(state.last_hook)) +
                          " frame=" + std::to_string(state.last_frame) +
                          " cmds=" + std::to_string(state.command_count) +
                          " mem=" + std::to_string(state.memory_estimate_bytes);
      if (!state.skip_reason.empty()) {
        value += " skip=" + state.skip_reason;
      }
      if (!state.last_error.empty()) {
        value += " error=" + state.last_error;
      }
      QtDockAddProperty(panel, "runtime " + std::to_string(i + 1u), value);
    }
    const std::size_t variable_count = std::min<std::size_t>(runtime->variables.size(), 16u);
    if (variable_count > 0u) {
      QtDockAddProperty(panel, "variables", std::to_string(runtime->variables.size()) + " captured");
    }
    for (std::size_t i = 0; i < variable_count; ++i) {
      const auto& variable = runtime->variables[i];
      QtDockAddRuntimeVariableProperty(panel, variable);
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
