#include "app/UiValidationInternal.h"

#include "pathtracer/PathTracer.h"
#include "scene/Scene.h"

#ifdef PT_ENABLE_QT
#include "app/QtDockPanels.h"
#include "app/ViewportInteraction.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
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

}

}  // namespace vkpt::app