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

bool ReadJsonNumber(const vkpt::scene::JsonValue& value, const std::string& key, float& out) {
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

bool ReadJsonUInt(const vkpt::scene::JsonValue& value, const std::string& key, std::uint32_t& out) {
  std::uint64_t tmp = 0;
  if (!ReadJsonUInt64(value, key, tmp)) {
    return false;
  }
  if (tmp > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(tmp);
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
    case UiPanelId::Console:
      return "console";
    case UiPanelId::StatusBar:
      return "status_bar";
    case UiPanelId::Viewport:
      return "viewport";
    case UiPanelId::MenuBar:
      return "menu_bar";
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

MenuItem* FindPanelMutatingMenuItem(MenuBar& menu, std::string_view item_id) {
  for (auto& item : menu.top_level_menus) {
    if (auto* found = FindMenuItemRecursive(item, item_id)) {
      return found;
    }
  }
  return nullptr;
}

const MenuItem* FindPanelMutatingMenuItem(const MenuBar& menu, std::string_view item_id) {
  for (const auto& item : menu.top_level_menus) {
    if (auto* found = FindMenuItemRecursive(item, item_id)) {
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

const UiPanelState* FindPanel(const UiLayoutDocument& layout, std::string_view panel_id) {
  const auto it = std::find_if(layout.panels.begin(), layout.panels.end(),
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
      child.enabled = hasSelection;
    }
  }
}

}  // namespace

UiLayoutDocument CreateDefaultLayout() {
  return CreateLayoutPreset(LayoutPreset::Default);
}

std::vector<UiPanelState> BuildDefaultPanelStates(LayoutPreset preset) {
  switch (preset) {
    case LayoutPreset::Benchmark:
      return {
        {"scene_tree", true, true, false, false, true, true, false, true, true, 0.0f, 0.0f, 260.0f, 360.0f},
        {"inspector", true, true, true, true, true, false, false, true, true, 0.0f, 360.0f, 260.0f, 360.0f},
        {"asset_browser", false, true, true, true, true, false, false, true, true, 0.0f, 720.0f, 260.0f, 280.0f},
        {"benchmark_panel", true, true, true, true, true, false, false, true, true, 260.0f, 0.0f, 1020.0f, 360.0f},
        {"console", true, true, true, true, true, false, false, true, true, 260.0f, 720.0f, 500.0f, 220.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 1020.0f, 1080.0f}
      };
    case LayoutPreset::MaterialAuthoring:
      return {
        {"scene_tree", true, true, false, false, true, true, false, true, true, 0.0f, 0.0f, 260.0f, 360.0f},
        {"inspector", true, true, true, true, true, false, false, true, true, 0.0f, 360.0f, 260.0f, 360.0f},
        {"material_editor", true, true, true, true, true, false, false, true, true, 260.0f, 0.0f, 220.0f, 480.0f},
        {"asset_browser", false, true, true, true, true, false, false, true, true, 260.0f, 480.0f, 220.0f, 240.0f},
        {"console", true, true, true, true, true, false, false, true, true, 480.0f, 720.0f, 260.0f, 220.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 760.0f, 1080.0f}
      };
    case LayoutPreset::Scripting:
      return {
        {"scene_tree", true, true, false, false, true, true, false, true, true, 0.0f, 0.0f, 260.0f, 360.0f},
        {"inspector", true, true, true, true, true, false, false, true, true, 0.0f, 360.0f, 260.0f, 360.0f},
        {"asset_browser", false, true, true, true, true, false, false, true, true, 0.0f, 720.0f, 260.0f, 200.0f},
        {"script_panel", true, true, true, true, true, false, false, true, true, 260.0f, 0.0f, 300.0f, 720.0f},
        {"console", true, true, true, true, true, false, false, true, true, 560.0f, 720.0f, 380.0f, 200.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 760.0f, 1080.0f}
      };
    case LayoutPreset::AssetManagement:
      return {
        {"scene_tree", true, true, false, false, true, true, false, true, true, 0.0f, 0.0f, 260.0f, 360.0f},
        {"asset_browser", true, true, true, true, true, false, false, true, true, 0.0f, 360.0f, 260.0f, 360.0f},
        {"inspector", false, true, true, true, true, false, false, true, true, 0.0f, 720.0f, 260.0f, 280.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"console", true, true, true, true, true, false, false, true, true, 260.0f, 720.0f, 500.0f, 220.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 1020.0f, 1080.0f}
      };
    case LayoutPreset::DebugProfiler:
      return {
        {"scene_tree", true, true, false, false, true, true, false, true, true, 0.0f, 0.0f, 260.0f, 360.0f},
        {"inspector", true, true, true, true, true, false, false, true, true, 0.0f, 360.0f, 260.0f, 360.0f},
        {"console", true, true, true, true, true, false, false, true, true, 260.0f, 0.0f, 420.0f, 480.0f},
        {"benchmark_panel", true, true, true, true, true, false, false, true, true, 680.0f, 0.0f, 360.0f, 480.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 1040.0f, 1080.0f}
      };
    case LayoutPreset::MinimalViewport:
      return {
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 1280.0f, 1054.0f},
        {"scene_tree", false, true, true, true, true, false, false, true, true, 0.0f, 0.0f, 260.0f, 360.0f}
      };
    case LayoutPreset::FullscreenViewportWithOverlay:
      return {
        {"viewport", true, true, false, false, false, false, true, true, true, 0.0f, 0.0f, 1280.0f, 1080.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"console", false, true, true, true, true, false, false, true, true, 0.0f, 1054.0f, 1280.0f, 26.0f},
        {"inspector", false, true, true, true, true, false, false, true, true, 1020.0f, 0.0f, 260.0f, 360.0f}
      };
    case LayoutPreset::Default:
    default:
      return {
        {"scene_tree", true, true, false, false, true, true, false, true, true, 0.0f, 0.0f, 280.0f, 360.0f},
        {"inspector", true, true, true, true, true, false, false, true, true, 0.0f, 360.0f, 280.0f, 360.0f},
        {"asset_browser", true, true, true, true, true, false, false, true, true, 0.0f, 720.0f, 280.0f, 360.0f},
        {"benchmark_panel", false, true, true, true, true, false, false, true, true, 280.0f, 720.0f, 320.0f, 360.0f},
        {"console", true, true, true, true, true, false, false, true, true, 280.0f, 1080.0f, 280.0f, 220.0f},
        {"status_bar", false, true, false, false, false, false, false, false, false, 0.0f, 0.0f, 1280.0f, 26.0f},
        {"viewport", true, true, false, false, false, false, true, true, true, 280.0f, 0.0f, 1000.0f, 720.0f}
      };
  }
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
  top.back().children.push_back(MenuItemNode("view.panels", "Panels"));
  top.back().children.push_back(MenuItemNode("view.layouts", "Layouts"));
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
