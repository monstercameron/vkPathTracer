#include "scene/SceneScriptBootstrap.h"

#include <iostream>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "scene script bootstrap smoke failed: " << message << "\n";
    return false;
  }
  return true;
}

bool HasDiagnostic(const vkpt::scene::SceneScriptBootstrapDecision& decision,
                   std::string_view code) {
  for (const auto& diagnostic : decision.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

bool PathExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

std::filesystem::path FindRepoPath(const std::filesystem::path& relative_path) {
  auto current = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const auto candidate = current / relative_path;
    if (PathExists(candidate)) {
      return candidate;
    }
    if (!current.has_parent_path() || current.parent_path() == current) {
      break;
    }
    current = current.parent_path();
  }
  return relative_path;
}

std::vector<std::filesystem::path> SceneJsonFiles(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> out;
  if (!PathExists(root)) {
    return out;
  }
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      out.push_back(entry.path());
    }
  }
  return out;
}

vkpt::scene::SceneEntityDefinition MakeCameraEntity(vkpt::core::StableId id,
                                                    std::string name,
                                                    bool visible = true) {
  vkpt::scene::SceneEntityDefinition entity;
  entity.id = id;
  entity.name = std::move(name);
  entity.visible = visible;
  entity.has_camera = true;
  entity.has_transform = true;
  entity.transform.translation = {0.0f, 1.7f, 4.0f};
  return entity;
}

}  // namespace

int main() {
  {
    vkpt::scene::SceneDocument empty_document;
    const auto decision = vkpt::scene::DecideSceneScriptBootstrap(empty_document);
    if (!Check(decision.policy ==
                   vkpt::scene::SceneScriptBootstrapPolicy::ScriptlessWithoutCamera,
               "empty documents should spawn a runtime-only fallback camera") ||
        !Check(decision.fallback.operation ==
                   vkpt::scene::SceneScriptBootstrapFallbackOperation::
                       SpawnDefaultFpsCamera,
               "empty documents should plan a runtime-only camera spawn") ||
        !Check(decision.fallback.runtime_only &&
                   !decision.fallback.mutates_source_document,
               "fallback plans must not mutate source JSON") ||
        !Check(decision.authored_scene_init_supported &&
                   !decision.authored_scene_init_present,
               "SceneDocument scene_script support should be available but absent "
               "on empty documents")) {
      return 1;
    }
  }

  {
    vkpt::scene::SceneDocument scene_init_document;
    scene_init_document.scene_script.script = "assets/scripts/scene_bootstrap.lua";

    const auto decision =
        vkpt::scene::DecideSceneScriptBootstrap(scene_init_document);
    if (!Check(decision.policy ==
                   vkpt::scene::SceneScriptBootstrapPolicy::AuthoredSceneInit,
               "authored scene_script presence should take bootstrap priority") ||
        !Check(decision.authored_scene_init_present,
               "scene_script presence should be reported") ||
        !Check(decision.fallback.operation ==
                   vkpt::scene::SceneScriptBootstrapFallbackOperation::None,
               "authored scene init should suppress default fallback")) {
      return 1;
    }

    const auto runtime_world =
        vkpt::scene::BuildSceneScriptRuntimeWorld(scene_init_document);
    if (!Check(static_cast<bool>(runtime_world),
               "authored scene init should build a runtime world") ||
        !Check(runtime_world.value().scene_init_injected,
               "authored scene init should be injected as a runtime-only binding") ||
        !Check(!runtime_world.value().fallback_injected,
               "authored scene init should not also inject fallback") ||
        !Check(scene_init_document.entities.empty(),
               "scene init runtime injection must not mutate source document")) {
      return 1;
    }
    const auto& init_world = runtime_world.value().world;
    bool found_init_binding = false;
    for (const auto entity_id : init_world.all_entities()) {
      const auto* entity = init_world.get_entity(entity_id);
      found_init_binding = found_init_binding ||
          (entity != nullptr && entity->script.has_value() &&
           entity->script->script == "assets/scripts/scene_bootstrap.lua" &&
           entity->identity.name == "Scene Init Script");
    }
    if (!Check(found_init_binding,
               "scene init runtime world should expose one synthetic binding")) {
      return 1;
    }
  }

  {
    vkpt::scene::SceneDocument camera_document;
    camera_document.entities.push_back(MakeCameraEntity(100, "Hidden Camera", false));
    camera_document.entities.push_back(MakeCameraEntity(300, "Game Camera"));
    vkpt::scene::SceneCameraDefinition legacy_camera;
    legacy_camera.id = 200;
    camera_document.cameras.push_back(legacy_camera);

    const auto decision = vkpt::scene::DecideSceneScriptBootstrap(camera_document);
    if (!Check(decision.policy ==
                   vkpt::scene::SceneScriptBootstrapPolicy::ScriptlessWithCamera,
               "scriptless documents with a visible camera should attach fallback "
               "to that camera") ||
        !Check(decision.main_camera.has_value() &&
                   decision.main_camera->entity_id == 300,
               "main camera should use the first visible entity camera in document "
               "order") ||
        !Check(decision.fallback.operation ==
                   vkpt::scene::SceneScriptBootstrapFallbackOperation::
                       AttachDefaultFpsToMainCamera,
               "camera documents should attach the default FPS script at runtime") ||
        !Check(decision.fallback.target_entity_id == 300,
               "fallback target should be the selected main camera") ||
        !Check(HasDiagnostic(decision, "camera.hidden_skipped"),
               "hidden entity cameras should be diagnosed")) {
      return 1;
    }

    const auto runtime_world =
        vkpt::scene::BuildSceneScriptRuntimeWorld(camera_document);
    if (!Check(static_cast<bool>(runtime_world),
               "camera fallback should build a runtime world") ||
        !Check(runtime_world.value().fallback_injected,
               "camera fallback should be injected in the runtime world") ||
        !Check(camera_document.entities[1].script.script.empty(),
               "camera fallback must not mutate source scene JSON data")) {
      return 1;
    }
    const auto* fallback_entity = runtime_world.value().world.get_entity(300u);
    if (!Check(fallback_entity != nullptr &&
                   fallback_entity->script.has_value() &&
                   fallback_entity->script->script ==
                       "assets/scripts/systems/generic_fps_camera.lua",
               "camera fallback should attach the generic FPS system to the "
               "selected camera binding")) {
      return 1;
    }
  }

  {
    vkpt::scene::SceneDocument legacy_camera_document;
    vkpt::scene::SceneEntityDefinition camera_owner;
    camera_owner.id = 42;
    camera_owner.name = "Legacy Camera Owner";
    legacy_camera_document.entities.push_back(camera_owner);
    vkpt::scene::SceneCameraDefinition legacy_camera;
    legacy_camera.id = 42;
    legacy_camera_document.cameras.push_back(legacy_camera);

    const auto decision =
        vkpt::scene::DecideSceneScriptBootstrap(legacy_camera_document);
    if (!Check(decision.policy ==
                   vkpt::scene::SceneScriptBootstrapPolicy::ScriptlessWithCamera,
               "legacy camera sections should count as camera authoring") ||
        !Check(decision.main_camera.has_value() &&
                   decision.main_camera->entity_id == 42 &&
                   decision.main_camera->from_legacy_camera_section,
               "legacy camera fallback should select the top-level camera id")) {
      return 1;
    }
  }

  {
    vkpt::scene::SceneDocument scripted_document;
    scripted_document.entities.push_back(MakeCameraEntity(77, "Scripted Camera"));
    scripted_document.entities.back().script.script = "assets/scripts/play.lua";
    scripted_document.entities.back().script.enabled = true;

    const auto decision = vkpt::scene::DecideSceneScriptBootstrap(scripted_document);
    if (!Check(decision.policy ==
                   vkpt::scene::SceneScriptBootstrapPolicy::EntityScripts,
               "entity script sources should suppress default fallback") ||
        !Check(decision.entity_script_count == 1u &&
                   decision.enabled_entity_script_count == 1u,
               "entity script counts should include enabled script sources") ||
        !Check(decision.fallback.operation ==
                   vkpt::scene::SceneScriptBootstrapFallbackOperation::None,
               "scripted documents should not get fallback injection")) {
      return 1;
    }
  }

  {
    vkpt::scene::SceneDocument params_only_document;
    vkpt::scene::SceneEntityDefinition entity;
    entity.id = 500;
    entity.script.params["speed"] = "3.5";
    params_only_document.entities.push_back(entity);

    const auto decision =
        vkpt::scene::DecideSceneScriptBootstrap(params_only_document);
    if (!Check(decision.policy ==
                   vkpt::scene::SceneScriptBootstrapPolicy::ScriptlessWithoutCamera,
               "script params without a source should not suppress fallback") ||
        !Check(HasDiagnostic(decision, "script.params_without_source"),
               "params-only script data should be diagnosed")) {
      return 1;
    }

    const auto runtime_world =
        vkpt::scene::BuildSceneScriptRuntimeWorld(params_only_document);
    if (!Check(static_cast<bool>(runtime_world),
               "scriptless no-camera fallback should build a runtime world") ||
        !Check(runtime_world.value().fallback_injected,
               "scriptless no-camera fallback should inject a transient camera")) {
      return 1;
    }
    const auto& spawned_world = runtime_world.value().world;
    bool found_spawned_camera = false;
    for (const auto entity_id : spawned_world.all_entities()) {
      const auto* entity = spawned_world.get_entity(entity_id);
      found_spawned_camera = found_spawned_camera ||
          (entity != nullptr && entity->identity.name == "Runtime FPS Camera" &&
           entity->camera.has_value() && entity->script.has_value());
    }
    if (!Check(found_spawned_camera,
               "scriptless no-camera fallback should create one runtime FPS "
               "camera binding")) {
      return 1;
    }
  }

  {
    std::vector<std::filesystem::path> scene_files = SceneJsonFiles(FindRepoPath("assets/scenes"));
    const auto game_scene_files = SceneJsonFiles(FindRepoPath("game/scenes"));
    scene_files.insert(scene_files.end(), game_scene_files.begin(), game_scene_files.end());
    std::size_t loaded_count = 0u;
    std::size_t authored_init_count = 0u;
    std::size_t fallback_count = 0u;
    for (const auto& scene_path : scene_files) {
      auto document = vkpt::scene::SceneDocument::load_from_file(scene_path.string());
      if (!Check(static_cast<bool>(document),
                 "bootstrap matrix scene should load: " + scene_path.generic_string())) {
        return 1;
      }
      auto runtime_world = vkpt::scene::BuildSceneScriptRuntimeWorld(document.value());
      if (!Check(static_cast<bool>(runtime_world),
                 "bootstrap matrix scene should build runtime world: " +
                     scene_path.generic_string())) {
        return 1;
      }
      ++loaded_count;
      authored_init_count += runtime_world.value().scene_init_injected ? 1u : 0u;
      fallback_count += runtime_world.value().fallback_injected ? 1u : 0u;
    }
    if (!Check(loaded_count > 0u,
               "bootstrap matrix should discover checked-in scenes") ||
        !Check(fallback_count > 0u,
               "bootstrap matrix should exercise default fallback scenes")) {
      return 1;
    }
    (void)authored_init_count;
  }

  std::cout << "scene script bootstrap smoke: ok\n";
  return 0;
}
