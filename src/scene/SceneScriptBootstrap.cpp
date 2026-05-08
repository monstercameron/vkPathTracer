#include "scene/SceneScriptBootstrap.h"

#include <algorithm>
#include <cctype>
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

bool has_scene_script_source(const SceneDocument& document) {
  return !document.scene_script.script.empty() && document.scene_script.enabled;
}

vkpt::core::StableId allocate_runtime_entity_id(const SceneDocument& document) {
  vkpt::core::StableId next = 9'000'000'000ull;
  auto collides = [&](vkpt::core::StableId id) {
    for (const auto& entity : document.entities) {
      if (entity.id == id) {
        return true;
      }
    }
    for (const auto& camera : document.cameras) {
      if (camera.id == id) {
        return true;
      }
    }
    for (const auto& light : document.lights) {
      if (light.id == id) {
        return true;
      }
    }
    for (const auto& transform : document.transforms) {
      if (transform.id == id) {
        return true;
      }
    }
    return false;
  };
  while (next != 0 && collides(next)) {
    ++next;
  }
  return next;
}

SceneEntityDefinition* find_entity(SceneDocument& document,
                                   vkpt::core::StableId entity_id) {
  for (auto& entity : document.entities) {
    if (entity.id == entity_id) {
      return &entity;
    }
  }
  return nullptr;
}

const SceneCameraDefinition* find_legacy_camera(const SceneDocument& document,
                                                vkpt::core::StableId entity_id) {
  for (const auto& camera : document.cameras) {
    if (camera.id == entity_id) {
      return &camera;
    }
  }
  return nullptr;
}

const SceneTransformEntry* find_legacy_transform(const SceneDocument& document,
                                                 vkpt::core::StableId entity_id) {
  for (const auto& transform : document.transforms) {
    if (transform.id == entity_id) {
      return &transform;
    }
  }
  return nullptr;
}

void ensure_legacy_component_entities(SceneDocument& document) {
  auto ensure_entity = [&](vkpt::core::StableId id, std::string_view name) -> SceneEntityDefinition& {
    if (auto* existing = find_entity(document, id); existing != nullptr) {
      return *existing;
    }
    SceneEntityDefinition entity;
    entity.id = id;
    entity.name = std::string(name) + " " + std::to_string(id);
    document.entities.push_back(std::move(entity));
    return document.entities.back();
  };

  for (const auto& transform : document.transforms) {
    if (transform.id != 0) {
      auto& entity = ensure_entity(transform.id, "Legacy Transform Entity");
      entity.has_transform = true;
      entity.transform = transform.transform;
    }
  }
  for (const auto& camera : document.cameras) {
    if (camera.id != 0) {
      auto& entity = ensure_entity(camera.id, "Legacy Camera Entity");
      entity.has_camera = true;
      entity.camera = camera.camera;
    }
  }
  for (const auto& light : document.lights) {
    if (light.id != 0) {
      auto& entity = ensure_entity(light.id, "Legacy Light Entity");
      entity.has_light = true;
      entity.light = light.light;
    }
  }
}

ScriptComponent default_fps_script_component(const SceneScriptBootstrapOptions& options) {
  ScriptComponent script;
  script.script = options.default_fps_script;
  script.language = "lua";
  script.entry = "default";
  script.module_id = options.default_fps_module_id;
  script.enabled = true;
  script.reload_on_save = true;
  script.params["runtime_only"] = "true";
  script.params["bootstrap"] = "default_fps";
  return script;
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
  auto preferred_camera_name = [](std::string_view name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const unsigned char ch : name) {
      if (std::isalnum(ch) != 0) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
      } else if (!normalized.empty() && normalized.back() != '_') {
        normalized.push_back('_');
      }
    }
    while (!normalized.empty() && normalized.back() == '_') {
      normalized.pop_back();
    }
    return normalized == "main_camera" || normalized == "player_camera";
  };

  auto make_entity_camera_selection =
      [&](const SceneEntityDefinition& entity, std::size_t index) {
    SceneScriptBootstrapCameraSelection selection;
    selection.entity_id = entity.id;
    selection.entity_name = entity.name;
    selection.from_entity_camera = true;
    selection.has_authored_transform = has_transform_definition(document, entity.id);
    selection.document_order = index;
    return selection;
  };

  for (std::size_t index = 0; index < document.entities.size(); ++index) {
    const auto& entity = document.entities[index];
    if (!entity.has_camera || entity.id == 0 ||
        !preferred_camera_name(entity.name) ||
        !entity_visible_path(document, entity, entities_by_id)) {
      continue;
    }
    add_diagnostic(decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "camera.named_main",
                   "Selected a named gameplay camera before document-order "
                   "fallback.");
    return make_entity_camera_selection(entity, index);
  }

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
    SceneScriptBootstrapCameraSelection selection = make_entity_camera_selection(entity, index);
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

vkpt::core::Result<SceneScriptRuntimeWorld> BuildSceneScriptRuntimeWorld(
    const SceneDocument& document,
    const SceneScriptBootstrapOptions& options) {
  auto runtime_document = document;
  SceneScriptRuntimeWorld result;
  result.decision = DecideSceneScriptBootstrap(document, options);
  ensure_legacy_component_entities(runtime_document);

  if (has_scene_script_source(document)) {
    SceneEntityDefinition scene_init_entity;
    scene_init_entity.id = allocate_runtime_entity_id(runtime_document);
    scene_init_entity.name = options.scene_init_entity_name;
    scene_init_entity.visible = false;
    scene_init_entity.script = document.scene_script;
    scene_init_entity.script.params.emplace("runtime_only", "true");
    scene_init_entity.script.params.emplace("bootstrap", "scene_script");
    runtime_document.entities.insert(runtime_document.entities.begin(),
                                     std::move(scene_init_entity));
    result.scene_init_injected = true;
  } else if (document.has_scene_script && !document.scene_script.script.empty() &&
             !document.scene_script.enabled) {
    add_diagnostic(result.decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "scene_script.disabled",
                   "Scene init script is authored but disabled; it was not "
                   "injected into the runtime world.");
  }

  if (options.enable_default_fps_fallback &&
      result.decision.fallback.operation ==
          SceneScriptBootstrapFallbackOperation::AttachDefaultFpsToMainCamera) {
    auto* entity = find_entity(runtime_document, result.decision.fallback.target_entity_id);
    if (entity == nullptr) {
      SceneEntityDefinition camera_owner;
      camera_owner.id = result.decision.fallback.target_entity_id;
      camera_owner.name = result.decision.fallback.target_entity_name.empty()
          ? "Runtime Main Camera"
          : result.decision.fallback.target_entity_name;
      camera_owner.has_transform = true;
      if (const auto* legacy_transform =
              find_legacy_transform(runtime_document, camera_owner.id)) {
        camera_owner.transform = legacy_transform->transform;
      }
      if (const auto* legacy_camera = find_legacy_camera(runtime_document, camera_owner.id)) {
        camera_owner.has_camera = true;
        camera_owner.camera = legacy_camera->camera;
      }
      runtime_document.entities.push_back(std::move(camera_owner));
      entity = &runtime_document.entities.back();
    }
    entity->script = default_fps_script_component(options);
    result.fallback_injected = true;
  } else if (options.enable_default_fps_fallback &&
             result.decision.fallback.operation ==
                 SceneScriptBootstrapFallbackOperation::SpawnDefaultFpsCamera) {
    SceneEntityDefinition camera_entity;
    camera_entity.id = allocate_runtime_entity_id(runtime_document);
    camera_entity.name = options.runtime_camera_name;
    camera_entity.has_transform = true;
    camera_entity.transform.translation = {0.0f, 1.7f, 4.0f};
    camera_entity.has_camera = true;
    camera_entity.camera.fov = 65.0f;
    camera_entity.camera.focus_distance = 4.0f;
    camera_entity.script = default_fps_script_component(options);
    result.decision.fallback.target_entity_id = camera_entity.id;
    result.decision.fallback.target_entity_name = camera_entity.name;
    runtime_document.entities.push_back(std::move(camera_entity));
    result.fallback_injected = true;
  } else if (!options.enable_default_fps_fallback &&
             result.decision.fallback.operation !=
                 SceneScriptBootstrapFallbackOperation::None) {
    add_diagnostic(result.decision,
                   SceneScriptBootstrapDiagnosticSeverity::Info,
                   "fallback.disabled_for_mode",
                   "Default FPS fallback was resolved but not injected because "
                   "the current mode does not run gameplay scripts.");
  }

  auto world = runtime_document.to_world();
  if (!world) {
    return vkpt::core::Result<SceneScriptRuntimeWorld>::error(world.error());
  }
  result.world = std::move(world.value());
  result.mutates_source_document = false;
  return vkpt::core::Result<SceneScriptRuntimeWorld>::ok(std::move(result));
}

}  // namespace vkpt::scene
