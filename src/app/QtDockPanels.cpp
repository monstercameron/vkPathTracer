#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

namespace vkpt::app {

std::vector<QtDockPanelContent> BuildQtDockPanels(
    const vkpt::scene::SceneDocument& document,
    const vkpt::pathtracer::RTSceneData& scene,
    const vkpt::pathtracer::RenderSettings& settings,
    const vkpt::editor::UiRuntimeState& runtime,
    const vkpt::editor::SelectionState& selection,
    const vkpt::editor::UiLayoutDocument& layout,
    const vkpt::editor::BenchmarkPanelModel& benchmark,
    const QtDockFrameStats& frame_stats,
    const QtDockDeviceStats& device_stats,
    int active_camera_shot_slot,
    const std::array<bool, 4>& saved_camera_shot_slots,
    const QtDockScriptRuntimeState* script_runtime) {
  std::vector<QtDockPanelContent> panels;
  panels.reserve(15u);
  // The shell consumes this fixed order for deterministic dock placement and
  // stable UI validation snapshots.
  panels.push_back(BuildQtSceneTreeDock(document, selection, runtime, layout));
  panels.push_back(BuildQtInspectorDock(document, selection, runtime, layout));
  panels.push_back(BuildQtMaterialsDock(document, scene, layout));
  panels.push_back(BuildQtLightsDock(document, scene, layout));
  panels.push_back(BuildQtCameraDock(document,
                                     scene,
                                     runtime,
                                     layout,
                                     frame_stats,
                                     active_camera_shot_slot,
                                     saved_camera_shot_slots));
  panels.push_back(BuildQtRenderSettingsDock(scene, settings, runtime, layout, frame_stats, device_stats));
  panels.push_back(BuildQtBenchmarkDock(benchmark, layout));
  panels.push_back(BuildQtDiagnosticsDock(runtime, selection, layout, frame_stats));
  panels.push_back(BuildQtPerformanceDock(runtime, layout, frame_stats));
  panels.push_back(BuildQtDeviceDock(scene, runtime, layout, frame_stats, device_stats));
  panels.push_back(BuildQtDebugViewsDock(runtime, layout));
  panels.push_back(BuildQtAssetBrowserDock(document, scene, runtime, layout));
  panels.push_back(BuildQtTimelineDock(document, layout));
  panels.push_back(BuildQtScriptDock(document, selection, layout, script_runtime));
  panels.push_back(BuildQtPhysicsDock(document, layout));
  return panels;
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
