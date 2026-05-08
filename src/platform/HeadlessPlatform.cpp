#include <vector>

#include "platform/HeadlessPlatform.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <utility>

namespace vkpt::platform {

vkpt::core::Status HeadlessWindow::initialize_status(std::size_t width,
                                                     std::size_t height,
                                                     std::string_view title) {
  m_metrics.width = static_cast<int>(width);
  m_metrics.height = static_cast<int>(height);
  set_title(title);
  m_open = true;
  return vkpt::core::Status::ok();
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

vkpt::core::Status HeadlessWindow::poll_events_status() {
  if (!m_open) {
    return vkpt::core::Status::error(vkpt::core::StatusCode::NotReady,
                                     "headless window is closed");
  }
  return vkpt::core::Status::ok();
}

void HeadlessWindow::set_title(std::string_view title) {
  m_title.assign(title);
}

std::size_t HeadlessInput::consume(std::vector<InputEvent>& out) {
  out.clear();
  if (m_source) {
    std::vector<InputEvent> sourced;
    const auto sourcedCount = m_source->poll(sourced);
    for (auto& event : sourced) {
      queue(event);
    }
    (void)sourcedCount;
  }
  out.reserve(m_queue.size());
  while (!m_queue.empty()) {
    out.push_back(m_queue.front());
    m_queue.pop_front();
  }
  RecordUiEventQueueDepth(m_queue.size());
  return out.size();
}

vkpt::core::Status HeadlessInput::set_source_status(std::shared_ptr<IInputSource> source) {
  m_source = std::move(source);
  return vkpt::core::Status::ok();
}

void HeadlessInput::queue(InputEvent event) {
  m_queue.push_back(event);
  RecordUiInputEvent("headless_input", event, m_queue.size(), 0u);
}

vkpt::core::Status HeadlessEvents::publish_status(std::string_view source,
                                                  const InputEvent& event) {
  const auto start = std::chrono::steady_clock::now();
  m_events.push_back(event);
  m_highWaterMark = std::max(m_highWaterMark, m_events.size());
  const auto processing_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  RecordUiInputEvent(source, event, m_events.size(), processing_us);
  return vkpt::core::Status::ok();
}

std::size_t HeadlessEvents::consume(std::vector<InputEvent>& out) const {
  out.clear();
  out.reserve(m_events.size());
  for (const auto& event : m_events) {
    out.push_back(event);
  }
  RecordUiEventQueueDepth(m_events.size());
  return out.size();
}

std::size_t HeadlessEvents::drain(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  RecordUiEventQueueDepth(m_events.size());
  return out.size();
}

EventQueueStatus HeadlessEvents::status() const {
  return EventQueueStatus{m_events.size(), m_highWaterMark, m_droppedTotal};
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

vkpt::core::Status HeadlessPlatform::initialize_status() {
  if (m_initialized) {
    return vkpt::core::Status::ok();
  }
  // Headless still creates a logical window so render and input code can use
  // the same lifecycle contract as desktop platforms.
  const auto window_status = m_window.initialize_status(1280, 720, m_name);
  if (window_status.is_error()) {
    m_lastError = window_status.message.empty()
        ? "headless logical window initialization failed"
        : window_status.message;
    VKP_LOG(Warn,
            "platform",
            "operation_failed",
            "operation",
            "initialize",
            "reason",
            "window_initialize_failed",
            "flow_id",
            std::uint64_t{0});
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                     m_lastError);
  }
  m_initialized = true;
  m_lastError.clear();
  VKP_LIFECYCLE_CONFIG("platform",
                       "backend",
                       "headless",
                       "flow_id",
                       std::uint64_t{0},
                       "width",
                       std::uint64_t{1280},
                       "height",
                       std::uint64_t{720});
  VKP_LIFECYCLE_STARTED("platform",
                        "backend",
                        "headless",
                        "flow_id",
                        std::uint64_t{0});
  return vkpt::core::Status::ok();
}

vkpt::core::Status HeadlessPlatform::shutdown_status() {
  if (!m_initialized) {
    return vkpt::core::Status::ok();
  }
  if (m_window.is_open()) {
    m_window.close();
  }
  m_initialized = false;
  VKP_LIFECYCLE_STOPPED("platform",
                        "backend",
                        "headless",
                        "flow_id",
                        std::uint64_t{0});
  return vkpt::core::Status::ok();
}

void HeadlessPlatform::set_determinism(const vkpt::core::DeterminismContext& context) {
  const auto previous = m_determinism;
  m_determinism = context;
  SetUiDeterminismContext(context);
  vkpt::core::EmitDeterminismChangedIfNeeded("platform", previous, m_determinism);
}

vkpt::core::DeterminismContext HeadlessPlatform::determinism_context() const {
  return m_determinism;
}

bool HeadlessPlatform::is_headless() const {
  return true;
}

PlatformStatus HeadlessPlatform::status() const {
  PlatformStatus out;
  const auto ui_status = GetUiStatus();
  out.initialized = m_initialized;
  out.lifecycle = !m_lastError.empty()
      ? vkpt::core::contracts::ComponentLifecycle::Failed
      : (m_initialized
      ? vkpt::core::contracts::ComponentLifecycle::Ready
         : vkpt::core::contracts::ComponentLifecycle::Uninitialized);
  out.last_tick_ns = ui_status.last_tick_ns;
  out.ticks_total = ui_status.ticks_total;
  out.errors_total = ui_status.errors_total;
  out.headless = true;
  out.window_open = m_window.is_open();
  out.input_focused = true;
  out.vsync_mode = "headless";
  out.last_error = m_lastError;
  out.events = m_events.status();
  out.current_flow_id = std::max(ui_status.current_flow_id,
                                 m_determinism.frame_index);
  out.set_determinism(m_determinism);
  return out;
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
