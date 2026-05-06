#include "platform/qt/QtPlatform.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
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
  if (m_window == nullptr) {
    return 0u;
  }
  m_window->poll_events();
  out = m_window->drain_events();
  return out.size();
}

void QtInput::set_window(QtWindow* window) {
  m_window = window;
}

void QtEvents::publish(std::string_view source, const InputEvent& event) {
  (void)source;
  m_events.push_back(event);
}

std::size_t QtEvents::consume(std::vector<InputEvent>& out) {
  out.clear();
  out.reserve(m_events.size());
  while (!m_events.empty()) {
    out.push_back(m_events.front());
    m_events.pop_front();
  }
  return out.size();
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
