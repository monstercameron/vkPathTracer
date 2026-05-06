#pragma once

#include "editor/UiModelsCore.h"

namespace vkpt::editor {

// ----- Interfaces ---------------------------------------------------------

class IUiRenderer {
 public:
  virtual ~IUiRenderer() = default;
  virtual bool initialize() = 0;
  virtual void shutdown() = 0;
  virtual void begin_frame() = 0;
  virtual void end_frame() = 0;
};

class IViewportPanel {
 public:
  virtual ~IViewportPanel() = default;
  virtual const std::string& id() const = 0;
  virtual void open() = 0;
  virtual void close() = 0;
  virtual bool visible() const = 0;
};

class IViewportOverlayRenderer {
 public:
  virtual ~IViewportOverlayRenderer() = default;
  virtual void render_selection_box(vkpt::core::StableId entity_id) = 0;
  virtual void clear_overlay() = 0;
};

class IUiPlatformBridge {
 public:
  virtual ~IUiPlatformBridge() = default;
  virtual void set_window_title(std::string_view title) = 0;
  virtual bool open_file_dialog(std::string_view title,
                                const std::vector<std::string>& filters,
                                std::string& out_path) = 0;
};

class IUiSystem {
 public:
  virtual ~IUiSystem() = default;
  virtual void update_state(const UiRuntimeState& state) = 0;
  virtual const UiRuntimeState& state() const = 0;
};

class IEditorCommandSink {
 public:
  virtual ~IEditorCommandSink() = default;
  virtual void submit(const EditorCommand& command) = 0;
};

class ISelectionService {
 public:
  virtual ~ISelectionService() = default;
  virtual const SelectionState& selection() const = 0;
  virtual void set_selection(const SelectionState& selection) = 0;
};

class IInspectorModelProvider {
 public:
  virtual ~IInspectorModelProvider() = default;
  virtual void build_from_selection(const SelectionState& selection) = 0;
};

class ISceneTreeModelProvider {
 public:
  virtual ~ISceneTreeModelProvider() = default;
  virtual std::vector<vkpt::core::StableId> visible_nodes() const = 0;
};

class IAssetBrowserModelProvider {
 public:
  virtual ~IAssetBrowserModelProvider() = default;
  virtual std::vector<std::string> asset_paths() const = 0;
};

class IBenchmarkPanelModelProvider {
 public:
  virtual ~IBenchmarkPanelModelProvider() = default;
  virtual std::vector<std::string> benchmark_history() const = 0;
};

class IUiLogger {
 public:
  virtual ~IUiLogger() = default;
  virtual void log_event(const UiEvent& event) = 0;
  virtual void log_command(const EditorCommand& command) = 0;
};

// ----- Rich editor model contracts -----------------------------------------

struct UiPanelDefinition {
  std::string id;
  std::string title;
  std::string menu_action_id;
  bool default_visible = true;
  bool can_dock = true;
  bool can_float = true;
  bool can_close = true;
  float default_width = 320.0f;
  float default_height = 240.0f;
  UiDockArea default_area = UiDockArea::Left;
  std::string tab_group;
  std::uint32_t sort_order = 0;
  std::string property_group_id;
  std::string status_hint;
};

struct SceneTreeRow {
  vkpt::core::StableId entity_id = 0;
  vkpt::core::StableId parent_id = 0;
  std::uint32_t depth = 0;
  std::uint32_t sibling_order = 0;
  std::string label;
  std::string icon;
  std::vector<std::string> component_badges;
  bool expanded = false;
  bool selected = false;
  bool hovered = false;
  bool hidden = false;
  bool locked = false;
  bool has_warning = false;
  bool has_children = false;
};

struct SceneTreeEntityModel {
  vkpt::core::StableId entity_id = 0;
  vkpt::core::StableId parent_id = 0;
  std::uint32_t sibling_order = 0;
  std::string name;
  std::vector<std::string> component_badges;
  bool expanded = true;
  bool visible = true;
  bool locked = false;
};

enum class InspectorControlKind {
  Text,
  Number,
  Slider,
  Toggle,
  Vector3,
  Color,
  EntityPicker,
  AssetPicker,
  Enum,
};

struct InspectorFieldSchema {
  std::string field_id;
  std::string label;
  std::string component;
  InspectorControlKind control = InspectorControlKind::Text;
  bool editable = true;
  bool supports_mixed_values = true;
  float min_value = 0.0f;
  float max_value = 1.0f;
  float step = 0.01f;
  std::vector<std::string> enum_values;
};

struct UiPanelPropertyGroup {
  std::string panel_id;
  std::string group_id;
  std::string title;
  std::string description;
  std::vector<InspectorFieldSchema> fields;
};

struct GizmoSettings {
  GizmoMode mode = GizmoMode::None;
  std::string coordinate_space = "world";
  std::string pivot_mode = "selection_center";
  bool snap_enabled = false;
  float translation_snap = 0.25f;
  float rotation_snap_degrees = 15.0f;
  float scale_snap = 0.1f;
};

struct MultiTransformEdit {
  std::vector<vkpt::core::StableId> entity_ids;
  std::array<float, 10> delta_transform{};
  std::string pivot_mode = "selection_center";
  bool preserve_relative_offsets = true;
};

struct AssetBrowserFilter {
  std::string query;
  std::string category;
  std::string sort_key = "name";
  bool ascending = true;
  bool show_missing = true;
  bool show_generated = true;
};

struct AssetPreviewCard {
  std::string asset_id;
  std::string path;
  std::string display_name;
  std::string category;
  std::string status;
  std::string thumbnail_hint;
  bool missing = false;
  bool selected = false;
};

struct FilePickerModel {
  std::string label;
  std::string current_path;
  std::vector<std::string> accepted_extensions;
  bool valid = false;
  std::string validation_message;
};

struct ImportValidationModel {
  std::string source_path;
  std::string detected_file_type;
  std::string selected_importer;
  std::string asset_category;
  std::string target_path;
  std::vector<std::string> lossy_conversion_notes;
  std::vector<std::string> unsupported_fields;
  bool can_import = false;
};

struct ScriptLifecycleHookState {
  std::string hook_name;
  bool implemented = false;
  std::uint64_t last_fired_frame = 0;
  std::string last_error;
};

struct ScriptParameterModel {
  std::string name;
  std::string type;
  std::string value;
  std::string min_value;
  std::string max_value;
  std::vector<std::string> enum_values;
  bool valid = true;
  std::string validation_message;
};

struct BenchmarkScoreModel {
  double normalized_score = 0.0;
  double raw_samples_per_second = 0.0;
  double raw_paths_per_second = 0.0;
  double raw_gpu_ms = 0.0;
  double raw_cpu_ms = 0.0;
  double workload_units = 0.0;
  double expected_units_per_second = 0.0;
  double measured_units_per_second = 0.0;
  std::string calibration_profile_id;
  std::string confidence = "uncalibrated";
  bool calibration_valid = false;
  std::vector<std::string> warnings;
};

struct BenchmarkRawMetricsModel {
  double fps = 0.0;
  double frame_ms = 0.0;
  double gpu_ms = 0.0;
  double cpu_ms = 0.0;
  double samples_per_second = 0.0;
  double paths_per_second = 0.0;
  double path_vertices_per_second = 0.0;
  std::uint32_t spp_accumulated = 0;
  std::uint64_t memory_estimate_bytes = 0;
  double bvh_build_ms = 0.0;
  double shader_compile_ms = 0.0;
};

struct WorkloadComplexityModel {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t samples_per_pixel = 0;
  std::uint32_t max_depth = 0;
  std::uint32_t light_count = 0;
  std::uint64_t triangle_count = 0;
  std::uint64_t bvh_node_count = 0;
  std::uint64_t texture_bytes = 0;
  double normalized_cost_units = 0.0;
  std::vector<std::string> cost_drivers;
};

struct BenchmarkHistoryEntry {
  std::string scene;
  std::string backend;
  std::string renderer_path;
  BenchmarkScoreModel score;
  std::string artifact_path;
  std::string timestamp_utc;
  std::string regression_marker;
};

struct BenchmarkCalibrationActionModel {
  std::string id;
  std::string label;
  std::string backend;
  std::string renderer_path;
  bool supported = true;
  std::string unavailable_reason;
};

struct BenchmarkPanelModel {
  vkpt::benchmark::BenchmarkRunDesc run_desc;
  bool can_run = true;
  bool can_cancel = false;
  std::string unavailable_reason;
  std::string artifact_location;
  std::string result_summary;
  BenchmarkRawMetricsModel raw_metrics;
  BenchmarkScoreModel score;
  WorkloadComplexityModel workload;
  std::vector<BenchmarkCalibrationActionModel> calibration_actions;
  std::vector<BenchmarkHistoryEntry> history;
};

struct LogPanelFilter {
  std::string severity;
  std::string subsystem;
  std::string search;
  bool paused = false;
  bool auto_scroll = true;
};

struct StatusBarModel {
  std::string active_scene;
  std::string backend;
  std::string renderer_path;
  std::uint32_t spp = 0;
  double fps = 0.0;
  double frame_ms = 0.0;
  double normalized_score = 0.0;
  std::size_t selected_entity_count = 0;
  std::string active_tool;
  std::string last_warning_or_error;
  std::uint32_t background_job_count = 0;
};

struct ModalModel {
  std::string id;
  std::string type;
  std::string title;
  std::string message;
  bool blocking = false;
  bool open = false;
};

struct ToastNotificationModel {
  std::string id;
  std::string severity;
  std::string message;
  std::uint64_t created_ms = 0;
  std::uint32_t ttl_ms = 5000;
};

struct ShortcutResolution {
  bool matched = false;
  bool conflict = false;
  std::string action_id;
  std::vector<std::string> conflicting_action_ids;
};

struct AccessibilitySettings {
  float ui_scale = 1.0f;
  float dpi_scale = 1.0f;
  bool large_text = false;
  bool high_contrast_selection = false;
  bool reduced_motion = false;
  std::uint32_t tooltip_delay_ms = 450;
};

struct UiPerformanceBudget {
  double ui_frame_ms = 4.0;
  double panel_build_ms = 1.0;
  double scene_tree_build_ms = 1.0;
  double inspector_build_ms = 1.0;
  double asset_browser_build_ms = 1.0;
  double thumbnail_job_ms = 2.0;
  double event_routing_ms = 0.5;
  double overlay_draw_ms = 1.0;
};

struct UiReleaseGateItem {
  std::string id;
  std::string label;
  bool required = true;
  bool passed = false;
  bool deferred = false;
  std::string evidence;
  std::string deferred_reason;
};

}  // namespace vkpt::editor