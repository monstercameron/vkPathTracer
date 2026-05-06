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
  std::string scene_tree_name_filter;
  std::uint32_t scene_tree_type_filter_mask = 0;
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

}  // namespace vkpt::editor
