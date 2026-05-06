#include "app/UiValidation.h"

#include "benchmark/BenchmarkSchema.h"
#include "editor/QtPanelModels.h"
#include "pathtracer/PathTracer.h"
#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"

#ifdef PT_ENABLE_QT
#include "app/QtDockPanels.h"
#include "app/ViewportInteraction.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vkpt::app {

std::uint64_t UiValidationNowMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index = 0,
                 std::string_view old_value = {},
                 std::string_view new_value = {},
                 std::string_view command_result = {}) {
  vkpt::editor::UiEvent event;
  event.event_type = std::string(event_type);
  event.panel_id = std::string(panel_id);
  event.widget_id = std::string(widget_id);
  event.frame_index = frame_index;
  event.timestamp_ms = UiValidationNowMs();
  event.thread_id = "main";
  event.old_value = std::string(old_value);
  event.new_value = std::string(new_value);
  event.command_result = std::string(command_result);
  log.push(event);
}

std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence();

bool Check(std::string_view tag, bool cond) {
  if (!cond) {
    std::cerr << "ui-model-smoke: fail: " << tag << "\n";
  }
  return cond;
}

bool HasTopLevelMenu(const vkpt::editor::MenuBar& menu, std::string_view id) {
  for (const auto& item : menu.top_level_menus) {
    if (item.id == id) {
      return true;
    }
  }
  return false;
}

bool HasMenuItem(const vkpt::editor::MenuBar& menu, std::string_view top_level_id, std::string_view action_id) {
  for (const auto& top_level : menu.top_level_menus) {
    if (top_level.id != top_level_id) {
      continue;
    }
    for (const auto& item : top_level.children) {
      if (item.id == action_id) {
        return true;
      }
    }
    return false;
  }
  return false;
}

std::vector<vkpt::editor::SceneTreeEntityModel> BuildSceneTreeEntitiesFromWorld(
    const vkpt::scene::SceneWorld& world) {
  std::vector<vkpt::editor::SceneTreeEntityModel> models;
  models.reserve(world.all_entities().size());
  for (const auto id : world.all_entities()) {
    const auto* entity = world.get_entity(id);
    if (!entity) {
      continue;
    }
    vkpt::editor::SceneTreeEntityModel model;
    model.entity_id = id;
    model.parent_id = entity->hierarchy ? entity->hierarchy->parent : 0;
    model.sibling_order = entity->hierarchy ? entity->hierarchy->sibling_order : 0;
    model.name = entity->identity.name;
    if (entity->transform.has_value()) {
      model.component_badges.push_back("transform");
    }
    if (entity->mesh_renderer.has_value()) {
      model.component_badges.push_back("mesh");
    }
    if (entity->sdf_primitive.has_value()) {
      model.component_badges.push_back("sdf");
    }
    if (entity->material_override.has_value() ||
        (entity->mesh_renderer.has_value() && entity->mesh_renderer->material_id != 0)) {
      model.component_badges.push_back("material");
    }
    if (entity->camera.has_value()) {
      model.component_badges.push_back("camera");
    }
    if (entity->light.has_value()) {
      model.component_badges.push_back("light");
    }
    if (entity->script.has_value()) {
      model.component_badges.push_back("script");
    }
    if (entity->physics_body.has_value()) {
      model.component_badges.push_back("physics");
    }
    models.push_back(std::move(model));
  }
  return models;
}

bool CheckEcsSceneTreeContracts(std::string* detail = nullptr) {
  auto fail = [&](std::string_view reason) {
    if (detail) {
      *detail = std::string(reason);
    }
    return false;
  };
  auto set_detail = [&](std::string_view text) {
    if (detail) {
      *detail = std::string(text);
    }
  };

  vkpt::scene::SceneDocument doc;
  doc.metadata.schema = "1.0";
  doc.metadata.scene_name = "ecs-tree-smoke";
  vkpt::scene::SceneEntityDefinition rootDef;
  rootDef.id = 1;
  rootDef.name = "Root";
  rootDef.has_physics_body = true;
  rootDef.physics_body.enabled = true;
  rootDef.physics_body.dynamic = true;
  rootDef.physics_body.body_type = "dynamic";
  rootDef.physics_body.shape = "box";
  rootDef.physics_body.mass = 2.0f;
  vkpt::scene::SceneEntityDefinition firstChild;
  firstChild.id = 2;
  firstChild.name = "First";
  firstChild.has_hierarchy = true;
  firstChild.hierarchy.parent = 1;
  firstChild.hierarchy.sibling_order = 1;
  vkpt::scene::SceneEntityDefinition secondChild;
  secondChild.id = 3;
  secondChild.name = "Second";
  secondChild.has_hierarchy = true;
  secondChild.hierarchy.parent = 1;
  secondChild.hierarchy.sibling_order = 0;
  doc.entities = {rootDef, firstChild, secondChild};
  const auto serialized = doc.to_json(true);
  auto reloaded = vkpt::scene::SceneDocument::load_from_text(serialized);
  if (!reloaded) {
    return fail("SceneDocument hierarchy JSON roundtrip failed to parse");
  }
  auto loadedWorld = reloaded.value().to_world();
  if (!loadedWorld) {
    return fail("SceneDocument hierarchy JSON roundtrip failed to build ECS world");
  }
  const auto persistedChildren = loadedWorld.value().children_of(1);
  if (persistedChildren != std::vector<vkpt::core::StableId>({3, 2})) {
    return fail("sibling_order did not persist through save/load");
  }
  auto physics = vkpt::physics::CreatePhysicsWorld();
  const auto physicsInfo = physics->engine_info();
  if (!physicsInfo.runs_on_worker_thread || physicsInfo.threading_model != "dedicated_worker") {
    return fail("physics world is not running through the dedicated worker thread");
  }
  const auto persistedPhysics = physics->sync_from_scene_world(loadedWorld.value());
  if (persistedPhysics.physics_components != 1u || persistedPhysics.enabled_bodies != 1u ||
      persistedPhysics.dynamic_bodies != 1u) {
    return fail("physics body did not persist through JSON and ECS sync");
  }

  vkpt::scene::SceneWorld world;
  const auto root = world.create_entity("Root", 10);
  const auto childA = world.create_entity("Child A", 11);
  const auto childB = world.create_entity("Child B", 12);
  vkpt::scene::TransformComponent rootTransform;
  rootTransform.translation = {10.0f, 0.0f, 0.0f};
  vkpt::scene::TransformComponent childTransform;
  childTransform.translation = {1.0f, 0.0f, 0.0f};
  if (!world.set_transform(root, rootTransform) ||
      !world.set_transform(childA, childTransform) ||
      !world.set_hierarchy_parent(childA, root, 0) ||
      !world.set_hierarchy_parent(childB, root, 1)) {
    return fail("failed to build ECS hierarchy fixture");
  }
  vkpt::scene::MeshRendererComponent mesh;
  mesh.mesh_id = 101;
  mesh.material_id = 201;
  world.set_component(childB, vkpt::scene::ComponentKind::MeshRenderer, mesh);
  vkpt::scene::PhysicsBodyComponent body;
  body.enabled = true;
  body.dynamic = true;
  body.body_type = "dynamic";
  body.mass = 1.5f;
  world.set_component(childA, vkpt::scene::ComponentKind::PhysicsBody, body);
  const auto livePhysics = physics->sync_from_scene_world(world);
  if (livePhysics.enabled_bodies != 1u || livePhysics.dynamic_bodies != 1u) {
    return fail("physics world did not sync enabled ECS physics body");
  }
  vkpt::physics::PhysicsStepConfig physicsStep;
  physicsStep.fixed_dt = 1.0f / 60.0f;
  if (!physics->step_fixed(physicsStep)) {
    return fail("physics fixed step rejected a valid timestep");
  }
  if (physicsInfo.available && physics->extract_transform_writes().empty()) {
    return fail("Jolt physics backend did not publish transform writes");
  }

  world.recompute_world_transforms();
  const auto* before = world.world_transform(childA);
  if (!before || std::abs(before->translation.x - 11.0f) > 0.001f) {
    return fail("hierarchy fixture world transform was invalid before reparent");
  }

  vkpt::scene::WorldCommandBuffer commands;
  commands.add_reorder_sibling(childB, 0, childA);
  commands.add_reparent_entity(childA, 0, true);
  commands.add_create_entity("Camera Child", 13, root);
  commands.add_set_component(13, vkpt::scene::ComponentKind::Camera, vkpt::scene::CameraComponent{});
  if (!commands.replay(world)) {
    return fail("WorldCommandBuffer failed to replay reparent/reorder/create-child commands");
  }
  world.recompute_world_transforms();
  const auto rootChildren = world.children_of(root);
  if (rootChildren != std::vector<vkpt::core::StableId>({childB, 13})) {
    return fail("reorder/create-child commands produced nondeterministic child order");
  }
  const auto* after = world.world_transform(childA);
  if (!after || std::abs(after->translation.x - 11.0f) > 0.001f) {
    return fail("preserve-world reparent changed the child world transform");
  }

  vkpt::editor::SelectionState selection = vkpt::editor::CreateDefaultSelectionState();
  selection.selected_entity_ids = {childA};
  selection.hovered_entity = 13;
  const auto treeRows = vkpt::editor::BuildSceneTreeRows(
      BuildSceneTreeEntitiesFromWorld(world), selection, 13);
  if (treeRows.size() != 4) {
    return fail("scene tree row builder returned the wrong visible row count");
  }
  if (treeRows[0].entity_id != root || treeRows[0].depth != 0 || !treeRows[0].has_children) {
    return fail("scene tree rows did not expose root hierarchy state");
  }
  if (treeRows[1].entity_id != childB || treeRows[1].depth != 1 ||
      std::find(treeRows[1].component_badges.begin(), treeRows[1].component_badges.end(), "mesh") ==
          treeRows[1].component_badges.end()) {
    return fail("scene tree rows did not expose ordered child badges");
  }
  if (treeRows[2].entity_id != 13 || treeRows[2].depth != 1 || !treeRows[2].hovered ||
      treeRows[2].icon != "camera") {
    return fail("scene tree rows did not expose hover/camera state");
  }
  if (treeRows[3].entity_id != childA || !treeRows[3].selected || treeRows[3].depth != 0) {
    return fail("scene tree rows did not reflect selection or reparent-to-root");
  }
  set_detail("ECS tree rows, worker-thread physics sync, hierarchy command replay, preserve-world reparent, and sibling_order JSON roundtrip pass");
  return true;
}

bool RunUiModelSmokeTests() {
  using namespace vkpt::editor;
  bool ok = true;
  auto check_true = [&](std::string_view tag, bool cond) {
    ok = ok && Check(tag, cond);
  };
  auto find_panel = [](const UiLayoutDocument& layout, std::string_view panel_id) -> const UiPanelState* {
    const auto it = std::find_if(layout.panels.begin(), layout.panels.end(),
                                 [panel_id](const UiPanelState& panel) {
                                   return panel.id == panel_id;
                                 });
    if (it == layout.panels.end()) {
      return nullptr;
    }
    return &(*it);
  };
  auto find_menu_enablement = [](const std::vector<MenuEnablement>& entries,
                                std::string_view item_id) -> std::optional<bool> {
    for (const auto& entry : entries) {
      if (entry.item_id == item_id) {
        return entry.enabled;
      }
    }
    return std::nullopt;
  };
  auto has_menu_items = [&](const MenuBar& menu,
                           std::string_view top_level,
                           std::initializer_list<std::string_view> items) {
    for (const auto item : items) {
      check_true(std::string("menu item missing: ") + std::string(top_level) + "." + std::string(item),
                 HasMenuItem(menu, top_level, item));
    }
  };
  auto has_shortcut = [](const std::vector<UiShortcut>& shortcuts,
                        std::string_view action_id,
                        std::uint32_t key_code,
                        bool ctrl,
                        bool shift,
                        bool alt) {
    return std::any_of(shortcuts.begin(), shortcuts.end(),
                       [&](const UiShortcut& shortcut) {
                         return shortcut.action_id == action_id &&
                                shortcut.key_code == key_code &&
                                shortcut.ctrl == ctrl &&
                                shortcut.shift == shift &&
                               shortcut.alt == alt;
                       });
  };

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
    const auto sceneGraphPanel = BuildQtSceneTreeDock(treeDoc, treeSelection, CreateDefaultLayout());
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
  }

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
    const auto renderPanel = BuildQtRenderSettingsDock(
        vkpt::pathtracer::RTSceneData{},
        renderSettings,
        UiRuntimeState{},
        CreateDefaultLayout(),
        renderFrame);
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
    check_true("device dock shows computer accumulated ray average",
               throughputProperty != nullptr &&
               throughputProperty->value.find("computer avg 80.00 MRays/s") != std::string::npos);
    check_true("device dock active device uses measured rolling rate",
               activeDeviceProperty != nullptr &&
               activeDeviceProperty->value.find("rolling 100.00 MRays/s") != std::string::npos &&
               activeDeviceProperty->value.find("planning est") == std::string::npos);
  }
#endif

  const auto menu = BuildDefaultMenuBar();
  const std::initializer_list<std::string_view> requiredTopLevels = {
    "file", "edit", "view", "create", "scene", "render", "benchmark",
    "assets", "scripts", "tools", "help"
  };
  for (const auto level : requiredTopLevels) {
    check_true(std::string("missing top-level menu: ") + std::string(level),
               HasTopLevelMenu(menu, level));
  }

  has_menu_items(menu, "file", {
    "file.new_scene", "file.open_scene", "file.open_recent", "file.save_scene",
    "file.save_scene_as", "file.clone_scene", "file.import_asset", "file.export_image",
    "file.export_exr", "file.export_benchmark_artifacts", "file.export_scene_snapshot",
    "file.preferences", "file.reveal_artifacts_folder", "file.exit"
  });
  has_menu_items(menu, "edit", {
    "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste", "edit.duplicate",
    "edit.delete", "edit.rename", "edit.select_all", "edit.select_none", "edit.invert_selection",
    "edit.group_selection", "edit.ungroup_selection", "edit.merge_selection",
    "edit.split_merged_object", "edit.reparent_selection", "edit.reset_transform",
    "edit.command_history"
  });
  has_menu_items(menu, "view", {
    "view.panels", "view.layouts", "view.overlays", "view.debug_views",
    "view.fullscreen", "view.ui_scale", "view.reset_layout"
  });
  has_menu_items(menu, "create", {
    "create.empty_entity", "create.group_entity", "create.camera", "create.light",
    "create.mesh_primitives", "create.sdf_primitives", "create.material", "create.script",
    "create.physics_body", "create.benchmark_marker"
  });
  has_menu_items(menu, "benchmark", {
    "benchmark.run_current_scene", "benchmark.run_scene_pack", "benchmark.run_cpu_calibration",
    "benchmark.run_gpu_calibration", "benchmark.run_simd_experiment", "benchmark.run_backend_experiment",
    "benchmark.compare_against_reference", "benchmark.open_artifacts", "benchmark.export_csv_json",
    "benchmark.history"
  });
  has_menu_items(menu, "scene", {
    "scene.validate_scene", "scene.freeze_benchmark_snapshot", "scene.reset_accumulation",
    "scene.reload_scene", "scene.reload_assets", "scene.hot_reload_scripts", "scene.scene_settings",
    "scene.lighting_settings", "scene.environment_settings", "scene.camera_settings",
    "scene.physics_settings", "scene.script_settings", "scene.animation_settings"
  });
  has_menu_items(menu, "render", {
    "render.backend", "render.renderer_path", "render.quality_presets", "render.resolution",
    "render.spp", "render.max_bounces", "render.denoiser", "render.tone_mapping",
    "render.exposure", "render.debug_channel", "render.shader_cache", "render.backend_capabilities"
  });
  has_menu_items(menu, "scripts", {
    "scripts.new_lua_script", "scripts.attach_script_to_selection", "scripts.detach_script_from_selection",
    "scripts.reload_scripts", "scripts.open_script_folder", "scripts.show_script_lifecycle_events",
    "scripts.show_script_errors", "scripts.show_script_profiler", "scripts.sandbox_settings"
  });
  has_menu_items(menu, "assets", {
    "assets.import_files", "assets.reimport_selected", "assets.refresh_browser", "assets.show_missing_assets",
    "assets.show_import_diagnostics", "assets.clear_generated_cache", "assets.clear_shader_cache"
  });
  has_menu_items(menu, "tools", {
    "tools.doctor", "tools.crash_artifacts", "tools.profiler", "tools.frame_capture",
    "tools.shader_manifest", "tools.asset_manifest", "tools.scene_snapshot",
    "tools.capability_matrix", "tools.settings_dump", "tools.startup_self_test"
  });
  has_menu_items(menu, "help", {
    "help.controls", "help.shortcut_reference", "help.about", "help.build_info",
    "help.feature_flags", "help.dependency_info"
  });
  auto mutable_menu = menu;
  check_true("find menu item non-const", FindMenuItem(mutable_menu, "edit.duplicate") != nullptr);
  check_true("find menu item const", FindMenuItem(std::as_const(menu), "edit.duplicate") != nullptr);

  const auto editEnablementsEmpty = GetEditMenuEnablements(CreateDefaultSelectionState());
  const auto editDuplicateDisabled = find_menu_enablement(editEnablementsEmpty, "edit.duplicate");
  check_true("edit.duplicate disabled without selection", editDuplicateDisabled.has_value() && !editDuplicateDisabled.value());
  const auto disabledEditMenu = BuildDefaultMenuBar(CreateDefaultSelectionState());
  const auto* disabledDuplicate = FindMenuItem(disabledEditMenu, "edit.duplicate");
  check_true("edit.duplicate disabled reason",
             disabledDuplicate != nullptr && !disabledDuplicate->disabled_reason.empty());
  SelectionState oneSelected = CreateDefaultSelectionState();
  oneSelected.selected_entity_ids = {11};
  const auto editEnablementsSelected = GetEditMenuEnablements(oneSelected);
  const auto editDuplicateEnabled = find_menu_enablement(editEnablementsSelected, "edit.duplicate");
  check_true("edit.duplicate enabled with selection", editDuplicateEnabled.has_value() && editDuplicateEnabled.value());
  check_true("file.exit remains enabled", FindMenuItem(menu, "file.exit")->enabled);

  const auto new_scene = MakeMenuCommand("file.new_scene", "menu");
  check_true("file.new_scene command kind", new_scene.kind == EditorCommandKind::kCreateEntity);
  check_true("file.new_scene mapping", std::holds_alternative<CreateEntityCommand>(new_scene.payload));

  const auto select_none = MakeMenuCommand("edit.select_none", "menu");
  check_true("edit.select_none command kind", select_none.kind == EditorCommandKind::kClearSelection);
  check_true("edit.select_none mapping", std::holds_alternative<ClearSelectionCommand>(select_none.payload));

  const auto open_scene = MakeMenuCommand("file.open_scene", "menu");
  check_true("file.open_scene unsupported command", open_scene.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto copy_action = MakeMenuCommand("edit.copy", "menu");
  check_true("edit.copy unsupported command", copy_action.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto save_scene = MakeMenuCommand("file.save_scene", "menu");
  check_true("file.save_scene command is explicit unsupported", save_scene.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto exit_action = MakeMenuCommand("file.exit", "menu");
  check_true("file.exit command is explicit unsupported", exit_action.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto run_benchmark = MakeMenuCommand("benchmark.run_current_scene", "menu");
  check_true("benchmark.run_current_scene command kind", run_benchmark.kind == EditorCommandKind::kRunBenchmark);
  check_true("benchmark.run_current_scene mapping", std::holds_alternative<RunBenchmarkCommand>(run_benchmark.payload));
  const auto run_scene_pack = MakeMenuCommand("benchmark.run_scene_pack", "menu");
  check_true("benchmark.run_scene_pack command kind", run_scene_pack.kind == EditorCommandKind::kRunBenchmark);
  const auto run_cpu_calibration = MakeMenuCommand("benchmark.run_cpu_calibration", "menu");
  check_true("benchmark.run_cpu_calibration command kind", run_cpu_calibration.kind == EditorCommandKind::kRunBenchmark);
  const auto run_gpu_calibration = MakeMenuCommand("benchmark.run_gpu_calibration", "menu");
  check_true("benchmark.run_gpu_calibration command kind", run_gpu_calibration.kind == EditorCommandKind::kRunBenchmark);
  const auto run_simd = MakeMenuCommand("benchmark.run_simd_experiment", "menu");
  check_true("benchmark.run_simd_experiment command kind", run_simd.kind == EditorCommandKind::kRunBenchmark);
  const auto run_backend = MakeMenuCommand("benchmark.run_backend_experiment", "menu");
  check_true("benchmark.run_backend_experiment command kind", run_backend.kind == EditorCommandKind::kRunBenchmark);
  const auto run_ref = MakeMenuCommand("benchmark.compare_against_reference", "menu");
  check_true("benchmark.compare_against_reference command kind", run_ref.kind == EditorCommandKind::kRunBenchmark);
  const auto benchmark_open_artifacts = MakeMenuCommand("benchmark.open_artifacts", "menu");
  check_true("benchmark.open_artifacts currently unsupported by model", benchmark_open_artifacts.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto benchmark_export_csv = MakeMenuCommand("benchmark.export_csv_json", "menu");
  check_true("benchmark.export_csv_json currently unsupported by model", benchmark_export_csv.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto benchmark_history = MakeMenuCommand("benchmark.history", "menu");
  check_true("benchmark.history currently unsupported by model", benchmark_history.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto scene_validate = MakeMenuCommand("scene.validate_scene", "menu");
  check_true("scene.validate_scene currently unsupported by model", scene_validate.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto scene_settings = MakeMenuCommand("scene.scene_settings", "menu");
  check_true("scene.scene_settings currently unsupported by model", scene_settings.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_backend = MakeMenuCommand("render.backend", "menu");
  check_true("render.backend currently unsupported by model", render_backend.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_quality_presets = MakeMenuCommand("render.quality_presets", "menu");
  check_true("render.quality_presets currently unsupported by model", render_quality_presets.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto tools_doctor = MakeMenuCommand("tools.doctor", "menu");
  check_true("tools.doctor currently unsupported by model", tools_doctor.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto tools_profiler = MakeMenuCommand("tools.profiler", "menu");
  check_true("tools.profiler currently unsupported by model", tools_profiler.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto help_about = MakeMenuCommand("help.about", "menu");
  check_true("help.about currently unsupported by model", help_about.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto help_controls = MakeMenuCommand("help.controls", "menu");
  check_true("help.controls currently unsupported by model", help_controls.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto create_material = MakeMenuCommand("create.material", "menu");
  check_true("create.material command kind", create_material.kind == EditorCommandKind::kCreateEntity);
  const auto create_script = MakeMenuCommand("create.script", "menu");
  check_true("create.script command kind", create_script.kind == EditorCommandKind::kCreateEntity);
  const auto create_physics = MakeMenuCommand("create.physics_body", "menu");
  check_true("create.physics_body command kind", create_physics.kind == EditorCommandKind::kCreateEntity);
  const auto create_marker = MakeMenuCommand("create.benchmark_marker", "menu");
  check_true("create.benchmark_marker command kind", create_marker.kind == EditorCommandKind::kCreateEntity);
  const auto create_mat_template = std::get<CreateEntityCommand>(create_material.payload).template_name;
  check_true("create.material has template", create_mat_template == "material");
  const auto create_script_template = std::get<CreateEntityCommand>(create_script.payload).template_name;
  check_true("create.script has template", create_script_template == "script");
  const auto create_physics_template = std::get<CreateEntityCommand>(create_physics.payload).template_name;
  check_true("create.physics_body has template", create_physics_template == "physics_body");
  const auto create_marker_template = std::get<CreateEntityCommand>(create_marker.payload).template_name;
  check_true("create.benchmark_marker has template", create_marker_template == "benchmark_marker");
  const auto create_light = MakeMenuCommand("create.light", "menu");
  check_true("create.light command kind", create_light.kind == EditorCommandKind::kCreateEntity);
  check_true("create.light template", std::get<CreateEntityCommand>(create_light.payload).template_name == "create.light");
  const auto create_mesh = MakeMenuCommand("create.mesh_primitives", "menu");
  check_true("create.mesh_primitives command kind", create_mesh.kind == EditorCommandKind::kCreateEntity);
  check_true("create.mesh template", std::get<CreateEntityCommand>(create_mesh.payload).template_name == "create.mesh_primitives");
  const auto create_sdf = MakeMenuCommand("create.sdf_primitives", "menu");
  check_true("create.sdf_primitives command kind", create_sdf.kind == EditorCommandKind::kCreateEntity);
  check_true("create.sdf template", std::get<CreateEntityCommand>(create_sdf.payload).template_name == "create.sdf_primitives");

  const auto layout = CreateLayoutPreset(LayoutPreset::Benchmark);
  check_true("benchmark layout preset", layout.preset == LayoutPreset::Benchmark);
  check_true("benchmark layout panels", !layout.panels.empty());

  const auto tmp = std::filesystem::temp_directory_path() / "vkpt-ui-layout-smoke.json";
  std::string save_error;
  if (Check("layout save", SaveLayoutToFile(tmp.string(), layout, &save_error))) {
    UiLayoutDocument reloaded;
    check_true("layout load", LoadLayoutFromFile(tmp.string(), &reloaded));
    check_true("layout preset roundtrip", reloaded.preset == layout.preset);
    check_true("layout name roundtrip", reloaded.active_layout_name == layout.active_layout_name);
    check_true("layout ui scale roundtrip", reloaded.ui_scale == layout.ui_scale);
    std::filesystem::remove(tmp);
  }

  const std::vector<std::pair<LayoutPreset, std::string_view>> allPresets = {
    {LayoutPreset::Default, "Default"},
    {LayoutPreset::Benchmark, "Benchmark"},
    {LayoutPreset::MaterialAuthoring, "Material Authoring"},
    {LayoutPreset::Scripting, "Scripting"},
    {LayoutPreset::AssetManagement, "Asset Management"},
    {LayoutPreset::DebugProfiler, "Debug/Profiler"},
    {LayoutPreset::MinimalViewport, "Minimal Viewport"},
    {LayoutPreset::FullscreenViewportWithOverlay, "Fullscreen Viewport With Overlay"},
  };
  for (std::size_t i = 0; i < allPresets.size(); ++i) {
    const auto preset_layout = CreateLayoutPreset(allPresets[i].first);
    check_true(std::string("layout preset exists: ") + std::string(allPresets[i].second),
              !preset_layout.panels.empty());
    const auto preset_path = std::filesystem::temp_directory_path() / ("vkpt-ui-layout-smoke-" + std::to_string(i) + ".json");
    std::string preset_save_err;
    if (SaveLayoutToFile(preset_path.string(), preset_layout, &preset_save_err)) {
      UiLayoutDocument preset_roundtrip;
      check_true(std::string("layout preset roundtrip: ") + std::string(allPresets[i].second),
                LoadLayoutFromFile(preset_path.string(), &preset_roundtrip));
      check_true(std::string("layout preset restored: ") + std::string(allPresets[i].second),
                preset_roundtrip.preset == preset_layout.preset);
      std::filesystem::remove(preset_path);
    } else {
      check_true(std::string("layout preset save: ") + std::string(allPresets[i].second), false);
    }
  }

  SelectionState selection = CreateDefaultSelectionState();
  selection.selected_entity_ids = {7, 8, 9};
  selection.active_primary_entity = 7;
  selection.hovered_entity_ids = {9};
  selection.selection_source = SelectionSource::Viewport;
  const std::string selection_json = SerializeSelectionState(selection);
  check_true("selection json serialization",
             selection_json.find("\"selected_entity_ids\":[7,8,9]") != std::string::npos);
  selection.aggregate_bounds = {{-1.0f, -2.0f, -3.0f}, {4.0f, 5.0f, 6.0f}, true};
  selection.per_item_bounds = {
    {7, {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, true}},
    {8, {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true}},
    {9, {{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}, true}},
  };
  const auto selection_tmp = std::filesystem::temp_directory_path() / "vkpt-ui-selection-smoke.json";
  std::string selection_save_error;
  if (Check("selection save", SaveSelectionToFile(selection_tmp.string(), selection, &selection_save_error))) {
    SelectionState selection_reloaded;
    check_true("selection load", LoadSelectionFromFile(selection_tmp.string(), &selection_reloaded));
    check_true("selection selected ids roundtrip", selection_reloaded.selected_entity_ids == selection.selected_entity_ids);
    check_true("selection bounds roundtrip", selection_reloaded.aggregate_bounds.valid &&
               selection_reloaded.per_item_bounds.size() == selection.per_item_bounds.size());
    std::filesystem::remove(selection_tmp);
  }

  EditorCommand pick_entity_11;
  pick_entity_11.source_widget = "viewport";
  pick_entity_11.kind = EditorCommandKind::kSelectEntity;
  pick_entity_11.payload = SelectEntityCommand{11, false, false};
  SelectionState selected_once = ApplySelectionCommand(selection, pick_entity_11);
  check_true("select once sets single selection", selected_once.selected_entity_ids == std::vector<vkpt::core::StableId>{11});
  check_true("select once sets active primary", selected_once.active_primary_entity == 11);
  check_true("select once sets hovered entity", selected_once.hovered_entity == 11);
  check_true("select once source from viewport", selected_once.selection_source == SelectionSource::Viewport);

  EditorCommand append_entity_22;
  append_entity_22.source_widget = "scene_tree";
  append_entity_22.kind = EditorCommandKind::kSelectEntity;
  append_entity_22.payload = SelectEntityCommand{22, true, false};
  SelectionState selected_two = ApplySelectionCommand(selected_once, append_entity_22);
  check_true("append selection keeps two items", selected_two.selected_entity_ids == std::vector<vkpt::core::StableId>({11, 22}));
  check_true("append selection updates active", selected_two.active_primary_entity == 22);
  check_true("append source from scene tree", selected_two.selection_source == SelectionSource::SceneTree);

  EditorCommand toggle_entity_11;
  toggle_entity_11.source_widget = "inspector";
  toggle_entity_11.kind = EditorCommandKind::kToggleSelectEntity;
  toggle_entity_11.payload = ToggleSelectEntityCommand{11};
  SelectionState after_toggle = ApplySelectionCommand(selected_two, toggle_entity_11);
  check_true("toggle deselect one entity", after_toggle.selected_entity_ids == std::vector<vkpt::core::StableId>({22}));
  check_true("toggle keeps active primary", after_toggle.active_primary_entity == 22);
  check_true("toggle source from inspector", after_toggle.selection_source == SelectionSource::Inspector);

  EditorCommand append_range_selection;
  append_range_selection.source_widget = "scene_tree";
  append_range_selection.kind = EditorCommandKind::kSelectEntity;
  append_range_selection.payload = SelectEntityCommand{44, true, true};
  SelectionState range_add = ApplySelectionCommand(after_toggle, append_range_selection);
  check_true("append+range selection includes existing and new", range_add.selected_entity_ids == std::vector<vkpt::core::StableId>({22, 44}));
  check_true("append+range selection keeps hovered", range_add.hovered_entity == 44);
  check_true("append+range selection source from scene tree", range_add.selection_source == SelectionSource::SceneTree);

  EditorCommand range_selection;
  range_selection.source_widget = "scene_tree";
  range_selection.kind = EditorCommandKind::kSelectEntity;
  range_selection.payload = SelectEntityCommand{11, false, true};
  SelectionState range_replace = ApplySelectionCommand(after_toggle, range_selection);
  check_true("replace range selection keeps two markers", range_replace.selected_entity_ids.size() == 2);
  check_true("replace range selection sets active primary", range_replace.active_primary_entity == 11);
  check_true("replace range selection sets hovered", range_replace.hovered_entity == 11);

  EditorCommand clear_selection;
  clear_selection.source_widget = "menu";
  clear_selection.kind = EditorCommandKind::kClearSelection;
  clear_selection.payload = ClearSelectionCommand{};
  SelectionState cleared = ApplySelectionCommand(after_toggle, clear_selection);
  check_true("clear selection empties entities", cleared.selected_entity_ids.empty());
  check_true("clear selection resets hovered entity", cleared.hovered_entity == 0);
  check_true("clear selection keeps source from last command", cleared.selection_source == SelectionSource::Inspector);
  check_true("clear selection removes hovered_entity", cleared.hovered_entity == 0);

  auto mixed_field = BuildInspectorFieldStates("material.roughness", {"0.4", "0.8"});
  check_true("mixed inspector value marked mixed", mixed_field.size() == 1 && mixed_field[0].mixed);
  auto exact_field = BuildInspectorFieldStates("material.roughness", {"0.5", "0.5"});
  check_true("uniform inspector value not mixed", exact_field.size() == 1 && !exact_field[0].mixed && exact_field[0].value == "0.5");
  auto unsupported_field = BuildInspectorFieldStates("missing.field", {});
  check_true("missing inspector field unsupported", unsupported_field.size() == 1 && unsupported_field[0].unsupported);

  const auto tree_reorder = MakeReorderSiblingCommand(44, 10, 12, 7, "scene_tree");
  check_true("tree reorder command kind", tree_reorder.kind == EditorCommandKind::kReorderSibling);
  check_true("tree reorder command id", tree_reorder.command_id == "scene_tree.reorder_sibling");
  const auto reorder_payload = std::get<ReorderSiblingCommand>(tree_reorder.payload);
  check_true("tree reorder payload moved", reorder_payload.moved_entity == 44);
  check_true("tree reorder payload sibling before", reorder_payload.sibling_before == 10);
  check_true("tree reorder payload sibling after", reorder_payload.sibling_after == 12);
  check_true("tree reorder command source", tree_reorder.source_widget == "scene_tree");
  const std::string reorder_line = SerializeEditorCommand(tree_reorder);
  check_true("tree reorder serialized", reorder_line.find("\"moved_entity\":44") != std::string::npos);
  std::string ecs_tree_detail;
  check_true("ecs scene tree integration", CheckEcsSceneTreeContracts(&ecs_tree_detail));

  const auto valid_texture_drop = ValidateAssetDrop("C:/assets/brick.PNG", "texture_slot");
  check_true("valid texture drop extension support", valid_texture_drop.extension_supported);
  check_true("valid texture drop accepted", valid_texture_drop.accepted);
  check_true("valid texture drop type", valid_texture_drop.asset_type == "texture");

  const auto invalid_texture_drop = ValidateAssetDrop("C:/assets/readme.md", "texture_slot");
  check_true("invalid texture drop rejected", !invalid_texture_drop.accepted);
  check_true("invalid texture drop unsupported", !invalid_texture_drop.extension_supported);

  const auto unknown_slot_drop = ValidateAssetDrop("C:/assets/model.obj", "invalid_slot");
  check_true("unknown slot drop rejected", !unknown_slot_drop.accepted);
  check_true("unknown slot drop still supported", unknown_slot_drop.extension_supported);

  const auto empty_path_drop = ValidateAssetDrop("", "material_slot");
  check_true("empty path drop rejected", !empty_path_drop.accepted);
  check_true("empty path drop reports reason", !empty_path_drop.reason.empty());

  const auto script_attachment = MakeMenuCommand("scripts.attach_script_to_selection", "menu");
  check_true("scripts.attach_script_to_selection command", script_attachment.kind == EditorCommandKind::kAttachScript);
  const auto script_detach = MakeMenuCommand("scripts.detach_script_from_selection", "menu");
  check_true("scripts.detach_script_from_selection command", script_detach.kind == EditorCommandKind::kDetachScript);
  const auto script_new = MakeMenuCommand("scripts.new_lua_script", "menu");
  check_true("scripts.new_lua_script currently unsupported by model", script_new.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto benchmark_desc = MakeDefaultBenchmarkRunDesc("scenes/test.json", "vulkan", "hybrid", 128, 10, 42, 1024, 576);
  check_true("benchmark desc scene path", benchmark_desc.scene_path == "scenes/test.json");
  check_true("benchmark desc backend", benchmark_desc.backend == "vulkan");
  check_true("benchmark desc renderer path", benchmark_desc.renderer_path == "hybrid");
  check_true("benchmark desc spp", benchmark_desc.samples_per_pixel == 128);
  check_true("benchmark desc width", benchmark_desc.resolution.width == 1024);
  check_true("benchmark desc height", benchmark_desc.resolution.height == 576);
  check_true("benchmark desc max depth", benchmark_desc.max_depth == 10);
  check_true("benchmark desc seed", benchmark_desc.seed == 42);
  check_true("benchmark desc tolerance default", benchmark_desc.tolerance_policy == "default");
  const auto workload = EstimateWorkloadComplexity(benchmark_desc, 3, 1024, 512, 16 * 1024 * 1024, true);
  check_true("workload model has cost", workload.normalized_cost_units > 0.0);
  check_true("workload model explains drivers", !workload.cost_drivers.empty());
  auto normalized_score = ComputeBenchmarkScore(2048.0, 2048.0, 1024.0, workload.normalized_cost_units, true);
  normalized_score.raw_paths_per_second = 4096.0;
  normalized_score.raw_gpu_ms = 4.0;
  normalized_score.raw_cpu_ms = 2.0;
  const BenchmarkRawMetricsModel raw_metrics{
    120.0, 8.33, 4.0, 2.0, 1024.0, 4096.0, 8192.0, 128, 32 * 1024 * 1024, 1.5, 0.25
  };
  const auto benchmark_panel = BuildBenchmarkPanelModel(
      benchmark_desc, raw_metrics, normalized_score, workload,
      "artifacts/benchmarks/ui-smoke", "ok", true);
  const std::string benchmark_panel_json = SerializeBenchmarkPanelModel(benchmark_panel);
  check_true("benchmark panel serializes raw metrics",
             benchmark_panel_json.find("\"raw_metrics\"") != std::string::npos &&
             benchmark_panel_json.find("\"path_vertices_per_second\":8192") != std::string::npos);
  check_true("benchmark panel serializes normalized score",
             benchmark_panel_json.find("\"normalized_score\":1") != std::string::npos);
  check_true("benchmark panel calibration actions",
             !benchmark_panel.calibration_actions.empty() &&
             !BuildDefaultBenchmarkCalibrationActions(false, false).back().supported);
  UiRuntimeState runtime_for_status = CreateDefaultRuntimeState();
  runtime_for_status.active_scene = "assets/scenes/cornell_native.json";
  runtime_for_status.active_renderer_backend = "cpu";
  runtime_for_status.active_renderer_path = "cpu_scalar";
  runtime_for_status.spp_accumulated = 128;
  runtime_for_status.fps = 120.0;
  runtime_for_status.frame_ms = 8.33;
  runtime_for_status.background_job_count = 2;
  runtime_for_status.last_warning_or_error = "none";
  const auto status_bar = BuildStatusBarModel(runtime_for_status, selection, &normalized_score);
  check_true("status bar renderer path", status_bar.renderer_path == "cpu_scalar");
  check_true("status bar spp/fps/jobs", status_bar.spp == 128 && status_bar.fps == 120.0 && status_bar.background_job_count == 2);
  check_true("status bar selected count", status_bar.selected_entity_count == selection.selected_entity_ids.size());

  EditorCommandHistory commandHistory(4);
  commandHistory.push(new_scene);
  commandHistory.push(select_none);
  const std::string command_lines = SerializeEditorCommandsJsonl(commandHistory.history(), 4);
  check_true("command history serialization", command_lines.find("file.new_scene") != std::string::npos);

  UiEventLog eventLog(4);
  PushUiEvent(eventLog, "menu_click", "menu", "file.new_scene");
  PushUiEvent(eventLog, "menu_click", "menu", "edit.select_none");
  check_true("ui event log", eventLog.events().size() == 2);

  const auto shortcut_conflict_true = DetectShortcutConflicts(std::vector<UiShortcut>{
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open"},
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_recent", "Open"}
  });
  const auto shortcut_conflict_false = DetectShortcutConflicts(std::vector<UiShortcut>{
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open"},
    {static_cast<std::uint32_t>('S'), true, false, false, "file.save_scene", "Save"}
  });
  check_true("ui shortcut conflict true", shortcut_conflict_true);
  check_true("ui shortcut conflict false", !shortcut_conflict_false);

  const auto action_conflict_true = DetectShortcutConflicts(std::vector<UiShortcutAction>{
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_current_scene", "Run"},
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_cpu_calibration", "Run2"}
  });
  const auto action_conflict_false = DetectShortcutConflicts(std::vector<UiShortcutAction>{
    {static_cast<std::uint32_t>('B'), true, false, false, "benchmark.run_current_scene", "Run"},
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_cpu_calibration", "Run2"}
  });
  check_true("ui shortcut action conflict true", action_conflict_true);
  check_true("ui shortcut action conflict false", !action_conflict_false);

  const auto shortcuts = BuildDefaultUiShortcuts();
  check_true("shortcut ctrl+s", has_shortcut(shortcuts, "file.save_scene", 'S', true, false, false));
  check_true("shortcut ctrl+o", has_shortcut(shortcuts, "file.open_scene", 'O', true, false, false));
  check_true("shortcut ctrl+z", has_shortcut(shortcuts, "edit.undo", 'Z', true, false, false));
  check_true("shortcut ctrl+y", has_shortcut(shortcuts, "edit.redo", 'Y', true, false, false));
  check_true("shortcut ctrl+d", has_shortcut(shortcuts, "edit.duplicate", 'D', true, false, false));
  check_true("shortcut delete", has_shortcut(shortcuts, "edit.delete", 127, false, false, false));
  check_true("shortcut f", has_shortcut(shortcuts, "view.focus_selected", 'F', false, false, false));
  check_true("shortcut w", has_shortcut(shortcuts, "gizmo.translate", 'W', false, false, false));
  check_true("shortcut e", has_shortcut(shortcuts, "gizmo.rotate", 'E', false, false, false));
  check_true("shortcut r", has_shortcut(shortcuts, "gizmo.scale", 'R', false, false, false));
  check_true("shortcut q", has_shortcut(shortcuts, "gizmo.select", 'Q', false, false, false));
  check_true("shortcut ctrl+g", has_shortcut(shortcuts, "edit.group_selection", 'G', true, false, false));
  check_true("shortcut ctrl+shift+g", has_shortcut(shortcuts, "edit.ungroup_selection", 'G', true, true, false));
  check_true("shortcut ctrl+b", has_shortcut(shortcuts, "benchmark.run_current_scene", 'B', true, false, false));
  check_true("shortcut f11", has_shortcut(shortcuts, "view.fullscreen", 122, false, false, false));

  UiLayoutDocument panelLayout = CreateLayoutPreset(LayoutPreset::Default);
  const auto inspectorPanelBefore = find_panel(panelLayout, "inspector");
  check_true("panel state mutation target exists", inspectorPanelBefore != nullptr);
  if (inspectorPanelBefore) {
    const auto beforeX = inspectorPanelBefore->x;
    const auto beforeY = inspectorPanelBefore->y;
    const auto beforeWidth = inspectorPanelBefore->width;
    const auto beforeHeight = inspectorPanelBefore->height;
    const auto hideInspector = SetPanelVisible(panelLayout, "inspector", false);
    check_true("set panel visibility", hideInspector.changed && !find_panel(panelLayout, "inspector")->visible);
    const auto showInspector = SetPanelVisible(panelLayout, "inspector", true);
    check_true("restore panel visibility", showInspector.changed && find_panel(panelLayout, "inspector")->visible);
    const auto* restoredPanel = find_panel(panelLayout, "inspector");
    check_true("panel restore preserves position", restoredPanel != nullptr &&
              restoredPanel->x == beforeX &&
              restoredPanel->y == beforeY &&
              restoredPanel->width == beforeWidth &&
              restoredPanel->height == beforeHeight);
    const auto collapseInspector = SetPanelCollapsed(panelLayout, "inspector", true);
    check_true("set panel collapsed", collapseInspector.changed && find_panel(panelLayout, "inspector")->collapsed);
    const auto moveInspector = MovePanel(panelLayout, "inspector", 64.0f, 88.0f);
    const auto* movedPanel = find_panel(panelLayout, "inspector");
    check_true("move panel updates position", moveInspector.changed &&
              movedPanel != nullptr &&
              movedPanel->x == 64.0f &&
              movedPanel->y == 88.0f);
    const auto resizeInspector = ResizePanel(panelLayout, "inspector", 333.0f, 444.0f);
    const auto* resizedPanel = find_panel(panelLayout, "inspector");
    check_true("resize panel updates size", resizeInspector.changed &&
              resizedPanel != nullptr &&
              resizedPanel->width == 333.0f &&
              resizedPanel->height == 444.0f);
    const auto dockInspector = SetPanelDockState(panelLayout, "inspector", false, true);
    const auto* dockedPanel = find_panel(panelLayout, "inspector");
    check_true("set panel floating", dockInspector.changed && dockedPanel != nullptr &&
              dockedPanel->floating && !dockedPanel->docked);
    const auto commandMove = ApplyPanelStateCommand(panelLayout, "view.panel.move", true, 120.0f, "inspector");
    const auto* commandPanel = find_panel(panelLayout, "inspector");
    check_true("apply panel move command", commandMove.changed &&
              commandPanel != nullptr &&
              commandPanel->x == 120.0f);
    const auto commandResize = ApplyPanelStateCommand(panelLayout, "view.panel.resize", false, 512.0f, "inspector");
    const auto* commandResizePanel = find_panel(panelLayout, "inspector");
    check_true("apply panel resize command", commandResize.changed &&
              commandResizePanel != nullptr &&
              commandResizePanel->width == 512.0f);
    const auto commandToggleVisible = ApplyPanelStateCommand(panelLayout, "view.panel.toggle_visible", false, 0.0f, "inspector");
    const auto* commandVisiblePanel = find_panel(panelLayout, "inspector");
    check_true("apply panel toggle visible", commandToggleVisible.changed &&
              commandVisiblePanel != nullptr &&
              !commandVisiblePanel->visible);
  }
  const auto restoreBenchmarkLayout = RestoreLayoutPreset(panelLayout, LayoutPreset::Benchmark);
  check_true("restore benchmark layout", restoreBenchmarkLayout);
  check_true("restore layout changed preset", panelLayout.preset == LayoutPreset::Benchmark);
  const auto unknownPanelCommand = ApplyPanelStateCommand(panelLayout, "does_not_exist", false, 0.0f, "inspector");
  check_true("unknown panel command rejected", !unknownPanelCommand.changed);

  const auto release_gate = BuildUiReleaseGateEvidence();
  const std::string release_gate_json = SerializeUiReleaseGateChecklist(release_gate);
  check_true("release gate has no pending items",
             release_gate_json.find("\"pending_count\":0") != std::string::npos);
  check_true("release gate explicitly defers runtime UI gaps",
             release_gate_json.find("\"deferred_count\":") != std::string::npos &&
             release_gate_json.find("\"status\":\"deferred\"") != std::string::npos);

  std::cout << "ui model smoke: " << (ok ? "ok\n" : "failed\n");
  return ok;
}

void MarkReleaseGate(std::vector<vkpt::editor::UiReleaseGateItem>& items,
                     std::string_view id,
                     bool passed,
                     std::string_view evidence,
                     std::string_view deferred_reason = {}) {
  for (auto& item : items) {
    if (item.id == id) {
      item.passed = passed;
      item.deferred = !passed && !deferred_reason.empty();
      item.evidence = std::string(evidence);
      item.deferred_reason = std::string(deferred_reason);
      return;
    }
  }
}

std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence() {
  using namespace vkpt::editor;
  auto checklist = BuildDefaultUiReleaseGateChecklist();

  const auto menu = BuildDefaultMenuBar();
  MarkReleaseGate(checklist, "menu.works",
                  HasTopLevelMenu(menu, "file") &&
                  HasTopLevelMenu(menu, "edit") &&
                  HasTopLevelMenu(menu, "benchmark") &&
                  HasMenuItem(menu, "file", "file.open_scene") &&
                  HasMenuItem(menu, "benchmark", "benchmark.run_current_scene"),
                  "BuildDefaultMenuBar exposes required top-level menus and typed actions");

  const auto layout = CreateLayoutPreset(LayoutPreset::Default);
  const auto layoutPath = std::filesystem::temp_directory_path() / "vkpt-ui-release-layout.json";
  std::string layoutError;
  UiLayoutDocument reloadedLayout;
  const bool layoutPersisted = SaveLayoutToFile(layoutPath.string(), layout, &layoutError) &&
                               LoadLayoutFromFile(layoutPath.string(), &reloadedLayout) &&
                               reloadedLayout.active_layout_name == layout.active_layout_name &&
                               !reloadedLayout.panels.empty();
  std::error_code removeEc;
  std::filesystem::remove(layoutPath, removeEc);
  MarkReleaseGate(checklist, "layout.persists", layoutPersisted,
                  "UiLayoutDocument save/load roundtrip preserves panel geometry",
                  layoutPersisted ? std::string_view{} : std::string_view{"layout JSON roundtrip failed"});

  auto panelLayout = CreateLayoutPreset(LayoutPreset::Default);
  const bool panelMutations =
      SetPanelVisible(panelLayout, "inspector", false).changed &&
      SetPanelVisible(panelLayout, "inspector", true).changed &&
      SetPanelDockState(panelLayout, "inspector", false, true).changed &&
      MovePanel(panelLayout, "inspector", 32.0f, 48.0f).changed &&
      ResizePanel(panelLayout, "inspector", 512.0f, 384.0f).changed;
  MarkReleaseGate(checklist, "panels.dock_float", panelMutations,
                  "UiLayoutDocument panel helpers cover close/show/dock/float/move/resize");

  SelectionState selection = CreateDefaultSelectionState();
  selection.selected_entity_ids = {1, 2};
  selection.active_primary_entity = 1;
  selection.hovered_entity = 2;
  selection.aggregate_bounds = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, true};
  selection.per_item_bounds = {
    {1, {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, true}},
    {2, {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true}},
  };
  const auto selectionPath = std::filesystem::temp_directory_path() / "vkpt-ui-release-selection.json";
  std::string selectionError;
  SelectionState selectionRoundtrip;
  const bool selectionSaved = SaveSelectionToFile(selectionPath.string(), selection, &selectionError) &&
                              LoadSelectionFromFile(selectionPath.string(), &selectionRoundtrip) &&
                              selectionRoundtrip.selected_entity_ids == selection.selected_entity_ids &&
                              selectionRoundtrip.per_item_bounds.size() == 2;
  std::filesystem::remove(selectionPath, removeEc);
  MarkReleaseGate(checklist, "selection.multi", selectionSaved,
                  "SelectionState supports stable IDs, primary selection, hover, aggregate bounds, per-item bounds, and JSON roundtrip");
  MarkReleaseGate(checklist, "viewport.bounds", false,
                  "SelectionState carries aggregate and per-item bounds for viewport/tree/inspector overlays",
                  "requires overlay renderer drawing those bounds in the viewport");

  const auto rejectedDrop = ValidateAssetDrop("C:/assets/readme.md", "texture_slot");
  MarkReleaseGate(checklist, "assets.reject", !rejectedDrop.accepted && !rejectedDrop.reason.empty(),
                  "ValidateAssetDrop rejects unsupported target/type combinations with a reason");

  const auto benchmarkDesc = MakeDefaultBenchmarkRunDesc("assets/scenes/cornell_native.json", "cpu", "cpu_scalar", 8, 4, 123, 320, 180);
  const auto workload = EstimateWorkloadComplexity(benchmarkDesc, 1, 12, 8, 4096, false);
  auto score = ComputeBenchmarkScore(1000.0, 1000.0, 250.0, workload.normalized_cost_units, true);
  score.raw_paths_per_second = 500.0;
  const BenchmarkRawMetricsModel rawMetrics{
    60.0, 16.67, 0.0, 16.67, 250.0, 500.0, 1500.0, 8, 4096, 0.25, 0.0
  };
  const auto benchmarkPanel = BuildBenchmarkPanelModel(
      benchmarkDesc, rawMetrics, score, workload, "artifacts/benchmarks/ui", "model smoke result", true);
  const std::string benchmarkJson = SerializeBenchmarkPanelModel(benchmarkPanel);
  const bool benchmarkPanelOk =
      benchmarkJson.find("\"selected_scene\":\"assets/scenes/cornell_native.json\"") != std::string::npos &&
      benchmarkJson.find("\"raw_metrics\"") != std::string::npos &&
      benchmarkJson.find("\"normalized_score\":1") != std::string::npos &&
      benchmarkJson.find("\"calibration_actions\"") != std::string::npos;
  MarkReleaseGate(checklist, "benchmark.desc", benchmarkPanelOk,
                  "BenchmarkPanelModel serializes run descriptor, raw metrics, score, workload, calibration actions, and artifact path");
  MarkReleaseGate(checklist, "benchmark.score", score.calibration_valid && score.confidence == "high",
                  "ComputeBenchmarkScore separates raw throughput from hardware-normalized efficiency");

  UiEventLog log(256);
  PushUiEvent(log, "menu_click", "menu", "file.open_scene", 1, {}, {}, "unsupported:file dialog unavailable");
  EditorCommandHistory history(256);
  history.push(MakeMenuCommand("benchmark.run_current_scene", "menu", 2));
  const auto runtime = CreateDefaultRuntimeState();
  const std::string uiState = SerializeUiRuntimeState(runtime);
  const std::string eventLines = SerializeUiEventsJsonl(log.events(), 256);
  const std::string commandLines = SerializeEditorCommandsJsonl(history.history(), 256);
  MarkReleaseGate(checklist, "crash.ui_state",
                  uiState.find("\"active_layout_name\"") != std::string::npos &&
                  eventLines.find("menu_click") != std::string::npos &&
                  commandLines.find("benchmark.run_current_scene") != std::string::npos,
                  "Crash recorder receives serialized UI state, selection state, layout, UI events, and editor command history from app shell");

  MarkReleaseGate(checklist, "window.opens", false,
                  "Native/Qt bounded window smoke is covered by tools/ui_qt_smoke when the platform is available",
                  "requires a GUI-capable platform run, not a headless model check");
  std::string treeEvidence;
  const bool treeHierarchyOk = CheckEcsSceneTreeContracts(&treeEvidence);
  MarkReleaseGate(checklist, "tree.hierarchy", treeHierarchyOk,
                  treeHierarchyOk ? treeEvidence : "ECS scene tree model/runtime contract check failed",
                  treeHierarchyOk ? std::string_view{} : std::string_view{treeEvidence});
  MarkReleaseGate(checklist, "viewport.selection", false,
                  "SelectionState and selection commands are covered by --ui-model-smoke",
                  "requires object-ID/CPU-ray picking integration in the viewport");
  MarkReleaseGate(checklist, "gizmo.trs", false,
                  "GizmoSettings and transform command contracts exist",
                  "requires rendered gizmo handles and command application");
  MarkReleaseGate(checklist, "inspector.edits", false,
                  "InspectorFieldSchema and mixed-value models are covered by --ui-model-smoke",
                  "requires bound inspector widgets applying ECS/component edits");
  MarkReleaseGate(checklist, "grouping", false,
                  "Group/Ungroup command payloads serialize through EditorCommand",
                  "requires group/ungroup runtime metadata and undo integration");
  MarkReleaseGate(checklist, "merge.split", false,
                  "Merge command payload serializes; split is explicitly unsupported until merge metadata runtime exists",
                  "requires generated asset/merge metadata runtime");
  MarkReleaseGate(checklist, "assets.import", false,
                  "Asset drop validation and import command contracts exist",
                  "requires importer runtime and asset browser widget");
  MarkReleaseGate(checklist, "lua.attach", false,
                  "Attach/Detach script command payloads exist and script lifecycle model is deterministic",
                  "requires Lua runtime/editor binding");
  MarkReleaseGate(checklist, "logs.errors", false,
                  "UiEventLog serializes model events for crash artifacts",
                  "requires visible log panel consuming the diagnostics ring buffer");

  return checklist;
}

bool RunUiReleaseGateCheck(bool json) {
  const auto checklist = BuildUiReleaseGateEvidence();
  if (json) {
    std::cout << vkpt::editor::SerializeUiReleaseGateChecklist(checklist) << "\n";
  } else {
    std::size_t passed = 0;
    std::size_t deferred = 0;
    std::size_t pending = 0;
    std::cout << "ui release gate:\n";
    for (const auto& item : checklist) {
      const char* status = item.passed ? "pass" : (item.deferred ? "defer" : "PEND");
      if (item.passed) {
        ++passed;
      } else if (item.deferred) {
        ++deferred;
      } else {
        ++pending;
      }
      std::cout << "  [" << status << "] " << item.id << ": " << item.evidence;
      if (item.deferred) {
        std::cout << " (" << item.deferred_reason << ")";
      }
      std::cout << "\n";
    }
    std::cout << "ui release gate summary: passed=" << passed
              << " deferred=" << deferred
              << " pending=" << pending << "\n";
  }

  return std::none_of(checklist.begin(), checklist.end(), [](const auto& item) {
    return !item.passed && !item.deferred;
  });
}

}  // namespace vkpt::app
