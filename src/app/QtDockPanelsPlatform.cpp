#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vkpt::app {

vkpt::platform::QtDockArea QtDockAreaForPanel(std::string_view panel_id) {
  if (panel_id == "scene_graph" || panel_id == "asset_browser") {
    return vkpt::platform::QtDockArea::Left;
  }
  if (panel_id == "diagnostics" ||
      panel_id == "performance" ||
      panel_id == "device" ||
      panel_id == "debug_views" ||
      panel_id == "benchmark_panel" ||
      panel_id == "timeline") {
    return vkpt::platform::QtDockArea::Bottom;
  }
  return vkpt::platform::QtDockArea::Right;
}

vkpt::platform::QtDockRow ToQtPlatformDockRow(const QtDockTreeRow& row) {
  vkpt::platform::QtDockRow out;
  out.id = row.id;
  out.label = row.label;
  out.value = row.value;
  out.icon = row.icon;
  out.entity_id = row.entity_id;
  out.selected = row.selected;
  out.activatable = row.activatable;
  out.draggable = row.draggable;
  out.children.reserve(row.children.size());
  for (const auto& child : row.children) {
    out.children.push_back(ToQtPlatformDockRow(child));
  }
  return out;
}

std::vector<vkpt::platform::QtDockPanel> ToQtPlatformDockPanels(
    const std::vector<QtDockPanelContent>& panels) {
  std::vector<vkpt::platform::QtDockPanel> out;
  out.reserve(panels.size());
  for (const auto& panel : panels) {
    vkpt::platform::QtDockPanel dock;
    dock.id = panel.id;
    dock.title = panel.title;
    dock.area = QtDockAreaForPanel(panel.id);
    dock.content = panel.properties.empty()
        ? vkpt::platform::QtDockPanelContent::Tree
        : vkpt::platform::QtDockPanelContent::Properties;
    dock.visible = panel.visible && !panel.collapsed;
    dock.enabled = true;
    dock.closable = true;
    dock.movable = panel.docked;
    dock.floatable = true;
    dock.tree_single_column = panel.tree_single_column;
    dock.tree_stretch = panel.tree_stretch;
    dock.property_stretch = panel.property_stretch;
    dock.property_preferred_height = panel.property_preferred_height;
    dock.preferred_width = QtDockPreferredPixels(panel.width);
    dock.preferred_height = QtDockPreferredPixels(panel.height);
    dock.rows = panel.rows;
    dock.tree_rows.reserve(panel.tree_rows.size());
    for (const auto& row : panel.tree_rows) {
      dock.tree_rows.push_back(ToQtPlatformDockRow(row));
    }
    dock.properties.reserve(panel.properties.size());
    for (const auto& property : panel.properties) {
      vkpt::platform::QtDockProperty dockProperty;
      dockProperty.id = property.id;
      dockProperty.group = property.group;
      dockProperty.name = property.label;
      dockProperty.value = property.value;
      dockProperty.unit = property.unit;
      dockProperty.editor = property.editor;
      dockProperty.options = property.options;
      dockProperty.minimum = property.minimum;
      dockProperty.maximum = property.maximum;
      dockProperty.step = property.step;
      dockProperty.default_value = property.default_value;
      dockProperty.has_numeric_range = property.has_numeric_range;
      dockProperty.has_default = property.has_default;
      dockProperty.editable = property.editable;
      dockProperty.enabled = property.enabled;
      dock.properties.push_back(std::move(dockProperty));
    }
    out.push_back(std::move(dock));
  }
  return out;
}

std::string BuildQtStatusBarText(const vkpt::editor::StatusBarModel& status) {
  std::ostringstream out;
  out << "Scene: " << (status.active_scene.empty() ? "none" : status.active_scene)
      << " | Backend: " << status.backend
      << " | Renderer: " << status.renderer_path
      << " | SPP: " << status.spp
      << " | FPS: " << QtDockNumber(status.fps, 1)
      << " | Frame: " << QtDockNumber(status.frame_ms, 2) << " ms"
      << " | Selected: " << status.selected_entity_count
      << " | Tool: " << status.active_tool;
  if (!status.last_warning_or_error.empty()) {
    out << " | " << status.last_warning_or_error;
  }
  return out.str();
}

void ApplyQtDockPanelsToWindow(vkpt::platform::QtWindow* window,
                               const std::vector<QtDockPanelContent>& panels) {
  if (window == nullptr) {
    return;
  }
  window->set_dock_panels(ToQtPlatformDockPanels(panels));
}

template <typename WindowT>
void ApplyQtStatusBarToWindowTyped(WindowT* window, std::string_view status_text) {
  if (window == nullptr) {
    return;
  }
  if constexpr (requires(WindowT& w, std::string_view text) {
                  w.set_status_bar_text(text);
                }) {
    window->set_status_bar_text(status_text);
  } else if constexpr (requires(WindowT& w, const std::string& text) {
                         w.set_status_bar_text(text);
                       }) {
    const std::string text(status_text);
    window->set_status_bar_text(text);
  } else {
    // App-side adapter only: current QtPlatform.h has no status-bar sink yet.
    (void)status_text;
  }
}

void ApplyQtStatusBarToWindow(vkpt::platform::QtWindow* window,
                              std::string_view status_text) {
  ApplyQtStatusBarToWindowTyped(window, status_text);
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
