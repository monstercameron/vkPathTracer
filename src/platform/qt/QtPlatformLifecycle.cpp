#include "platform/qt/QtPlatform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "core/Logging.h"
#include "platform/qt/QtPlatformStyle.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMessageLogContext>
#include <QPalette>
#include <QScreen>
#include <QString>
#include <QtGlobal>

namespace vkpt::platform {

namespace {

constexpr const char* kQtLogSubsystem = "qt";

QString ToQString(std::string_view text) {
  const auto size = std::min<std::size_t>(
      text.size(),
      static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return QString::fromUtf8(text.data(), static_cast<int>(size));
}

std::string ToUtf8String(const QString& text) {
  const QByteArray bytes = text.toUtf8();
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

std::string QtMessageTypeName(QtMsgType type) {
  switch (type) {
    case QtDebugMsg:
      return "debug";
    case QtInfoMsg:
      return "info";
    case QtWarningMsg:
      return "warning";
    case QtCriticalMsg:
      return "critical";
    case QtFatalMsg:
      return "fatal";
    default:
      return "unknown";
  }
}

vkpt::log::Severity QtMessageSeverity(QtMsgType type) {
  switch (type) {
    case QtDebugMsg:
      return vkpt::log::Severity::Debug;
    case QtInfoMsg:
      return vkpt::log::Severity::Info;
    case QtWarningMsg:
      return vkpt::log::Severity::Warning;
    case QtCriticalMsg:
      return vkpt::log::Severity::Error;
    case QtFatalMsg:
      return vkpt::log::Severity::Fatal;
    default:
      return vkpt::log::Severity::Info;
  }
}

QtMessageHandler g_previousQtMessageHandler = nullptr;
bool g_qtMessageHandlerInstalled = false;

void ForwardQtMessage(QtMsgType type, const QMessageLogContext& context, const QString& message) {
  vkpt::log::Logger::instance().log(
      QtMessageSeverity(type),
      kQtLogSubsystem,
      ToUtf8String(message),
      {
        {"qt_type", QtMessageTypeName(type)},
        {"category", context.category != nullptr ? context.category : ""},
        {"file", context.file != nullptr ? context.file : ""},
        {"line", std::to_string(context.line)}
      });
}

void InstallQtDiagnostics() {
  if (!g_qtMessageHandlerInstalled) {
    g_previousQtMessageHandler = qInstallMessageHandler(ForwardQtMessage);
    g_qtMessageHandlerInstalled = true;
  }
}

void RestoreQtDiagnostics() {
  if (g_qtMessageHandlerInstalled) {
    qInstallMessageHandler(g_previousQtMessageHandler);
    g_previousQtMessageHandler = nullptr;
    g_qtMessageHandlerInstalled = false;
  }
}


struct QtAppRuntime {
  int argc = 1;
  std::array<char, 6> arg0 = {'p', 't', 'a', 'p', 'p', '\0'};
  char* argv[1] = {arg0.data()};
  std::unique_ptr<QApplication> app;
};

QtAppRuntime& AppRuntime() {
  static QtAppRuntime runtime;
  return runtime;
}


}  // namespace

QtPlatform::QtPlatform(std::string_view name) : m_name(name) {}

vkpt::core::Status QtPlatform::initialize_status() {
  if (m_initialized) {
    return vkpt::core::Status::ok();
  }

  InstallQtDiagnostics();

  auto& runtime = AppRuntime();
  if (QCoreApplication::instance() == nullptr) {
    QCoreApplication::setAttribute(Qt::AA_CompressHighFrequencyEvents, false);
    QCoreApplication::setAttribute(Qt::AA_CompressTabletEvents, false);
    runtime.app = std::make_unique<QApplication>(runtime.argc, runtime.argv);
  } else if (qobject_cast<QApplication*>(QCoreApplication::instance()) == nullptr) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error,
        kQtLogSubsystem,
        "existing QCoreApplication is not a QApplication");
    m_lastError = "existing QCoreApplication is not a QApplication";
    RestoreQtDiagnostics();
    return vkpt::core::Status::error(vkpt::core::StatusCode::Unsupported,
                                     m_lastError);
  }
  QCoreApplication::setApplicationName(ToQString(m_name));
  if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
    app->setPalette(StartupDarkPalette());
  }

  m_input.set_window(&m_window);
  m_surface.set_window(&m_window);

  const auto window_status = m_window.initialize_status(1280, 720, m_name);
  if (window_status.is_error()) {
    m_input.set_window(nullptr);
    m_surface.set_window(nullptr);
    m_lastError = window_status.message.empty()
        ? "Qt window initialization failed"
        : window_status.message;
    RestoreQtDiagnostics();
    return vkpt::core::Status::error(vkpt::core::StatusCode::InternalError,
                                     m_lastError);
  }

  const QString platformName = QGuiApplication::platformName();
  const QScreen* screen = QGuiApplication::primaryScreen();
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      kQtLogSubsystem,
      "Qt platform initialized",
      {
        {"qt_version", qVersion()},
        {"qt_platform_plugin", ToUtf8String(platformName)},
        {"qt_high_dpi_policy", std::to_string(static_cast<int>(QGuiApplication::highDpiScaleFactorRoundingPolicy()))},
        {"window_system", "qt_widgets"},
        {"primary_screen", screen != nullptr ? ToUtf8String(screen->name()) : std::string("none")},
        {"device_pixel_ratio", screen != nullptr ? std::to_string(screen->devicePixelRatio()) : std::string("1")},
        {"viewport_native_handle_valid", m_window.native_handle() != nullptr ? "true" : "false"},
        {"viewport_device_pixel_ratio", std::to_string(m_window.metrics().dpiScale)}
      });

  m_initialized = true;
  m_lastError.clear();
  return vkpt::core::Status::ok();
}

vkpt::core::Status QtPlatform::shutdown_status() {
  if (!m_initialized) {
    return vkpt::core::Status::ok();
  }

  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      kQtLogSubsystem,
      "Qt platform shutdown");

  if (m_window.is_open()) {
    m_window.close();
  }
  if (QCoreApplication::instance() != nullptr) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
  }
  m_window.destroy();
  m_input.set_window(nullptr);
  m_surface.set_window(nullptr);
  m_initialized = false;
  RestoreQtDiagnostics();
  return vkpt::core::Status::ok();
}

void QtPlatform::set_determinism(const vkpt::core::DeterminismContext& context) {
  const auto previous = m_determinism;
  m_determinism = context;
  SetUiDeterminismContext(context);
  vkpt::core::EmitDeterminismChangedIfNeeded("platform", previous, m_determinism);
}

vkpt::core::DeterminismContext QtPlatform::determinism_context() const {
  return m_determinism;
}

bool QtPlatform::is_headless() const {
  return false;
}

PlatformStatus QtPlatform::status() const {
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
  out.headless = false;
  out.window_open = m_window.is_open();
  out.input_focused = true;
  out.vsync_mode = "qt";
  out.last_error = m_lastError;
  out.events = m_events.status();
  out.current_flow_id = std::max(ui_status.current_flow_id,
                                 m_determinism.frame_index);
  out.set_determinism(m_determinism);
  return out;
}

IWindow* QtPlatform::window() { return &m_window; }
const IWindow* QtPlatform::window() const { return &m_window; }
IInput* QtPlatform::input() { return &m_input; }
const IInput* QtPlatform::input() const { return &m_input; }
IEvents* QtPlatform::events() { return &m_events; }
const IEvents* QtPlatform::events() const { return &m_events; }
IFileSystem* QtPlatform::file_system() { return &m_file_system; }
const IFileSystem* QtPlatform::file_system() const { return &m_file_system; }
ITimeSource* QtPlatform::time_source() { return &m_time_source; }
const ITimeSource* QtPlatform::time_source() const { return &m_time_source; }
IClipboard* QtPlatform::clipboard() { return &m_clipboard; }
const IClipboard* QtPlatform::clipboard() const { return &m_clipboard; }
INativeSurfaceProvider* QtPlatform::native_surface() { return &m_surface; }
const INativeSurfaceProvider* QtPlatform::native_surface() const { return &m_surface; }


}  // namespace vkpt::platform
