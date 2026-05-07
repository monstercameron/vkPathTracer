#include "platform/qt/QtPlatform.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/Logging.h"

namespace vkpt::platform {

namespace {

constexpr const char* kQtLogSubsystem = "qt";

}  // namespace

void QtWindow::emit_focus_change(bool focused) {
  if (m_focused == focused) {
    return;
  }
  m_focused = focused;
  queue_event(InputEventNormalizer::focus(focused));
}

void QtWindow::emit_close_requested() {
  if (m_closeEventQueued) {
    return;
  }
  m_closeEventQueued = true;
  queue_event(InputEventNormalizer::close());
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      kQtLogSubsystem,
      "Qt window close requested");
}

void QtWindow::emit_key(std::int32_t key, std::int32_t raw_key, bool pressed) {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Debug,
      kQtLogSubsystem,
      "Qt key event emitted",
      {{"key", std::to_string(key)},
       {"raw_key", std::to_string(raw_key)},
       {"pressed", pressed ? "true" : "false"},
       {"is_f1_key", (key == 0x01000030 || raw_key == 0x01000030) ? "true" : "false"}});
  InputEvent event = InputEventNormalizer::key(key, pressed);
  event.raw_code = raw_key;
  queue_event(event);
}

void QtWindow::emit_mouse_move(int x, int y) {
  queue_event(InputEventNormalizer::mouse_move(
      static_cast<float>(x),
      static_cast<float>(y),
      static_cast<float>(x - m_lastMouseX),
      static_cast<float>(y - m_lastMouseY)));
  m_lastMouseX = x;
  m_lastMouseY = y;
}

void QtWindow::emit_mouse_move_delta(int x, int y, float dx, float dy) {
  queue_event(InputEventNormalizer::mouse_move(
      static_cast<float>(x),
      static_cast<float>(y),
      dx,
      dy));
  m_lastMouseX = x;
  m_lastMouseY = y;
}

void QtWindow::emit_mouse_button(std::int32_t button, bool pressed, int x, int y) {
  m_lastMouseX = x;
  m_lastMouseY = y;
  queue_event(InputEventNormalizer::mouse_button(button,
                                                 pressed,
                                                 static_cast<float>(x),
                                                 static_cast<float>(y)));
}

void QtWindow::emit_mouse_wheel(float delta_x, float delta_y, int x, int y) {
  m_lastMouseX = x;
  m_lastMouseY = y;
  InputEvent event = InputEventNormalizer::mouse_wheel(
      delta_y != 0.0f ? delta_y : delta_x,
      static_cast<float>(x),
      static_cast<float>(y));
  event.delta_x = delta_x;
  event.delta_y = delta_y;
  queue_event(event);
}

void QtWindow::emit_menu_command(std::uint32_t command_id) {
  queue_event(InputEventNormalizer::menu_command(command_id));
}

void QtWindow::emit_dock_property_edit(std::string panel_id,
                                       std::string property_id,
                                       std::string value) {
  if (panel_id.empty() || property_id.empty()) {
    return;
  }
  m_dockPropertyEdits.push_back(QtDockPropertyEdit{
      std::move(panel_id),
      std::move(property_id),
      std::move(value)});
}

void QtWindow::emit_dock_row_activation(std::string panel_id,
                                        std::string row_id,
                                        vkpt::core::StableId entity_id,
                                        bool append,
                                        bool range_mode,
                                        bool viewport_drop,
                                        float x,
                                        float y,
                                        std::string action,
                                        std::vector<std::string> row_ids,
                                        std::vector<vkpt::core::StableId> entity_ids,
                                        vkpt::core::StableId target_entity_id) {
  if (row_id.empty() && !row_ids.empty()) {
    row_id = row_ids.front();
  }
  if (entity_id == 0u && !entity_ids.empty()) {
    entity_id = entity_ids.front();
  }
  if (row_ids.empty() && !row_id.empty()) {
    row_ids.push_back(row_id);
  }
  if (entity_ids.empty() && entity_id != 0u) {
    entity_ids.push_back(entity_id);
  }
  if (panel_id.empty() ||
      (entity_id == 0u && row_id.empty() && row_ids.empty() && entity_ids.empty())) {
    return;
  }
  m_dockRowActivations.push_back(QtDockRowActivation{
      std::move(panel_id),
      std::move(row_id),
      entity_id,
      append,
      range_mode,
      viewport_drop,
      x,
      y,
      std::move(action),
      std::move(row_ids),
      std::move(entity_ids),
      target_entity_id});
}

std::vector<InputEvent> QtWindow::drain_events() {
  std::vector<InputEvent> out;
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out;
}

std::vector<QtDockPropertyEdit> QtWindow::drain_dock_property_edits() {
  std::vector<QtDockPropertyEdit> out;
  out.reserve(m_dockPropertyEdits.size());
  while (!m_dockPropertyEdits.empty()) {
    out.push_back(std::move(m_dockPropertyEdits.front()));
    m_dockPropertyEdits.pop_front();
  }
  return out;
}

std::vector<QtDockRowActivation> QtWindow::drain_dock_row_activations() {
  std::vector<QtDockRowActivation> out;
  out.reserve(m_dockRowActivations.size());
  while (!m_dockRowActivations.empty()) {
    out.push_back(std::move(m_dockRowActivations.front()));
    m_dockRowActivations.pop_front();
  }
  return out;
}
}  // namespace vkpt::platform
