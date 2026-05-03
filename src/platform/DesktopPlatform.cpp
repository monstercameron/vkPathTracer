#include "platform/DesktopPlatform.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace vkpt::platform {

bool DesktopWindow::initialize(std::size_t width, std::size_t height, std::string_view title) {
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  m_title.assign(title);
  m_open = true;
  m_events.emplace_back(InputEventNormalizer::resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
  m_events.emplace_back(InputEventNormalizer::focus(true));
  return true;
}

bool DesktopWindow::is_open() const {
  return m_open;
}

void DesktopWindow::close() {
  if (!m_open) {
    return;
  }
  m_open = false;
  m_events.emplace_back(InputEventNormalizer::close());
}

WindowMetrics DesktopWindow::metrics() const {
  return m_metrics;
}

bool DesktopWindow::poll_events() {
  return m_open;
}

bool DesktopWindow::resize(std::size_t width, std::size_t height) {
  if (!m_open) {
    return false;
  }
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  m_events.emplace_back(InputEventNormalizer::resize(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
  return true;
}

void DesktopWindow::set_title(std::string_view title) {
  m_title.assign(title);
}

void DesktopWindow::emit_focus_change(bool focused) {
  m_focused = focused;
  m_events.emplace_back(InputEventNormalizer::focus(focused));
}

void DesktopWindow::emit_close_requested() {
  m_events.emplace_back(InputEventNormalizer::close());
}

std::vector<InputEvent> DesktopWindow::drain_events() {
  std::vector<InputEvent> out;
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out;
}

std::size_t DesktopInput::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_queue.size());
  while (!m_queue.empty()) {
    out.push_back(m_queue.front());
    m_queue.pop_front();
  }
  return out.size();
}

void DesktopInput::queue(InputEvent event) {
  m_queue.push_back(event);
}

void DesktopInput::queue_normalized(InputEvent event) {
  m_queue.push_back(event);
}

void DesktopInput::emit_key(std::int32_t key, bool pressed) {
  queue(InputEventNormalizer::key(key, pressed));
}

void DesktopInput::emit_mouse_move(float x, float y, float dx, float dy) {
  queue(InputEventNormalizer::mouse_move(x, y, dx, dy));
}

void DesktopInput::emit_mouse_button(std::int32_t button, bool pressed, float x, float y) {
  queue(InputEventNormalizer::mouse_button(button, pressed, x, y));
}

void DesktopInput::emit_mouse_wheel(float delta, float x, float y) {
  queue(InputEventNormalizer::mouse_wheel(delta, x, y));
}

void DesktopInput::emit_touch(std::int32_t touch_id, std::int32_t phase, float x, float y) {
  InputEvent event{InputEventType::None, 0, x, y, 0u, touch_id};
  event.delta_z = static_cast<float>(phase);
  m_queue.push_back(event);
}

void DesktopEvents::publish(std::string_view source, const InputEvent& event) {
  (void)source;
  m_events.push_back(event);
}

std::size_t DesktopEvents::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out.size();
}

std::uint64_t DesktopTimeSource::now_ms() const {
  using namespace std::chrono;
  const auto now = duration_cast<std::chrono::milliseconds>(system_clock::now().time_since_epoch()).count();
  return now - m_startMs;
}

vkpt::core::Result<std::string> DesktopFileSystem::read_text_file(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  if (!stream) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  std::ostringstream out;
  out << stream.rdbuf();
  return vkpt::core::Result<std::string>::ok(out.str());
}

bool DesktopFileSystem::file_exists(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  return static_cast<bool>(stream);
}

vkpt::core::Result<void> DesktopClipboard::set_text(std::string_view text) {
  m_text = std::string(text);
  return vkpt::core::Result<void>::ok();
}

vkpt::core::Result<std::string> DesktopClipboard::get_text() const {
  if (m_text.empty()) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  return vkpt::core::Result<std::string>::ok(m_text);
}

void* DesktopSurfaceProvider::native_window_handle() const {
  return nullptr;
}

void* DesktopSurfaceProvider::native_instance_handle() const {
  return nullptr;
}

DesktopPlatform::DesktopPlatform(std::string_view name) : m_name(name) {}

vkpt::core::Result<void> DesktopPlatform::initialize() {
  if (m_initialized) {
    return vkpt::core::Result<void>::ok();
  }
  if (!m_window.initialize(1280, 720, m_name)) {
    return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
  }
  m_initialized = true;
  return vkpt::core::Result<void>::ok();
}

void DesktopPlatform::shutdown() {
  if (!m_initialized) {
    return;
  }
  if (m_window.is_open()) {
    m_window.close();
  }
  m_initialized = false;
}

bool DesktopPlatform::is_headless() const {
  return false;
}

IWindow* DesktopPlatform::window() { return &m_window; }
const IWindow* DesktopPlatform::window() const { return &m_window; }
IInput* DesktopPlatform::input() { return &m_input; }
const IInput* DesktopPlatform::input() const { return &m_input; }
IEvents* DesktopPlatform::events() { return &m_events; }
const IEvents* DesktopPlatform::events() const { return &m_events; }
IFileSystem* DesktopPlatform::file_system() { return &m_file_system; }
const IFileSystem* DesktopPlatform::file_system() const { return &m_file_system; }
ITimeSource* DesktopPlatform::time_source() { return &m_time_source; }
const ITimeSource* DesktopPlatform::time_source() const { return &m_time_source; }
IClipboard* DesktopPlatform::clipboard() { return &m_clipboard; }
const IClipboard* DesktopPlatform::clipboard() const { return &m_clipboard; }
INativeSurfaceProvider* DesktopPlatform::native_surface() { return &m_surface; }
const INativeSurfaceProvider* DesktopPlatform::native_surface() const { return &m_surface; }

}  // namespace vkpt::platform
