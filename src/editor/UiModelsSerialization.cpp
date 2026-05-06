#include "editor/UiModelsInternal.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vkpt::editor {

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
  out << "\"scene_tree_name_filter\":\"" << EscapeJson(state.scene_tree_name_filter) << "\",";
  out << "\"scene_tree_type_filter_mask\":" << state.scene_tree_type_filter_mask << ",";
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
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      if (error) {
        *error = "failed to create parent directory: " + ec.message();
      }
      return false;
    }
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
  const auto parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      if (error) {
        *error = "failed to create parent directory: " + ec.message();
      }
      return false;
    }
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

}  // namespace vkpt::editor
