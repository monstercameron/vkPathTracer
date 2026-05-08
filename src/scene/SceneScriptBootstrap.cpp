#include "scene/SceneScriptBootstrap.h"

#include <concepts>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vkpt::scene {
namespace {

void add_diagnostic(SceneScriptBootstrapDecision& decision,
                    SceneScriptBootstrapDiagnosticSeverity severity,
                    std::string code,
                    std::string message) {
  decision.diagnostics.push_back(
      SceneScriptBootstrapDiagnostic{severity, std::move(code), std::move(message)});
}

template <typename T>
bool string_like_present(const T& value) {
  if constexpr (requires { { value.empty() } -> std::convertible_to<bool>; }) {
    return !value.empty();
  } else {
    return false;
  }
}

template <typename T>
bool scene_script_value_present(const T& value) {
  if constexpr (requires { { value.has_value() } -> std::convertible_to<bool>; *value; }) {
    return value.has_value() && scene_script_value_present(*value);
  } else if constexpr (requires { { value.empty() } -> std::convertible_to<bool>; }) {
    return !value.empty();
  } else {
    bool present = false;
    if constexpr (requires { value.script; }) {
      present = present || string_like_present(value.script);
    }
    if constexpr (requires { value.source; }) {
      present = present || string_like_present(value.source);
    }
    if constexpr (requires { value.path; }) {
      present = present || string_like_present(value.path);
    }
    if constexpr (requires { value.params; }) {
      present = present || !value.params.empty();
    }
    return present;
  }
}

template <typename Document>
bool scene_script_field_supported(const Document& document) {
  if constexpr (requires { document.scene_script; }) {
    return true;
  } else {
    return false;
  }
}

template <typename Document>
bool authored_scene_init_present(const Document& document) {
  if constexpr (requires {
                  { document.has_scene_script } -> std::convertible_to<bool>;
                  document.scene_script;
                }) {
    return document.has_scene_script ||
        scene_script_value_present(document.scene_script);
  } else if constexpr (requires { { document.has_scene_script } -> std::convertible_to<bool>; }) {
    return document.has_scene_script;
  } else if constexpr (requires { document.scene_script; }) {
    return scene_script_value_present(document.scene_script);
  } else {
    return false;
  }
}

bool has_entity_script_source(const SceneEntityDefinition& entity) {
  return !entity.script.script.empty();
}

bool has_transform_definition(const SceneDocument& document,
                              vkpt::core::StableId entity_id) {
  if (entity_id == 0) {
    return false;
  }
  for (const auto& entity : document.entities) {
    if (entity.id == entity_id) {
      if (entity.has_transform) {
        return true;
      }
      break;
    }
  }
  for (const auto& transform : document.transforms) {
    if (transform.id == entity_id) {
      return true;
    }
  }
  return false;
}

std::unordered_map<vkpt::core::StableId, const SceneEntityDefinition*>
build_entity_index(const SceneDocument& document) {
  std::unordered_map<vkpt::core::StableId, const SceneEntityDefinition*> entities_by_id;
  entities_by_id.reserve(document.entities.size());
  for (const auto& entity : document.entities) {
    if (entity.id != 0) {
      entities_by_id[entity.id] = &entity;
    }
  }
  return entities_by_id;
}

bool entity_visible_path(
    const SceneDocument& document,
    const SceneEntityDefinition& entity,
    const std::unordered_map<vkpt::core::StableId, const SceneEntityDefinition*>&
        entities_by_id) {
  const auto* current = &entity;
  std::unordered_set<vkpt::core::StableId> visited;
  visited.reserve(8u);
  for (std::size_t depth = 0u;
       current != nullptr && depth <= document.entities.size() &&
       visited.insert(current->id).second;
       ++depth) {
    if (!current->visible) {
      return false;
    }
    const auto parent = current->hierarchy.parent;
    if (parent == 0) {
      break;
    }
    const auto parent_it = entities_by_id.find(parent);
    current = parent_it == entities_by_id.end() ? nullptr : parent_it->second;
  }
  return true;
}

std::optional<SceneScriptBootstrapCameraSelection> select_main_camera(
    const SceneDocument& document,
    SceneScriptBootstrapDecision& decision) {
  const auto entities_by_id = build_entity_index(document);

  std::size_t camera_definition_count = 0;
  std::size_t hidden_entity_camera_count = 0;

  for (std::size_t index = 0; index < document.entities.size(); ++index) {
    const auto& entity = document.entities[index];
    if (!entity.has_camera) {
      continue;
    }
    ++camera_definition_count;
    if (entity.id == 0) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Warning,
                     "camera.invalid_id",
                     "Skipped an entity camera with stable id 0.");
      continue;
    }
    if (!entity_visible_path(document, entity, entities_by_id)) {
      ++hidden_entity_camera_count;
      continue;
    }
    SceneScriptBootstrapCameraSelection selection;
    selection.entity_id = entity.id;
    selection.entity_name = entity.name;
    selection.from_entity_camera = true;
    selection.has_authored_transform = has_transform_definition(document, entity.id);
    selection.document_order = index;
    if (hidden_entity_camera_count > 0) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Info,
                     "camera.hidden_skipped",
                     "Skipped " + std::to_string(hidden_entity_camera_count) +
                         " hidden entity camera definition(s).");
    }
    if (camera_definition_count + document.cameras.size() > 1) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Info,
                     "camera.multiple",
                     "Selected the first visible entity camera in document order.");
    }
    return selection;
  }

  for (std::size_t index = 0; index < document.cameras.size(); ++index) {
    const auto& camera = document.cameras[index];
    ++camera_definition_count;
    if (camera.id == 0) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Warning,
                     "camera.invalid_id",
                     "Skipped a legacy camera entry with stable id 0.");
      continue;
    }
    SceneScriptBootstrapCameraSelection selection;
    selection.entity_id = camera.id;
    if (const auto entity_it = entities_by_id.find(camera.id);
        entity_it != entities_by_id.end() && entity_it->second != nullptr) {
      selection.entity_name = entity_it->second->name;
    }
    selection.from_legacy_camera_section = true;
    selection.has_authored_transform = has_transform_definition(document, camera.id);
    selection.document_order = index;
    if (hidden_entity_camera_count > 0) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Info,
                     "camera.hidden_skipped",
                     "Skipped " + std::to_string(hidden_entity_camera_count) +
                         " hidden entity camera definition(s).");
    }
    if (camera_definition_count > 1) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Info,
                     "camera.multiple",
                     "Selected the first legacy camera entry because no visible "
                     "entity camera was available.");
    }
    return selection;
  }

  if (hidden_entity_camera_count > 0) {
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "camera.hidden_skipped",
                   "No visible main camera was selected because all entity camera "
                   "definitions are hidden.");
  }
  return std::nullopt;
}

std::string camera_label(const SceneScriptBootstrapCameraSelection& camera) {
  std::ostringstream out;
  out << "entity " << camera.entity_id;
  if (!camera.entity_name.empty()) {
    out << " (" << camera.entity_name << ")";
  }
  return out.str();
}

}  // namespace

std::string_view to_string(SceneScriptBootstrapPolicy policy) {
  switch (policy) {
    case SceneScriptBootstrapPolicy::AuthoredSceneInit:
      return "authored_scene_init";
    case SceneScriptBootstrapPolicy::EntityScripts:
      return "entity_scripts";
    case SceneScriptBootstrapPolicy::ScriptlessWithCamera:
      return "scriptless_with_camera";
    case SceneScriptBootstrapPolicy::ScriptlessWithoutCamera:
      return "scriptless_without_camera";
    default:
      return "scriptless_without_camera";
  }
}

std::string_view to_string(SceneScriptBootstrapFallbackOperation operation) {
  switch (operation) {
    case SceneScriptBootstrapFallbackOperation::None:
      return "none";
    case SceneScriptBootstrapFallbackOperation::AttachDefaultFpsToMainCamera:
      return "attach_default_fps_to_main_camera";
    case SceneScriptBootstrapFallbackOperation::SpawnDefaultFpsCamera:
      return "spawn_default_fps_camera";
    default:
      return "none";
  }
}

SceneScriptBootstrapDecision DecideSceneScriptBootstrap(
    const SceneDocument& document,
    const SceneScriptBootstrapOptions& options) {
  SceneScriptBootstrapDecision decision;
  decision.authored_scene_init_supported = scene_script_field_supported(document);
  decision.authored_scene_init_present = authored_scene_init_present(document);
  decision.main_camera = select_main_camera(document, decision);

  if (!decision.authored_scene_init_supported) {
    add_diagnostic(
        decision,
        SceneScriptBootstrapDiagnosticSeverity::Info,
        "scene_script.unsupported",
        "SceneDocument has no scene_script field yet; authored scene init "
        "detection will begin using it when that schema field lands.");
  }

  for (const auto& entity : document.entities) {
    if (has_entity_script_source(entity)) {
      ++decision.entity_script_count;
      if (entity.script.enabled) {
        ++decision.enabled_entity_script_count;
      }
    } else if (!entity.script.params.empty()) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Warning,
                     "script.params_without_source",
                     "Entity " + std::to_string(entity.id) +
                         " has script params but no script source; it is treated "
                         "as scriptless for bootstrap fallback.");
    }
  }

  if (decision.authored_scene_init_present) {
    decision.policy = SceneScriptBootstrapPolicy::AuthoredSceneInit;
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "policy.authored_scene_init",
                   "Authored scene init is present; runtime fallback injection is "
                   "not required.");
  } else if (decision.entity_script_count > 0) {
    decision.policy = SceneScriptBootstrapPolicy::EntityScripts;
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "policy.entity_scripts",
                   "Entity scripts are authored; runtime fallback injection is "
                   "not required.");
    if (decision.enabled_entity_script_count == 0) {
      add_diagnostic(decision,
                     SceneScriptBootstrapDiagnosticSeverity::Warning,
                     "script.none_enabled",
                     "Entity scripts are authored, but none are enabled.");
    }
  } else if (decision.main_camera.has_value()) {
    decision.policy = SceneScriptBootstrapPolicy::ScriptlessWithCamera;
    decision.fallback.operation =
        SceneScriptBootstrapFallbackOperation::AttachDefaultFpsToMainCamera;
    decision.fallback.target_entity_id = decision.main_camera->entity_id;
    decision.fallback.target_entity_name = decision.main_camera->entity_name;
    decision.fallback.script_source = options.default_fps_script;
    decision.fallback.description =
        "Runtime-only bootstrap should attach the default FPS script to " +
        camera_label(*decision.main_camera) +
        " for Game/Live Edit without writing the script component back to the "
        "source scene JSON.";
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "policy.scriptless_with_camera",
                   decision.fallback.description);
  } else {
    decision.policy = SceneScriptBootstrapPolicy::ScriptlessWithoutCamera;
    decision.fallback.operation =
        SceneScriptBootstrapFallbackOperation::SpawnDefaultFpsCamera;
    decision.fallback.target_entity_name = options.runtime_camera_name;
    decision.fallback.script_source = options.default_fps_script;
    decision.fallback.description =
        "Runtime-only bootstrap should spawn a transient FPS camera entity named '" +
        options.runtime_camera_name +
        "' with the default FPS script for Game/Live Edit without writing the "
        "camera or script component back to the source scene JSON.";
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "policy.scriptless_without_camera",
                   decision.fallback.description);
  }

  if (decision.fallback.operation != SceneScriptBootstrapFallbackOperation::None &&
      decision.fallback.script_source.empty()) {
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Warning,
                   "fallback.empty_script",
                   "Default FPS fallback was selected, but the configured script "
                   "source is empty.");
  }

  return decision;
}

std::string DescribeRuntimeFallbackInjection(
    const SceneScriptBootstrapDecision& decision) {
  if (decision.fallback.operation == SceneScriptBootstrapFallbackOperation::None) {
    return "No runtime fallback injection is required.";
  }
  return decision.fallback.description;
}

}  // namespace vkpt::scene
