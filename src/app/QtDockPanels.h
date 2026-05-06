#pragma once

#ifdef PT_ENABLE_QT

#include "editor/UiModels.h"
#include "pathtracer/PathTracer.h"
#include "platform/qt/QtPlatform.h"
#include "render/interface/RenderContracts.h"
#include "scene/Scene.h"
#include "scripting/ScriptRuntime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::app {

struct QtDockProperty {
  std::string id;
  std::string group;
  std::string label;
  std::string value;
  std::string unit;
  std::string editor;
  std::vector<std::string> options;
  double minimum = 0.0;
  double maximum = 1.0;
  double step = 0.01;
  double default_value = 0.0;
  bool has_numeric_range = false;
  bool has_default = false;
  bool editable = false;
  bool enabled = true;
};

struct QtDockTreeRow {
  std::string id;
  std::string label;
  std::string value;
  std::string icon;
  vkpt::core::StableId entity_id = 0;
  bool selected = false;
  bool activatable = false;
  bool draggable = false;
  bool visible = true;
  bool visibility_toggle_enabled = false;
  std::vector<QtDockTreeRow> children;
};

struct QtDockPanelContent {
  std::string id;
  std::string title;
  bool visible = true;
  bool docked = true;
  bool floating = false;
  bool collapsed = false;
  bool tree_single_column = false;
  int tree_stretch = -1;
  int property_stretch = -1;
  int property_preferred_height = 0;
  float width = 320.0f;
  float height = 240.0f;
  std::vector<QtDockProperty> properties;
  std::vector<std::string> rows;
  std::vector<QtDockTreeRow> tree_rows;
};

struct QtDockFrameStats {
  std::uint32_t sample_count = 0u;
  std::uint32_t frame_width = 0u;
  std::uint32_t frame_height = 0u;
  std::uint32_t canvas_width = 0u;
  std::uint32_t canvas_height = 0u;
  std::uint32_t displayed_image_width = 0u;
  std::uint32_t displayed_image_height = 0u;
  std::uint32_t preview_publish_hz = 0u;
  std::uint32_t gpu_batches_per_tick = 0u;
  double gpu_batch_ms = 0.0;
  double ui_frame_ms = 0.0;
  std::uint64_t total_rays = 0u;
  double instant_rays_per_second = 0.0;
  double rolling_rays_per_second = 0.0;
  double accumulated_rays_per_second = 0.0;
  std::uint64_t render_published = 0u;
  std::uint64_t render_dropped = 0u;
  std::uint64_t window_received = 0u;
  std::uint64_t window_presented = 0u;
  std::uint64_t window_dropped = 0u;
  bool background_thread = false;
  bool tracer_ready = false;
  std::string preview_status;
  std::string render_mode;
  std::string publish_cap;
  std::string camera_mode;
  bool fps_player_grounded = false;
  bool fps_player_crouching = false;
  bool fps_player_running = false;
  float fps_player_speed = 0.0f;
  float fps_player_eye_height = 0.0f;
};

struct QtDockScriptRuntimeState {
  bool scripts_enabled = true;
  bool playing = false;
  bool benchmark_scripts_allowed = false;
  std::string status = "idle";
  std::string last_hook = "none";
  vkpt::core::FrameIndex last_frame = 0u;
  std::uint64_t dispatch_count = 0u;
  vkpt::scripting::ScriptBindingSummary binding_summary{};
  vkpt::scripting::ScriptDispatchSummary last_dispatch{};
  std::vector<vkpt::scripting::ScriptBinding> bindings;
  std::vector<vkpt::scripting::ScriptDiagnostic> diagnostics;
};

struct QtDockRayDeviceMetric {
  std::string device_key;
  std::string device_name;
  std::uint32_t sample_count = 0u;
  std::uint64_t total_rays = 0u;
  double instant_rays_per_second = 0.0;
  double rolling_rays_per_second = 0.0;
  double accumulated_rays_per_second = 0.0;
  bool measured = false;
};

struct QtRayMetricAccumulator {
  bool initialized = false;
  std::uint32_t last_sample_count = 0u;
  std::uint32_t observed_sample_count = 0u;
  std::uint64_t baseline_total_rays = 0u;
  std::uint64_t last_total_rays = 0u;
  std::uint64_t observed_total_rays = 0u;
  std::chrono::steady_clock::time_point start_time{};
  std::chrono::steady_clock::time_point last_time{};
  double instant_rays_per_second = 0.0;
  double rolling_rays_per_second = 0.0;
  double accumulated_rays_per_second = 0.0;

  QtDockRayDeviceMetric update(std::string device_key,
                               std::string device_name,
                               std::uint64_t total_rays,
                               std::uint32_t sample_count,
                               std::chrono::steady_clock::time_point now);
};

struct QtDockDeviceStats {
  vkpt::render::RenderBackendCapabilities backend_caps;
  std::vector<vkpt::render::AcceleratorCapabilities> accelerators;
  std::vector<QtDockRayDeviceMetric> ray_metrics;
  std::vector<std::string> runtime_backend_options;
  std::string selected_backend;
  std::string active_renderer_path;
  std::string active_device_key;
  bool has_selected_accelerator = false;
  vkpt::render::AcceleratorCapabilities selected_accelerator;
};

std::string QtDockActiveRayDeviceKey(const QtDockDeviceStats& device_stats);
std::string QtDockActiveRayDeviceName(const QtDockDeviceStats& device_stats);
std::string QtDockRayDeviceKeyForAccelerator(const vkpt::render::AcceleratorCapabilities& accel);
void QtDockUpsertRayMetric(std::vector<QtDockRayDeviceMetric>& metrics,
                           QtDockRayDeviceMetric metric);
const vkpt::scene::SceneMaterialDefinition* FindQtSceneMaterial(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
vkpt::scene::SceneMaterialDefinition* FindQtMutableSceneMaterial(
    vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
const vkpt::scene::SceneGeometryDefinition* FindQtSceneGeometry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id);
std::string QtTrim(std::string_view text);
std::vector<std::string> QtSplitPropertyPath(std::string_view id);
bool QtParseFloat(std::string_view text, float& out);
bool QtParseStableId(std::string_view text, vkpt::core::StableId& out);
bool QtParseBool(std::string_view text, bool& out);
bool QtParseVec3(std::string_view text, vkpt::scene::Vec3& out);
bool QtParseQuat(std::string_view text, vkpt::scene::Quat& out);
bool QtParseToneMapMode(std::string_view text, vkpt::pathtracer::ToneMapMode& out);
bool QtParseOutputTransformMode(std::string_view text,
                                vkpt::pathtracer::OutputTransformMode& out);
vkpt::scene::PhysicsBodyComponent QtDefaultDynamicPhysicsBody();
std::string QtEntityDisplayName(const vkpt::scene::SceneEntityDefinition& entity);

QtDockPanelContent BuildQtSceneTreeDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiLayoutDocument& layout);
QtDockPanelContent BuildQtRenderSettingsDock(const vkpt::pathtracer::RTSceneData& scene,
                                             const vkpt::pathtracer::RenderSettings& settings,
                                             const vkpt::editor::UiRuntimeState& runtime,
                                             const vkpt::editor::UiLayoutDocument& layout,
                                             const QtDockFrameStats& frame_stats);
QtDockPanelContent BuildQtDeviceDock(const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     const QtDockDeviceStats& device_stats);
QtDockPanelContent BuildQtAssetBrowserDock(const vkpt::scene::SceneDocument& document,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           const vkpt::editor::UiRuntimeState& runtime,
                                           const vkpt::editor::UiLayoutDocument& layout);
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
    const QtDockScriptRuntimeState* script_runtime = nullptr);

std::string BuildQtStatusBarText(const vkpt::editor::StatusBarModel& status);
void ApplyQtDockPanelsToWindow(vkpt::platform::QtWindow* window,
                               const std::vector<QtDockPanelContent>& panels);
void ApplyQtStatusBarToWindow(vkpt::platform::QtWindow* window,
                              std::string_view status_text);

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
