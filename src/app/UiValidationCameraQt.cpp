#include "app/UiValidationInternal.h"

#include "pathtracer/PathTracer.h"
#include "scene/Scene.h"

#ifdef PT_ENABLE_QT
#include "app/AppQtSceneDocumentActions.h"
#include "app/QtDockPanels.h"
#include "app/ViewportInteraction.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

namespace vkpt::app {

void RunUiCameraAndQtDockSmokeChecks(const UiSmokeCheckFn& check_true) {
  using namespace vkpt::editor;
  vkpt::scene::SceneDocument cameraDoc;
  cameraDoc.metadata.schema = "1.0";
  vkpt::scene::SceneEntityDefinition cameraEntity;
  cameraEntity.id = 77;
  cameraEntity.name = "Camera";
  cameraEntity.has_camera = true;
  cameraEntity.camera.fov = 47.0f;
  cameraEntity.camera.aperture_radius = 0.08f;
  cameraEntity.camera.focus_distance = 4.25f;
  cameraEntity.camera.f_stop = 2.8f;
  cameraEntity.camera.shutter_seconds = 1.0f / 125.0f;
  cameraEntity.camera.iso = 400.0f;
  cameraEntity.camera.exposure_compensation = 1.0f;
  cameraEntity.camera.white_balance_kelvin = 5200.0f;
  cameraEntity.camera.iris_blade_count = 7u;
  cameraEntity.camera.iris_rotation_degrees = 15.0f;
  cameraEntity.camera.iris_roundness = 0.35f;
  cameraEntity.camera.anamorphic_squeeze = 1.6f;
  cameraDoc.entities.push_back(cameraEntity);
  const auto cameraRoundTrip = vkpt::scene::SceneDocument::load_from_text(cameraDoc.to_json(false));
  check_true("camera json roundtrip parses", cameraRoundTrip.has_value());
  if (cameraRoundTrip && !cameraRoundTrip.value().entities.empty()) {
    const auto& camera = cameraRoundTrip.value().entities.front().camera;
    check_true("camera json roundtrip iris", camera.iris_blade_count == 7u);
    check_true("camera json roundtrip anamorphic", std::abs(camera.anamorphic_squeeze - 1.6f) < 0.001f);
    check_true("camera json roundtrip exposure", std::abs(camera.exposure_compensation - 1.0f) < 0.001f);
  }

  vkpt::pathtracer::RTSceneData physicalScene;
  physicalScene.camera_f_stop = 2.8f;
  physicalScene.camera_shutter_seconds = 1.0f / 30.0f;
  physicalScene.camera_iso = 200.0f;
  physicalScene.camera_exposure_compensation = 1.0f;
  physicalScene.camera_white_balance_kelvin = 5200.0f;
  vkpt::pathtracer::FilmResolveSettings resolveSettings;
  resolveSettings.exposure = 1.0f;
  resolveSettings.tone_map = vkpt::pathtracer::ToneMapMode::Reinhard;
  resolveSettings.output_transform = vkpt::pathtracer::OutputTransformMode::Linear;
  const auto adjustedResolve =
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(resolveSettings, physicalScene);
  check_true("physical camera exposure affects resolve", adjustedResolve.exposure > 1.0f);
  check_true("camera white balance affects resolve",
             std::abs(adjustedResolve.white_balance_kelvin - 5200.0f) < 0.001f);
  vkpt::pathtracer::FilmHdr hdr;
  hdr.width = 1u;
  hdr.height = 1u;
  hdr.rgbf = {0.5f, 0.5f, 0.5f};
  const auto ldrLinear = vkpt::pathtracer::ApplyFilmResolve(hdr, adjustedResolve);
  resolveSettings.output_transform = vkpt::pathtracer::OutputTransformMode::Gamma;
  const auto ldrGamma = vkpt::pathtracer::ApplyFilmResolve(hdr, resolveSettings);
  check_true("output transform changes film resolve",
             !ldrLinear.rgba8.empty() && !ldrGamma.rgba8.empty() &&
             ldrLinear.rgba8[0] != ldrGamma.rgba8[0]);
  const auto layoutManifest = vkpt::pathtracer::BuildRTSceneDataLayoutManifest();
  check_true("camera gpu layout manifest builds", layoutManifest.has_value());
  if (layoutManifest) {
    auto has_layout_field = [&](std::string_view field_name) {
      const auto& fields = layoutManifest.value().fields;
      return std::any_of(fields.begin(), fields.end(), [field_name](const auto& field) {
        return field.field == field_name;
      });
    };
    check_true("camera gpu layout has aperture", has_layout_field("camera_aperture_radius"));
    check_true("camera gpu layout has iris", has_layout_field("camera_iris_blade_count"));
    check_true("camera gpu layout has anamorphic", has_layout_field("camera_anamorphic_squeeze"));
  }
#ifdef PT_ENABLE_QT
  const auto cameraDockScene = vkpt::pathtracer::BuildSceneDataFromDocument(cameraDoc);
  check_true("camera dock scene builds", cameraDockScene.has_value());
  if (cameraDockScene) {
    const auto cameraDockPanels = BuildQtDockPanels(cameraDoc,
                                                   cameraDockScene.value(),
                                                   vkpt::pathtracer::RenderSettings{},
                                                   UiRuntimeState{},
                                                   SelectionState{},
                                                   CreateDefaultLayout(),
                                                   BenchmarkPanelModel{},
                                                   QtDockFrameStats{},
                                                   QtDockDeviceStats{},
                                                   0,
                                                   std::array<bool, 4>{});
    const auto cameraPanel = std::find_if(cameraDockPanels.begin(),
                                          cameraDockPanels.end(),
                                          [](const QtDockPanelContent& panel) {
                                            return panel.id == "camera";
                                          });
    const auto assetBrowserPanel = std::find_if(cameraDockPanels.begin(),
                                                cameraDockPanels.end(),
                                                [](const QtDockPanelContent& panel) {
                                                  return panel.id == "asset_browser";
                                                });
    const auto scriptPanel = std::find_if(cameraDockPanels.begin(),
                                          cameraDockPanels.end(),
                                          [](const QtDockPanelContent& panel) {
                                            return panel.id == "script_panel";
                                          });
    auto hasActivatableAssetRow = [](const QtDockPanelContent& panel,
                                     std::string_view idPrefix,
                                     std::string_view icon) {
      for (const auto& group : panel.tree_rows) {
        for (const auto& child : group.children) {
          if (child.activatable &&
              child.id.starts_with(idPrefix) &&
              child.icon == icon) {
            return true;
          }
        }
      }
      return false;
    };
    check_true("asset browser lists activatable scenes",
               assetBrowserPanel != cameraDockPanels.end() &&
               assetBrowserPanel->tree_single_column &&
               assetBrowserPanel->property_preferred_height > 0 &&
               hasActivatableAssetRow(*assetBrowserPanel, "asset.scene.", "scene"));
    check_true("asset browser lists activatable models",
               assetBrowserPanel != cameraDockPanels.end() &&
               hasActivatableAssetRow(*assetBrowserPanel, "asset.model.", "model"));
    check_true("asset browser model rows are draggable",
               assetBrowserPanel != cameraDockPanels.end() &&
               std::any_of(assetBrowserPanel->tree_rows.begin(),
                           assetBrowserPanel->tree_rows.end(),
                           [](const QtDockTreeRow& group) {
                             return std::any_of(group.children.begin(),
                                                group.children.end(),
                                                [](const QtDockTreeRow& child) {
                                                  return child.id.starts_with("asset.model.") &&
                                                         child.draggable;
                                                });
                           }));
    check_true("camera dock has focus buttons",
               cameraPanel != cameraDockPanels.end() &&
               std::any_of(cameraPanel->properties.begin(),
                           cameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.focus.pick" &&
                                    property.editor == "button";
                           }) &&
               std::any_of(cameraPanel->properties.begin(),
                           cameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.focus.selected" &&
                                    property.editor == "button";
                           }));
    check_true("camera dock has fps toggle",
               cameraPanel != cameraDockPanels.end() &&
               std::any_of(cameraPanel->properties.begin(),
                           cameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.mode.fps_toggle" &&
                                    property.editor == "button";
                           }));
    check_true("camera dock has configurable motion preview fps",
               cameraPanel != cameraDockPanels.end() &&
               std::any_of(cameraPanel->properties.begin(),
                           cameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.preview.fps" &&
                                    property.editor == "slider" &&
                                    property.maximum >= 120.0;
                           }));
    check_true("scripting tab exposes game mode controls",
               scriptPanel != cameraDockPanels.end() &&
               scriptPanel->visible &&
               scriptPanel->id == "script_panel" &&
               std::any_of(scriptPanel->properties.begin(),
                           scriptPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "script.runtime.play" &&
                                    property.editor == "button";
                           }) &&
               std::any_of(scriptPanel->properties.begin(),
                           scriptPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "script.runtime.pause" &&
                                    property.editor == "button";
                           }) &&
               std::any_of(scriptPanel->properties.begin(),
                           scriptPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.label == "runtime status";
                           }));
    vkpt::scene::SceneDocument scriptDoc;
    vkpt::scene::SceneEntityDefinition scriptedEntity;
    scriptedEntity.id = 901;
    scriptedEntity.name = "Scripted Probe";
    scriptedEntity.script.script = "assets/scripts/probe.lua";
    scriptedEntity.script.language = "lua";
    scriptedEntity.script.entry = "default";
    scriptedEntity.script.params["offset_x"] = "2.5";
    scriptedEntity.script.params["enabled"] = "true";
    scriptDoc.entities.push_back(scriptedEntity);
    SelectionState scriptSelection;
    scriptSelection.selected_entity_ids = {scriptedEntity.id};
    scriptSelection.active_primary_entity = scriptedEntity.id;
    QtDockScriptRuntimeState scriptRuntime;
    scriptRuntime.mode = "live_edit";
    scriptRuntime.status = "smoke runtime";
    scriptRuntime.playing = false;
    scriptRuntime.viewport_input_forwarding = true;
    scriptRuntime.binding_summary.lua_compiled_in = true;
    scriptRuntime.binding_summary.execution_available = true;
    scriptRuntime.binding_summary.binding_count = 1u;
    scriptRuntime.binding_summary.runnable_count = 1u;
    vkpt::scripting::ScriptBinding binding;
    binding.entity = scriptedEntity.id;
    binding.stable_order = 0u;
    binding.entity_name = scriptedEntity.name;
    binding.source = scriptedEntity.script.script;
    binding.language = "lua";
    binding.entry = "default";
    binding.module_id = "default";
    binding.enabled = true;
    binding.reload_on_save = true;
    binding.params = scriptedEntity.script.params;
    vkpt::scripting::ScriptEditorParam annotatedParam;
    annotatedParam.name = "annotated_gain";
    annotatedParam.type = "number";
    annotatedParam.label = "Annotated Gain";
    annotatedParam.default_value = "4";
    annotatedParam.minimum = 0.0;
    annotatedParam.maximum = 8.0;
    annotatedParam.step = 0.5;
    annotatedParam.has_minimum = true;
    annotatedParam.has_maximum = true;
    annotatedParam.has_step = true;
    binding.editor_params.push_back(annotatedParam);
    vkpt::scripting::ScriptEditorParam optionalParam;
    optionalParam.name = "optional_focus";
    optionalParam.type = "number";
    optionalParam.label = "Optional Focus";
    optionalParam.minimum = 0.0;
    optionalParam.maximum = 100.0;
    optionalParam.step = 0.5;
    optionalParam.has_minimum = true;
    optionalParam.has_maximum = true;
    optionalParam.has_step = true;
    binding.editor_params.push_back(optionalParam);
    scriptRuntime.bindings.push_back(binding);
    vkpt::scripting::ScriptBindingRuntimeState runtimeState;
    runtimeState.entity = scriptedEntity.id;
    runtimeState.last_hook = vkpt::scripting::ScriptLifecycleHook::OnUpdate;
    runtimeState.last_frame = 12u;
    runtimeState.command_count = 1u;
    scriptRuntime.runtime_states.push_back(runtimeState);
    vkpt::scripting::ScriptVariableSnapshot variable;
    variable.entity = scriptedEntity.id;
    variable.frame = 12u;
    variable.hook = vkpt::scripting::ScriptLifecycleHook::OnUpdate;
    variable.source = scriptedEntity.script.script;
    variable.scope = "ctx";
    variable.name = "offset_x";
    variable.value = "2.5";
    scriptRuntime.variables.push_back(variable);
    vkpt::scripting::ScriptVariableSnapshot stringVariable = variable;
    stringVariable.scope = "upvalue";
    stringVariable.name = "CAMERA_NAME";
    stringVariable.value = "\"Audio Demo Camera Listener\"";
    scriptRuntime.variables.push_back(stringVariable);
    vkpt::scripting::ScriptVariableSnapshot rangeVariable = variable;
    rangeVariable.scope = "upvalue";
    rangeVariable.name = "MIN_CAMERA_PITCH";
    rangeVariable.value = "0.12";
    scriptRuntime.variables.push_back(rangeVariable);
    vkpt::scripting::ScriptVariableSnapshot listVariable = variable;
    listVariable.scope = "script";
    listVariable.name = "waypoints";
    listVariable.value = "{1=\"alpha\", 2=\"bravo\"}";
    scriptRuntime.variables.push_back(listVariable);
    vkpt::scripting::ScriptDiagnostic diagnostic;
    diagnostic.severity = vkpt::scripting::ScriptDiagnosticSeverity::Warning;
    diagnostic.hook = vkpt::scripting::ScriptLifecycleHook::OnUpdate;
    diagnostic.entity = scriptedEntity.id;
    diagnostic.frame = 12u;
    diagnostic.source = scriptedEntity.script.script;
    diagnostic.message = "probe warning";
    scriptRuntime.diagnostics.push_back(diagnostic);
    const auto scriptScene = vkpt::pathtracer::BuildSceneDataFromDocument(scriptDoc);
    check_true("script dock scene builds", scriptScene.has_value());
    if (scriptScene) {
      const auto scriptDockPanels = BuildQtDockPanels(scriptDoc,
                                                      scriptScene.value(),
                                                      vkpt::pathtracer::RenderSettings{},
                                                      UiRuntimeState{},
                                                      scriptSelection,
                                                      CreateDefaultLayout(),
                                                      BenchmarkPanelModel{},
                                                      QtDockFrameStats{},
                                                      QtDockDeviceStats{},
                                                      0,
                                                      std::array<bool, 4>{},
                                                      &scriptRuntime);
      const auto scriptDock = std::find_if(scriptDockPanels.begin(),
                                           scriptDockPanels.end(),
                                           [](const QtDockPanelContent& panel) {
                                             return panel.id == "script_panel";
                                           });
      auto hasScriptProperty = [&](std::string_view id,
                                   std::string_view editor = {}) {
        return scriptDock != scriptDockPanels.end() &&
               std::any_of(scriptDock->properties.begin(),
                           scriptDock->properties.end(),
                           [&](const QtDockProperty& property) {
                             return property.id == id &&
                                    (editor.empty() || property.editor == editor);
                           });
      };
      auto hasAvailableScriptRow = [&]() {
        return scriptDock != scriptDockPanels.end() &&
               std::any_of(scriptDock->tree_rows.begin(),
                           scriptDock->tree_rows.end(),
                           [](const QtDockTreeRow& group) {
                             return group.label.find("Available Lua Scripts") != std::string::npos &&
                                    std::any_of(group.children.begin(),
                                                group.children.end(),
                                                [](const QtDockTreeRow& child) {
                                                  return child.id.starts_with("script.asset.") &&
                                                         child.icon == "script" &&
                                                         child.activatable;
                                                });
                           });
      };
      check_true("script dock exposes attach detach new open reload controls",
                 hasScriptProperty("script.selection.attach", "button") &&
                 hasScriptProperty("script.selection.detach", "button") &&
                 hasScriptProperty("script.actions.new_lua_script", "button") &&
                 hasScriptProperty("script.actions.open_folder", "button") &&
                 hasScriptProperty("script.selection.open", "button") &&
                 hasScriptProperty("script.runtime.reload", "button") &&
                 hasAvailableScriptRow());
      check_true("script dock exposes future live play controls and runtime mode",
                 hasScriptProperty("script.runtime.run_live", "button") &&
                 hasScriptProperty("script.runtime.play", "button") &&
                 hasScriptProperty("script.runtime.stop", "button") &&
                 hasScriptProperty("script.runtime.send_viewport_input", "button") &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                              return property.id == "script.runtime.mode" &&
                                      property.value == "live_edit";
                             }) &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.id == "script.runtime.status" &&
                                      property.value == "smoke runtime";
                             }) &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.id == "script.runtime.viewport_input" &&
                                      property.value == "true";
                             }) &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.id == "script.runtime.playing" &&
                                      property.editor == "toggle" &&
                                      property.value == "true";
                             }));
      check_true("script dock exposes lifecycle hook controls",
                 hasScriptProperty("script.runtime.dispatch_on_load", "button") &&
                 hasScriptProperty("script.runtime.dispatch_on_spawn", "button") &&
                 hasScriptProperty("script.runtime.dispatch_on_enable", "button") &&
                 hasScriptProperty("script.runtime.dispatch_on_update", "button") &&
                 hasScriptProperty("script.runtime.dispatch_fixed_update", "button") &&
                 hasScriptProperty("script.runtime.dispatch_late_update", "button") &&
                 hasScriptProperty("script.runtime.dispatch_on_disable", "button") &&
                 hasScriptProperty("script.runtime.dispatch_on_destroy", "button") &&
                 hasScriptProperty("script.runtime.dispatch_on_unload", "button"));
      check_true("script dock exposes editable typed params and runtime state",
                 hasScriptProperty("entity.901.script.path", "text") &&
                 hasScriptProperty("entity.901.script.language", "dropdown") &&
                 hasScriptProperty("entity.901.script.enabled", "toggle") &&
                 hasScriptProperty("entity.901.script.param.offset_x", "number") &&
                 hasScriptProperty("entity.901.script.param.annotated_gain", "slider") &&
                 hasScriptProperty("entity.901.script.param.optional_focus", "slider") &&
                 hasScriptProperty("entity.901.script.param.enabled", "toggle") &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.id == "entity.901.script.param.annotated_gain" &&
                                      property.label == "Annotated Gain" &&
                                      property.has_numeric_range &&
                                      property.minimum == 0.0 &&
                                      property.maximum == 8.0 &&
                                      property.step == 0.5;
                             }) &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.id == "entity.901.script.param.optional_focus" &&
                                      property.label == "Optional Focus" &&
                                      property.editor == "slider" &&
                                      property.has_numeric_range &&
                                      !property.has_default &&
                                      property.minimum == 0.0 &&
                                      property.maximum == 100.0 &&
                                      property.step == 0.5;
                             }) &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.label == "runtime 1" &&
                                      property.value.find("cmds=1") != std::string::npos;
                             }) &&
                 std::any_of(scriptDock->properties.begin(),
                             scriptDock->properties.end(),
                             [](const QtDockProperty& property) {
                               return property.label == "diagnostic 1" &&
                               property.value.find("probe warning") != std::string::npos;
                             }));
      auto hasVariableControl = [&](std::string_view label,
                                    std::string_view editor) {
        return scriptDock != scriptDockPanels.end() &&
               std::any_of(scriptDock->properties.begin(),
                           scriptDock->properties.end(),
                           [&](const QtDockProperty& property) {
                             return property.label == label && property.editor == editor;
                           });
      };
      check_true("script dock labels captured variables by name with typed controls",
                 hasVariableControl("offset_x", "number") &&
                 hasVariableControl("CAMERA_NAME", "text") &&
                 hasVariableControl("MIN_CAMERA_PITCH", "slider") &&
                 hasVariableControl("waypoints", "list"));
    }
    QtDockFrameStats fpsFrame;
    fpsFrame.camera_mode = "fps";
    fpsFrame.fps_player_grounded = true;
    fpsFrame.fps_player_crouching = true;
    fpsFrame.fps_player_running = false;
    fpsFrame.fps_player_speed = 1.5f;
    fpsFrame.fps_player_eye_height = 1.1f;
    const auto fpsCameraDockPanels = BuildQtDockPanels(cameraDoc,
                                                       cameraDockScene.value(),
                                                       vkpt::pathtracer::RenderSettings{},
                                                       UiRuntimeState{},
                                                       SelectionState{},
                                                       CreateDefaultLayout(),
                                                       BenchmarkPanelModel{},
                                                       fpsFrame,
                                                       QtDockDeviceStats{},
                                                       0,
                                                       std::array<bool, 4>{});
    const auto fpsCameraPanel = std::find_if(fpsCameraDockPanels.begin(),
                                             fpsCameraDockPanels.end(),
                                             [](const QtDockPanelContent& panel) {
                                               return panel.id == "camera";
                                             });
    check_true("camera dock exposes fps player state",
               fpsCameraPanel != fpsCameraDockPanels.end() &&
               std::any_of(fpsCameraPanel->properties.begin(),
                           fpsCameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.label == "FPS body" &&
                                    property.value == "grounded";
                           }) &&
               std::any_of(fpsCameraPanel->properties.begin(),
                           fpsCameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.mode.fps_toggle" &&
                                    property.value == "Exit FPS";
                           }));
  }

  {
    ViewportMouseClickState clickState;
    BeginViewportMouseClick(clickState,
                            ViewportMouseInputMode::Editor,
                            0,
                            10.0f,
                            10.0f);
    auto clickResult = EndViewportMouseClick(clickState,
                                             ViewportMouseInputMode::Editor,
                                             0,
                                             11.0f,
                                             10.5f,
                                             6.0f);
    check_true("viewport click state allows editor selection",
               clickResult.editor_pick_allowed && !clickResult.suppressed_editor_pick);

    BeginViewportMouseClick(clickState,
                            ViewportMouseInputMode::GameModeScript,
                            0,
                            20.0f,
                            20.0f);
    clickResult = EndViewportMouseClick(clickState,
                                        ViewportMouseInputMode::GameModeScript,
                                        0,
                                        20.5f,
                                        20.0f,
                                        6.0f);
    check_true("viewport click state sends game clicks to scripts",
               !clickResult.editor_pick_allowed &&
               clickResult.game_mode_click &&
               clickResult.suppressed_editor_pick);

    BeginViewportMouseClick(clickState,
                            ViewportMouseInputMode::GameModeScript,
                            0,
                            30.0f,
                            30.0f);
    clickResult = EndViewportMouseClick(clickState,
                                        ViewportMouseInputMode::Editor,
                                        0,
                                        30.0f,
                                        30.0f,
                                        6.0f);
    check_true("viewport click state keeps game-mode ownership through release",
               !clickResult.editor_pick_allowed &&
               clickResult.game_mode_click &&
               clickResult.suppressed_editor_pick);

    BeginViewportMouseClick(clickState,
                            ViewportMouseInputMode::Editor,
                            0,
                            40.0f,
                            40.0f);
    (void)UpdateViewportMouseClickDrag(clickState, 55.0f, 40.0f);
    clickResult = EndViewportMouseClick(clickState,
                                        ViewportMouseInputMode::Editor,
                                        0,
                                        55.0f,
                                        40.0f,
                                        6.0f);
    check_true("viewport click state rejects editor drag selection",
               !clickResult.editor_pick_allowed && clickResult.drag_pixels > 6.0f);
  }

  {
    ViewportPickable floor;
    floor.require_triangle_hit = true;
    floor.triangles.push_back(MakeViewportTriangle({-1.0f, 0.0f, -1.0f},
                                                   { 1.0f, 0.0f, -1.0f},
                                                   { 0.0f, 0.0f,  1.0f}));
    ExpandBounds(floor.bounds, {-1.0f, 0.0f, -1.0f});
    ExpandBounds(floor.bounds, { 1.0f, 0.0f,  1.0f});
    const auto ground = TraceFpsGround({floor}, {0.0f, 1.0f, 0.0f}, 2.0f, 0.62f);
    check_true("fps ground trace hits floor",
               ground.hit && std::fabs(ground.position.y) < 0.001f);

    ViewportPickable wall;
    wall.require_triangle_hit = true;
    wall.triangles.push_back(MakeViewportTriangle({-1.0f, 0.0f, 0.0f},
                                                  { 1.0f, 0.0f, 0.0f},
                                                  { 0.0f, 2.0f, 0.0f}));
    ExpandBounds(wall.bounds, {-1.0f, 0.0f, 0.0f});
    ExpandBounds(wall.bounds, { 1.0f, 2.0f, 0.0f});
    const auto wallHit = TraceFpsWall({wall},
                                      {0.0f, 1.0f, -1.0f},
                                      {0.0f, 0.0f, 1.0f},
                                      2.0f,
                                      0.62f);
    check_true("fps wall trace hits vertical surface",
               wallHit.hit && std::fabs(wallHit.position.z) < 0.001f);
  }

  {
    vkpt::scene::SceneDocument treeDoc;
    treeDoc.metadata.scene_name = "qt-scene-graph-smoke";
    vkpt::scene::SceneEntityDefinition root;
    root.id = 10;
    root.name = "Root";
    vkpt::scene::SceneEntityDefinition model;
    model.id = 20;
    model.name = "Model";
    model.hierarchy.parent = root.id;
    model.has_mesh = true;
    vkpt::scene::SceneEntityDefinition light;
    light.id = 30;
    light.name = "Key Light";
    light.hierarchy.parent = root.id;
    light.has_light = true;
    treeDoc.entities = {root, model, light};
    SelectionState treeSelection;
    treeSelection.selected_entity_ids = {model.id};
    UiRuntimeState treeRuntime;
    const auto sceneGraphPanel =
        BuildQtSceneTreeDock(treeDoc, treeSelection, treeRuntime, CreateDefaultLayout());
    check_true("qt scene graph uses one-column tree", sceneGraphPanel.tree_single_column);
    check_true("qt scene graph emits tree rows", sceneGraphPanel.tree_rows.size() == 1u);
    check_true("qt scene graph visually nests children",
               !sceneGraphPanel.tree_rows.empty() &&
               sceneGraphPanel.tree_rows.front().children.size() == 2u);
    check_true("qt scene graph model icon and selection",
               !sceneGraphPanel.tree_rows.empty() &&
               sceneGraphPanel.tree_rows.front().children.size() >= 1u &&
               sceneGraphPanel.tree_rows.front().children.front().icon == "model" &&
               sceneGraphPanel.tree_rows.front().children.front().selected);
    check_true("qt scene graph light icon",
               !sceneGraphPanel.tree_rows.empty() &&
               sceneGraphPanel.tree_rows.front().children.size() >= 2u &&
               sceneGraphPanel.tree_rows.front().children[1].icon == "light");
    check_true("qt scene graph exposes model visibility toggle",
               !sceneGraphPanel.tree_rows.empty() &&
               sceneGraphPanel.tree_rows.front().children.size() >= 1u &&
               sceneGraphPanel.tree_rows.front().children.front().visibility_toggle_enabled &&
               sceneGraphPanel.tree_rows.front().children.front().visible);
    check_true("qt scene graph exposes name filter",
               std::any_of(sceneGraphPanel.properties.begin(),
                           sceneGraphPanel.properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "scene_tree.filter.name" &&
                                    property.editor == "text";
                           }));
    treeRuntime.scene_tree_type_filter_mask = kQtSceneTreeFilterLight;
    const auto lightFilteredPanel =
        BuildQtSceneTreeDock(treeDoc, treeSelection, treeRuntime, CreateDefaultLayout());
    check_true("qt scene graph type filter keeps matching descendants",
               !lightFilteredPanel.tree_rows.empty() &&
               lightFilteredPanel.tree_rows.front().children.size() == 1u &&
               lightFilteredPanel.tree_rows.front().children.front().icon == "light");
    treeRuntime.scene_tree_type_filter_mask = 0u;
    treeRuntime.scene_tree_name_filter = "model";
    const auto nameFilteredPanel =
        BuildQtSceneTreeDock(treeDoc, treeSelection, treeRuntime, CreateDefaultLayout());
    check_true("qt scene graph name filter is case insensitive",
               !nameFilteredPanel.tree_rows.empty() &&
               nameFilteredPanel.tree_rows.front().children.size() == 1u &&
               nameFilteredPanel.tree_rows.front().children.front().label == "Model");

    vkpt::scene::SceneParticleEmitterDefinition rainEmitter;
    rainEmitter.id = 100;
    rainEmitter.name = "Benchmark rain";
    rainEmitter.type = "rain";
    rainEmitter.count = 64;
    treeDoc.particle_emitters = {rainEmitter};
    treeRuntime.scene_tree_name_filter.clear();
    treeRuntime.scene_tree_type_filter_mask = 0u;
    const auto particlePanel =
        BuildQtSceneTreeDock(treeDoc, treeSelection, treeRuntime, CreateDefaultLayout());
    const auto particleGroup = std::find_if(
        particlePanel.tree_rows.begin(),
        particlePanel.tree_rows.end(),
        [](const QtDockTreeRow& row) {
          return row.id == "particle_emitters";
        });
    check_true("qt scene graph exposes particle emitter group",
               particleGroup != particlePanel.tree_rows.end() &&
               particleGroup->children.size() == 1u &&
               particleGroup->children.front().icon == "particle" &&
               particleGroup->children.front().label == "Benchmark rain");
    treeRuntime.scene_tree_type_filter_mask = kQtSceneTreeFilterParticle;
    const auto particleFilteredPanel =
        BuildQtSceneTreeDock(treeDoc, treeSelection, treeRuntime, CreateDefaultLayout());
    check_true("qt scene graph particle filter keeps emitters",
               !particleFilteredPanel.tree_rows.empty() &&
               std::any_of(particleFilteredPanel.tree_rows.begin(),
                           particleFilteredPanel.tree_rows.end(),
                           [](const QtDockTreeRow& row) {
                             return row.id == "particle_emitters" && !row.children.empty();
                           }));
  }

  {
    vkpt::scene::SceneDocument visibilityDoc;
    vkpt::scene::SceneGeometryDefinition tri;
    tri.id = 100;
    tri.vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    tri.indices = {0, 1, 2};
    visibilityDoc.geometry.push_back(tri);
    vkpt::scene::SceneEntityDefinition hiddenRoot;
    hiddenRoot.id = 200;
    hiddenRoot.name = "Hidden Root";
    hiddenRoot.visible = false;
    vkpt::scene::SceneEntityDefinition mesh;
    mesh.id = 201;
    mesh.name = "Hidden Mesh";
    mesh.has_mesh = true;
    mesh.mesh.mesh_id = tri.id;
    mesh.has_hierarchy = true;
    mesh.hierarchy.parent = hiddenRoot.id;
    visibilityDoc.entities = {hiddenRoot, mesh};
    const auto hiddenScene = vkpt::pathtracer::BuildSceneDataFromDocument(visibilityDoc);
    check_true("hidden scene graph branch skips render instances",
               hiddenScene &&
               hiddenScene.value().instances.empty() &&
               hiddenScene.value().vertices.empty());
  }

  {
    vkpt::scene::SceneDocument hiddenLightDoc;
    vkpt::scene::SceneGeometryDefinition tri;
    tri.id = 300;
    tri.vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    tri.indices = {0, 1, 2};
    hiddenLightDoc.geometry.push_back(tri);
    vkpt::scene::SceneEntityDefinition camera;
    camera.id = 301;
    camera.name = "Camera";
    camera.has_camera = true;
    vkpt::scene::SceneEntityDefinition mesh;
    mesh.id = 302;
    mesh.name = "Mesh";
    mesh.has_mesh = true;
    mesh.mesh.mesh_id = tri.id;
    vkpt::scene::SceneEntityDefinition hiddenLight;
    hiddenLight.id = 303;
    hiddenLight.name = "Hidden Light";
    hiddenLight.visible = false;
    hiddenLight.has_light = true;
    hiddenLight.light.intensity = 10.0f;
    hiddenLightDoc.entities = {camera, mesh, hiddenLight};
    const auto hiddenLightScene = vkpt::pathtracer::BuildSceneDataFromDocument(hiddenLightDoc);
    check_true("hidden authored light does not create fallback lighting",
               hiddenLightScene &&
               hiddenLightScene.value().instances.size() == 1u &&
               hiddenLightScene.value().lights.size() == 1u &&
               hiddenLightScene.value().lights.front().intensity <= 1.0e-6f);
  }

  {
    vkpt::scene::SceneDocument unlitDoc;
    vkpt::scene::SceneGeometryDefinition tri;
    tri.id = 360;
    tri.vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    tri.indices = {0, 1, 2};
    unlitDoc.geometry.push_back(tri);
    vkpt::scene::SceneEntityDefinition camera;
    camera.id = 361;
    camera.name = "Camera";
    camera.has_camera = true;
    vkpt::scene::SceneEntityDefinition mesh;
    mesh.id = 362;
    mesh.name = "Mesh";
    mesh.has_mesh = true;
    mesh.mesh.mesh_id = tri.id;
    unlitDoc.entities = {camera, mesh};
    const auto unlitScene = vkpt::pathtracer::BuildSceneDataFromDocument(unlitDoc);
    check_true("authored unlit scene does not get implicit render lighting",
               unlitScene &&
               unlitScene.value().instances.size() == 1u &&
               unlitScene.value().lights.empty() &&
               std::max({unlitScene.value().environment_color.x,
                         unlitScene.value().environment_color.y,
                         unlitScene.value().environment_color.z}) <= 1.0e-6f);
  }

  {
    vkpt::scene::SceneDocument lightToggleDoc;
    vkpt::scene::SceneGeometryDefinition tri;
    tri.id = 370;
    tri.vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    tri.indices = {0, 1, 2};
    lightToggleDoc.geometry.push_back(tri);
    vkpt::scene::SceneEntityDefinition mesh;
    mesh.id = 371;
    mesh.name = "Mesh";
    mesh.has_mesh = true;
    mesh.mesh.mesh_id = tri.id;
    vkpt::scene::SceneEntityDefinition light;
    light.id = 372;
    light.name = "Key Light";
    light.has_light = true;
    light.light.intensity = 12.0f;
    lightToggleDoc.entities = {mesh, light};
    const auto visibleLightScene = vkpt::pathtracer::BuildSceneDataFromDocument(lightToggleDoc);
    lightToggleDoc.entities.back().visible = false;
    const auto hiddenLightScene = vkpt::pathtracer::BuildSceneDataFromDocument(lightToggleDoc);
    const auto lightDelta = visibleLightScene && hiddenLightScene
        ? vkpt::pathtracer::BuildSceneDeltaUpdate(visibleLightScene.value(), hiddenLightScene.value())
        : std::optional<vkpt::pathtracer::RTSceneDeltaUpdate>{};
    check_true("light visibility toggle remains a render light delta",
               visibleLightScene &&
               hiddenLightScene &&
               visibleLightScene.value().lights.size() == hiddenLightScene.value().lights.size() &&
               !hiddenLightScene.value().lights.empty() &&
               hiddenLightScene.value().lights.front().intensity <= 1.0e-6f &&
               lightDelta &&
               lightDelta->lights.size() == 1u &&
               lightDelta->materials.empty() &&
               !lightDelta->environment_color_changed);
  }

  {
    const auto emptyScene = vkpt::pathtracer::BuildSceneDataFromDocument(vkpt::scene::SceneDocument{});
    check_true("empty scene document does not create implicit render content",
               emptyScene &&
               emptyScene.value().vertices.empty() &&
               emptyScene.value().indices.empty() &&
               emptyScene.value().instances.empty() &&
               emptyScene.value().sdf_primitives.empty() &&
               emptyScene.value().lights.empty());
  }

#ifdef PT_ENABLE_QT
  {
    vkpt::scene::SceneDocument fallbackLightingDoc;
    vkpt::scene::SceneGeometryDefinition tri;
    tri.id = 400;
    tri.vertices = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    tri.indices = {0, 1, 2};
    fallbackLightingDoc.geometry.push_back(tri);
    vkpt::scene::SceneEntityDefinition mesh;
    mesh.id = 401;
    mesh.name = "Mesh";
    mesh.has_mesh = true;
    mesh.mesh.mesh_id = tri.id;
    fallbackLightingDoc.entities = {mesh};
    EnsureQtFallbackLightingEntities(fallbackLightingDoc);
    const auto hiddenFallbackScene =
        vkpt::pathtracer::BuildSceneDataFromDocument(fallbackLightingDoc);
    const auto fallbackGroup = std::find_if(
        fallbackLightingDoc.entities.begin(),
        fallbackLightingDoc.entities.end(),
        [](const vkpt::scene::SceneEntityDefinition& entity) {
          return entity.name == "Fallback Lighting (off by default)";
        });
    check_true("qt fallback lighting is injected hidden by default",
               fallbackGroup != fallbackLightingDoc.entities.end() &&
               !fallbackGroup->visible &&
               hiddenFallbackScene &&
               hiddenFallbackScene.value().lights.size() == 1u &&
               hiddenFallbackScene.value().lights.front().intensity <= 1.0e-6f);
    if (fallbackGroup != fallbackLightingDoc.entities.end()) {
      fallbackGroup->visible = true;
    }
    const auto visibleFallbackScene =
        vkpt::pathtracer::BuildSceneDataFromDocument(fallbackLightingDoc);
    check_true("qt fallback lighting can be enabled from scene graph",
               visibleFallbackScene &&
               !visibleFallbackScene.value().lights.empty() &&
               std::max({visibleFallbackScene.value().environment_color.x,
                         visibleFallbackScene.value().environment_color.y,
                         visibleFallbackScene.value().environment_color.z}) > 1.0e-6f);
  }
#endif

  {
    vkpt::pathtracer::RenderSettings renderSettings;
    renderSettings.width = 320u;
    renderSettings.height = 240u;
    renderSettings.enable_denoiser = true;
    renderSettings.enable_temporal_aa = true;
    QtDockFrameStats renderFrame;
    renderFrame.frame_width = 320u;
    renderFrame.frame_height = 240u;
    renderFrame.canvas_width = 1150u;
    renderFrame.canvas_height = 900u;
    renderFrame.displayed_image_width = 810u;
    renderFrame.displayed_image_height = 608u;
    renderFrame.sample_count = 7u;
    renderFrame.render_mode = "on (event loop)";
    QtDockDeviceStats renderDevice;
    renderDevice.selected_backend = "cpu";
    renderDevice.runtime_backend_options = {"auto", "cpu", "d3d12", "d3d12-dxr", "vulkan", "null"};
    const auto renderPanel = BuildQtRenderSettingsDock(
        vkpt::pathtracer::RTSceneData{},
        renderSettings,
        UiRuntimeState{},
        CreateDefaultLayout(),
        renderFrame,
        renderDevice);
    const auto findRenderProperty = [&](std::string_view label) -> const QtDockProperty* {
      const auto it = std::find_if(renderPanel.properties.begin(),
                                   renderPanel.properties.end(),
                                   [&](const QtDockProperty& property) {
                                     return property.label == label;
                                   });
      return it == renderPanel.properties.end() ? nullptr : &(*it);
    };
    const auto* renderResolution = findRenderProperty("Render resolution");
    const auto* canvasResolution = findRenderProperty("Viewport canvas");
    const auto* displayedImage = findRenderProperty("Displayed image");
    const auto* gpuDenoiser = findRenderProperty("GPU denoiser");
    const auto* temporalAa = findRenderProperty("Temporal AA");
    const auto* runtimeBackend = findRenderProperty("Backend");
    check_true("render settings separates render resolution from canvas",
               renderResolution != nullptr &&
               renderResolution->value == "320x240" &&
               canvasResolution != nullptr &&
               canvasResolution->value == "1150x900" &&
               displayedImage != nullptr &&
               displayedImage->value == "810x608");
    check_true("render settings exposes GPU denoiser toggle",
               gpuDenoiser != nullptr &&
               gpuDenoiser->value == "true" &&
               gpuDenoiser->editor == "toggle");
    check_true("render settings exposes temporal AA toggle",
               temporalAa != nullptr &&
               temporalAa->value == "true" &&
               temporalAa->editor == "toggle");
    check_true("render settings exposes backend dropdown",
               runtimeBackend != nullptr &&
               runtimeBackend->id == "render.backend" &&
               runtimeBackend->editor == "dropdown" &&
               runtimeBackend->value == "cpu" &&
               std::find(runtimeBackend->options.begin(),
                         runtimeBackend->options.end(),
                         "d3d12-dxr") != runtimeBackend->options.end() &&
               std::find(runtimeBackend->options.begin(),
                         runtimeBackend->options.end(),
                         "vulkan") != runtimeBackend->options.end());
  }

  {
    QtDockFrameStats metricFrame;
    metricFrame.sample_count = 64u;
    metricFrame.total_rays = 64'000'000u;
    metricFrame.rolling_rays_per_second = 100'000'000.0;
    metricFrame.accumulated_rays_per_second = 80'000'000.0;
    vkpt::render::AcceleratorCapabilities measuredGpu;
    measuredGpu.id = "gpu:test";
    measuredGpu.name = "Measured GPU";
    measuredGpu.available = true;
    measuredGpu.hardware = true;
    measuredGpu.d3d12 = true;
    measuredGpu.compute = true;
    measuredGpu.ray_tracing = true;
    measuredGpu.accelerator_kind = vkpt::render::AcceleratorKind::DiscreteGpu;
    measuredGpu.estimated_rays_per_ms = 1'820'000.0;
    QtDockDeviceStats metricDevice;
    metricDevice.selected_backend = "d3d12";
    metricDevice.active_renderer_path = "d3d12-dxr";
    metricDevice.backend_caps.backend_name = "d3d12-dxr";
    metricDevice.runtime_backend_options = {"auto", "cpu", "d3d12", "d3d12-dxr", "vulkan", "null"};
    metricDevice.has_selected_accelerator = true;
    metricDevice.selected_accelerator = measuredGpu;
    metricDevice.accelerators.push_back(measuredGpu);
    metricDevice.ray_metrics.push_back(QtDockRayDeviceMetric{
        QtDockRayDeviceKeyForAccelerator(measuredGpu),
        measuredGpu.name,
        metricFrame.sample_count,
        metricFrame.total_rays,
        120'000'000.0,
        metricFrame.rolling_rays_per_second,
        metricFrame.accumulated_rays_per_second,
        true});
    const auto devicePanel = BuildQtDeviceDock(
        vkpt::pathtracer::RTSceneData{},
        UiRuntimeState{},
        CreateDefaultLayout(),
        metricFrame,
        metricDevice);
    const auto findProperty = [&](std::string_view label) -> const QtDockProperty* {
      const auto it = std::find_if(devicePanel.properties.begin(),
                                   devicePanel.properties.end(),
                                   [&](const QtDockProperty& property) {
                                     return property.label == label;
                                   });
      return it == devicePanel.properties.end() ? nullptr : &(*it);
    };
    const auto* throughputProperty = findProperty("Ray throughput");
    const auto* activeDeviceProperty = findProperty("Active ray device");
    const auto* runtimeBackendProperty = findProperty("Runtime backend");
    check_true("device dock exposes runtime backend dropdown",
               runtimeBackendProperty != nullptr &&
               runtimeBackendProperty->id == "render.backend" &&
               runtimeBackendProperty->editor == "dropdown" &&
               runtimeBackendProperty->value == "d3d12" &&
               std::find(runtimeBackendProperty->options.begin(),
                         runtimeBackendProperty->options.end(),
                         "d3d12-dxr") != runtimeBackendProperty->options.end() &&
               std::find(runtimeBackendProperty->options.begin(),
                         runtimeBackendProperty->options.end(),
                         "vulkan") != runtimeBackendProperty->options.end());
    check_true("device dock shows computer accumulated ray average",
               throughputProperty != nullptr &&
               throughputProperty->value.find("computer avg 80.00 MRays/s") != std::string::npos);
    check_true("device dock active device uses measured rolling rate",
               activeDeviceProperty != nullptr &&
               activeDeviceProperty->value.find("rolling 100.00 MRays/s") != std::string::npos &&
               activeDeviceProperty->value.find("planning est") == std::string::npos);
  }
#endif

}

}  // namespace vkpt::app
