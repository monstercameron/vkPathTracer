#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark/BenchmarkSchema.h"
#include "core/Types.h"

namespace vkpt::editor {

// ----- Core enums ----------------------------------------------------------

enum class UiPanelId {
  SceneTree,
  Inspector,
  AssetBrowser,
  BenchmarkPanel,
  BenchmarkHistory,
  ScriptPanel,
  MaterialEditor,
  LightsPanel,
  CameraPanel,
  RenderSettings,
  Diagnostics,
  Performance,
  DebugViews,
  Timeline,
  Physics,
  Console,
  StatusBar,
  Viewport,
  MenuBar,
  Toolbar,
  Unknown,
};

enum class SelectionSource {
  Unknown,
  Viewport,
  SceneTree,
  Inspector,
  AssetBrowser,
  ScriptPanel,
};

enum class GizmoMode {
  None,
  Translate,
  Rotate,
  Scale,
  Universal,
};

enum class ViewportTool {
  None,
  Select,
  Orbit,
  Fps,
  Turntable,
  Walk,
};

enum class CameraControllerMode {
  Orbit,
  Fps,
  Turntable,
  ScriptedBenchmarkPath,
};

enum class UiDockArea {
  Top,
  Left,
  Right,
  Bottom,
  Center,
  Status,
  Floating,
};

struct CameraOrbitState {
  float target_x = 0.0f;
  float target_y = 0.0f;
  float target_z = 0.0f;
  float radius = 5.0f;
  float azimuth = 0.0f;   // radians
  float elevation = 0.0f; // radians
};

struct CameraFpsState {
  float position_x = 0.0f;
  float position_y = 1.0f;
  float position_z = 5.0f;
  float yaw = 0.0f;
  float pitch = 0.0f;
};

struct CameraWaypoint {
  float position_x = 0.0f;
  float position_y = 0.0f;
  float position_z = 0.0f;
  float target_x = 0.0f;
  float target_y = 0.0f;
  float target_z = 0.0f;
  float dwell_seconds = 1.0f;
};

struct CameraControllerState {
  CameraControllerMode mode = CameraControllerMode::Orbit;
  CameraOrbitState orbit{};
  CameraFpsState fps{};
  std::vector<CameraWaypoint> benchmark_waypoints;
  vkpt::core::FrameIndex last_dirty_frame = 0;
};

class CameraController {
 public:
  explicit CameraController(CameraControllerMode mode = CameraControllerMode::Orbit);

  void set_mode(CameraControllerMode mode);
  CameraControllerMode mode() const;

  // Orbit mode: delta angles in radians, zoom changes radius.
  void apply_orbit_delta(float d_azimuth, float d_elevation, float d_zoom);

  // FPS mode: d_yaw/d_pitch in radians, d_forward/d_strafe in world units.
  void apply_fps_delta(float d_yaw, float d_pitch, float d_forward, float d_strafe);

  // Turntable: same as orbit but elevation clamped and azimuth wraps.
  void apply_turntable_delta(float d_azimuth, float d_zoom);

  // Scripted benchmark path.
  void set_benchmark_path(std::vector<CameraWaypoint> waypoints);
  void advance_benchmark_path(float delta_seconds);

  // Accumulation-reset protocol.
  bool is_dirty(vkpt::core::FrameIndex current_frame) const;
  void acknowledge_dirty(vkpt::core::FrameIndex current_frame);

  const CameraControllerState& state() const;

 private:
  void mark_dirty(vkpt::core::FrameIndex frame = 0);
  CameraControllerState m_state;
  std::size_t m_benchmark_waypoint_index = 0;
  float m_benchmark_waypoint_elapsed = 0.0f;
  bool m_dirty = false;
};

const char* ToString(CameraControllerMode mode);

enum class LayoutPreset {
  Default,
  Benchmark,
  MaterialAuthoring,
  Scripting,
  AssetManagement,
  DebugProfiler,
  MinimalViewport,
  FullscreenViewportWithOverlay,
};

const char* ToString(UiPanelId id);
const char* ToString(SelectionSource source);
const char* ToString(GizmoMode mode);
const char* ToString(ViewportTool tool);
const char* ToString(UiDockArea area);
std::string_view ToString(LayoutPreset preset);
std::string_view ParseLayoutName(LayoutPreset preset);

// ----- Layout / panel primitives ------------------------------------------

struct UiPanelState {
  std::string id;
  bool visible = true;
  bool docked = true;
  bool floating = false;
  bool closable = true;
  bool collapsible = true;
  bool collapsed = false;
  bool focused = false;
  bool resized = true;
  bool movable = true;
  float x = 0.0f;
  float y = 0.0f;
  float width = 320.0f;
  float height = 240.0f;
};

struct UiLayoutDocument {
  LayoutPreset preset = LayoutPreset::Default;
  std::string active_layout_name;
  std::vector<UiPanelState> panels;
  std::vector<std::string> panel_order;
  float dpi_scale = 1.0f;
  float ui_scale = 1.0f;
  bool fullscreen_overlay = false;
};

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Bounds {
  Vec3 min{};
  Vec3 max{};
  bool valid = false;
};

struct SceneEntityBounds {
  vkpt::core::StableId entity_id = 0;
  Bounds bounds{};
};

struct SelectionState {
  std::vector<vkpt::core::StableId> selected_entity_ids;
  std::vector<vkpt::core::StableId> hovered_entity_ids;
  vkpt::core::StableId active_primary_entity = 0;
  vkpt::core::StableId hovered_entity = 0;
  vkpt::core::StableId selected_group = 0;
  std::string selected_group_name;
  SelectionSource selection_source = SelectionSource::Unknown;
  Bounds aggregate_bounds{};
  std::vector<SceneEntityBounds> per_item_bounds;
  vkpt::core::FrameIndex last_change_frame = 0;
};

struct UiRuntimeState {
  std::string active_layout_name;
  std::vector<std::string> visible_panels;
  std::vector<std::string> collapsed_panels;
  std::string focused_panel;
  std::string active_modal;
  std::string hovered_widget;
  std::string active_drag_drop_operation;
  std::vector<vkpt::core::StableId> expanded_tree_nodes;
  std::vector<vkpt::core::StableId> collapsed_tree_nodes;
  ViewportTool active_viewport_tool = ViewportTool::None;
  GizmoMode active_gizmo_mode = GizmoMode::None;
  float dpi_scale = 1.0f;
  float ui_scale = 1.0f;
  std::string selected_debug_view;
  std::string active_debug_channel;
  std::string status_message;
  std::string active_scene;
  std::string active_camera;
  std::string active_renderer_backend;
  std::string active_renderer_path;
  std::uint32_t spp_accumulated = 0;
  double fps = 0.0;
  double frame_ms = 0.0;
  std::uint32_t background_job_count = 0;
  std::string last_warning_or_error;
  vkpt::core::StableId last_clicked_entity = 0;
  std::string last_menu_action;
  std::string last_inspector_property_edit;
  std::string last_scene_tree_operation;
  std::string last_file_drop_path;
  std::string last_benchmark_command;
};

struct UiShortcut {
  std::uint32_t key_code = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  std::string action_id;
  std::string label;
};

struct AssetDropValidation {
  bool accepted = false;
  bool extension_supported = false;
  std::string asset_type;
  std::string target_slot;
  std::string normalized_path;
  std::string reason;
};

struct InspectorFieldValue {
  std::string field_id;
  bool mixed = false;
  bool unsupported = false;
  std::string value;
};

// ----- Menu model ---------------------------------------------------------

struct MenuItem {
  std::string id;
  std::string label;
  bool enabled = true;
  std::string disabled_reason;
  std::vector<MenuItem> children;
};

struct MenuBar {
  std::vector<MenuItem> top_level_menus;
};

struct UiShortcutAction {
  std::uint32_t key_code = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  std::string action_id;
  std::string label;
};

struct MenuEnablement {
  std::string item_id;
  bool enabled = true;
};

struct PanelMutationResult {
  bool changed = false;
  std::string reason;
};

// ----- Command payloads ---------------------------------------------------

struct SelectEntityCommand {
  vkpt::core::StableId entity_id = 0;
  bool append = false;
  bool range_mode = false;
};

struct ToggleSelectEntityCommand {
  vkpt::core::StableId entity_id = 0;
};

struct ClearSelectionCommand {};

struct SetTransformCommand {
  vkpt::core::StableId entity_id = 0;
  std::array<float, 10> old_transform{};
  std::array<float, 10> new_transform{};
};

struct SetMaterialCommand {
  vkpt::core::StableId entity_id = 0;
  std::string old_material_id;
  std::string new_material_id;
};

struct SetLightPropertyCommand {
  vkpt::core::StableId entity_id = 0;
  std::string property;
  std::string old_value;
  std::string new_value;
};

struct SetCameraPropertyCommand {
  vkpt::core::StableId entity_id = 0;
  std::string property;
  std::string old_value;
  std::string new_value;
};

struct SetComponentPropertyCommand {
  vkpt::core::StableId entity_id = 0;
  std::string component;
  std::string property;
  std::string old_value;
  std::string new_value;
};

struct CreateEntityCommand {
  std::string template_name;
  std::string entity_name;
  vkpt::core::StableId requested_parent = 0;
  bool as_group = false;
};

struct DeleteEntityCommand {
  std::vector<vkpt::core::StableId> entity_ids;
};

struct DuplicateEntityCommand {
  std::vector<vkpt::core::StableId> entity_ids;
  bool include_children = true;
};

struct GroupEntitiesCommand {
  std::vector<vkpt::core::StableId> entity_ids;
  std::string group_name;
};

struct UngroupEntitiesCommand {
  vkpt::core::StableId group_entity = 0;
};

struct MergeEntitiesCommand {
  std::vector<vkpt::core::StableId> entity_ids;
  std::string merge_kind;
};

struct ReparentEntityCommand {
  vkpt::core::StableId entity_id = 0;
  vkpt::core::StableId old_parent = 0;
  vkpt::core::StableId new_parent = 0;
  bool preserve_world_transform = true;
};

struct ReorderSiblingCommand {
  vkpt::core::StableId moved_entity = 0;
  vkpt::core::StableId sibling_before = 0;
  vkpt::core::StableId sibling_after = 0;
};

struct AttachScriptCommand {
  vkpt::core::StableId entity_id = 0;
  std::string script_path;
};

struct DetachScriptCommand {
  vkpt::core::StableId entity_id = 0;
  std::string script_path;
};

struct ImportAssetCommand {
  std::string asset_path;
  std::string asset_type;
};

struct AssignAssetCommand {
  vkpt::core::StableId target_entity = 0;
  std::string target_slot;
  std::string asset_path;
};

struct RunBenchmarkCommand {
  vkpt::benchmark::BenchmarkRunDesc desc;
};

struct UnsupportedUiActionCommand {
  std::string action_id;
  std::string reason;
};

enum class EditorCommandKind {
  kSelectEntity,
  kToggleSelectEntity,
  kClearSelection,
  kSetTransform,
  kSetMaterial,
  kSetLightProperty,
  kSetCameraProperty,
  kSetComponentProperty,
  kCreateEntity,
  kDeleteEntity,
  kDuplicateEntity,
  kGroupEntities,
  kUngroupEntities,
  kMergeEntities,
  kReparentEntity,
  kReorderSibling,
  kAttachScript,
  kDetachScript,
  kImportAsset,
  kAssignAsset,
  kRunBenchmark,
  kUnsupportedUiAction,
};

const char* ToString(EditorCommandKind kind);

using EditorCommandPayload = std::variant<
  SelectEntityCommand,
  ToggleSelectEntityCommand,
  ClearSelectionCommand,
  SetTransformCommand,
  SetMaterialCommand,
  SetLightPropertyCommand,
  SetCameraPropertyCommand,
  SetComponentPropertyCommand,
  CreateEntityCommand,
  DeleteEntityCommand,
  DuplicateEntityCommand,
  GroupEntitiesCommand,
  UngroupEntitiesCommand,
  MergeEntitiesCommand,
  ReparentEntityCommand,
  ReorderSiblingCommand,
  AttachScriptCommand,
  DetachScriptCommand,
  ImportAssetCommand,
  AssignAssetCommand,
  RunBenchmarkCommand,
  UnsupportedUiActionCommand>;

struct EditorCommand {
  std::string command_id;
  EditorCommandKind kind = EditorCommandKind::kSelectEntity;
  std::string source_widget;
  vkpt::core::FrameIndex frame_index = 0;
  bool undoable = true;
  bool redoable = true;
  bool validated = true;
  EditorCommandPayload payload;
};

struct UiEvent {
  std::string event_type;
  std::string panel_id;
  std::string widget_id;
  vkpt::core::StableId entity_id = 0;
  vkpt::core::StableId asset_id = 0;
  std::string old_value;
  std::string new_value;
  std::string command_result;
  vkpt::core::FrameIndex frame_index = 0;
  std::string thread_id;
  std::uint64_t timestamp_ms = 0;
};

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

// ----- Log/History helpers ------------------------------------------------

class UiEventLog {
 public:
  explicit UiEventLog(std::size_t max_events = 256);

  void push(UiEvent event);
  const std::deque<UiEvent>& events() const;

 private:
  std::size_t m_maxEvents;
  std::deque<UiEvent> m_events;
};

class EditorCommandHistory {
 public:
  explicit EditorCommandHistory(std::size_t max_events = 1024);

  void push(EditorCommand command);
  const std::vector<EditorCommand>& history() const;
  void clear();

 private:
  std::size_t m_maxEvents;
  std::vector<EditorCommand> m_commands;
};

// ----- Construction helpers ------------------------------------------------

UiLayoutDocument CreateDefaultLayout();
UiLayoutDocument CreateLayoutPreset(LayoutPreset preset);
std::vector<UiPanelState> BuildDefaultPanelStates(LayoutPreset preset);
MenuBar BuildDefaultMenuBar();
MenuBar BuildDefaultMenuBar(const SelectionState& selection);
MenuItem* FindMenuItem(MenuBar& menu, std::string_view item_id);
const MenuItem* FindMenuItem(const MenuBar& menu, std::string_view item_id);
MenuBar ApplySelectionRulesToEditMenu(MenuBar menu, const SelectionState& selection);
std::vector<MenuEnablement> GetEditMenuEnablements(const SelectionState& selection);
UiRuntimeState CreateDefaultRuntimeState();
SelectionState CreateDefaultSelectionState();
SelectionState ApplySelectionCommand(const SelectionState& state, const EditorCommand& command);
std::vector<InspectorFieldValue> BuildInspectorFieldStates(
    std::string_view field_id,
    const std::vector<std::string>& values);
AssetDropValidation ValidateAssetDrop(std::string_view asset_path,
                                     std::string_view target_slot);
vkpt::benchmark::BenchmarkRunDesc MakeDefaultBenchmarkRunDesc(std::string_view scene_path,
                                                             std::string_view backend = "cpu_scalar",
                                                             std::string_view renderer_path = "pathtracer",
                                                             std::uint32_t spp = 32,
                                                             std::uint32_t max_depth = 6,
                                                             std::uint64_t seed = 0xBEEFF00DULL,
                                                             std::uint32_t width = 1280,
                                                             std::uint32_t height = 720);
std::vector<UiShortcut> BuildDefaultUiShortcuts();
bool DetectShortcutConflicts(const std::vector<UiShortcut>& shortcuts);
bool DetectShortcutConflicts(const std::vector<UiShortcutAction>& shortcuts);
std::vector<UiPanelDefinition> BuildDefaultPanelDefinitions();
std::vector<InspectorFieldSchema> BuildDefaultInspectorSchemas();
std::vector<InspectorFieldSchema> BuildInspectorSchemasForPanel(std::string_view panel_id);
std::vector<UiPanelPropertyGroup> BuildDefaultPanelPropertyGroups();
std::vector<ScriptLifecycleHookState> BuildDefaultScriptLifecycleHooks();
std::vector<UiReleaseGateItem> BuildDefaultUiReleaseGateChecklist();
ShortcutResolution ResolveShortcut(const std::vector<UiShortcut>& shortcuts,
                                   std::uint32_t key_code,
                                   bool ctrl,
                                   bool shift,
                                   bool alt);
std::vector<AssetPreviewCard> FilterAndSortAssetCards(const std::vector<AssetPreviewCard>& cards,
                                                      const AssetBrowserFilter& filter);
BenchmarkScoreModel ComputeBenchmarkScore(double measured_units_per_second,
                                          double expected_units_per_second,
                                          double raw_samples_per_second,
                                          double workload_units,
                                          bool calibration_valid);
WorkloadComplexityModel EstimateWorkloadComplexity(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                                   std::uint32_t light_count = 0,
                                                   std::uint64_t triangle_count = 0,
                                                   std::uint64_t bvh_node_count = 0,
                                                   std::uint64_t texture_bytes = 0,
                                                   bool denoiser_enabled = false);
std::vector<BenchmarkCalibrationActionModel> BuildDefaultBenchmarkCalibrationActions(
    bool gpu_compute_available,
    bool hardware_rt_available);
BenchmarkPanelModel BuildBenchmarkPanelModel(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                            const BenchmarkRawMetricsModel& raw_metrics,
                                            const BenchmarkScoreModel& score,
                                            const WorkloadComplexityModel& workload,
                                            std::string_view artifact_location,
                                            std::string_view result_summary,
                                            bool can_run = true,
                                            std::string_view unavailable_reason = {});
StatusBarModel BuildStatusBarModel(const UiRuntimeState& runtime,
                                   const SelectionState& selection,
                                   const BenchmarkScoreModel* score = nullptr);
std::vector<SceneTreeRow> BuildSceneTreeRows(const std::vector<SceneTreeEntityModel>& entities,
                                             const SelectionState& selection,
                                             vkpt::core::StableId hovered_entity = 0,
                                             std::size_t max_rows = 0);

// ----- Serialization -------------------------------------------------------

std::string SerializeUiRuntimeState(const UiRuntimeState& state);
std::string SerializeSelectionState(const SelectionState& state);
std::string SerializeLayoutDocument(const UiLayoutDocument& layout);
std::string SerializeMenuBar(const MenuBar& menu);
std::string SerializePanelDefinitions(const std::vector<UiPanelDefinition>& panels);
std::string SerializeInspectorSchemas(const std::vector<InspectorFieldSchema>& schemas);
std::string SerializePanelPropertyGroups(const std::vector<UiPanelPropertyGroup>& groups);
std::string SerializeBenchmarkPanelModel(const BenchmarkPanelModel& model);
std::string SerializeUiReleaseGateChecklist(const std::vector<UiReleaseGateItem>& checklist);
std::string SerializeEditorCommand(const EditorCommand& command);
std::string SerializeEditorCommandsJsonl(const std::vector<EditorCommand>& commands, std::size_t max_lines = 256);
std::string SerializeUiEventsJsonl(const std::deque<UiEvent>& events, std::size_t max_lines = 256);

bool LoadSelectionFromFile(const std::string& path, SelectionState* out_selection);
bool SaveSelectionToFile(const std::string& path, const SelectionState& selection, std::string* error = nullptr);
bool LoadLayoutFromFile(const std::string& path, UiLayoutDocument* out_layout);
bool SaveLayoutToFile(const std::string& path, const UiLayoutDocument& layout, std::string* error = nullptr);
PanelMutationResult SetPanelVisible(UiLayoutDocument& layout, std::string_view panel_id, bool visible);
PanelMutationResult SetPanelCollapsed(UiLayoutDocument& layout, std::string_view panel_id, bool collapsed);
PanelMutationResult SetPanelDockState(UiLayoutDocument& layout, std::string_view panel_id, bool docked, bool floating);
PanelMutationResult MovePanel(UiLayoutDocument& layout, std::string_view panel_id, float x, float y);
PanelMutationResult ResizePanel(UiLayoutDocument& layout, std::string_view panel_id, float width, float height);
bool RestoreLayoutPreset(UiLayoutDocument& layout, LayoutPreset preset);

PanelMutationResult ApplyPanelStateCommand(UiLayoutDocument& layout,
                                          const std::string& command_id,
                                          bool value,
                                          float value_float,
                                          std::string_view target_panel_id);

// ----- Convenience factories for command generation ------------------------

EditorCommand MakeMenuCommand(std::string_view action_id,
                             std::string_view source = "menu",
                             vkpt::core::FrameIndex frame_index = 0);
EditorCommand MakeClearSelectionCommand(vkpt::core::FrameIndex frame_index = 0);
EditorCommand MakeCreateEntityCommand(std::string_view name,
                                     std::string_view template_name,
                                     vkpt::core::FrameIndex frame_index = 0);
EditorCommand MakeReorderSiblingCommand(vkpt::core::StableId moved_entity,
                                       vkpt::core::StableId sibling_before,
                                       vkpt::core::StableId sibling_after,
                                       vkpt::core::FrameIndex frame_index = 0,
                                       std::string_view source = "scene_tree");

}  // namespace vkpt::editor
