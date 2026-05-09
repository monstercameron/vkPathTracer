#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

#include "core/metrics/Metrics.h"

#include <chrono>

namespace vkpt::app {

namespace {

// RAII helper that records elapsed microseconds against a histogram metric.
// The pointer to the metric is resolved lazily and stored in a function-local
// static (matches the pattern used by VKP_METRIC_OBSERVE) so subsequent calls
// skip the registry lookup. Each instantiation site (template argument) gets
// its own static slot.
template <const char* Name>
struct DockBuilderTimer {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~DockBuilderTimer() {
    static auto* metric =
        &::vkpt::core::metrics::MetricsRegistry::instance().histogram(Name);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    metric->record(static_cast<std::uint64_t>(us < 0 ? 0 : us));
  }
};

// External names used as template parameters (must have linkage).
extern const char kDockSceneTreeMetric[] = "vkp.ui.dock_scene_tree_us";
extern const char kDockInspectorMetric[] = "vkp.ui.dock_inspector_us";
extern const char kDockMaterialsMetric[] = "vkp.ui.dock_materials_us";
extern const char kDockLightsMetric[] = "vkp.ui.dock_lights_us";
extern const char kDockCameraMetric[] = "vkp.ui.dock_camera_us";
extern const char kDockRenderSettingsMetric[] = "vkp.ui.dock_render_settings_us";
extern const char kDockBenchmarkMetric[] = "vkp.ui.dock_benchmark_us";
extern const char kDockDiagnosticsMetric[] = "vkp.ui.dock_diagnostics_us";
extern const char kDockPerformanceMetric[] = "vkp.ui.dock_performance_us";
extern const char kDockMetricsMetric[] = "vkp.ui.dock_metrics_us";
extern const char kDockEventsMetric[] = "vkp.ui.dock_events_us";
extern const char kDockHealthMetric[] = "vkp.ui.dock_health_us";
extern const char kDockDeviceMetric[] = "vkp.ui.dock_device_us";
extern const char kDockDebugViewsMetric[] = "vkp.ui.dock_debug_views_us";
extern const char kDockAssetBrowserMetric[] = "vkp.ui.dock_asset_browser_us";
extern const char kDockTimelineMetric[] = "vkp.ui.dock_timeline_us";
extern const char kDockScriptMetric[] = "vkp.ui.dock_script_us";
extern const char kDockPhysicsMetric[] = "vkp.ui.dock_physics_us";

#define VKP_DOCK_TIMED(metric_var, expr)                                     \
  ([&] {                                                                       \
    DockBuilderTimer<metric_var> _vkp_timer;                                   \
    return expr;                                                                \
  })()

}  // namespace

std::vector<QtDockPanelContent> BuildQtDockPanels(
    const vkpt::scene::SceneDocument& document,
    const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
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
  panels.reserve(18u);
  // The shell consumes this fixed order for deterministic dock placement and
  // stable UI validation snapshots.
  panels.push_back(VKP_DOCK_TIMED(kDockSceneTreeMetric,
      BuildQtSceneTreeDock(document, selection, runtime, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockInspectorMetric,
      BuildQtInspectorDock(document, selection, runtime, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockMaterialsMetric,
      BuildQtMaterialsDock(document, scene, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockLightsMetric,
      BuildQtLightsDock(document, scene, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockCameraMetric,
      BuildQtCameraDock(document,
                        scene,
                        runtime,
                        layout,
                        frame_stats,
                        active_camera_shot_slot,
                        saved_camera_shot_slots)));
  panels.push_back(VKP_DOCK_TIMED(kDockRenderSettingsMetric,
      BuildQtRenderSettingsDock(scene, settings, runtime, layout, frame_stats, device_stats)));
  panels.push_back(VKP_DOCK_TIMED(kDockBenchmarkMetric,
      BuildQtBenchmarkDock(benchmark, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockDiagnosticsMetric,
      BuildQtDiagnosticsDock(runtime, selection, layout, frame_stats)));
  panels.push_back(VKP_DOCK_TIMED(kDockPerformanceMetric,
      BuildQtPerformanceDock(runtime, layout, frame_stats)));
  panels.push_back(VKP_DOCK_TIMED(kDockMetricsMetric,
      BuildQtMetricsDock(layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockEventsMetric,
      BuildQtEventsDock(layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockHealthMetric,
      BuildQtHealthDock(layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockDeviceMetric,
      BuildQtDeviceDock(scene, runtime, layout, frame_stats, device_stats)));
  panels.push_back(VKP_DOCK_TIMED(kDockDebugViewsMetric,
      BuildQtDebugViewsDock(runtime, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockAssetBrowserMetric,
      BuildQtAssetBrowserDock(document, scene, runtime, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockTimelineMetric,
      BuildQtTimelineDock(document, layout)));
  panels.push_back(VKP_DOCK_TIMED(kDockScriptMetric,
      BuildQtScriptDock(document, selection, layout, script_runtime)));
  panels.push_back(VKP_DOCK_TIMED(kDockPhysicsMetric,
      BuildQtPhysicsDock(document, layout)));
  return panels;
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
