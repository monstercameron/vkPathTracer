#include "editor/UiModels.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <type_traits>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "scene/Scene.h"

namespace vkpt::editor {
namespace {

std::string EscapeJson(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

std::string_view LayoutName(LayoutPreset preset) {
  switch (preset) {
    case LayoutPreset::Default:
      return "Default";
    case LayoutPreset::Benchmark:
      return "Benchmark";
    case LayoutPreset::MaterialAuthoring:
      return "Material Authoring";
    case LayoutPreset::Scripting:
      return "Scripting";
    case LayoutPreset::AssetManagement:
      return "Asset Management";
    case LayoutPreset::DebugProfiler:
      return "Debug/Profiler";
    case LayoutPreset::MinimalViewport:
      return "Minimal Viewport";
    case LayoutPreset::FullscreenViewportWithOverlay:
      return "Fullscreen Viewport With Overlay";
    default:
      return "Default";
  }
}

bool ReadJsonBool(const vkpt::scene::JsonValue& value, const std::string& key, bool& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Boolean) {
    return false;
  }
  out = it->second.boolean;
  return true;
}

bool ReadJsonString(const vkpt::scene::JsonValue& value, const std::string& key, std::string& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::String) {
    return false;
  }
  out = it->second.string;
  return true;
}

bool ReadJsonArray(const vkpt::scene::JsonValue& value, const std::string& key, std::vector<std::string>& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Array) {
    return false;
  }
  out.clear();
  out.reserve(it->second.array.size());
  for (const auto& entry : it->second.array) {
    if (entry.kind != vkpt::scene::JsonValue::Kind::String) {
      return false;
    }
    out.push_back(entry.string);
  }
  return true;
}

bool ReadJsonStableIdArray(const vkpt::scene::JsonValue& value,
                           const std::string& key,
                           std::vector<vkpt::core::StableId>& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Array) {
    return false;
  }
  out.clear();
  out.reserve(it->second.array.size());
  for (const auto& entry : it->second.array) {
    if (entry.kind != vkpt::scene::JsonValue::Kind::Number ||
        entry.number < 0.0 ||
        !std::isfinite(entry.number)) {
      return false;
    }
    out.push_back(static_cast<vkpt::core::StableId>(entry.number));
  }
  return true;
}

bool ReadJsonFloat(const vkpt::scene::JsonValue& value, const std::string& key, float& out);

bool ReadJsonUInt64(const vkpt::scene::JsonValue& value, const std::string& key, std::uint64_t& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  if (it->second.number < 0.0 || !std::isfinite(it->second.number)) {
    return false;
  }
  out = static_cast<std::uint64_t>(it->second.number);
  return true;
}

bool ReadJsonVec3(const vkpt::scene::JsonValue& value, Vec3& out) {
  return ReadJsonFloat(value, "x", out.x) &&
         ReadJsonFloat(value, "y", out.y) &&
         ReadJsonFloat(value, "z", out.z);
}

bool ReadJsonBounds(const vkpt::scene::JsonValue& value, const std::string& key, Bounds& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto min_it = it->second.object.find("min");
  const auto max_it = it->second.object.find("max");
  if (min_it == it->second.object.end() || max_it == it->second.object.end() ||
      min_it->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      max_it->second.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  Bounds parsed;
  if (!ReadJsonVec3(min_it->second, parsed.min) ||
      !ReadJsonVec3(max_it->second, parsed.max)) {
    return false;
  }
  ReadJsonBool(it->second, "valid", parsed.valid);
  out = parsed;
  return true;
}

bool ReadJsonFloat(const vkpt::scene::JsonValue& value, const std::string& key, float& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = value.object.find(key);
  if (it == value.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  out = static_cast<float>(it->second.number);
  return true;
}

std::string ToLower(std::string_view text) {
  std::string lowered = std::string(text);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered;
}

bool IsIn(const std::vector<std::string_view>& values, std::string_view key) {
  for (const auto value : values) {
    if (value == key) {
      return true;
    }
  }
  return false;
}

SelectionSource ParseSelectionSource(std::string_view source) {
  if (source == "viewport") {
    return SelectionSource::Viewport;
  }
  if (source == "scene_tree") {
    return SelectionSource::SceneTree;
  }
  if (source == "inspector") {
    return SelectionSource::Inspector;
  }
  if (source == "asset_browser") {
    return SelectionSource::AssetBrowser;
  }
  if (source == "script_panel") {
    return SelectionSource::ScriptPanel;
  }
  return SelectionSource::Unknown;
}

}  // namespace

const char* ToString(UiPanelId id) {
  switch (id) {
    case UiPanelId::SceneTree:
      return "scene_tree";
    case UiPanelId::Inspector:
      return "inspector";
    case UiPanelId::AssetBrowser:
      return "asset_browser";
    case UiPanelId::BenchmarkPanel:
      return "benchmark_panel";
    case UiPanelId::BenchmarkHistory:
      return "benchmark_history";
    case UiPanelId::ScriptPanel:
      return "script_panel";
    case UiPanelId::MaterialEditor:
      return "material_editor";
    case UiPanelId::LightsPanel:
      return "lights_panel";
    case UiPanelId::CameraPanel:
      return "camera_panel";
    case UiPanelId::RenderSettings:
      return "render_settings";
    case UiPanelId::Diagnostics:
      return "diagnostics";
    case UiPanelId::Performance:
      return "performance";
    case UiPanelId::DebugViews:
      return "debug_views";
    case UiPanelId::Timeline:
      return "timeline";
    case UiPanelId::Physics:
      return "physics";
    case UiPanelId::Console:
      return "console";
    case UiPanelId::StatusBar:
      return "status_bar";
    case UiPanelId::Viewport:
      return "viewport";
    case UiPanelId::MenuBar:
      return "menu_bar";
    case UiPanelId::Toolbar:
      return "toolbar";
    case UiPanelId::Unknown:
      return "unknown";
    default:
      return "unknown";
  }
}

const char* ToString(SelectionSource source) {
  switch (source) {
    case SelectionSource::Viewport:
      return "viewport";
    case SelectionSource::SceneTree:
      return "scene_tree";
    case SelectionSource::Inspector:
      return "inspector";
    case SelectionSource::AssetBrowser:
      return "asset_browser";
    case SelectionSource::ScriptPanel:
      return "script_panel";
    case SelectionSource::Unknown:
    default:
      return "unknown";
  }
}

const char* ToString(GizmoMode mode) {
  switch (mode) {
    case GizmoMode::Translate:
      return "translate";
    case GizmoMode::Rotate:
      return "rotate";
    case GizmoMode::Scale:
      return "scale";
    case GizmoMode::Universal:
      return "universal";
    case GizmoMode::None:
    default:
      return "none";
  }
}

const char* ToString(ViewportTool tool) {
  switch (tool) {
    case ViewportTool::Select:
      return "select";
    case ViewportTool::Orbit:
      return "orbit";
    case ViewportTool::Fps:
      return "fps";
    case ViewportTool::Turntable:
      return "turntable";
    case ViewportTool::Walk:
      return "walk";
    case ViewportTool::None:
    default:
      return "none";
  }
}

const char* ToString(UiDockArea area) {
  switch (area) {
    case UiDockArea::Top:
      return "top";
    case UiDockArea::Left:
      return "left";
    case UiDockArea::Right:
      return "right";
    case UiDockArea::Bottom:
      return "bottom";
    case UiDockArea::Center:
      return "center";
    case UiDockArea::Status:
      return "status";
    case UiDockArea::Floating:
      return "floating";
    default:
      return "left";
  }
}

std::string_view ToString(LayoutPreset preset) {
  return LayoutName(preset);
}

std::string_view ParseLayoutName(LayoutPreset preset) {
  return LayoutName(preset);
}

const char* ToString(EditorCommandKind kind) {
  switch (kind) {
    case EditorCommandKind::kSelectEntity:
      return "SelectEntity";
    case EditorCommandKind::kToggleSelectEntity:
      return "ToggleSelectEntity";
    case EditorCommandKind::kClearSelection:
      return "ClearSelection";
    case EditorCommandKind::kSetTransform:
      return "SetTransform";
    case EditorCommandKind::kSetMaterial:
      return "SetMaterial";
    case EditorCommandKind::kSetLightProperty:
      return "SetLightProperty";
    case EditorCommandKind::kSetCameraProperty:
      return "SetCameraProperty";
    case EditorCommandKind::kSetComponentProperty:
      return "SetComponentProperty";
    case EditorCommandKind::kCreateEntity:
      return "CreateEntity";
    case EditorCommandKind::kDeleteEntity:
      return "DeleteEntity";
    case EditorCommandKind::kDuplicateEntity:
      return "DuplicateEntity";
    case EditorCommandKind::kGroupEntities:
      return "GroupEntities";
    case EditorCommandKind::kUngroupEntities:
      return "UngroupEntities";
    case EditorCommandKind::kMergeEntities:
      return "MergeEntities";
    case EditorCommandKind::kReparentEntity:
      return "ReparentEntity";
    case EditorCommandKind::kReorderSibling:
      return "ReorderSibling";
    case EditorCommandKind::kAttachScript:
      return "AttachScript";
    case EditorCommandKind::kDetachScript:
      return "DetachScript";
    case EditorCommandKind::kImportAsset:
      return "ImportAsset";
    case EditorCommandKind::kAssignAsset:
      return "AssignAsset";
    case EditorCommandKind::kRunBenchmark:
      return "RunBenchmark";
    case EditorCommandKind::kUnsupportedUiAction:
      return "UnsupportedUiAction";
    default:
      return "Unknown";
  }
}

namespace {

template <std::size_t N>
std::string SerializeFloatArray(const std::array<float, N>& values) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < N; ++i) {
    if (i > 0) {
      out << ",";
    }
    out << values[i];
  }
  out << "]";
  return out.str();
}

std::string SerializeUiEventInternal(const UiEvent& event, bool include_new_line = false) {
  std::ostringstream out;
  out << "{";
  out << "\"event_type\":\"" << EscapeJson(event.event_type) << "\",";
  out << "\"panel_id\":\"" << EscapeJson(event.panel_id) << "\",";
  out << "\"widget_id\":\"" << EscapeJson(event.widget_id) << "\",";
  out << "\"entity_id\":" << event.entity_id << ",";
  out << "\"asset_id\":" << event.asset_id << ",";
  out << "\"old_value\":\"" << EscapeJson(event.old_value) << "\",";
  out << "\"new_value\":\"" << EscapeJson(event.new_value) << "\",";
  out << "\"command_result\":\"" << EscapeJson(event.command_result) << "\",";
  out << "\"frame_index\":" << event.frame_index << ",";
  out << "\"thread_id\":\"" << EscapeJson(event.thread_id) << "\",";
  out << "\"timestamp_ms\":" << event.timestamp_ms;
  out << "}";
  if (include_new_line) {
    out << '\n';
  }
  return out.str();
}

std::string SerializeEditorCommandInternal(const EditorCommand& command) {
  std::ostringstream out;
  out << "{";
  out << "\"command_id\":\"" << EscapeJson(command.command_id) << "\",";
  out << "\"command\":\"" << ToString(command.kind) << "\",";
  out << "\"source_widget\":\"" << EscapeJson(command.source_widget) << "\",";
  out << "\"frame_index\":" << command.frame_index << ",";
  out << "\"undoable\":" << (command.undoable ? "true" : "false") << ",";
  out << "\"redoable\":" << (command.redoable ? "true" : "false") << ",";
  out << "\"validated\":" << (command.validated ? "true" : "false") << ",";
  out << "\"payload\":{";

  std::visit([&out](auto&& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, SelectEntityCommand>) {
      out << "\"type\":\"SelectEntityCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"append\":" << (value.append ? "true" : "false") << ",";
      out << "\"range_mode\":" << (value.range_mode ? "true" : "false");
    } else if constexpr (std::is_same_v<T, ToggleSelectEntityCommand>) {
      out << "\"type\":\"ToggleSelectEntityCommand\",";
      out << "\"entity_id\":" << value.entity_id;
    } else if constexpr (std::is_same_v<T, ClearSelectionCommand>) {
      out << "\"type\":\"ClearSelectionCommand\"";
    } else if constexpr (std::is_same_v<T, SetTransformCommand>) {
      out << "\"type\":\"SetTransformCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"old_transform\":" << SerializeFloatArray(value.old_transform) << ",";
      out << "\"new_transform\":" << SerializeFloatArray(value.new_transform);
    } else if constexpr (std::is_same_v<T, SetMaterialCommand>) {
      out << "\"type\":\"SetMaterialCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"old_material_id\":\"" << EscapeJson(value.old_material_id) << "\",";
      out << "\"new_material_id\":\"" << EscapeJson(value.new_material_id) << "\"";
    } else if constexpr (std::is_same_v<T, SetLightPropertyCommand>) {
      out << "\"type\":\"SetLightPropertyCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"property\":\"" << EscapeJson(value.property) << "\",";
      out << "\"old_value\":\"" << EscapeJson(value.old_value) << "\",";
      out << "\"new_value\":\"" << EscapeJson(value.new_value) << "\"";
    } else if constexpr (std::is_same_v<T, SetCameraPropertyCommand>) {
      out << "\"type\":\"SetCameraPropertyCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"property\":\"" << EscapeJson(value.property) << "\",";
      out << "\"old_value\":\"" << EscapeJson(value.old_value) << "\",";
      out << "\"new_value\":\"" << EscapeJson(value.new_value) << "\"";
    } else if constexpr (std::is_same_v<T, SetComponentPropertyCommand>) {
      out << "\"type\":\"SetComponentPropertyCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"component\":\"" << EscapeJson(value.component) << "\",";
      out << "\"property\":\"" << EscapeJson(value.property) << "\",";
      out << "\"old_value\":\"" << EscapeJson(value.old_value) << "\",";
      out << "\"new_value\":\"" << EscapeJson(value.new_value) << "\"";
    } else if constexpr (std::is_same_v<T, CreateEntityCommand>) {
      out << "\"type\":\"CreateEntityCommand\",";
      out << "\"template_name\":\"" << EscapeJson(value.template_name) << "\",";
      out << "\"entity_name\":\"" << EscapeJson(value.entity_name) << "\",";
      out << "\"requested_parent\":" << value.requested_parent << ",";
      out << "\"as_group\":" << (value.as_group ? "true" : "false");
    } else if constexpr (std::is_same_v<T, DeleteEntityCommand>) {
      out << "\"type\":\"DeleteEntityCommand\",";
      out << "\"entity_ids\":[";
      for (std::size_t i = 0; i < value.entity_ids.size(); ++i) {
        if (i > 0) out << ",";
        out << value.entity_ids[i];
      }
      out << "]";
    } else if constexpr (std::is_same_v<T, DuplicateEntityCommand>) {
      out << "\"type\":\"DuplicateEntityCommand\",";
      out << "\"entity_ids\":[";
      for (std::size_t i = 0; i < value.entity_ids.size(); ++i) {
        if (i > 0) out << ",";
        out << value.entity_ids[i];
      }
      out << "],\"include_children\":" << (value.include_children ? "true" : "false");
    } else if constexpr (std::is_same_v<T, GroupEntitiesCommand>) {
      out << "\"type\":\"GroupEntitiesCommand\",";
      out << "\"entity_ids\":[";
      for (std::size_t i = 0; i < value.entity_ids.size(); ++i) {
        if (i > 0) out << ",";
        out << value.entity_ids[i];
      }
      out << "],\"group_name\":\"" << EscapeJson(value.group_name) << "\"";
    } else if constexpr (std::is_same_v<T, UngroupEntitiesCommand>) {
      out << "\"type\":\"UngroupEntitiesCommand\",";
      out << "\"group_entity\":" << value.group_entity;
    } else if constexpr (std::is_same_v<T, MergeEntitiesCommand>) {
      out << "\"type\":\"MergeEntitiesCommand\",";
      out << "\"entity_ids\":[";
      for (std::size_t i = 0; i < value.entity_ids.size(); ++i) {
        if (i > 0) out << ",";
        out << value.entity_ids[i];
      }
      out << "],\"merge_kind\":\"" << EscapeJson(value.merge_kind) << "\"";
    } else if constexpr (std::is_same_v<T, ReparentEntityCommand>) {
      out << "\"type\":\"ReparentEntityCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"old_parent\":" << value.old_parent << ",";
      out << "\"new_parent\":" << value.new_parent << ",";
      out << "\"preserve_world_transform\":" << (value.preserve_world_transform ? "true" : "false");
    } else if constexpr (std::is_same_v<T, ReorderSiblingCommand>) {
      out << "\"type\":\"ReorderSiblingCommand\",";
      out << "\"moved_entity\":" << value.moved_entity << ",";
      out << "\"sibling_before\":" << value.sibling_before << ",";
      out << "\"sibling_after\":" << value.sibling_after;
    } else if constexpr (std::is_same_v<T, AttachScriptCommand>) {
      out << "\"type\":\"AttachScriptCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"script_path\":\"" << EscapeJson(value.script_path) << "\"";
    } else if constexpr (std::is_same_v<T, DetachScriptCommand>) {
      out << "\"type\":\"DetachScriptCommand\",";
      out << "\"entity_id\":" << value.entity_id << ",";
      out << "\"script_path\":\"" << EscapeJson(value.script_path) << "\"";
    } else if constexpr (std::is_same_v<T, ImportAssetCommand>) {
      out << "\"type\":\"ImportAssetCommand\",";
      out << "\"asset_path\":\"" << EscapeJson(value.asset_path) << "\",";
      out << "\"asset_type\":\"" << EscapeJson(value.asset_type) << "\"";
    } else if constexpr (std::is_same_v<T, AssignAssetCommand>) {
      out << "\"type\":\"AssignAssetCommand\",";
      out << "\"target_entity\":" << value.target_entity << ",";
      out << "\"target_slot\":\"" << EscapeJson(value.target_slot) << "\",";
      out << "\"asset_path\":\"" << EscapeJson(value.asset_path) << "\"";
    } else if constexpr (std::is_same_v<T, UnsupportedUiActionCommand>) {
      out << "\"type\":\"UnsupportedUiActionCommand\",";
      out << "\"action_id\":\"" << EscapeJson(value.action_id) << "\",";
      out << "\"reason\":\"" << EscapeJson(value.reason) << "\"";
    } else if constexpr (std::is_same_v<T, RunBenchmarkCommand>) {
      out << "\"type\":\"RunBenchmarkCommand\",";
      out << "\"scene_path\":\"" << EscapeJson(value.desc.scene_path) << "\",";
      out << "\"backend\":\"" << EscapeJson(value.desc.backend) << "\",";
      out << "\"renderer_path\":\"" << EscapeJson(value.desc.renderer_path) << "\",";
      out << "\"spp\":" << value.desc.samples_per_pixel;
    }
  }, command.payload);

  out << "}}";
  return out.str();
}

std::string SerializeUiLayoutJson(const UiLayoutDocument& layout) {
  std::ostringstream out;
  out << "{";
  out << "\"layout_name\":\"" << EscapeJson(layout.active_layout_name) << "\",";
  out << "\"preset\":\"" << ParseLayoutName(layout.preset) << "\",";
  out << "\"dpi_scale\":" << layout.dpi_scale << ",";
  out << "\"ui_scale\":" << layout.ui_scale << ",";
  out << "\"fullscreen_overlay\":" << (layout.fullscreen_overlay ? "true" : "false") << ",";
  out << "\"panel_order\":[";
  for (std::size_t i = 0; i < layout.panel_order.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(layout.panel_order[i]) << "\"";
  }
  out << "],";
  out << "\"panels\":[";
  for (std::size_t i = 0; i < layout.panels.size(); ++i) {
    const auto& panel = layout.panels[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"id\":\"" << EscapeJson(panel.id) << "\",";
    out << "\"visible\":" << (panel.visible ? "true" : "false") << ",";
    out << "\"docked\":" << (panel.docked ? "true" : "false") << ",";
    out << "\"floating\":" << (panel.floating ? "true" : "false") << ",";
    out << "\"closable\":" << (panel.closable ? "true" : "false") << ",";
    out << "\"collapsible\":" << (panel.collapsible ? "true" : "false") << ",";
    out << "\"collapsed\":" << (panel.collapsed ? "true" : "false") << ",";
    out << "\"focused\":" << (panel.focused ? "true" : "false") << ",";
    out << "\"resized\":" << (panel.resized ? "true" : "false") << ",";
    out << "\"movable\":" << (panel.movable ? "true" : "false") << ",";
    out << "\"x\":" << panel.x << ",";
    out << "\"y\":" << panel.y << ",";
    out << "\"width\":" << panel.width << ",";
    out << "\"height\":" << panel.height;
    out << "}";
  }
  out << "]";
  out << "}";
  return out.str();
}

std::string SerializeSelectionStateInternal(const SelectionState& state) {
  std::ostringstream out;
  out << "{";
  out << "\"selected_entity_ids\":[";
  for (std::size_t i = 0; i < state.selected_entity_ids.size(); ++i) {
    if (i > 0) out << ",";
    out << state.selected_entity_ids[i];
  }
  out << "],";
  out << "\"hovered_entity_ids\":[";
  for (std::size_t i = 0; i < state.hovered_entity_ids.size(); ++i) {
    if (i > 0) out << ",";
    out << state.hovered_entity_ids[i];
  }
  out << "],";
  out << "\"active_primary_entity\":" << state.active_primary_entity << ",";
  out << "\"hovered_entity\":" << state.hovered_entity << ",";
  out << "\"selected_group\":" << state.selected_group << ",";
  out << "\"selected_group_name\":\"" << EscapeJson(state.selected_group_name) << "\",";
  out << "\"selection_source\":\"" << ToString(state.selection_source) << "\",";
  out << "\"aggregate_bounds\":{";
  out << "\"min\":{\"x\":" << state.aggregate_bounds.min.x << ",\"y\":" << state.aggregate_bounds.min.y << ",\"z\":" << state.aggregate_bounds.min.z << "},";
  out << "\"max\":{\"x\":" << state.aggregate_bounds.max.x << ",\"y\":" << state.aggregate_bounds.max.y << ",\"z\":" << state.aggregate_bounds.max.z << "},";
  out << "\"valid\":" << (state.aggregate_bounds.valid ? "true" : "false") << "},";
  out << "\"per_item_bounds\":[";
  for (std::size_t i = 0; i < state.per_item_bounds.size(); ++i) {
    const auto& item = state.per_item_bounds[i];
    if (i > 0) out << ",";
    out << "{";
    out << "\"entity_id\":" << item.entity_id << ",";
    out << "\"bounds\":{";
    out << "\"min\":{\"x\":" << item.bounds.min.x << ",\"y\":" << item.bounds.min.y << ",\"z\":" << item.bounds.min.z << "},";
    out << "\"max\":{\"x\":" << item.bounds.max.x << ",\"y\":" << item.bounds.max.y << ",\"z\":" << item.bounds.max.z << "},";
    out << "\"valid\":" << (item.bounds.valid ? "true" : "false");
    out << "}";
    out << "}";
  }
  out << "],";
  out << "\"last_change_frame\":" << state.last_change_frame;
  out << "}";
  return out.str();
}

std::string SerializeUiRuntimeStateInternal(const UiRuntimeState& state) {
  std::ostringstream out;
  out << "{";
  out << "\"active_layout_name\":\"" << EscapeJson(state.active_layout_name) << "\",";
  out << "\"visible_panels\":[";
  for (std::size_t i = 0; i < state.visible_panels.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(state.visible_panels[i]) << "\"";
  }
  out << "],";
  out << "\"collapsed_panels\":[";
  for (std::size_t i = 0; i < state.collapsed_panels.size(); ++i) {
    if (i > 0) out << ",";
    out << "\"" << EscapeJson(state.collapsed_panels[i]) << "\"";
  }
  out << "],";
  out << "\"expanded_tree_nodes\":[";
  for (std::size_t i = 0; i < state.expanded_tree_nodes.size(); ++i) {
    if (i > 0) out << ",";
    out << state.expanded_tree_nodes[i];
  }
  out << "],";
  out << "\"collapsed_tree_nodes\":[";
  for (std::size_t i = 0; i < state.collapsed_tree_nodes.size(); ++i) {
    if (i > 0) out << ",";
    out << state.collapsed_tree_nodes[i];
  }
  out << "],";
  out << "\"focused_panel\":\"" << EscapeJson(state.focused_panel) << "\",";
  out << "\"active_modal\":\"" << EscapeJson(state.active_modal) << "\",";
  out << "\"hovered_widget\":\"" << EscapeJson(state.hovered_widget) << "\",";
  out << "\"active_drag_drop_operation\":\"" << EscapeJson(state.active_drag_drop_operation) << "\",";
  out << "\"active_viewport_tool\":\"" << ToString(state.active_viewport_tool) << "\",";
  out << "\"active_gizmo_mode\":\"" << ToString(state.active_gizmo_mode) << "\",";
  out << "\"dpi_scale\":" << state.dpi_scale << ",";
  out << "\"ui_scale\":" << state.ui_scale << ",";
  out << "\"selected_debug_view\":\"" << EscapeJson(state.selected_debug_view) << "\",";
  out << "\"active_debug_channel\":\"" << EscapeJson(state.active_debug_channel) << "\",";
  out << "\"status_message\":\"" << EscapeJson(state.status_message) << "\",";
  out << "\"active_scene\":\"" << EscapeJson(state.active_scene) << "\",";
  out << "\"active_camera\":\"" << EscapeJson(state.active_camera) << "\",";
  out << "\"active_renderer_backend\":\"" << EscapeJson(state.active_renderer_backend) << "\",";
  out << "\"active_renderer_path\":\"" << EscapeJson(state.active_renderer_path) << "\",";
  out << "\"spp_accumulated\":" << state.spp_accumulated << ",";
  out << "\"fps\":" << state.fps << ",";
  out << "\"frame_ms\":" << state.frame_ms << ",";
  out << "\"background_job_count\":" << state.background_job_count << ",";
  out << "\"last_warning_or_error\":\"" << EscapeJson(state.last_warning_or_error) << "\",";
  out << "\"last_clicked_entity\":" << state.last_clicked_entity << ",";
  out << "\"last_menu_action\":\"" << EscapeJson(state.last_menu_action) << "\",";
  out << "\"last_inspector_property_edit\":\"" << EscapeJson(state.last_inspector_property_edit) << "\",";
  out << "\"last_scene_tree_operation\":\"" << EscapeJson(state.last_scene_tree_operation) << "\",";
  out << "\"last_file_drop_path\":\"" << EscapeJson(state.last_file_drop_path) << "\",";
  out << "\"last_benchmark_command\":\"" << EscapeJson(state.last_benchmark_command) << "\"";
  out << "}";
  return out.str();
  }

MenuItem MenuItemNode(std::string_view id, std::string_view label, bool enabled = true) {
  MenuItem item;
  item.id = std::string(id);
  item.label = std::string(label);
  item.enabled = enabled;
  return item;
}

void DisableMenuItem(MenuItem& item, std::string_view reason) {
  item.enabled = false;
  item.disabled_reason = std::string(reason);
}

MenuItem* FindMenuItemRecursive(MenuItem& node, std::string_view item_id) {
  if (node.id == item_id) {
    return &node;
  }
  for (auto& child : node.children) {
    if (auto* found = FindMenuItemRecursive(child, item_id)) {
      return found;
    }
  }
  return nullptr;
}

const MenuItem* FindMenuItemRecursive(const MenuItem& node, std::string_view item_id) {
  if (node.id == item_id) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (auto* found = FindMenuItemRecursive(child, item_id)) {
      return found;
    }
  }
  return nullptr;
}

UiPanelState* FindPanel(UiLayoutDocument& layout, std::string_view panel_id) {
  auto it = std::find_if(layout.panels.begin(), layout.panels.end(),
                         [panel_id](const UiPanelState& panel) {
                           return panel.id == panel_id;
                         });
  if (it == layout.panels.end()) {
    return nullptr;
  }
  return &(*it);
}

void EnsurePanelInOrder(UiLayoutDocument& layout, std::string_view panel_id) {
  const auto already = std::find(layout.panel_order.begin(), layout.panel_order.end(), std::string(panel_id));
  if (already == layout.panel_order.end()) {
    layout.panel_order.push_back(std::string(panel_id));
  }
}

void RemovePanelFromOrder(UiLayoutDocument& layout, std::string_view panel_id) {
  auto it = std::find(layout.panel_order.begin(), layout.panel_order.end(), std::string(panel_id));
  if (it != layout.panel_order.end()) {
    layout.panel_order.erase(it);
  }
}

bool HasSelection(const SelectionState& selection) {
  return !selection.selected_entity_ids.empty();
}

void ApplyEditSelectionRules(MenuItem& edit_menu, const SelectionState& selection) {
  const bool hasSelection = HasSelection(selection);
  const std::array<std::string_view, 15> selectionItems{
    "edit.cut",
    "edit.copy",
    "edit.paste",
    "edit.duplicate",
    "edit.delete",
    "edit.rename",
    "edit.group_selection",
    "edit.ungroup_selection",
    "edit.merge_selection",
    "edit.split_merged_object",
    "edit.reparent_selection",
    "edit.reset_transform",
    "scene_tree.cut",
    "scene_tree.duplicate",
    "scene_tree.delete"
  };

  for (auto& child : edit_menu.children) {
    const bool dependsOnSelection =
        std::find(selectionItems.begin(), selectionItems.end(), child.id) != selectionItems.end();
    if (dependsOnSelection) {
      if (hasSelection) {
        child.enabled = true;
        child.disabled_reason.clear();
      } else {
        DisableMenuItem(child, "requires at least one selected entity");
      }
    }
  }
}

UiPanelState MakePanelStateFromDefinition(const UiPanelDefinition& definition) {
  UiPanelState state;
  state.id = definition.id;
  state.visible = definition.default_visible;
  state.docked = definition.default_area != UiDockArea::Floating;
  state.floating = definition.default_area == UiDockArea::Floating;
  state.closable = definition.can_close;
  state.collapsible = definition.can_dock;
  state.collapsed = false;
  state.focused = definition.default_area == UiDockArea::Center;
  state.resized = true;
  state.movable = definition.can_dock || definition.can_float;
  state.width = definition.default_width;
  state.height = definition.default_height;
  return state;
}

UiPanelState* FindPanelById(std::vector<UiPanelState>& panels, std::string_view panel_id) {
  const auto it = std::find_if(panels.begin(), panels.end(),
                               [panel_id](const UiPanelState& panel) {
                                 return panel.id == panel_id;
                               });
  if (it == panels.end()) {
    return nullptr;
  }
  return &(*it);
}

void PlacePanel(std::vector<UiPanelState>& panels,
                std::string_view panel_id,
                bool visible,
                float x,
                float y,
                float width,
                float height,
                bool collapsed = false) {
  if (auto* panel = FindPanelById(panels, panel_id)) {
    panel->visible = visible;
    panel->x = x;
    panel->y = y;
    panel->width = width;
    panel->height = height;
    panel->collapsed = collapsed;
  }
}

void SetPanelVisibleIfPresent(std::vector<UiPanelState>& panels, std::string_view panel_id, bool visible) {
  if (auto* panel = FindPanelById(panels, panel_id)) {
    panel->visible = visible;
  }
}

std::vector<UiPanelState> BuildAllPanelStates() {
  const auto definitions = BuildDefaultPanelDefinitions();
  std::vector<UiPanelState> panels;
  panels.reserve(definitions.size());
  for (const auto& definition : definitions) {
    panels.push_back(MakePanelStateFromDefinition(definition));
  }
  return panels;
}

}  // namespace

UiLayoutDocument CreateDefaultLayout() {
  return CreateLayoutPreset(LayoutPreset::Default);
}

std::vector<UiPanelState> BuildDefaultPanelStates(LayoutPreset preset) {
  auto panels = BuildAllPanelStates();
  for (auto& panel : panels) {
    panel.visible = false;
    panel.collapsed = false;
    panel.focused = false;
  }

  auto placeShell = [&](bool chromeVisible) {
    PlacePanel(panels, "menu_bar", chromeVisible, 0.0f, 0.0f, 1280.0f, 28.0f);
    PlacePanel(panels, "toolbar", chromeVisible, 0.0f, 28.0f, 1280.0f, 36.0f);
    PlacePanel(panels, "status_bar", chromeVisible, 0.0f, 1048.0f, 1280.0f, 32.0f);
  };

  switch (preset) {
    case LayoutPreset::Benchmark:
      placeShell(true);
      PlacePanel(panels, "scene_tree", true, 0.0f, 64.0f, 260.0f, 360.0f);
      PlacePanel(panels, "inspector", true, 0.0f, 424.0f, 260.0f, 300.0f);
      PlacePanel(panels, "viewport", true, 260.0f, 64.0f, 620.0f, 660.0f);
      PlacePanel(panels, "benchmark_panel", true, 880.0f, 64.0f, 400.0f, 330.0f);
      PlacePanel(panels, "benchmark_history", true, 880.0f, 394.0f, 400.0f, 330.0f);
      PlacePanel(panels, "performance", true, 260.0f, 724.0f, 500.0f, 324.0f);
      PlacePanel(panels, "diagnostics", true, 760.0f, 724.0f, 520.0f, 324.0f);
      break;
    case LayoutPreset::MaterialAuthoring:
      placeShell(true);
      PlacePanel(panels, "scene_tree", true, 0.0f, 64.0f, 260.0f, 360.0f);
      PlacePanel(panels, "inspector", true, 0.0f, 424.0f, 260.0f, 300.0f);
      PlacePanel(panels, "viewport", true, 260.0f, 64.0f, 560.0f, 660.0f);
      PlacePanel(panels, "material_editor", true, 820.0f, 64.0f, 460.0f, 250.0f);
      PlacePanel(panels, "lights_panel", true, 820.0f, 314.0f, 230.0f, 220.0f);
      PlacePanel(panels, "camera_panel", true, 1050.0f, 314.0f, 230.0f, 220.0f);
      PlacePanel(panels, "render_settings", true, 820.0f, 534.0f, 460.0f, 190.0f);
      PlacePanel(panels, "asset_browser", true, 260.0f, 724.0f, 660.0f, 324.0f);
      PlacePanel(panels, "console", true, 920.0f, 724.0f, 360.0f, 324.0f);
      break;
    case LayoutPreset::Scripting:
      placeShell(true);
      PlacePanel(panels, "scene_tree", true, 0.0f, 64.0f, 260.0f, 360.0f);
      PlacePanel(panels, "inspector", true, 0.0f, 424.0f, 260.0f, 300.0f);
      PlacePanel(panels, "script_panel", true, 260.0f, 64.0f, 420.0f, 660.0f);
      PlacePanel(panels, "diagnostics", true, 680.0f, 64.0f, 300.0f, 320.0f);
      PlacePanel(panels, "asset_browser", true, 680.0f, 384.0f, 300.0f, 340.0f);
      PlacePanel(panels, "console", true, 260.0f, 724.0f, 1020.0f, 324.0f);
      PlacePanel(panels, "viewport", true, 980.0f, 64.0f, 300.0f, 660.0f);
      break;
    case LayoutPreset::AssetManagement:
      placeShell(true);
      PlacePanel(panels, "scene_tree", true, 0.0f, 64.0f, 260.0f, 360.0f);
      PlacePanel(panels, "asset_browser", true, 260.0f, 64.0f, 700.0f, 660.0f);
      PlacePanel(panels, "inspector", true, 960.0f, 64.0f, 320.0f, 360.0f);
      PlacePanel(panels, "diagnostics", true, 960.0f, 424.0f, 320.0f, 300.0f);
      PlacePanel(panels, "console", true, 260.0f, 724.0f, 1020.0f, 324.0f);
      PlacePanel(panels, "viewport", true, 0.0f, 424.0f, 260.0f, 300.0f);
      break;
    case LayoutPreset::DebugProfiler:
      placeShell(true);
      PlacePanel(panels, "scene_tree", true, 0.0f, 64.0f, 260.0f, 320.0f);
      PlacePanel(panels, "inspector", true, 0.0f, 384.0f, 260.0f, 340.0f);
      PlacePanel(panels, "diagnostics", true, 260.0f, 64.0f, 340.0f, 330.0f);
      PlacePanel(panels, "performance", true, 600.0f, 64.0f, 340.0f, 330.0f);
      PlacePanel(panels, "debug_views", true, 940.0f, 64.0f, 340.0f, 330.0f);
      PlacePanel(panels, "render_settings", true, 940.0f, 394.0f, 340.0f, 330.0f);
      PlacePanel(panels, "console", true, 260.0f, 724.0f, 1020.0f, 324.0f);
      PlacePanel(panels, "viewport", true, 260.0f, 394.0f, 680.0f, 330.0f);
      break;
    case LayoutPreset::MinimalViewport:
      PlacePanel(panels, "viewport", true, 0.0f, 0.0f, 1280.0f, 1080.0f);
      SetPanelVisibleIfPresent(panels, "status_bar", false);
      break;
    case LayoutPreset::FullscreenViewportWithOverlay:
      PlacePanel(panels, "viewport", true, 0.0f, 0.0f, 1280.0f, 1080.0f);
      PlacePanel(panels, "status_bar", false, 0.0f, 1048.0f, 1280.0f, 32.0f);
      PlacePanel(panels, "console", false, 0.0f, 844.0f, 1280.0f, 204.0f);
      PlacePanel(panels, "inspector", false, 960.0f, 64.0f, 320.0f, 480.0f);
      break;
    case LayoutPreset::Default:
    default:
      placeShell(true);
      PlacePanel(panels, "scene_tree", true, 0.0f, 64.0f, 280.0f, 500.0f);
      PlacePanel(panels, "asset_browser", true, 0.0f, 564.0f, 280.0f, 484.0f);
      PlacePanel(panels, "inspector", true, 960.0f, 64.0f, 320.0f, 360.0f);
      PlacePanel(panels, "render_settings", true, 960.0f, 424.0f, 320.0f, 300.0f);
      PlacePanel(panels, "console", true, 280.0f, 724.0f, 1000.0f, 324.0f);
      PlacePanel(panels, "viewport", true, 280.0f, 64.0f, 680.0f, 660.0f);
      break;
  }

  return panels;
}

UiLayoutDocument CreateLayoutPreset(LayoutPreset preset) {
  UiLayoutDocument layout;
  layout.preset = preset;
  layout.active_layout_name = std::string(ParseLayoutName(preset));
  layout.dpi_scale = 1.0f;
  layout.ui_scale = 1.0f;
  layout.fullscreen_overlay = (preset == LayoutPreset::FullscreenViewportWithOverlay);
  layout.panels = BuildDefaultPanelStates(preset);
  layout.panel_order.clear();
  for (const auto& panel : layout.panels) {
    if (panel.visible) {
      layout.panel_order.push_back(panel.id);
    }
  }
  return layout;
}

MenuBar BuildDefaultMenuBar() {
  MenuBar result;
  auto& top = result.top_level_menus;
  top.push_back(MenuItemNode("file", "File"));
  top.back().children.push_back(MenuItemNode("file.new_scene", "New Scene"));
  top.back().children.push_back(MenuItemNode("file.open_scene", "Open Scene"));
  top.back().children.push_back(MenuItemNode("file.open_recent", "Open Recent"));
  top.back().children.push_back(MenuItemNode("file.save_scene", "Save Scene"));
  top.back().children.push_back(MenuItemNode("file.save_scene_as", "Save Scene As"));
  top.back().children.push_back(MenuItemNode("file.clone_scene", "Clone Scene"));
  top.back().children.push_back(MenuItemNode("file.import_asset", "Import Asset"));
  top.back().children.push_back(MenuItemNode("file.export_image", "Export Image"));
  top.back().children.push_back(MenuItemNode("file.export_exr", "Export EXR"));
  top.back().children.push_back(MenuItemNode("file.export_benchmark_artifacts", "Export Benchmark Artifacts"));
  top.back().children.push_back(MenuItemNode("file.export_scene_snapshot", "Export Scene Snapshot"));
  top.back().children.push_back(MenuItemNode("file.preferences", "Preferences"));
  top.back().children.push_back(MenuItemNode("file.reveal_artifacts_folder", "Reveal Artifacts Folder"));
  top.back().children.push_back(MenuItemNode("file.exit", "Exit"));

  top.push_back(MenuItemNode("edit", "Edit"));
  top.back().children.push_back(MenuItemNode("edit.undo", "Undo"));
  top.back().children.push_back(MenuItemNode("edit.redo", "Redo"));
  top.back().children.push_back(MenuItemNode("edit.cut", "Cut"));
  top.back().children.push_back(MenuItemNode("edit.copy", "Copy"));
  top.back().children.push_back(MenuItemNode("edit.paste", "Paste"));
  top.back().children.push_back(MenuItemNode("edit.duplicate", "Duplicate"));
  top.back().children.push_back(MenuItemNode("edit.delete", "Delete"));
  top.back().children.push_back(MenuItemNode("edit.rename", "Rename"));
  top.back().children.push_back(MenuItemNode("edit.select_all", "Select All"));
  top.back().children.push_back(MenuItemNode("edit.select_none", "Select None"));
  top.back().children.push_back(MenuItemNode("edit.invert_selection", "Invert Selection"));
  top.back().children.push_back(MenuItemNode("edit.group_selection", "Group Selection"));
  top.back().children.push_back(MenuItemNode("edit.ungroup_selection", "Ungroup Selection"));
  top.back().children.push_back(MenuItemNode("edit.merge_selection", "Merge Selection"));
  top.back().children.push_back(MenuItemNode("edit.split_merged_object", "Split Merged Object"));
  top.back().children.push_back(MenuItemNode("edit.reparent_selection", "Reparent Selection"));
  top.back().children.push_back(MenuItemNode("edit.reset_transform", "Reset Transform"));
  top.back().children.push_back(MenuItemNode("edit.command_history", "Command History"));

  top.push_back(MenuItemNode("view", "View"));
  MenuItem panelsMenu = MenuItemNode("view.panels", "Panels");
  for (const auto& panel : BuildDefaultPanelDefinitions()) {
    if (panel.can_close || panel.default_area == UiDockArea::Bottom || panel.default_area == UiDockArea::Right ||
        panel.default_area == UiDockArea::Left) {
      panelsMenu.children.push_back(MenuItemNode(panel.menu_action_id, panel.title));
    }
  }
  top.back().children.push_back(std::move(panelsMenu));
  MenuItem layoutsMenu = MenuItemNode("view.layouts", "Layouts");
  layoutsMenu.children.push_back(MenuItemNode("view.layout.default", "Default"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.benchmark", "Benchmark"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.material_authoring", "Material Authoring"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.scripting", "Scripting"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.asset_management", "Asset Management"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.debug_profiler", "Debug/Profiler"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.minimal_viewport", "Minimal Viewport"));
  top.back().children.push_back(std::move(layoutsMenu));
  top.back().children.push_back(MenuItemNode("view.overlays", "Overlays"));
  top.back().children.push_back(MenuItemNode("view.debug_views", "Debug Views"));
  top.back().children.push_back(MenuItemNode("view.fullscreen", "Fullscreen"));
  top.back().children.push_back(MenuItemNode("view.ui_scale", "UI Scale"));
  top.back().children.push_back(MenuItemNode("view.reset_layout", "Reset Layout"));

  top.push_back(MenuItemNode("create", "Create"));
  top.back().children.push_back(MenuItemNode("create.empty_entity", "Empty Entity"));
  top.back().children.push_back(MenuItemNode("create.group_entity", "Group Entity"));
  top.back().children.push_back(MenuItemNode("create.camera", "Camera"));
  top.back().children.push_back(MenuItemNode("create.light", "Light"));
  top.back().children.push_back(MenuItemNode("create.mesh_primitives", "Primitive Mesh"));
  top.back().children.push_back(MenuItemNode("create.sdf_primitives", "SDF Primitive"));
  top.back().children.push_back(MenuItemNode("create.material", "Material"));
  top.back().children.push_back(MenuItemNode("create.script", "Script"));
  top.back().children.push_back(MenuItemNode("create.physics_body", "Physics Body"));
  top.back().children.push_back(MenuItemNode("create.benchmark_marker", "Benchmark Marker"));

  top.push_back(MenuItemNode("scene", "Scene"));
  top.back().children.push_back(MenuItemNode("scene.validate_scene", "Validate Scene"));
  top.back().children.push_back(MenuItemNode("scene.freeze_benchmark_snapshot", "Freeze Benchmark Snapshot"));
  top.back().children.push_back(MenuItemNode("scene.reset_accumulation", "Reset Accumulation"));
  top.back().children.push_back(MenuItemNode("scene.reload_scene", "Reload Scene"));
  top.back().children.push_back(MenuItemNode("scene.reload_assets", "Reload Assets"));
  top.back().children.push_back(MenuItemNode("scene.hot_reload_scripts", "Hot Reload Scripts"));
  top.back().children.push_back(MenuItemNode("scene.scene_settings", "Scene Settings"));
  top.back().children.push_back(MenuItemNode("scene.lighting_settings", "Lighting Settings"));
  top.back().children.push_back(MenuItemNode("scene.environment_settings", "Environment Settings"));
  top.back().children.push_back(MenuItemNode("scene.camera_settings", "Camera Settings"));
  top.back().children.push_back(MenuItemNode("scene.physics_settings", "Physics Settings"));
  top.back().children.push_back(MenuItemNode("scene.script_settings", "Script Settings"));
  top.back().children.push_back(MenuItemNode("scene.animation_settings", "Animation Settings"));

  top.push_back(MenuItemNode("render", "Render"));
  top.back().children.push_back(MenuItemNode("render.backend", "Backend"));
  top.back().children.push_back(MenuItemNode("render.renderer_path", "Renderer Path"));
  top.back().children.push_back(MenuItemNode("render.quality_presets", "Quality Presets"));
  top.back().children.push_back(MenuItemNode("render.resolution", "Resolution"));
  top.back().children.push_back(MenuItemNode("render.spp", "SPP"));
  top.back().children.push_back(MenuItemNode("render.max_bounces", "Max Bounces"));
  top.back().children.push_back(MenuItemNode("render.denoiser", "Denoiser"));
  top.back().children.push_back(MenuItemNode("render.tone_mapping", "Tone Mapping"));
  top.back().children.push_back(MenuItemNode("render.exposure", "Exposure"));
  top.back().children.push_back(MenuItemNode("render.debug_channel", "Debug Channel"));
  top.back().children.push_back(MenuItemNode("render.shader_cache", "Shader Cache"));
  top.back().children.push_back(MenuItemNode("render.backend_capabilities", "Backend Capabilities"));

  top.push_back(MenuItemNode("benchmark", "Benchmark"));
  top.back().children.push_back(MenuItemNode("benchmark.run_current_scene", "Run Current Scene"));
  top.back().children.push_back(MenuItemNode("benchmark.run_scene_pack", "Run Scene Pack"));
  top.back().children.push_back(MenuItemNode("benchmark.run_cpu_calibration", "Run CPU Calibration"));
  top.back().children.push_back(MenuItemNode("benchmark.run_gpu_calibration", "Run GPU Calibration"));
  top.back().children.push_back(MenuItemNode("benchmark.run_simd_experiment", "Run SIMD Experiment"));
  top.back().children.push_back(MenuItemNode("benchmark.run_backend_experiment", "Run Backend Experiment"));
  top.back().children.push_back(MenuItemNode("benchmark.compare_against_reference", "Compare Against Reference"));
  top.back().children.push_back(MenuItemNode("benchmark.open_artifacts", "Open Benchmark Artifacts"));
  top.back().children.push_back(MenuItemNode("benchmark.export_csv_json", "Export CSV/JSON"));
  top.back().children.push_back(MenuItemNode("benchmark.history", "Benchmark History"));

  top.push_back(MenuItemNode("assets", "Assets"));
  top.back().children.push_back(MenuItemNode("assets.import_files", "Import Files"));
  top.back().children.push_back(MenuItemNode("assets.reimport_selected", "Reimport Selected"));
  top.back().children.push_back(MenuItemNode("assets.refresh_browser", "Refresh Asset Browser"));
  top.back().children.push_back(MenuItemNode("assets.show_missing_assets", "Show Missing Assets"));
  top.back().children.push_back(MenuItemNode("assets.show_import_diagnostics", "Show Import Diagnostics"));
  top.back().children.push_back(MenuItemNode("assets.clear_generated_cache", "Clear Generated Cache"));
  top.back().children.push_back(MenuItemNode("assets.clear_shader_cache", "Clear Shader Cache"));

  top.push_back(MenuItemNode("scripts", "Scripts"));
  top.back().children.push_back(MenuItemNode("scripts.new_lua_script", "New Lua Script"));
  top.back().children.push_back(MenuItemNode("scripts.attach_script_to_selection", "Attach Script To Selection"));
  top.back().children.push_back(MenuItemNode("scripts.detach_script_from_selection", "Detach Script From Selection"));
  top.back().children.push_back(MenuItemNode("scripts.reload_scripts", "Reload Scripts"));
  top.back().children.push_back(MenuItemNode("scripts.open_script_folder", "Open Script Folder"));
  top.back().children.push_back(MenuItemNode("scripts.show_script_lifecycle_events", "Show Script Lifecycle Events"));
  top.back().children.push_back(MenuItemNode("scripts.show_script_errors", "Show Script Errors"));
  top.back().children.push_back(MenuItemNode("scripts.show_script_profiler", "Show Script Profiler"));
  top.back().children.push_back(MenuItemNode("scripts.sandbox_settings", "Sandbox Settings"));

  top.push_back(MenuItemNode("tools", "Tools"));
  top.back().children.push_back(MenuItemNode("tools.doctor", "Doctor"));
  top.back().children.push_back(MenuItemNode("tools.crash_artifacts", "Crash Artifacts"));
  top.back().children.push_back(MenuItemNode("tools.profiler", "Profiler"));
  top.back().children.push_back(MenuItemNode("tools.frame_capture", "Frame Capture"));
  top.back().children.push_back(MenuItemNode("tools.shader_manifest", "Shader Manifest"));
  top.back().children.push_back(MenuItemNode("tools.asset_manifest", "Asset Manifest"));
  top.back().children.push_back(MenuItemNode("tools.scene_snapshot", "Scene Snapshot"));
  top.back().children.push_back(MenuItemNode("tools.capability_matrix", "Capability Matrix"));
  top.back().children.push_back(MenuItemNode("tools.settings_dump", "Settings Dump"));
  top.back().children.push_back(MenuItemNode("tools.startup_self_test", "Run Startup Self-Test"));

  top.push_back(MenuItemNode("help", "Help"));
  top.back().children.push_back(MenuItemNode("help.controls", "Controls"));
  top.back().children.push_back(MenuItemNode("help.shortcut_reference", "Shortcut Reference"));
  top.back().children.push_back(MenuItemNode("help.about", "About"));
  top.back().children.push_back(MenuItemNode("help.build_info", "Build Info"));
  top.back().children.push_back(MenuItemNode("help.feature_flags", "Feature Flags"));
  top.back().children.push_back(MenuItemNode("help.dependency_info", "Dependency Info"));
  return result;
}

MenuBar BuildDefaultMenuBar(const SelectionState& selection) {
  auto menu = BuildDefaultMenuBar();
  return ApplySelectionRulesToEditMenu(std::move(menu), selection);
}

MenuItem* FindMenuItem(MenuBar& menu, std::string_view item_id) {
  for (auto& item : menu.top_level_menus) {
    if (auto* found = FindMenuItemRecursive(item, item_id)) {
      return found;
    }
  }
  return nullptr;
}

const MenuItem* FindMenuItem(const MenuBar& menu, std::string_view item_id) {
  for (const auto& item : menu.top_level_menus) {
    if (auto* found = FindMenuItemRecursive(item, item_id)) {
      return found;
    }
  }
  return nullptr;
}

MenuBar ApplySelectionRulesToEditMenu(MenuBar menu, const SelectionState& selection) {
  auto* edit_menu = FindMenuItem(menu, "edit");
  if (!edit_menu) {
    return menu;
  }
  ApplyEditSelectionRules(*edit_menu, selection);
  return menu;
}

std::vector<MenuEnablement> GetEditMenuEnablements(const SelectionState& selection) {
  std::vector<MenuEnablement> result;
  const auto edit_menu = BuildDefaultMenuBar(selection);
  const auto* edit = FindMenuItem(edit_menu, "edit");
  if (!edit) {
    return result;
  }
  result.reserve(edit->children.size());
  for (const auto& item : edit->children) {
    result.push_back({item.id, item.enabled});
  }
  return result;
}

UiRuntimeState CreateDefaultRuntimeState() {
  UiRuntimeState state;
  state.active_layout_name = ParseLayoutName(LayoutPreset::Default);
  state.focused_panel = "viewport";
  state.active_modal.clear();
  state.hovered_widget.clear();
  state.active_drag_drop_operation.clear();
  state.active_viewport_tool = ViewportTool::Select;
  state.active_gizmo_mode = GizmoMode::None;
  state.dpi_scale = 1.0f;
  state.ui_scale = 1.0f;
  state.selected_debug_view = "none";
  state.active_renderer_backend = "cpu";
  state.active_renderer_path = "cpu_scalar";
  state.spp_accumulated = 0;
  state.fps = 0.0;
  state.frame_ms = 0.0;
  state.background_job_count = 0;
  state.visible_panels = {"scene_tree", "inspector", "asset_browser", "viewport", "status_bar", "console"};
  return state;
}

SelectionState CreateDefaultSelectionState() {
  SelectionState state;
  state.selection_source = SelectionSource::Unknown;
  state.selected_group = 0;
  state.active_primary_entity = 0;
  state.aggregate_bounds.valid = false;
  return state;
}

SelectionState ApplySelectionCommand(const SelectionState& state, const EditorCommand& command) {
  SelectionState next = state;
  next.last_change_frame = command.frame_index;

  if (command.source_widget.find("viewport") != std::string::npos) {
    next.selection_source = SelectionSource::Viewport;
  } else if (command.source_widget.find("scene_tree") != std::string::npos) {
    next.selection_source = SelectionSource::SceneTree;
  } else if (command.source_widget.find("inspector") != std::string::npos) {
    next.selection_source = SelectionSource::Inspector;
  } else if (command.source_widget.find("asset") != std::string::npos) {
    next.selection_source = SelectionSource::AssetBrowser;
  } else if (command.source_widget.find("script") != std::string::npos) {
    next.selection_source = SelectionSource::ScriptPanel;
  }

  auto drop_nonselected_bounds = [&next]() {
    next.per_item_bounds.erase(
      std::remove_if(next.per_item_bounds.begin(), next.per_item_bounds.end(),
                     [&next](const SceneEntityBounds& item) {
                       return std::find(next.selected_entity_ids.begin(),
                                        next.selected_entity_ids.end(),
                                        item.entity_id) == next.selected_entity_ids.end();
                     }),
      next.per_item_bounds.end());
    if (next.selected_entity_ids.empty()) {
      next.hovered_entity = 0;
      next.active_primary_entity = 0;
      next.aggregate_bounds = Bounds{};
    }
  };

  auto clear_all_selection = [&next]() {
    next.selected_entity_ids.clear();
    next.active_primary_entity = 0;
    next.hovered_entity_ids.clear();
    next.hovered_entity = 0;
    next.aggregate_bounds = Bounds{};
    next.per_item_bounds.clear();
  };

  auto add_or_toggle = [&next](vkpt::core::StableId entity_id) {
    const auto existing = std::find(next.selected_entity_ids.begin(),
                                    next.selected_entity_ids.end(),
                                    entity_id);
    if (existing == next.selected_entity_ids.end()) {
      next.selected_entity_ids.push_back(entity_id);
      next.hovered_entity = entity_id;
      next.active_primary_entity = entity_id;
      return;
    }
    next.selected_entity_ids.erase(existing);
    if (next.active_primary_entity == entity_id) {
      next.active_primary_entity = next.selected_entity_ids.empty() ? 0u : next.selected_entity_ids.back();
    }
    if (next.hovered_entity == entity_id) {
      next.hovered_entity = next.active_primary_entity;
    }
  };

  auto ensure_primary = [&next]() {
    if (next.active_primary_entity == 0 && !next.selected_entity_ids.empty()) {
      next.active_primary_entity = next.selected_entity_ids.front();
      next.hovered_entity = next.active_primary_entity;
    }
  };

  auto set_single_selection = [&](vkpt::core::StableId entity_id) {
    next.selected_entity_ids.clear();
    if (entity_id != 0) {
      next.selected_entity_ids.push_back(entity_id);
    }
    next.active_primary_entity = entity_id;
    next.hovered_entity = entity_id;
    ensure_primary();
    drop_nonselected_bounds();
  };

  std::visit([&](auto&& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, SelectEntityCommand>) {
      if (payload.entity_id == 0) {
        return;
      }
      if (payload.append) {
        if (payload.range_mode) {
          if (next.active_primary_entity == 0) {
            set_single_selection(payload.entity_id);
          } else {
            if (std::find(next.selected_entity_ids.begin(), next.selected_entity_ids.end(), payload.entity_id) ==
                next.selected_entity_ids.end()) {
              next.selected_entity_ids.push_back(payload.entity_id);
            }
            if (std::find(next.selected_entity_ids.begin(), next.selected_entity_ids.end(),
                          next.active_primary_entity) == next.selected_entity_ids.end()) {
              next.selected_entity_ids.push_back(next.active_primary_entity);
            }
            next.hovered_entity = payload.entity_id;
            next.active_primary_entity = payload.entity_id;
          }
        } else {
          add_or_toggle(payload.entity_id);
          ensure_primary();
        }
      } else {
        if (payload.range_mode) {
          next.selected_entity_ids.clear();
          next.selected_entity_ids.push_back(payload.entity_id);
          if (next.active_primary_entity != 0 && next.active_primary_entity != payload.entity_id) {
            next.selected_entity_ids.push_back(next.active_primary_entity);
          }
          next.hovered_entity = payload.entity_id;
          next.active_primary_entity = payload.entity_id;
        } else {
          set_single_selection(payload.entity_id);
        }
      }
      ensure_primary();
      drop_nonselected_bounds();
    } else if constexpr (std::is_same_v<T, ToggleSelectEntityCommand>) {
      add_or_toggle(payload.entity_id);
      ensure_primary();
      drop_nonselected_bounds();
    } else if constexpr (std::is_same_v<T, ClearSelectionCommand>) {
      clear_all_selection();
    }
  }, command.payload);

  return next;
}

std::vector<InspectorFieldValue> BuildInspectorFieldStates(
    std::string_view field_id,
    const std::vector<std::string>& values) {
  InspectorFieldValue field;
  field.field_id = std::string(field_id);
  if (values.empty()) {
    field.unsupported = true;
    field.value.clear();
    return {field};
  }
  if (values.size() == 1) {
    field.value = values[0];
    return {field};
  }
  field.value = values[0];
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i] != field.value) {
      field.mixed = true;
      break;
    }
  }
  return {field};
}

AssetDropValidation ValidateAssetDrop(std::string_view asset_path,
                                     std::string_view target_slot) {
  AssetDropValidation result;
  if (asset_path.empty()) {
    result.reason = "empty asset path";
    return result;
  }

  const std::filesystem::path path(asset_path);
  result.normalized_path = std::filesystem::absolute(path).string();
  const auto ext = ToLower(path.extension().string());
  const std::string_view ext_no_dot = ext.empty() ? std::string_view{} : std::string_view(ext).substr(1);

  static const std::vector<std::string_view> texture_extensions{
    "png", "jpg", "jpeg", "bmp", "tga", "tiff", "tif", "exr", "hdr"
  };
  static const std::vector<std::string_view> model_extensions{
    "obj", "fbx", "gltf", "glb", "ply", "abc", "usd", "usdc", "usda"
  };
  static const std::vector<std::string_view> scene_extensions{
    "json", "ptscene"
  };
  static const std::vector<std::string_view> lua_extensions{
    "lua"
  };
  static const std::vector<std::string_view> benchmark_extensions{
    "json", "csv", "txt"
  };

  const bool is_texture = IsIn(texture_extensions, ext_no_dot);
  const bool is_model = IsIn(model_extensions, ext_no_dot);
  const bool is_scene = IsIn(scene_extensions, ext_no_dot);
  const bool is_lua = IsIn(lua_extensions, ext_no_dot);
  const bool is_benchmark = IsIn(benchmark_extensions, ext_no_dot);
  result.extension_supported = is_texture || is_model || is_scene || is_lua || is_benchmark;

  if (!result.extension_supported) {
    result.reason = "unsupported extension";
    return result;
  }

  if (target_slot == "texture_slot" || target_slot == "material_slot" || target_slot == "material") {
    if (!is_texture) {
      result.reason = "target expects texture/texture-like asset";
      return result;
    }
    result.accepted = true;
    result.asset_type = "texture";
    result.target_slot = std::string(target_slot);
    return result;
  }
  if (target_slot == "script_slot" || target_slot == "script" || target_slot == "lua_slot") {
    if (!is_lua) {
      result.reason = "target expects lua script";
      return result;
    }
    result.accepted = true;
    result.asset_type = "script";
    result.target_slot = std::string(target_slot);
    return result;
  }
  if (target_slot == "scene_slot" || target_slot == "scene" || target_slot == "scene_open") {
    if (!is_scene) {
      result.reason = "target expects scene asset";
      return result;
    }
    result.accepted = true;
    result.asset_type = "scene";
    result.target_slot = std::string(target_slot);
    return result;
  }
  if (target_slot == "benchmark_slot" || target_slot == "benchmark_reference") {
    if (!is_benchmark) {
      result.reason = "target expects benchmark descriptor";
      return result;
    }
    result.accepted = true;
    result.asset_type = "benchmark_descriptor";
    result.target_slot = std::string(target_slot);
    return result;
  }

  if (target_slot == "model_slot" || target_slot == "asset_browser" || target_slot.empty()) {
    result.accepted = true;
    if (is_model) result.asset_type = "model";
    else if (is_texture) result.asset_type = "texture";
    else if (is_scene) result.asset_type = "scene";
    else if (is_lua) result.asset_type = "script";
    else result.asset_type = "benchmark_descriptor";
    result.target_slot = std::string(target_slot);
    return result;
  }

  result.reason = "unknown drop target";
  return result;
}

vkpt::benchmark::BenchmarkRunDesc MakeDefaultBenchmarkRunDesc(std::string_view scene_path,
                                                             std::string_view backend,
                                                             std::string_view renderer_path,
                                                             std::uint32_t spp,
                                                             std::uint32_t max_depth,
                                                             std::uint64_t seed,
                                                             std::uint32_t width,
                                                             std::uint32_t height) {
  vkpt::benchmark::BenchmarkRunDesc desc;
  desc.scene_path = std::string(scene_path);
  desc.backend = std::string(backend);
  desc.renderer_path = std::string(renderer_path);
  desc.resolution.width = width;
  desc.resolution.height = height;
  desc.samples_per_pixel = spp;
  desc.max_depth = max_depth;
  desc.seed = seed;
  desc.duration = 0.0;
  desc.warmup_frames = 0;
  desc.tolerance_policy = "default";
  return desc;
}

std::vector<UiShortcut> BuildDefaultUiShortcuts() {
  return {
    {static_cast<std::uint32_t>('S'), true, false, false, "file.save_scene", "Save Scene"},
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open Scene"},
    {static_cast<std::uint32_t>('Z'), true, false, false, "edit.undo", "Undo"},
    {static_cast<std::uint32_t>('Y'), true, false, false, "edit.redo", "Redo"},
    {static_cast<std::uint32_t>('D'), true, false, false, "edit.duplicate", "Duplicate"},
    {static_cast<std::uint32_t>(127), false, false, false, "edit.delete", "Delete"},
    {static_cast<std::uint32_t>('F'), false, false, false, "view.focus_selected", "Focus Selected"},
    {static_cast<std::uint32_t>('W'), false, false, false, "gizmo.translate", "Translate"},
    {static_cast<std::uint32_t>('E'), false, false, false, "gizmo.rotate", "Rotate"},
    {static_cast<std::uint32_t>('R'), false, false, false, "gizmo.scale", "Scale"},
    {static_cast<std::uint32_t>('Q'), false, false, false, "gizmo.select", "Select"},
    {static_cast<std::uint32_t>('G'), true, false, false, "edit.group_selection", "Group Selection"},
    {static_cast<std::uint32_t>('G'), true, true, false, "edit.ungroup_selection", "Ungroup Selection"},
    {static_cast<std::uint32_t>('B'), true, false, false, "benchmark.run_current_scene", "Run Benchmark"},
    {static_cast<std::uint32_t>(122), false, false, false, "view.fullscreen", "Fullscreen"},
  };
}

bool DetectShortcutConflicts(const std::vector<UiShortcut>& shortcuts) {
  std::vector<std::tuple<std::uint32_t, bool, bool, bool>> seen;
  for (const auto& shortcut : shortcuts) {
    const auto key = std::make_tuple(shortcut.key_code, shortcut.ctrl, shortcut.shift, shortcut.alt);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      return true;
    }
    seen.push_back(key);
  }
  return false;
}

bool DetectShortcutConflicts(const std::vector<UiShortcutAction>& shortcuts) {
  std::vector<std::tuple<std::uint32_t, bool, bool, bool>> seen;
  for (const auto& shortcut : shortcuts) {
    const auto key = std::make_tuple(shortcut.key_code, shortcut.ctrl, shortcut.shift, shortcut.alt);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      return true;
    }
    seen.push_back(key);
  }
  return false;
}

std::vector<UiPanelDefinition> BuildDefaultPanelDefinitions() {
  return {
    {"menu_bar", "Menu Bar", "view.panel.menu_bar", true, false, false, false, 1280.0f, 28.0f,
     UiDockArea::Top, "chrome", 0, "chrome", "Application menus and command routing."},
    {"toolbar", "Toolbar", "view.panel.toolbar", true, false, false, false, 1280.0f, 36.0f,
     UiDockArea::Top, "chrome", 1, "chrome", "Primary viewport and editing commands."},
    {"viewport", "Viewport", "view.panel.viewport", true, true, false, false, 960.0f, 540.0f,
     UiDockArea::Center, "center", 10, "viewport", "Interactive render viewport."},
    {"scene_tree", "Scene Graph", "view.panel.scene_tree", true, true, true, true, 280.0f, 600.0f,
     UiDockArea::Left, "scene", 20, "scene_tree", "Hierarchy, visibility, lock, and selection state."},
    {"inspector", "Inspector", "view.panel.inspector", true, true, true, true, 360.0f, 600.0f,
     UiDockArea::Right, "properties", 30, "inspector", "Selected entity and component properties."},
    {"material_editor", "Materials", "view.panel.material_editor", false, true, true, true, 520.0f, 420.0f,
     UiDockArea::Right, "properties", 40, "materials", "Material assignment and shader parameters."},
    {"lights_panel", "Lights", "view.panel.lights", false, true, true, true, 360.0f, 360.0f,
     UiDockArea::Right, "properties", 50, "lights", "Light list and selected light controls."},
    {"camera_panel", "Camera", "view.panel.camera", false, true, true, true, 360.0f, 320.0f,
     UiDockArea::Right, "properties", 60, "camera", "Camera pose, lens, and navigation settings."},
    {"render_settings", "Render Settings", "view.panel.render_settings", true, true, true, true, 380.0f, 420.0f,
     UiDockArea::Right, "properties", 70, "render_settings", "Renderer backend, quality, accumulation, and tone mapping."},
    {"benchmark_panel", "Benchmark", "view.panel.benchmark", false, true, true, true, 560.0f, 480.0f,
     UiDockArea::Right, "benchmark", 80, "benchmark", "Benchmark descriptor and run controls."},
    {"benchmark_history", "Benchmark History", "view.panel.benchmark_history", false, true, true, true, 680.0f, 360.0f,
     UiDockArea::Bottom, "benchmark", 90, "benchmark_history", "Stored benchmark runs, artifacts, and regressions."},
    {"diagnostics", "Diagnostics", "view.panel.diagnostics", false, true, true, true, 520.0f, 300.0f,
     UiDockArea::Bottom, "debug", 100, "diagnostics", "Warnings, errors, import diagnostics, and event log filters."},
    {"performance", "Performance", "view.panel.performance", false, true, true, true, 520.0f, 300.0f,
     UiDockArea::Bottom, "debug", 110, "performance", "Frame timing, job pressure, memory, and UI budget."},
    {"debug_views", "Debug Views", "view.panel.debug_views", false, true, true, true, 420.0f, 320.0f,
     UiDockArea::Right, "debug", 120, "debug_views", "Renderer debug channels and visualization toggles."},
    {"asset_browser", "Asset Browser", "view.panel.asset_browser", true, true, true, true, 720.0f, 260.0f,
     UiDockArea::Bottom, "assets", 130, "asset_browser", "Asset search, validation, import, and assignment."},
    {"timeline", "Timeline", "view.panel.timeline", false, true, true, true, 720.0f, 220.0f,
     UiDockArea::Bottom, "animation", 140, "timeline", "Animation timeline and keyframe controls."},
    {"script_panel", "Scripts", "view.panel.script_panel", false, true, true, true, 520.0f, 420.0f,
     UiDockArea::Right, "scripting", 150, "scripts", "Script attachment, lifecycle events, and sandbox settings."},
    {"physics", "Physics", "view.panel.physics", false, true, true, true, 420.0f, 360.0f,
     UiDockArea::Right, "simulation", 160, "physics", "Physics bodies, colliders, and simulation properties."},
    {"console", "Console", "view.panel.console", true, true, true, true, 720.0f, 260.0f,
     UiDockArea::Bottom, "debug", 170, "console", "Command log and textual diagnostics."},
    {"status_bar", "Status Bar", "view.panel.status_bar", true, false, false, false, 1280.0f, 28.0f,
     UiDockArea::Status, "chrome", 180, "status", "Scene, renderer, performance, and selection summary."},
  };
}

std::vector<InspectorFieldSchema> BuildDefaultInspectorSchemas() {
  return {
    {"transform.position", "Position", "Transform", InspectorControlKind::Vector3, true, true, -100000.0f, 100000.0f, 0.01f, {}},
    {"transform.rotation", "Rotation", "Transform", InspectorControlKind::Vector3, true, true, -360.0f, 360.0f, 0.1f, {}},
    {"transform.scale", "Scale", "Transform", InspectorControlKind::Vector3, true, true, 0.0001f, 10000.0f, 0.01f, {}},
    {"transform.visible", "Visible", "Transform", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"transform.locked", "Locked", "Transform", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"material.id", "Material", "Material", InspectorControlKind::AssetPicker, true, true, 0.0f, 0.0f, 0.0f, {}},
    {"material.base_color", "Base Color", "Material", InspectorControlKind::Color, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.opacity", "Opacity", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.roughness", "Roughness", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.metallic", "Metallic", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.transmission", "Transmission", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.ior", "Index of Refraction", "Material", InspectorControlKind::Number, true, true, 1.0f, 3.0f, 0.01f, {}},
    {"material.emission_color", "Emission Color", "Material", InspectorControlKind::Color, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.emission_strength", "Emission Strength", "Material", InspectorControlKind::Number, true, true, 0.0f, 100000.0f, 0.1f, {}},
    {"light.type", "Type", "Light", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"point", "sphere", "directional", "environment"}},
    {"light.intensity", "Intensity", "Light", InspectorControlKind::Number, true, true, 0.0f, 100000.0f, 0.1f, {}},
    {"light.color", "Color", "Light", InspectorControlKind::Color, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"light.radius", "Radius", "Light", InspectorControlKind::Number, true, true, 0.0f, 10000.0f, 0.01f, {}},
    {"light.direction", "Direction", "Light", InspectorControlKind::Vector3, true, true, -1.0f, 1.0f, 0.01f, {}},
    {"light.enabled", "Enabled", "Light", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"camera.fov_y", "Vertical FOV", "Camera", InspectorControlKind::Slider, true, false, 1.0f, 179.0f, 0.1f, {}},
    {"camera.aperture", "Aperture", "Camera", InspectorControlKind::Number, true, false, 0.0f, 64.0f, 0.01f, {}},
    {"camera.focus_distance", "Focus Distance", "Camera", InspectorControlKind::Number, true, false, 0.001f, 100000.0f, 0.01f, {}},
    {"camera.controller", "Controller", "Camera", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"orbit", "fps", "turntable", "scripted_benchmark_path"}},
    {"camera.exposure", "Exposure", "Camera", InspectorControlKind::Number, true, false, -16.0f, 16.0f, 0.1f, {}},
    {"camera.white_balance", "White Balance", "Camera", InspectorControlKind::Number, true, false, 1000.0f, 40000.0f, 50.0f, {}},
    {"render.backend", "Backend", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"cpu_scalar", "cpu_simd", "vulkan", "d3d12"}},
    {"render.renderer_path", "Renderer Path", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"pathtracer", "hybrid", "reference"}},
    {"render.samples_per_pixel", "Samples Per Pixel", "Render", InspectorControlKind::Number, true, false, 1.0f, 65536.0f, 1.0f, {}},
    {"render.max_depth", "Max Bounces", "Render", InspectorControlKind::Number, true, false, 1.0f, 256.0f, 1.0f, {}},
    {"render.accumulation", "Accumulate", "Render", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"render.denoiser", "Denoiser", "Render", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"render.tone_mapping", "Tone Mapping", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"filmic", "aces", "linear", "reinhard"}},
    {"render.debug_channel", "Debug Channel", "Debug", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"beauty", "albedo", "normal", "depth", "variance", "heatmap"}},
    {"render.reset_accumulation", "Reset Accumulation", "Render", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"benchmark.scene", "Scene", "Benchmark", InspectorControlKind::AssetPicker, true, false, 0.0f, 0.0f, 0.0f, {}},
    {"benchmark.duration", "Duration", "Benchmark", InspectorControlKind::Number, true, false, 1.0f, 3600.0f, 1.0f, {}},
    {"benchmark.warmup_frames", "Warmup Frames", "Benchmark", InspectorControlKind::Number, true, false, 0.0f, 10000.0f, 1.0f, {}},
    {"benchmark.seed", "Seed", "Benchmark", InspectorControlKind::Number, true, false, 0.0f, 4294967295.0f, 1.0f, {}},
    {"diagnostics.severity", "Severity", "Diagnostics", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"trace", "info", "warning", "error"}},
    {"diagnostics.subsystem", "Subsystem", "Diagnostics", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"all", "render", "scene", "assets", "ui", "scripts"}},
    {"diagnostics.auto_scroll", "Auto Scroll", "Diagnostics", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"performance.ui_frame_budget_ms", "UI Frame Budget", "Performance", InspectorControlKind::Number, true, false, 0.1f, 33.0f, 0.1f, {}},
    {"performance.panel_build_budget_ms", "Panel Build Budget", "Performance", InspectorControlKind::Number, true, false, 0.1f, 16.0f, 0.1f, {}},
    {"performance.thumbnail_jobs", "Thumbnail Jobs", "Performance", InspectorControlKind::Number, true, false, 0.0f, 64.0f, 1.0f, {}},
    {"asset.query", "Search", "Asset", InspectorControlKind::Text, true, false, 0.0f, 0.0f, 0.0f, {}},
    {"asset.category", "Category", "Asset", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"all", "mesh", "texture", "material", "script", "scene"}},
    {"asset.show_missing", "Show Missing", "Asset", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"script.path", "Script", "Script", InspectorControlKind::AssetPicker, true, true, 0.0f, 0.0f, 0.0f, {}},
    {"script.enabled", "Enabled", "Script", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"script.reload_on_save", "Reload on Save", "Script", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"timeline.current_frame", "Current Frame", "Timeline", InspectorControlKind::Number, true, false, 0.0f, 1000000.0f, 1.0f, {}},
    {"timeline.playback_rate", "Playback Rate", "Timeline", InspectorControlKind::Number, true, false, 0.01f, 8.0f, 0.01f, {}},
    {"physics.enabled", "Physics Enabled", "Physics", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"physics.body_type", "Body Type", "Physics", InspectorControlKind::Enum, true, true, 0.0f, 0.0f, 0.0f, {"static", "dynamic", "kinematic"}},
    {"physics.mass", "Mass", "Physics", InspectorControlKind::Number, true, true, 0.0f, 100000.0f, 0.01f, {}},
    {"physics.collision_shape", "Collision Shape", "Physics", InspectorControlKind::Enum, true, true, 0.0f, 0.0f, 0.0f, {"box", "sphere", "capsule", "mesh"}},
  };
}

std::vector<InspectorFieldSchema> BuildInspectorSchemasForPanel(std::string_view panel_id) {
  const auto schemas = BuildDefaultInspectorSchemas();
  if (panel_id == "inspector") {
    return schemas;
  }

  std::vector<std::string_view> components;
  if (panel_id == "material_editor") {
    components = {"Material"};
  } else if (panel_id == "lights_panel") {
    components = {"Light"};
  } else if (panel_id == "camera_panel") {
    components = {"Camera"};
  } else if (panel_id == "render_settings") {
    components = {"Render"};
  } else if (panel_id == "benchmark_panel" || panel_id == "benchmark_history") {
    components = {"Benchmark"};
  } else if (panel_id == "diagnostics" || panel_id == "console") {
    components = {"Diagnostics"};
  } else if (panel_id == "performance") {
    components = {"Performance"};
  } else if (panel_id == "debug_views") {
    components = {"Debug"};
  } else if (panel_id == "asset_browser") {
    components = {"Asset"};
  } else if (panel_id == "script_panel") {
    components = {"Script"};
  } else if (panel_id == "timeline") {
    components = {"Timeline"};
  } else if (panel_id == "physics") {
    components = {"Physics"};
  } else if (panel_id == "scene_tree" || panel_id == "viewport") {
    components = {"Transform"};
  }

  std::vector<InspectorFieldSchema> filtered;
  for (const auto& schema : schemas) {
    const std::string_view component(schema.component.data(), schema.component.size());
    if (std::find(components.begin(), components.end(), component) != components.end()) {
      filtered.push_back(schema);
    }
  }
  return filtered;
}

std::vector<UiPanelPropertyGroup> BuildDefaultPanelPropertyGroups() {
  const auto definitions = BuildDefaultPanelDefinitions();
  std::vector<UiPanelPropertyGroup> groups;
  groups.reserve(definitions.size());
  for (const auto& definition : definitions) {
    auto fields = BuildInspectorSchemasForPanel(definition.id);
    if (fields.empty()) {
      continue;
    }
    UiPanelPropertyGroup group;
    group.panel_id = definition.id;
    group.group_id = definition.property_group_id.empty() ? definition.id : definition.property_group_id;
    group.title = definition.title;
    group.description = definition.status_hint;
    group.fields = std::move(fields);
    groups.push_back(std::move(group));
  }
  return groups;
}

std::vector<ScriptLifecycleHookState> BuildDefaultScriptLifecycleHooks() {
  const std::vector<std::string_view> names = {
    "on_load", "on_spawn", "on_enable", "on_disable", "on_update",
    "on_fixed_update", "on_late_update", "on_collision", "on_trigger",
    "on_animation_event", "on_animation_loop", "on_keyframe_reached",
    "on_destroy", "on_unload"
  };
  std::vector<ScriptLifecycleHookState> hooks;
  hooks.reserve(names.size());
  for (const auto name : names) {
    ScriptLifecycleHookState hook;
    hook.hook_name = std::string(name);
    hooks.push_back(std::move(hook));
  }
  return hooks;
}

std::vector<UiReleaseGateItem> BuildDefaultUiReleaseGateChecklist() {
  const std::vector<std::pair<std::string_view, std::string_view>> items = {
    {"window.opens", "window opens"},
    {"menu.works", "menu bar works"},
    {"layout.persists", "layout persists"},
    {"panels.dock_float", "panels dock/float"},
    {"tree.hierarchy", "scene tree displays hierarchy"},
    {"viewport.selection", "viewport selection works"},
    {"viewport.bounds", "bounding boxes display"},
    {"gizmo.trs", "translate/rotate/scale gizmos work"},
    {"inspector.edits", "inspector edits selected entity"},
    {"selection.multi", "multi-select works"},
    {"grouping", "group/ungroup works"},
    {"merge.split", "merge/split works"},
    {"assets.import", "asset browser imports valid files"},
    {"assets.reject", "invalid file drops are rejected"},
    {"lua.attach", "Lua script attach UI works"},
    {"benchmark.desc", "benchmark panel runs benchmark descriptor"},
    {"benchmark.score", "normalized score displays"},
    {"logs.errors", "logs panel shows errors"},
    {"crash.ui_state", "crash snapshot includes UI state"},
  };
  std::vector<UiReleaseGateItem> checklist;
  checklist.reserve(items.size());
  for (const auto& [id, label] : items) {
    UiReleaseGateItem item;
    item.id = std::string(id);
    item.label = std::string(label);
    item.required = true;
    checklist.push_back(std::move(item));
  }
  return checklist;
}

std::vector<SceneTreeRow> BuildSceneTreeRows(const std::vector<SceneTreeEntityModel>& entities,
                                             const SelectionState& selection,
                                             vkpt::core::StableId hovered_entity,
                                             std::size_t max_rows) {
  std::unordered_set<vkpt::core::StableId> known_ids;
  known_ids.reserve(entities.size());
  for (const auto& entity : entities) {
    if (entity.entity_id != 0) {
      known_ids.insert(entity.entity_id);
    }
  }

  std::unordered_map<vkpt::core::StableId, const SceneTreeEntityModel*> by_id;
  std::unordered_map<vkpt::core::StableId, std::vector<const SceneTreeEntityModel*>> children;
  by_id.reserve(entities.size());
  children.reserve(entities.size());
  std::vector<const SceneTreeEntityModel*> roots;
  roots.reserve(entities.size());
  for (const auto& entity : entities) {
    if (entity.entity_id == 0) {
      continue;
    }
    by_id[entity.entity_id] = &entity;
  }
  for (const auto& entity : entities) {
    if (entity.entity_id == 0) {
      continue;
    }
    if (entity.parent_id == 0 || !known_ids.contains(entity.parent_id) || entity.parent_id == entity.entity_id) {
      roots.push_back(&entity);
    } else {
      children[entity.parent_id].push_back(&entity);
    }
  }

  auto sort_rows = [](std::vector<const SceneTreeEntityModel*>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const SceneTreeEntityModel* lhs, const SceneTreeEntityModel* rhs) {
                       if (lhs->sibling_order != rhs->sibling_order) {
                         return lhs->sibling_order < rhs->sibling_order;
                       }
                       return lhs->entity_id < rhs->entity_id;
                     });
  };
  sort_rows(roots);
  for (auto& entry : children) {
    sort_rows(entry.second);
  }

  std::vector<SceneTreeRow> rows;
  rows.reserve(max_rows == 0 ? entities.size() : std::min(max_rows, entities.size()));
  const auto is_selected = [&](vkpt::core::StableId id) {
    return std::find(selection.selected_entity_ids.begin(), selection.selected_entity_ids.end(), id) !=
           selection.selected_entity_ids.end();
  };
  const auto is_hovered = [&](vkpt::core::StableId id) {
    if (hovered_entity != 0 && hovered_entity == id) {
      return true;
    }
    if (selection.hovered_entity == id) {
      return true;
    }
    return std::find(selection.hovered_entity_ids.begin(), selection.hovered_entity_ids.end(), id) !=
           selection.hovered_entity_ids.end();
  };

  std::unordered_set<vkpt::core::StableId> visiting;
  auto append_tree = [&](auto&& self, const SceneTreeEntityModel* entity, std::uint32_t depth) -> void {
    if (!entity || (max_rows != 0 && rows.size() >= max_rows)) {
      return;
    }
    const bool cycle = !visiting.insert(entity->entity_id).second;
    const auto childIt = children.find(entity->entity_id);
    const bool hasChildren = childIt != children.end() && !childIt->second.empty();
    SceneTreeRow row;
    row.entity_id = entity->entity_id;
    row.parent_id = entity->parent_id;
    row.depth = depth;
    row.sibling_order = entity->sibling_order;
    row.label = entity->name.empty()
        ? std::string("Entity ") + std::to_string(entity->entity_id)
        : entity->name;
    row.component_badges = entity->component_badges;
    if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "camera") !=
        entity->component_badges.end()) {
      row.icon = "camera";
    } else if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "light") !=
               entity->component_badges.end()) {
      row.icon = "light";
    } else if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "mesh") !=
               entity->component_badges.end()) {
      row.icon = "mesh";
    } else if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "sdf") !=
               entity->component_badges.end()) {
      row.icon = "sdf";
    } else {
      row.icon = "entity";
    }
    row.expanded = entity->expanded;
    row.selected = is_selected(entity->entity_id);
    row.hovered = is_hovered(entity->entity_id);
    row.hidden = !entity->visible;
    row.locked = entity->locked;
    row.has_warning = cycle ||
                      (entity->parent_id != 0 && !known_ids.contains(entity->parent_id)) ||
                      entity->parent_id == entity->entity_id;
    row.has_children = hasChildren;
    rows.push_back(std::move(row));
    if (!cycle && entity->expanded && hasChildren) {
      for (const auto* child : childIt->second) {
        self(self, child, depth + 1u);
        if (max_rows != 0 && rows.size() >= max_rows) {
          break;
        }
      }
    }
    visiting.erase(entity->entity_id);
  };

  for (const auto* root : roots) {
    append_tree(append_tree, root, 0u);
    if (max_rows != 0 && rows.size() >= max_rows) {
      break;
    }
  }
  return rows;
}

ShortcutResolution ResolveShortcut(const std::vector<UiShortcut>& shortcuts,
                                   std::uint32_t key_code,
                                   bool ctrl,
                                   bool shift,
                                   bool alt) {
  ShortcutResolution result;
  for (const auto& shortcut : shortcuts) {
    if (shortcut.key_code == key_code &&
        shortcut.ctrl == ctrl &&
        shortcut.shift == shift &&
        shortcut.alt == alt) {
      result.matched = true;
      result.conflicting_action_ids.push_back(shortcut.action_id);
    }
  }
  if (result.conflicting_action_ids.size() == 1u) {
    result.action_id = result.conflicting_action_ids.front();
  } else if (result.conflicting_action_ids.size() > 1u) {
    result.conflict = true;
  }
  return result;
}

std::vector<AssetPreviewCard> FilterAndSortAssetCards(const std::vector<AssetPreviewCard>& cards,
                                                      const AssetBrowserFilter& filter) {
  const std::string query = ToLower(filter.query);
  const std::string category = ToLower(filter.category);
  std::vector<AssetPreviewCard> out;
  for (const auto& card : cards) {
    if (!filter.show_missing && card.missing) {
      continue;
    }
    if (!filter.show_generated && ToLower(card.status) == "generated") {
      continue;
    }
    if (!category.empty() && ToLower(card.category) != category) {
      continue;
    }
    if (!query.empty()) {
      const std::string haystack = ToLower(card.display_name + " " + card.path + " " + card.asset_id);
      if (haystack.find(query) == std::string::npos) {
        continue;
      }
    }
    out.push_back(card);
  }

  const std::string sortKey = ToLower(filter.sort_key);
  std::sort(out.begin(), out.end(), [&](const AssetPreviewCard& a, const AssetPreviewCard& b) {
    std::string av;
    std::string bv;
    if (sortKey == "category") {
      av = a.category;
      bv = b.category;
    } else if (sortKey == "status") {
      av = a.status;
      bv = b.status;
    } else if (sortKey == "path") {
      av = a.path;
      bv = b.path;
    } else {
      av = a.display_name;
      bv = b.display_name;
    }
    if (filter.ascending) {
      return ToLower(av) < ToLower(bv);
    }
    return ToLower(av) > ToLower(bv);
  });
  return out;
}

BenchmarkScoreModel ComputeBenchmarkScore(double measured_units_per_second,
                                          double expected_units_per_second,
                                          double raw_samples_per_second,
                                          double workload_units,
                                          bool calibration_valid) {
  BenchmarkScoreModel score;
  score.raw_samples_per_second = raw_samples_per_second;
  score.workload_units = workload_units;
  score.expected_units_per_second = expected_units_per_second;
  score.measured_units_per_second = measured_units_per_second;
  score.calibration_valid = calibration_valid;
  if (expected_units_per_second > 0.0) {
    score.normalized_score = measured_units_per_second / expected_units_per_second;
  }
  if (!calibration_valid) {
    score.confidence = "uncalibrated";
    score.warnings.push_back("hardware calibration profile is missing or invalid");
  } else if (score.normalized_score <= 0.0) {
    score.confidence = "invalid";
    score.warnings.push_back("measured throughput is zero");
  } else if (score.normalized_score < 0.75 || score.normalized_score > 1.25) {
    score.confidence = "low";
  } else {
    score.confidence = "high";
  }
  return score;
}

WorkloadComplexityModel EstimateWorkloadComplexity(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                                   std::uint32_t light_count,
                                                   std::uint64_t triangle_count,
                                                   std::uint64_t bvh_node_count,
                                                   std::uint64_t texture_bytes,
                                                   bool denoiser_enabled) {
  WorkloadComplexityModel model;
  model.width = desc.resolution.width;
  model.height = desc.resolution.height;
  model.samples_per_pixel = desc.samples_per_pixel;
  model.max_depth = desc.max_depth;
  model.light_count = light_count;
  model.triangle_count = triangle_count;
  model.bvh_node_count = bvh_node_count;
  model.texture_bytes = texture_bytes;

  const double pixel_count = static_cast<double>(std::max<std::uint32_t>(1u, model.width)) *
                             static_cast<double>(std::max<std::uint32_t>(1u, model.height));
  const double spp_cost = static_cast<double>(std::max<std::uint32_t>(1u, model.samples_per_pixel));
  const double depth_cost = static_cast<double>(std::max<std::uint32_t>(1u, model.max_depth));
  const double light_cost = 1.0 + static_cast<double>(light_count) * 0.08;
  const double triangle_cost = 1.0 + std::log2(1.0 + static_cast<double>(triangle_count)) * 0.02;
  const double bvh_cost = 1.0 + std::log2(1.0 + static_cast<double>(bvh_node_count)) * 0.015;
  const double texture_cost = 1.0 + (static_cast<double>(texture_bytes) / (1024.0 * 1024.0 * 1024.0)) * 0.05;
  const double denoiser_cost = denoiser_enabled ? 1.12 : 1.0;

  model.normalized_cost_units =
      (pixel_count * spp_cost * depth_cost * light_cost * triangle_cost * bvh_cost * texture_cost * denoiser_cost) /
      (1280.0 * 720.0);

  model.cost_drivers.push_back("resolution");
  model.cost_drivers.push_back("SPP");
  model.cost_drivers.push_back("max_depth");
  if (light_count > 0) {
    model.cost_drivers.push_back("light_count");
  }
  if (triangle_count > 0) {
    model.cost_drivers.push_back("triangle_count");
  }
  if (bvh_node_count > 0) {
    model.cost_drivers.push_back("BVH_node_count");
  }
  if (texture_bytes > 0) {
    model.cost_drivers.push_back("texture_memory");
  }
  if (denoiser_enabled) {
    model.cost_drivers.push_back("denoiser");
  }
  if (!desc.renderer_path.empty()) {
    model.cost_drivers.push_back("renderer_path:" + desc.renderer_path);
  }
  if (!desc.backend.empty()) {
    model.cost_drivers.push_back("backend:" + desc.backend);
  }
  return model;
}

std::vector<BenchmarkCalibrationActionModel> BuildDefaultBenchmarkCalibrationActions(
    bool gpu_compute_available,
    bool hardware_rt_available) {
  return {
    {"calibration.cpu_scalar", "Run CPU Scalar Calibration", "cpu", "cpu_scalar", true, {}},
    {"calibration.cpu_threaded", "Run CPU Threaded Calibration", "cpu", "cpu_threaded", true, {}},
    {"calibration.cpu_simd", "Run CPU SIMD Calibration", "cpu", "cpu_simd", true, {}},
    {"calibration.gpu_compute", "Run GPU Compute Calibration", "gpu", "gpu_compute", gpu_compute_available,
     gpu_compute_available ? std::string{} : std::string{"no GPU compute backend is available in this build"}},
    {"calibration.hardware_rt", "Run Hardware RT Calibration", "gpu", "hardware_rt", hardware_rt_available,
     hardware_rt_available ? std::string{} : std::string{"hardware ray tracing is unavailable or not selected"}},
    {"calibration.backend_compare", "Run Backend Comparison", "mixed", "backend_comparison",
     gpu_compute_available || hardware_rt_available,
     (gpu_compute_available || hardware_rt_available) ? std::string{} : std::string{"backend comparison needs at least one GPU backend"}}
  };
}

BenchmarkPanelModel BuildBenchmarkPanelModel(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                            const BenchmarkRawMetricsModel& raw_metrics,
                                            const BenchmarkScoreModel& score,
                                            const WorkloadComplexityModel& workload,
                                            std::string_view artifact_location,
                                            std::string_view result_summary,
                                            bool can_run,
                                            std::string_view unavailable_reason) {
  BenchmarkPanelModel model;
  model.run_desc = desc;
  model.can_run = can_run;
  model.can_cancel = false;
  model.unavailable_reason = std::string(unavailable_reason);
  model.artifact_location = std::string(artifact_location);
  model.result_summary = std::string(result_summary);
  model.raw_metrics = raw_metrics;
  model.score = score;
  model.workload = workload;
  model.calibration_actions = BuildDefaultBenchmarkCalibrationActions(true, false);
  return model;
}

StatusBarModel BuildStatusBarModel(const UiRuntimeState& runtime,
                                   const SelectionState& selection,
                                   const BenchmarkScoreModel* score) {
  StatusBarModel model;
  model.active_scene = runtime.active_scene;
  model.backend = runtime.active_renderer_backend;
  model.renderer_path = runtime.active_renderer_path.empty() ? runtime.selected_debug_view : runtime.active_renderer_path;
  model.spp = runtime.spp_accumulated;
  model.fps = runtime.fps;
  model.frame_ms = runtime.frame_ms;
  model.selected_entity_count = selection.selected_entity_ids.size();
  model.active_tool = ToString(runtime.active_viewport_tool);
  model.last_warning_or_error = runtime.last_warning_or_error.empty()
      ? runtime.status_message
      : runtime.last_warning_or_error;
  model.background_job_count = runtime.background_job_count;
  if (score != nullptr) {
    model.normalized_score = score->normalized_score;
  }
  return model;
}

std::string SerializeUiRuntimeState(const UiRuntimeState& state) {
  return SerializeUiRuntimeStateInternal(state);
}

std::string SerializeSelectionState(const SelectionState& state) {
  return SerializeSelectionStateInternal(state);
}

std::string SerializeLayoutDocument(const UiLayoutDocument& layout) {
  return SerializeUiLayoutJson(layout);
}

std::string SerializeMenuBar(const MenuBar& menu) {
  std::ostringstream out;
  auto serialize_item = [](const auto& item, auto&& self, std::size_t depth, std::ostringstream& out) -> void {
    (void)depth;
    out << "{";
    out << "\"id\":\"" << EscapeJson(item.id) << "\",";
    out << "\"label\":\"" << EscapeJson(item.label) << "\",";
    out << "\"enabled\":" << (item.enabled ? "true" : "false") << ",";
    out << "\"disabled_reason\":\"" << EscapeJson(item.disabled_reason) << "\",";
    out << "\"children\":[";
    for (std::size_t i = 0; i < item.children.size(); ++i) {
      if (i > 0) out << ",";
      self(item.children[i], self, depth + 1, out);
    }
    out << "]";
    out << "}";
  };
  out << "{";
  out << "\"top_level_menus\":[";
  for (std::size_t i = 0; i < menu.top_level_menus.size(); ++i) {
    if (i > 0) out << ",";
    serialize_item(menu.top_level_menus[i], serialize_item, 0, out);
  }
  out << "]}";
  return out.str();
}

namespace {

const char* InspectorControlKindName(InspectorControlKind kind) {
  switch (kind) {
    case InspectorControlKind::Text:
      return "text";
    case InspectorControlKind::Number:
      return "number";
    case InspectorControlKind::Slider:
      return "slider";
    case InspectorControlKind::Toggle:
      return "toggle";
    case InspectorControlKind::Vector3:
      return "vector3";
    case InspectorControlKind::Color:
      return "color";
    case InspectorControlKind::EntityPicker:
      return "entity_picker";
    case InspectorControlKind::AssetPicker:
      return "asset_picker";
    case InspectorControlKind::Enum:
      return "enum";
    default:
      return "text";
  }
}

void WriteStringArrayJson(std::ostringstream& out, const std::vector<std::string>& values) {
  out << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    out << "\"" << EscapeJson(values[i]) << "\"";
  }
  out << "]";
}

void WriteInspectorFieldSchemaJson(std::ostringstream& out, const InspectorFieldSchema& schema) {
  out << "{";
  out << "\"field_id\":\"" << EscapeJson(schema.field_id) << "\",";
  out << "\"label\":\"" << EscapeJson(schema.label) << "\",";
  out << "\"component\":\"" << EscapeJson(schema.component) << "\",";
  out << "\"control\":\"" << InspectorControlKindName(schema.control) << "\",";
  out << "\"editable\":" << (schema.editable ? "true" : "false") << ",";
  out << "\"supports_mixed_values\":" << (schema.supports_mixed_values ? "true" : "false") << ",";
  out << "\"min_value\":" << schema.min_value << ",";
  out << "\"max_value\":" << schema.max_value << ",";
  out << "\"step\":" << schema.step << ",";
  out << "\"enum_values\":";
  WriteStringArrayJson(out, schema.enum_values);
  out << "}";
}

}  // namespace

std::string SerializePanelDefinitions(const std::vector<UiPanelDefinition>& panels) {
  std::ostringstream out;
  out << "{\"panels\":[";
  for (std::size_t i = 0; i < panels.size(); ++i) {
    const auto& panel = panels[i];
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"id\":\"" << EscapeJson(panel.id) << "\",";
    out << "\"title\":\"" << EscapeJson(panel.title) << "\",";
    out << "\"menu_action_id\":\"" << EscapeJson(panel.menu_action_id) << "\",";
    out << "\"default_visible\":" << (panel.default_visible ? "true" : "false") << ",";
    out << "\"can_dock\":" << (panel.can_dock ? "true" : "false") << ",";
    out << "\"can_float\":" << (panel.can_float ? "true" : "false") << ",";
    out << "\"can_close\":" << (panel.can_close ? "true" : "false") << ",";
    out << "\"default_width\":" << panel.default_width << ",";
    out << "\"default_height\":" << panel.default_height << ",";
    out << "\"default_area\":\"" << ToString(panel.default_area) << "\",";
    out << "\"tab_group\":\"" << EscapeJson(panel.tab_group) << "\",";
    out << "\"sort_order\":" << panel.sort_order << ",";
    out << "\"property_group_id\":\"" << EscapeJson(panel.property_group_id) << "\",";
    out << "\"status_hint\":\"" << EscapeJson(panel.status_hint) << "\"";
    out << "}";
  }
  out << "]}";
  return out.str();
}

std::string SerializeInspectorSchemas(const std::vector<InspectorFieldSchema>& schemas) {
  std::ostringstream out;
  out << "{\"fields\":[";
  for (std::size_t i = 0; i < schemas.size(); ++i) {
    if (i > 0) {
      out << ",";
    }
    WriteInspectorFieldSchemaJson(out, schemas[i]);
  }
  out << "]}";
  return out.str();
}

std::string SerializePanelPropertyGroups(const std::vector<UiPanelPropertyGroup>& groups) {
  std::ostringstream out;
  out << "{\"groups\":[";
  for (std::size_t i = 0; i < groups.size(); ++i) {
    const auto& group = groups[i];
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"panel_id\":\"" << EscapeJson(group.panel_id) << "\",";
    out << "\"group_id\":\"" << EscapeJson(group.group_id) << "\",";
    out << "\"title\":\"" << EscapeJson(group.title) << "\",";
    out << "\"description\":\"" << EscapeJson(group.description) << "\",";
    out << "\"fields\":[";
    for (std::size_t field_index = 0; field_index < group.fields.size(); ++field_index) {
      if (field_index > 0) {
        out << ",";
      }
      WriteInspectorFieldSchemaJson(out, group.fields[field_index]);
    }
    out << "]}";
  }
  out << "]}";
  return out.str();
}

std::string SerializeBenchmarkPanelModel(const BenchmarkPanelModel& model) {
  auto write_string_array = [](std::ostringstream& out, const std::vector<std::string>& values) {
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (i > 0) {
        out << ",";
      }
      out << "\"" << EscapeJson(values[i]) << "\"";
    }
    out << "]";
  };

  std::ostringstream out;
  out << "{";
  out << "\"selected_scene\":\"" << EscapeJson(model.run_desc.scene_path) << "\",";
  out << "\"backend\":\"" << EscapeJson(model.run_desc.backend) << "\",";
  out << "\"renderer_path\":\"" << EscapeJson(model.run_desc.renderer_path) << "\",";
  out << "\"resolution\":{\"width\":" << model.run_desc.resolution.width
      << ",\"height\":" << model.run_desc.resolution.height << "},";
  out << "\"spp\":" << model.run_desc.samples_per_pixel << ",";
  out << "\"max_depth\":" << model.run_desc.max_depth << ",";
  out << "\"warmup_frames\":" << model.run_desc.warmup_frames << ",";
  out << "\"duration\":" << model.run_desc.duration << ",";
  out << "\"seed\":" << model.run_desc.seed << ",";
  out << "\"can_run\":" << (model.can_run ? "true" : "false") << ",";
  out << "\"can_cancel\":" << (model.can_cancel ? "true" : "false") << ",";
  out << "\"unavailable_reason\":\"" << EscapeJson(model.unavailable_reason) << "\",";
  out << "\"artifact_location\":\"" << EscapeJson(model.artifact_location) << "\",";
  out << "\"result_summary\":\"" << EscapeJson(model.result_summary) << "\",";
  out << "\"raw_metrics\":{";
  out << "\"fps\":" << model.raw_metrics.fps << ",";
  out << "\"frame_ms\":" << model.raw_metrics.frame_ms << ",";
  out << "\"gpu_ms\":" << model.raw_metrics.gpu_ms << ",";
  out << "\"cpu_ms\":" << model.raw_metrics.cpu_ms << ",";
  out << "\"samples_per_second\":" << model.raw_metrics.samples_per_second << ",";
  out << "\"paths_per_second\":" << model.raw_metrics.paths_per_second << ",";
  out << "\"path_vertices_per_second\":" << model.raw_metrics.path_vertices_per_second << ",";
  out << "\"spp_accumulated\":" << model.raw_metrics.spp_accumulated << ",";
  out << "\"memory_estimate_bytes\":" << model.raw_metrics.memory_estimate_bytes << ",";
  out << "\"bvh_build_ms\":" << model.raw_metrics.bvh_build_ms << ",";
  out << "\"shader_compile_ms\":" << model.raw_metrics.shader_compile_ms;
  out << "},";
  out << "\"score\":{";
  out << "\"normalized_score\":" << model.score.normalized_score << ",";
  out << "\"raw_samples_per_second\":" << model.score.raw_samples_per_second << ",";
  out << "\"raw_paths_per_second\":" << model.score.raw_paths_per_second << ",";
  out << "\"raw_gpu_ms\":" << model.score.raw_gpu_ms << ",";
  out << "\"raw_cpu_ms\":" << model.score.raw_cpu_ms << ",";
  out << "\"workload_units\":" << model.score.workload_units << ",";
  out << "\"expected_units_per_second\":" << model.score.expected_units_per_second << ",";
  out << "\"measured_units_per_second\":" << model.score.measured_units_per_second << ",";
  out << "\"calibration_profile_id\":\"" << EscapeJson(model.score.calibration_profile_id) << "\",";
  out << "\"confidence\":\"" << EscapeJson(model.score.confidence) << "\",";
  out << "\"calibration_valid\":" << (model.score.calibration_valid ? "true" : "false") << ",";
  out << "\"warnings\":";
  write_string_array(out, model.score.warnings);
  out << "},";
  out << "\"workload\":{";
  out << "\"width\":" << model.workload.width << ",";
  out << "\"height\":" << model.workload.height << ",";
  out << "\"samples_per_pixel\":" << model.workload.samples_per_pixel << ",";
  out << "\"max_depth\":" << model.workload.max_depth << ",";
  out << "\"light_count\":" << model.workload.light_count << ",";
  out << "\"triangle_count\":" << model.workload.triangle_count << ",";
  out << "\"bvh_node_count\":" << model.workload.bvh_node_count << ",";
  out << "\"texture_bytes\":" << model.workload.texture_bytes << ",";
  out << "\"normalized_cost_units\":" << model.workload.normalized_cost_units << ",";
  out << "\"cost_drivers\":";
  write_string_array(out, model.workload.cost_drivers);
  out << "},";
  out << "\"calibration_actions\":[";
  for (std::size_t i = 0; i < model.calibration_actions.size(); ++i) {
    const auto& action = model.calibration_actions[i];
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"id\":\"" << EscapeJson(action.id) << "\",";
    out << "\"label\":\"" << EscapeJson(action.label) << "\",";
    out << "\"backend\":\"" << EscapeJson(action.backend) << "\",";
    out << "\"renderer_path\":\"" << EscapeJson(action.renderer_path) << "\",";
    out << "\"supported\":" << (action.supported ? "true" : "false") << ",";
    out << "\"unavailable_reason\":\"" << EscapeJson(action.unavailable_reason) << "\"";
    out << "}";
  }
  out << "],";
  out << "\"history\":[";
  for (std::size_t i = 0; i < model.history.size(); ++i) {
    const auto& entry = model.history[i];
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"scene\":\"" << EscapeJson(entry.scene) << "\",";
    out << "\"backend\":\"" << EscapeJson(entry.backend) << "\",";
    out << "\"renderer_path\":\"" << EscapeJson(entry.renderer_path) << "\",";
    out << "\"score\":" << entry.score.normalized_score << ",";
    out << "\"raw_throughput\":" << entry.score.raw_samples_per_second << ",";
    out << "\"artifact_path\":\"" << EscapeJson(entry.artifact_path) << "\",";
    out << "\"timestamp_utc\":\"" << EscapeJson(entry.timestamp_utc) << "\",";
    out << "\"regression_marker\":\"" << EscapeJson(entry.regression_marker) << "\"";
    out << "}";
  }
  out << "]";
  out << "}";
  return out.str();
}

std::string SerializeUiReleaseGateChecklist(const std::vector<UiReleaseGateItem>& checklist) {
  std::ostringstream out;
  std::size_t passed = 0;
  std::size_t deferred = 0;
  std::size_t pending = 0;
  for (const auto& item : checklist) {
    if (item.passed) {
      ++passed;
    } else if (item.deferred) {
      ++deferred;
    } else {
      ++pending;
    }
  }

  out << "{";
  out << "\"total\":" << checklist.size() << ",";
  out << "\"passed_count\":" << passed << ",";
  out << "\"deferred_count\":" << deferred << ",";
  out << "\"pending_count\":" << pending << ",";
  out << "\"items\":[";
  for (std::size_t i = 0; i < checklist.size(); ++i) {
    const auto& item = checklist[i];
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"id\":\"" << EscapeJson(item.id) << "\",";
    out << "\"label\":\"" << EscapeJson(item.label) << "\",";
    out << "\"required\":" << (item.required ? "true" : "false") << ",";
    out << "\"passed\":" << (item.passed ? "true" : "false") << ",";
    out << "\"deferred\":" << (item.deferred ? "true" : "false") << ",";
    out << "\"status\":\"" << (item.passed ? "passed" : (item.deferred ? "deferred" : "pending")) << "\",";
    out << "\"evidence\":\"" << EscapeJson(item.evidence) << "\",";
    out << "\"deferred_reason\":\"" << EscapeJson(item.deferred_reason) << "\"";
    out << "}";
  }
  out << "]}";
  return out.str();
}

std::string SerializeEditorCommand(const EditorCommand& command) {
  return SerializeEditorCommandInternal(command);
}

std::string SerializeEditorCommandsJsonl(const std::vector<EditorCommand>& commands, std::size_t max_lines) {
  std::ostringstream out;
  const auto start = commands.size() > max_lines ? commands.size() - max_lines : 0;
  for (std::size_t i = start; i < commands.size(); ++i) {
    out << SerializeEditorCommand(commands[i]) << '\n';
  }
  return out.str();
}

std::string SerializeUiEventsJsonl(const std::deque<UiEvent>& events, std::size_t max_lines) {
  std::ostringstream out;
  const auto start = events.size() > max_lines ? events.size() - max_lines : 0;
  for (std::size_t i = start; i < events.size(); ++i) {
    out << SerializeUiEventInternal(events[i], true);
  }
  return out.str();
}

bool LoadSelectionFromFile(const std::string& path, SelectionState* out_selection) {
  if (!out_selection) {
    return false;
  }
  std::ifstream stream(path);
  if (!stream) {
    return false;
  }
  std::ostringstream text;
  text << stream.rdbuf();
  const auto value = vkpt::scene::JsonParser::parse(text.str());
  if (!value || value->kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }

  SelectionState selection = CreateDefaultSelectionState();
  ReadJsonStableIdArray(*value, "selected_entity_ids", selection.selected_entity_ids);
  ReadJsonStableIdArray(*value, "hovered_entity_ids", selection.hovered_entity_ids);
  ReadJsonUInt64(*value, "active_primary_entity", selection.active_primary_entity);
  ReadJsonUInt64(*value, "hovered_entity", selection.hovered_entity);
  ReadJsonUInt64(*value, "selected_group", selection.selected_group);
  ReadJsonString(*value, "selected_group_name", selection.selected_group_name);
  if (std::string source; ReadJsonString(*value, "selection_source", source)) {
    selection.selection_source = ParseSelectionSource(source);
  }
  ReadJsonBounds(*value, "aggregate_bounds", selection.aggregate_bounds);
  ReadJsonUInt64(*value, "last_change_frame", selection.last_change_frame);

  if (const auto it = value->object.find("per_item_bounds");
      it != value->object.end() && it->second.kind == vkpt::scene::JsonValue::Kind::Array) {
    selection.per_item_bounds.clear();
    for (const auto& entry : it->second.array) {
      if (entry.kind != vkpt::scene::JsonValue::Kind::Object) {
        continue;
      }
      SceneEntityBounds item;
      ReadJsonUInt64(entry, "entity_id", item.entity_id);
      ReadJsonBounds(entry, "bounds", item.bounds);
      selection.per_item_bounds.push_back(item);
    }
  }

  *out_selection = std::move(selection);
  return true;
}

bool SaveSelectionToFile(const std::string& path, const SelectionState& selection, std::string* error) {
  try {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
  } catch (...) {
    if (error) {
      *error = "failed to create parent directory";
    }
    return false;
  }
  std::ofstream out(path);
  if (!out) {
    if (error) {
      *error = "failed to open file";
    }
    return false;
  }
  out << SerializeSelectionState(selection) << '\n';
  return true;
}

bool LoadLayoutFromFile(const std::string& path, UiLayoutDocument* out_layout) {
  if (!out_layout) {
    return false;
  }
  std::ifstream stream(path);
  if (!stream) {
    return false;
  }
  std::ostringstream text;
  text << stream.rdbuf();
  const auto value = vkpt::scene::JsonParser::parse(text.str());
  if (!value || value->kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }

  UiLayoutDocument layout;
  std::string preset;
  if (ReadJsonString(*value, "layout_name", layout.active_layout_name)) {}
  if (ReadJsonString(*value, "preset", preset)) {
    if (preset == "Default") layout.preset = LayoutPreset::Default;
    else if (preset == "Benchmark") layout.preset = LayoutPreset::Benchmark;
    else if (preset == "Material Authoring") layout.preset = LayoutPreset::MaterialAuthoring;
    else if (preset == "Scripting") layout.preset = LayoutPreset::Scripting;
    else if (preset == "Asset Management") layout.preset = LayoutPreset::AssetManagement;
    else if (preset == "Debug/Profiler") layout.preset = LayoutPreset::DebugProfiler;
    else if (preset == "Minimal Viewport") layout.preset = LayoutPreset::MinimalViewport;
    else if (preset == "Fullscreen Viewport With Overlay") layout.preset = LayoutPreset::FullscreenViewportWithOverlay;
  }
  ReadJsonFloat(*value, "dpi_scale", layout.dpi_scale);
  ReadJsonFloat(*value, "ui_scale", layout.ui_scale);
  ReadJsonBool(*value, "fullscreen_overlay", layout.fullscreen_overlay);
  ReadJsonArray(*value, "panel_order", layout.panel_order);

  if (const auto it = value->object.find("panels"); it != value->object.end() && it->second.kind == vkpt::scene::JsonValue::Kind::Array) {
    for (const auto& panel : it->second.array) {
      if (panel.kind != vkpt::scene::JsonValue::Kind::Object) {
        continue;
      }
      UiPanelState state;
      ReadJsonString(panel, "id", state.id);
      ReadJsonBool(panel, "visible", state.visible);
      ReadJsonBool(panel, "docked", state.docked);
      ReadJsonBool(panel, "floating", state.floating);
      ReadJsonBool(panel, "closable", state.closable);
      ReadJsonBool(panel, "collapsible", state.collapsible);
      ReadJsonBool(panel, "collapsed", state.collapsed);
      ReadJsonBool(panel, "focused", state.focused);
      ReadJsonBool(panel, "resized", state.resized);
      ReadJsonBool(panel, "movable", state.movable);
      ReadJsonFloat(panel, "x", state.x);
      ReadJsonFloat(panel, "y", state.y);
      ReadJsonFloat(panel, "width", state.width);
      ReadJsonFloat(panel, "height", state.height);
      layout.panels.push_back(state);
    }
  }
  if (layout.panels.empty()) {
    layout = CreateLayoutPreset(layout.preset);
  } else {
    const auto default_panels = BuildDefaultPanelStates(layout.preset);
    for (const auto& default_panel : default_panels) {
      const auto exists = std::find_if(layout.panels.begin(), layout.panels.end(),
                                       [&](const UiPanelState& panel) {
                                         return panel.id == default_panel.id;
                                       });
      if (exists == layout.panels.end()) {
        layout.panels.push_back(default_panel);
      }
    }
    if (layout.panel_order.empty()) {
      for (const auto& panel : layout.panels) {
        if (panel.visible) {
          layout.panel_order.push_back(panel.id);
        }
      }
    } else {
      for (const auto& panel : layout.panels) {
        if (!panel.visible) {
          continue;
        }
        const auto in_order = std::find(layout.panel_order.begin(), layout.panel_order.end(), panel.id);
        if (in_order == layout.panel_order.end()) {
          layout.panel_order.push_back(panel.id);
        }
      }
    }
  }
  *out_layout = layout;
  return true;
}

bool SaveLayoutToFile(const std::string& path, const UiLayoutDocument& layout, std::string* error) {
  try {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  } catch (...) {
    if (error) {
      *error = "failed to create parent directory";
    }
    return false;
  }
  std::ofstream out(path);
  if (!out) {
    if (error) {
      *error = "failed to open file";
    }
    return false;
  }
  out << SerializeLayoutDocument(layout) << '\n';
  return true;
}

PanelMutationResult SetPanelVisible(UiLayoutDocument& layout, std::string_view panel_id, bool visible) {
  auto* panel = FindPanel(layout, panel_id);
  if (!panel) {
    return {false, "panel not found"};
  }
  if (!visible && !panel->closable) {
    return {false, "panel is not closable"};
  }
  if (panel->visible == visible) {
    return {false, "no change"};
  }
  panel->visible = visible;
  if (visible) {
    panel->collapsed = false;
    EnsurePanelInOrder(layout, panel_id);
  } else {
    panel->collapsed = false;
    RemovePanelFromOrder(layout, panel_id);
  }
  return {true, "visible changed"};
}

PanelMutationResult SetPanelCollapsed(UiLayoutDocument& layout, std::string_view panel_id, bool collapsed) {
  auto* panel = FindPanel(layout, panel_id);
  if (!panel) {
    return {false, "panel not found"};
  }
  if (!panel->collapsible) {
    return {false, "panel is not collapsible"};
  }
  if (panel->collapsed == collapsed) {
    return {false, "no change"};
  }
  panel->collapsed = collapsed;
  return {true, "collapsed changed"};
}

PanelMutationResult SetPanelDockState(UiLayoutDocument& layout,
                                      std::string_view panel_id,
                                      bool docked,
                                      bool floating) {
  auto* panel = FindPanel(layout, panel_id);
  if (!panel) {
    return {false, "panel not found"};
  }
  const bool normalizedDocked = docked || !floating;
  const bool normalizedFloating = floating || !docked;
  if (panel->docked == normalizedDocked && panel->floating == normalizedFloating) {
    return {false, "no change"};
  }
  panel->docked = normalizedDocked;
  panel->floating = normalizedFloating;
  return {true, "dock state changed"};
}

PanelMutationResult MovePanel(UiLayoutDocument& layout, std::string_view panel_id, float x, float y) {
  auto* panel = FindPanel(layout, panel_id);
  if (!panel) {
    return {false, "panel not found"};
  }
  if (!panel->movable) {
    return {false, "panel is not movable"};
  }
  if (panel->x == x && panel->y == y) {
    return {false, "no change"};
  }
  panel->x = x;
  panel->y = y;
  return {true, "panel moved"};
}

PanelMutationResult ResizePanel(UiLayoutDocument& layout, std::string_view panel_id, float width, float height) {
  auto* panel = FindPanel(layout, panel_id);
  if (!panel) {
    return {false, "panel not found"};
  }
  if (!panel->resized) {
    return {false, "panel is not resizable"};
  }
  const float clamped_width = std::max(1.0f, width);
  const float clamped_height = std::max(1.0f, height);
  if (panel->width == clamped_width && panel->height == clamped_height) {
    return {false, "no change"};
  }
  panel->width = clamped_width;
  panel->height = clamped_height;
  return {true, "panel resized"};
}

bool RestoreLayoutPreset(UiLayoutDocument& layout, LayoutPreset preset) {
  layout = CreateLayoutPreset(preset);
  return true;
}

PanelMutationResult ApplyPanelStateCommand(UiLayoutDocument& layout,
                                          const std::string& command_id,
                                          bool value,
                                          float value_float,
                                          std::string_view target_panel_id) {
  if (command_id.empty()) {
    return {false, "missing command id"};
  }
  if (target_panel_id.empty()) {
    return {false, "missing target panel id"};
  }

  if (command_id == "view.panel.visible" || command_id == "panel.set_visible") {
    return SetPanelVisible(layout, target_panel_id, value);
  }
  if (command_id == "view.panel.toggle_visible" || command_id == "panel.toggle_visible") {
    const auto* panel = FindPanel(layout, target_panel_id);
    if (!panel) {
      return {false, "panel not found"};
    }
    return SetPanelVisible(layout, target_panel_id, !panel->visible);
  }
  if (command_id == "view.panel.collapsed" || command_id == "panel.set_collapsed") {
    return SetPanelCollapsed(layout, target_panel_id, value);
  }
  if (command_id == "view.panel.toggle_collapsed" || command_id == "panel.toggle_collapsed") {
    const auto* panel = FindPanel(layout, target_panel_id);
    if (!panel) {
      return {false, "panel not found"};
    }
    return SetPanelCollapsed(layout, target_panel_id, !panel->collapsed);
  }
  if (command_id == "view.panel.dock" || command_id == "panel.set_docked") {
    return SetPanelDockState(layout, target_panel_id, value, false);
  }
  if (command_id == "view.panel.float" || command_id == "panel.set_floating") {
    return SetPanelDockState(layout, target_panel_id, false, true);
  }
  if (command_id == "view.panel.move" || command_id == "panel.move") {
    const auto* panel = FindPanel(layout, target_panel_id);
    if (!panel) {
      return {false, "panel not found"};
    }
    return MovePanel(layout, target_panel_id, value ? value_float : panel->x, value ? panel->y : value_float);
  }
  if (command_id == "view.panel.resize" || command_id == "panel.resize") {
    const auto* panel = FindPanel(layout, target_panel_id);
    if (!panel) {
      return {false, "panel not found"};
    }
    return ResizePanel(layout, target_panel_id, value ? panel->width : value_float, value ? value_float : panel->height);
  }
  return {false, "unknown panel command"};
}

UiEventLog::UiEventLog(std::size_t max_events)
    : m_maxEvents(max_events) {}

void UiEventLog::push(UiEvent event) {
  if (m_events.size() >= m_maxEvents) {
    m_events.pop_front();
  }
  m_events.push_back(std::move(event));
}

const std::deque<UiEvent>& UiEventLog::events() const {
  return m_events;
}

EditorCommandHistory::EditorCommandHistory(std::size_t max_events)
    : m_maxEvents(max_events) {}

void EditorCommandHistory::push(EditorCommand command) {
  if (m_commands.size() >= m_maxEvents) {
    m_commands.erase(m_commands.begin());
  }
  m_commands.push_back(std::move(command));
}

const std::vector<EditorCommand>& EditorCommandHistory::history() const {
  return m_commands;
}

void EditorCommandHistory::clear() {
  m_commands.clear();
}

EditorCommand MakeMenuCommand(std::string_view action_id, std::string_view source, vkpt::core::FrameIndex frame_index) {
  EditorCommand command;
  command.command_id = std::string(action_id);
  command.source_widget = std::string(source);
  command.frame_index = frame_index;

  const auto make_unsupported = [&](std::string_view reason) {
    command.kind = EditorCommandKind::kUnsupportedUiAction;
    command.undoable = false;
    command.redoable = false;
    command.validated = false;
    command.payload = UnsupportedUiActionCommand{
      std::string(action_id),
      std::string(reason)
    };
  };

  if (action_id == "file.new_scene") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "empty_scene";
    payload.entity_name = "New Scene";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "edit.select_none" || action_id == "edit.clear_selection") {
    command.kind = EditorCommandKind::kClearSelection;
    command.payload = ClearSelectionCommand{};
  } else if (action_id == "edit.duplicate") {
    command.kind = EditorCommandKind::kDuplicateEntity;
    command.payload = DuplicateEntityCommand{};
  } else if (action_id == "edit.delete") {
    command.kind = EditorCommandKind::kDeleteEntity;
    command.payload = DeleteEntityCommand{};
  } else if (action_id == "edit.group_selection") {
    command.kind = EditorCommandKind::kGroupEntities;
    command.payload = GroupEntitiesCommand{};
  } else if (action_id == "edit.ungroup_selection") {
    command.kind = EditorCommandKind::kUngroupEntities;
    command.payload = UngroupEntitiesCommand{};
  } else if (action_id == "edit.merge_selection") {
    command.kind = EditorCommandKind::kMergeEntities;
    command.payload = MergeEntitiesCommand{};
  } else if (action_id == "edit.split_merged_object") {
    make_unsupported("split merged object needs merged-object edit runtime");
  } else if (action_id == "create.empty_entity") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "empty";
    payload.entity_name = "New Entity";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.group_entity") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "group";
    payload.entity_name = "New Group";
    payload.requested_parent = 0;
    payload.as_group = true;
    command.payload = payload;
  } else if (action_id == "create.camera" || action_id == "create.light" ||
             action_id == "create.mesh_primitives" || action_id == "create.sdf_primitives") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = std::string(action_id);
    payload.entity_name = std::string(action_id);
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.material") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "material";
    payload.entity_name = "New Material";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.script") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "script";
    payload.entity_name = "New Script";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.physics_body") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "physics_body";
    payload.entity_name = "New Physics Body";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.benchmark_marker") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "benchmark_marker";
    payload.entity_name = "New Benchmark Marker";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "benchmark.run_current_scene") {
    command.kind = EditorCommandKind::kRunBenchmark;
    command.payload = RunBenchmarkCommand{};
  } else if (action_id == "benchmark.run_scene_pack" ||
             action_id == "benchmark.run_cpu_calibration" ||
             action_id == "benchmark.run_gpu_calibration" ||
             action_id == "benchmark.run_simd_experiment" ||
             action_id == "benchmark.run_backend_experiment" ||
             action_id == "benchmark.compare_against_reference") {
    command.kind = EditorCommandKind::kRunBenchmark;
    RunBenchmarkCommand payload;
    payload.desc = MakeDefaultBenchmarkRunDesc("scene.json");
    command.payload = payload;
  } else if (action_id == "benchmark.history" ||
             action_id == "benchmark.open_artifacts" ||
             action_id == "benchmark.export_csv_json") {
    make_unsupported("benchmark artifact action requires benchmark runtime wiring");
  } else if (action_id == "scripts.attach_script_to_selection") {
    command.kind = EditorCommandKind::kAttachScript;
    command.payload = AttachScriptCommand{};
  } else if (action_id == "scripts.detach_script_from_selection") {
    command.kind = EditorCommandKind::kDetachScript;
    command.payload = DetachScriptCommand{};
  } else if (action_id == "edit.command_history") {
    command.kind = EditorCommandKind::kUnsupportedUiAction;
    command.payload = UnsupportedUiActionCommand{
      "edit.command_history",
      "editor command history panel is modeled but not yet wired to command execution"
    };
  } else if (action_id == "assets.import_files" || action_id == "assets.refresh_browser" ||
             action_id == "assets.show_missing_assets" || action_id == "assets.clear_generated_cache" ||
             action_id == "assets.clear_shader_cache" || action_id == "assets.reimport_selected" ||
             action_id == "assets.show_import_diagnostics") {
    make_unsupported("asset actions need import/picker runtime integration");
  } else if (action_id == "file.open_scene" || action_id == "file.open_recent" ||
             action_id == "file.save_scene" || action_id == "file.save_scene_as" ||
             action_id == "file.clone_scene" || action_id == "file.import_asset" ||
             action_id == "file.export_image" || action_id == "file.export_exr" ||
             action_id == "file.export_benchmark_artifacts" || action_id == "file.export_scene_snapshot" ||
             action_id == "file.preferences" || action_id == "file.reveal_artifacts_folder" ||
             action_id == "file.exit") {
    make_unsupported("file menu action requires scene/document runtime integration");
  } else if (action_id == "edit.undo" || action_id == "edit.redo") {
    make_unsupported("undo/redo command stack not wired in stub model");
  } else if (action_id == "edit.cut" || action_id == "edit.copy" || action_id == "edit.paste" ||
             action_id == "edit.rename" || action_id == "edit.select_all" || action_id == "edit.invert_selection" ||
             action_id == "edit.reset_transform" || action_id == "scene.validate_scene" ||
             action_id == "scene.freeze_benchmark_snapshot" || action_id == "scene.reset_accumulation" ||
             action_id == "scene.reload_scene" || action_id == "scene.reload_assets" ||
             action_id == "scene.hot_reload_scripts" || action_id == "scene.scene_settings" ||
             action_id == "scene.lighting_settings" || action_id == "scene.environment_settings" ||
             action_id == "scene.camera_settings" || action_id == "scene.physics_settings" ||
             action_id == "scene.script_settings" || action_id == "scene.animation_settings" ||
             action_id == "edit.reparent_selection" ||
              action_id.starts_with("view.") || action_id.starts_with("render.") ||
              action_id.starts_with("tools.") || action_id.starts_with("help.") ||
              action_id.starts_with("scripts.") ||
              action_id.starts_with("scripts.new_lua_script")) {
    make_unsupported("action requires unimplemented editor runtime");
  } else {
    make_unsupported("unknown or unsupported menu action");
  }
  return command;
}

EditorCommand MakeClearSelectionCommand(vkpt::core::FrameIndex frame_index) {
  EditorCommand command;
  command.command_id = "edit.clear_selection";
  command.kind = EditorCommandKind::kClearSelection;
  command.source_widget = "edit_menu";
  command.frame_index = frame_index;
  command.payload = ClearSelectionCommand{};
  return command;
}

EditorCommand MakeCreateEntityCommand(std::string_view name, std::string_view template_name, vkpt::core::FrameIndex frame_index) {
  EditorCommand command;
  command.command_id = "create.empty_entity";
  command.kind = EditorCommandKind::kCreateEntity;
  command.source_widget = "create_menu";
  command.frame_index = frame_index;
  CreateEntityCommand payload;
  payload.entity_name = std::string(name);
  payload.template_name = std::string(template_name);
  payload.as_group = false;
  command.payload = payload;
  return command;
}

EditorCommand MakeReorderSiblingCommand(vkpt::core::StableId moved_entity,
                                       vkpt::core::StableId sibling_before,
                                       vkpt::core::StableId sibling_after,
                                       vkpt::core::FrameIndex frame_index,
                                       std::string_view source) {
  EditorCommand command;
  command.command_id = "scene_tree.reorder_sibling";
  command.kind = EditorCommandKind::kReorderSibling;
  command.source_widget = std::string(source);
  command.frame_index = frame_index;
  command.payload = ReorderSiblingCommand{
    moved_entity,
    sibling_before,
    sibling_after
  };
  return command;
}

// ----- CameraController implementation ------------------------------------

const char* ToString(CameraControllerMode mode) {
  switch (mode) {
    case CameraControllerMode::Orbit:                return "Orbit";
    case CameraControllerMode::Fps:                  return "Fps";
    case CameraControllerMode::Turntable:            return "Turntable";
    case CameraControllerMode::ScriptedBenchmarkPath:return "ScriptedBenchmarkPath";
    default:                                         return "Unknown";
  }
}

CameraController::CameraController(CameraControllerMode mode) {
  m_state.mode = mode;
}

void CameraController::set_mode(CameraControllerMode mode) {
  if (m_state.mode != mode) {
    m_state.mode = mode;
    mark_dirty();
  }
}

CameraControllerMode CameraController::mode() const {
  return m_state.mode;
}

void CameraController::apply_orbit_delta(float d_azimuth, float d_elevation, float d_zoom) {
  m_state.orbit.azimuth += d_azimuth;
  m_state.orbit.elevation += d_elevation;
  m_state.orbit.radius = std::max(0.01f, m_state.orbit.radius + d_zoom);
  mark_dirty();
}

void CameraController::apply_fps_delta(float d_yaw, float d_pitch, float d_forward, float d_strafe) {
  m_state.fps.yaw += d_yaw;
  m_state.fps.pitch += d_pitch;
  const float cy = std::cos(m_state.fps.yaw);
  const float sy = std::sin(m_state.fps.yaw);
  m_state.fps.position_x += cy * d_forward + (-sy) * d_strafe;
  m_state.fps.position_z += sy * d_forward + cy * d_strafe;
  mark_dirty();
}

void CameraController::apply_turntable_delta(float d_azimuth, float d_zoom) {
  constexpr float kPiOver4 = 0.7853981633974483f;
  constexpr float kTwoPi   = 6.2831853071795865f;
  m_state.orbit.azimuth = std::fmod(m_state.orbit.azimuth + d_azimuth, kTwoPi);
  m_state.orbit.elevation = std::max(-kPiOver4, std::min(kPiOver4, m_state.orbit.elevation));
  m_state.orbit.radius = std::max(0.01f, m_state.orbit.radius + d_zoom);
  mark_dirty();
}

void CameraController::set_benchmark_path(std::vector<CameraWaypoint> waypoints) {
  m_state.benchmark_waypoints = std::move(waypoints);
  m_benchmark_waypoint_index = 0;
  m_benchmark_waypoint_elapsed = 0.0f;
  mark_dirty();
}

void CameraController::advance_benchmark_path(float delta_seconds) {
  if (m_state.benchmark_waypoints.empty()) return;
  m_benchmark_waypoint_elapsed += delta_seconds;
  const auto& wp = m_state.benchmark_waypoints[m_benchmark_waypoint_index];
  if (m_benchmark_waypoint_elapsed >= wp.dwell_seconds) {
    m_benchmark_waypoint_elapsed -= wp.dwell_seconds;
    m_benchmark_waypoint_index = (m_benchmark_waypoint_index + 1) % m_state.benchmark_waypoints.size();
    mark_dirty();
  }
}

bool CameraController::is_dirty(vkpt::core::FrameIndex /*current_frame*/) const {
  return m_dirty;
}

void CameraController::acknowledge_dirty(vkpt::core::FrameIndex current_frame) {
  m_state.last_dirty_frame = current_frame;
  m_dirty = false;
}

const CameraControllerState& CameraController::state() const {
  return m_state;
}

void CameraController::mark_dirty(vkpt::core::FrameIndex frame) {
  m_state.last_dirty_frame = frame;
  m_dirty = true;
}

}  // namespace vkpt::editor
