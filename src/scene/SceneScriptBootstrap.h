#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"
#include "scene/SceneDocument.h"

namespace vkpt::scene {

enum class SceneScriptBootstrapPolicy : std::uint8_t {
  AuthoredSceneInit,
  EntityScripts,
  ScriptlessWithCamera,
  ScriptlessWithoutCamera
};

enum class SceneScriptBootstrapDiagnosticSeverity : std::uint8_t {
  Info,
  Warning
};

enum class SceneScriptBootstrapFallbackOperation : std::uint8_t {
  None,
  AttachDefaultFpsToMainCamera,
  SpawnDefaultFpsCamera
};

struct SceneScriptBootstrapOptions {
  std::string default_fps_script = "assets/scripts/generic_fps_camera.lua";
  std::string runtime_camera_name = "Runtime FPS Camera";
};

struct SceneScriptBootstrapDiagnostic {
  SceneScriptBootstrapDiagnosticSeverity severity =
      SceneScriptBootstrapDiagnosticSeverity::Info;
  std::string code;
  std::string message;
};

struct SceneScriptBootstrapCameraSelection {
  vkpt::core::StableId entity_id = 0;
  std::string entity_name;
  bool from_entity_camera = false;
  bool from_legacy_camera_section = false;
  bool has_authored_transform = false;
  std::size_t document_order = 0;
};

struct SceneScriptBootstrapFallbackPlan {
  SceneScriptBootstrapFallbackOperation operation =
      SceneScriptBootstrapFallbackOperation::None;
  bool runtime_only = true;
  bool mutates_source_document = false;
  vkpt::core::StableId target_entity_id = 0;
  std::string target_entity_name;
  std::string script_source;
  std::string description;
};

struct SceneScriptBootstrapDecision {
  SceneScriptBootstrapPolicy policy =
      SceneScriptBootstrapPolicy::ScriptlessWithoutCamera;
  bool authored_scene_init_supported = false;
  bool authored_scene_init_present = false;
  std::size_t entity_script_count = 0;
  std::size_t enabled_entity_script_count = 0;
  std::optional<SceneScriptBootstrapCameraSelection> main_camera;
  SceneScriptBootstrapFallbackPlan fallback;
  std::vector<SceneScriptBootstrapDiagnostic> diagnostics;
};

std::string_view to_string(SceneScriptBootstrapPolicy policy);
std::string_view to_string(SceneScriptBootstrapFallbackOperation operation);

SceneScriptBootstrapDecision DecideSceneScriptBootstrap(
    const SceneDocument& document,
    const SceneScriptBootstrapOptions& options = {});

std::string DescribeRuntimeFallbackInjection(
    const SceneScriptBootstrapDecision& decision);

}  // namespace vkpt::scene
