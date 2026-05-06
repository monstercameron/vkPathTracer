#include <vector>

#include "platform/HeadlessPlatform.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace vkpt::platform {

bool HeadlessWindow::initialize(std::size_t width, std::size_t height, std::string_view title) {
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  set_title(title);
  m_open = true;
  return true;
}

bool HeadlessWindow::is_open() const {
  return m_open;
}

void HeadlessWindow::close() {
  m_open = false;
}

WindowMetrics HeadlessWindow::metrics() const {
  return m_metrics;
}

bool HeadlessWindow::poll_events() {
  return m_open;
}

void HeadlessWindow::set_title(std::string_view title) {
  m_title.assign(title);
}

std::size_t HeadlessInput::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_queue.size());
  while (!m_queue.empty()) {
    out.push_back(m_queue.front());
    m_queue.pop_front();
  }
  return out.size();
}

void HeadlessInput::queue(InputEvent event) {
  m_queue.push_back(event);
}

void HeadlessEvents::publish(std::string_view source, const InputEvent& event) {
  (void)source;
  m_events.push_back(event);
}

std::size_t HeadlessEvents::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out.size();
}

std::uint64_t HeadlessTimeSource::now_ms() const {
  using namespace std::chrono;
  const auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  return now - m_startMs;
}

vkpt::core::Result<std::string> HeadlessFileSystem::read_text_file(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  if (!stream) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  std::ostringstream out;
  out << stream.rdbuf();
  return vkpt::core::Result<std::string>::ok(out.str());
}

bool HeadlessFileSystem::file_exists(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  return static_cast<bool>(stream);
}

vkpt::core::Result<void> HeadlessClipboard::set_text(std::string_view text) {
  m_text = std::string(text);
  return vkpt::core::Result<void>::ok();
}

vkpt::core::Result<std::string> HeadlessClipboard::get_text() const {
  if (m_text.empty()) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  return vkpt::core::Result<std::string>::ok(m_text);
}

void* HeadlessSurfaceProvider::native_window_handle() const {
  return nullptr;
}

void* HeadlessSurfaceProvider::native_instance_handle() const {
  return nullptr;
}

HeadlessPlatform::HeadlessPlatform(std::string_view name) : m_name(name) {
  m_time_source = HeadlessTimeSource(0);
}

vkpt::core::Result<void> HeadlessPlatform::initialize() {
  if (m_initialized) {
    return vkpt::core::Result<void>::ok();
  }
  // Headless still creates a logical window so render and input code can use
  // the same lifecycle contract as desktop platforms.
  if (!m_window.initialize(1280, 720, m_name)) {
    return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
  }
  m_initialized = true;
  return vkpt::core::Result<void>::ok();
}

void HeadlessPlatform::shutdown() {
  if (!m_initialized) {
    return;
  }
  if (m_window.is_open()) {
    m_window.close();
  }
  m_initialized = false;
}

bool HeadlessPlatform::is_headless() const {
  return true;
}

IWindow* HeadlessPlatform::window() { return &m_window; }
const IWindow* HeadlessPlatform::window() const { return &m_window; }
IInput* HeadlessPlatform::input() { return &m_input; }
const IInput* HeadlessPlatform::input() const { return &m_input; }
IEvents* HeadlessPlatform::events() { return &m_events; }
const IEvents* HeadlessPlatform::events() const { return &m_events; }
IFileSystem* HeadlessPlatform::file_system() { return &m_file_system; }
const IFileSystem* HeadlessPlatform::file_system() const { return &m_file_system; }
ITimeSource* HeadlessPlatform::time_source() { return &m_time_source; }
const ITimeSource* HeadlessPlatform::time_source() const { return &m_time_source; }
IClipboard* HeadlessPlatform::clipboard() { return &m_clipboard; }
const IClipboard* HeadlessPlatform::clipboard() const { return &m_clipboard; }
INativeSurfaceProvider* HeadlessPlatform::native_surface() { return &m_surface; }
const INativeSurfaceProvider* HeadlessPlatform::native_surface() const { return &m_surface; }

}  // namespace vkpt::platform
