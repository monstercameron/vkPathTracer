#include "editor/UiModelsInternal.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vkpt::editor {

namespace {

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
      PlacePanel(panels, "script_panel", true, 960.0f, 64.0f, 320.0f, 360.0f);
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
  state.visible_panels = {"scene_tree", "inspector", "script_panel", "asset_browser", "viewport", "status_bar", "console"};
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

}  // namespace vkpt::editor
