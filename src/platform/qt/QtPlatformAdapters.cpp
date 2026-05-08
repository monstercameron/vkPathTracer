#include "platform/qt/QtPlatform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/Logging.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QString>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vkpt::platform {

namespace {

constexpr const char* kQtLogSubsystem = "qt";

QString ToQString(std::string_view text) {
  return QString::fromUtf8(text.data(), static_cast<int>(std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));
}

std::string ToUtf8String(const QString& text) {
  const QByteArray bytes = text.toUtf8();
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

}  // namespace
std::size_t QtInput::consume(std::vector<InputEvent>& out) {
  out.clear();
  if (m_source) {
    std::vector<InputEvent> sourced;
    (void)m_source->poll(sourced);
    out.insert(out.end(), sourced.begin(), sourced.end());
  }
  if (m_window == nullptr) {
    return out.size();
  }
  m_window->poll_events();
  auto windowEvents = m_window->drain_events();
  out.insert(out.end(), windowEvents.begin(), windowEvents.end());
  RecordUiEventQueueDepth(0u);
  return out.size();
}

void QtInput::set_source(std::shared_ptr<IInputSource> source) {
  m_source = std::move(source);
}

void QtInput::set_window(QtWindow* window) {
  m_window = window;
}

void QtEvents::publish(std::string_view source, const InputEvent& event) {
  const auto start = std::chrono::steady_clock::now();
  m_events.push_back(event);
  m_highWaterMark = std::max(m_highWaterMark, m_events.size());
  const auto processing_us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  RecordUiInputEvent(source, event, m_events.size(), processing_us);
}

std::size_t QtEvents::consume(std::vector<InputEvent>& out) const {
  out.clear();
  out.reserve(m_events.size());
  for (const auto& event : m_events) {
    out.push_back(event);
  }
  RecordUiEventQueueDepth(m_events.size());
  return out.size();
}

std::size_t QtEvents::drain(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  RecordUiEventQueueDepth(m_events.size());
  return out.size();
}

EventQueueStatus QtEvents::status() const {
  return EventQueueStatus{m_events.size(), m_highWaterMark, m_droppedTotal};
}

std::uint64_t QtTimeSource::now_ms() const {
  using namespace std::chrono;
  const auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
  return now - m_startMs;
}

vkpt::core::Result<std::string> QtFileSystem::read_text_file(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  if (!stream) {
    return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
  }
  std::ostringstream out;
  out << stream.rdbuf();
  return vkpt::core::Result<std::string>::ok(out.str());
}

bool QtFileSystem::file_exists(std::string_view path) const {
  std::ifstream stream{std::string(path)};
  return static_cast<bool>(stream);
}

vkpt::core::Result<void> QtClipboard::set_text(std::string_view text) {
  if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr) {
    if (QClipboard* clipboard = QGuiApplication::clipboard()) {
      clipboard->setText(ToQString(text));
      m_text = std::string(text);
      return vkpt::core::Result<void>::ok();
    }
  }

  m_text = std::string(text);
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Warning,
      kQtLogSubsystem,
      "Qt clipboard unavailable; cached text only");
  return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
}

vkpt::core::Result<std::string> QtClipboard::get_text() const {
  if (qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr) {
    if (const QClipboard* clipboard = QGuiApplication::clipboard()) {
      const std::string text = ToUtf8String(clipboard->text());
      if (!text.empty()) {
        return vkpt::core::Result<std::string>::ok(text);
      }
    }
  }

  if (!m_text.empty()) {
    return vkpt::core::Result<std::string>::ok(m_text);
  }
  return vkpt::core::Result<std::string>::error(vkpt::core::ErrorCode::NotFound);
}

void* QtSurfaceProvider::native_window_handle() const {
  return m_window != nullptr ? m_window->native_handle() : nullptr;
}

void* QtSurfaceProvider::native_instance_handle() const {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetModuleHandleW(nullptr));
#else
  return nullptr;
#endif
}

void QtSurfaceProvider::set_window(QtWindow* window) {
  m_window = window;
}
}  // namespace vkpt::platform
