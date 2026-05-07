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

vkpt::core::Result<void> QtPlatform::initialize() {
  if (m_initialized) {
    return vkpt::core::Result<void>::ok();
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
    RestoreQtDiagnostics();
    return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Unsupported);
  }
  QCoreApplication::setApplicationName(ToQString(m_name));
  if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
    app->setPalette(StartupDarkPalette());
  }

  m_input.set_window(&m_window);
  m_surface.set_window(&m_window);

  if (!m_window.initialize(1280, 720, m_name)) {
    m_input.set_window(nullptr);
    m_surface.set_window(nullptr);
    RestoreQtDiagnostics();
    return vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Internal);
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
  return vkpt::core::Result<void>::ok();
}

void QtPlatform::shutdown() {
  if (!m_initialized) {
    return;
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
}

bool QtPlatform::is_headless() const {
  return false;
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
