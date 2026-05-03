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
  Console,
  StatusBar,
  Viewport,
  MenuBar,
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

// ----- Serialization -------------------------------------------------------

std::string SerializeUiRuntimeState(const UiRuntimeState& state);
std::string SerializeSelectionState(const SelectionState& state);
std::string SerializeLayoutDocument(const UiLayoutDocument& layout);
std::string SerializeMenuBar(const MenuBar& menu);
std::string SerializeEditorCommand(const EditorCommand& command);
std::string SerializeEditorCommandsJsonl(const std::vector<EditorCommand>& commands, std::size_t max_lines = 256);
std::string SerializeUiEventsJsonl(const std::deque<UiEvent>& events, std::size_t max_lines = 256);

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
