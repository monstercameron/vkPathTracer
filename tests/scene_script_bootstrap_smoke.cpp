#include "scene/SceneScriptBootstrap.h"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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
  }

  std::cout << "scene script bootstrap smoke: ok\n";
  return 0;
}
