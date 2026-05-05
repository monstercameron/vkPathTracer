#include "platform/qt/QtPlatform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include "core/Logging.h"

#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFocusEvent>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPaintEvent>
#include <QPoint>
#include <QPen>
#include <QRect>
#include <QResizeEvent>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QtGlobal>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vkpt::platform {

namespace {

constexpr const char* kQtLogSubsystem = "qt";
constexpr std::uint64_t kQtFramebufferStatsLogPeriod = 120u;

int ClampToQtInt(std::size_t value) {
  constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(value, kMax));
}

QString ToQString(std::string_view text) {
  const auto size = std::min<std::size_t>(
      text.size(),
      static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return QString::fromUtf8(text.data(), static_cast<int>(size));
}

bool CheckedRgbaByteCount(std::size_t width, std::size_t height, std::size_t& out) {
  out = 0u;
  if (width == 0u || height == 0u) {
    return false;
  }
  constexpr std::size_t kBytesPerPixel = 4u;
  const auto max = std::numeric_limits<std::size_t>::max();
  if (width > max / height) {
    return false;
  }
  const std::size_t pixels = width * height;
  if (pixels > max / kBytesPerPixel) {
    return false;
  }
  out = pixels * kBytesPerPixel;
  return true;
}

bool FitsQtImageDimensions(std::size_t width, std::size_t height) {
  constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
  return width <= kMax && height <= kMax;
}

bool ShouldLogQtFramebufferStats(std::uint64_t count) {
  return count != 0u && (count % kQtFramebufferStatsLogPeriod) == 0u;
}

void LogQtFramebufferStats(std::string_view message, const QtFramebufferStats& stats) {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      "traceprobe",
      message,
      {
        {"received", std::to_string(stats.received)},
        {"presented", std::to_string(stats.presented)},
        {"dropped", std::to_string(stats.dropped)},
        {"latest_published_id", std::to_string(stats.latestPublishedId)},
        {"latest_presented_id", std::to_string(stats.latestPresentedId)},
        {"latest_published_width", std::to_string(stats.latestPublishedWidth)},
        {"latest_published_height", std::to_string(stats.latestPublishedHeight)},
        {"latest_presented_width", std::to_string(stats.latestPresentedWidth)},
        {"latest_presented_height", std::to_string(stats.latestPresentedHeight)}
      });
}

std::string ToUtf8String(const QString& text) {
  const QByteArray bytes = text.toUtf8();
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

int QtMouseButtonCode(Qt::MouseButton button) {
  switch (button) {
    case Qt::LeftButton:
      return 0;
    case Qt::RightButton:
      return 1;
    case Qt::MiddleButton:
      return 2;
    case Qt::BackButton:
      return 3;
    case Qt::ForwardButton:
      return 4;
    default:
      return static_cast<int>(button);
  }
}

std::int32_t QtNativeKeyCode(const QKeyEvent& event) {
  const auto native = event.nativeVirtualKey();
  if (native != 0u && native <= static_cast<quint32>(std::numeric_limits<std::int32_t>::max())) {
    return static_cast<std::int32_t>(native);
  }
  return static_cast<std::int32_t>(event.key());
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

void* WindowIdToHandle(WId id) {
#if defined(_WIN32) || defined(Q_OS_MACOS)
  return reinterpret_cast<void*>(id);
#else
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
#endif
}

}  // namespace

class QtViewportWindow final : public QWidget {
 public:
  explicit QtViewportWindow(QtWindow* owner) : QWidget(nullptr), m_owner(owner) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    auto* timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this]() {
      if (m_dirty) {
        update();
      }
    });
    timer->start(16);
  }

  void setOwner(QtWindow* owner) {
    m_owner = owner;
  }

  void setOverlayText(QString text) {
    if (m_overlayText == text) {
      return;
    }
    m_overlayText = std::move(text);
    m_dirty = true;
    update();
  }

  void setSelectionOverlayBoxes(std::vector<QtSelectionOverlayBox> boxes) {
    m_overlayBoxes = std::move(boxes);
    m_dirty = true;
    update();
  }

  void clearFrameOnUiThread() {
    dropUnpresentedFrame();
    m_frame = QImage();
    m_frameId = 0u;
    m_frameWidth = 0u;
    m_frameHeight = 0u;
    m_framePresented = true;
    m_dirty = true;
    update();
  }

  bool deliverFrameOnUiThread(const std::uint8_t* src,
                              std::size_t srcBytes,
                              std::size_t width,
                              std::size_t height,
                              std::uint64_t frameId) {
    if (src == nullptr || !FitsQtImageDimensions(width, height)) {
      clearFrameOnUiThread();
      return false;
    }

    std::size_t required = 0u;
    if (!CheckedRgbaByteCount(width, height, required) ||
        srcBytes < required) {
      clearFrameOnUiThread();
      return false;
    }

    const int imageWidth = static_cast<int>(width);
    const int imageHeight = static_cast<int>(height);
    if (m_frame.width() != imageWidth || m_frame.height() != imageHeight ||
        m_frame.format() != QImage::Format_RGBA8888) {
      m_frame = QImage(imageWidth, imageHeight, QImage::Format_RGBA8888);
    }
    if (m_frame.isNull()) {
      return false;
    }

    const std::size_t srcStride = width * 4u;
    const auto dstStride = m_frame.bytesPerLine();
    if (dstStride < 0 || static_cast<std::size_t>(dstStride) < srcStride) {
      clearFrameOnUiThread();
      return false;
    }

    dropUnpresentedFrame();
    for (std::size_t y = 0; y < height; ++y) {
      const auto* srcRow = src + static_cast<std::size_t>(y) * srcStride;
      auto* dstRow = m_frame.scanLine(static_cast<int>(y));
      std::memcpy(dstRow, srcRow, srcStride);
    }

    m_frameId = frameId;
    m_frameWidth = width;
    m_frameHeight = height;
    m_framePresented = false;
    m_dirty = true;
    update();
    return true;
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(6, 8, 12));

    // UI thread rule: painting only draws the most recent immutable display image
    // and overlay. Rendering, resolving, and handoff publication happen elsewhere.
    if (!m_frame.isNull()) {
      painter.drawImage(rect(), m_frame);
    }

    if (!m_overlayBoxes.empty()) {
      painter.setRenderHint(QPainter::Antialiasing, true);
      QFont labelFont = painter.font();
      labelFont.setPointSize(9);
      labelFont.setBold(true);
      painter.setFont(labelFont);

      for (const auto& box : m_overlayBoxes) {
        const QRectF rawRect(box.x, box.y, box.width, box.height);
        QRectF clipped = rawRect.intersected(QRectF(rect()));
        if (!clipped.isValid() || clipped.width() < 2.0 || clipped.height() < 2.0) {
          continue;
        }

        const QColor stroke = box.primary ? QColor(255, 214, 64, 245)
                                          : QColor(102, 204, 255, 230);
        const QColor fill = box.primary ? QColor(255, 214, 64, 24)
                                        : QColor(102, 204, 255, 18);
        painter.fillRect(clipped, fill);
        QPen shadowPen(QColor(0, 0, 0, 190));
        shadowPen.setWidthF(4.0);
        painter.setPen(shadowPen);
        painter.drawRect(clipped);
        QPen strokePen(stroke);
        strokePen.setWidthF(box.primary ? 2.0 : 1.5);
        painter.setPen(strokePen);
        painter.drawRect(clipped);

        if (!box.label.empty()) {
          const QString label = ToQString(box.label);
          const QRectF labelRect(clipped.left() + 4.0,
                                 std::max(0.0, clipped.top() - 20.0),
                                 std::max(48.0, clipped.width()),
                                 18.0);
          painter.fillRect(labelRect.adjusted(-2.0, 0.0, 2.0, 0.0), QColor(0, 0, 0, 150));
          painter.setPen(stroke);
          painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label);
        }
      }
    }

    const QString text = m_overlayText.isEmpty()
        ? QStringLiteral("vkpt qt ui shell")
        : m_overlayText;
    const QRect textRect = rect().adjusted(12, 40, -12, -12);
    if (!text.isEmpty() && textRect.isValid()) {
      QFont overlayFont = painter.font();
      overlayFont.setStyleHint(QFont::Monospace);
      overlayFont.setPointSize(10);
      painter.setFont(overlayFont);
      painter.setRenderHint(QPainter::TextAntialiasing, true);
      constexpr int flags = Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap;

      painter.setPen(QColor(0, 0, 0, 210));
      painter.drawText(textRect.translated(1, 1), flags, text);
      painter.setPen(QColor(255, 255, 255, 235));
      painter.drawText(textRect, flags, text);
    }

    if (!m_frame.isNull() && !m_framePresented) {
      m_framePresented = true;
      if (m_owner != nullptr) {
        m_owner->record_frame_presented(m_frameId, m_frameWidth, m_frameHeight);
      }
    }

    m_dirty = false;
  }

  void closeEvent(QCloseEvent* event) override {
    if (m_owner != nullptr) {
      m_owner->emit_close_requested();
      m_owner->mark_closed();
    }
    event->accept();
    QWidget::closeEvent(event);
  }

  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    if (m_owner != nullptr) {
      m_owner->on_native_resize(static_cast<std::size_t>(event->size().width()),
                                static_cast<std::size_t>(event->size().height()));
    }
  }

  void focusInEvent(QFocusEvent* event) override {
    QWidget::focusInEvent(event);
    if (m_owner != nullptr) {
      m_owner->emit_focus_change(true);
    }
  }

  void focusOutEvent(QFocusEvent* event) override {
    QWidget::focusOutEvent(event);
    if (m_owner != nullptr) {
      m_owner->emit_focus_change(false);
    }
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (m_owner != nullptr && !event->isAutoRepeat()) {
      m_owner->emit_key(static_cast<std::int32_t>(event->key()), QtNativeKeyCode(*event), true);
    }
    event->accept();
  }

  void keyReleaseEvent(QKeyEvent* event) override {
    if (m_owner != nullptr && !event->isAutoRepeat()) {
      m_owner->emit_key(static_cast<std::int32_t>(event->key()), QtNativeKeyCode(*event), false);
    }
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (m_owner != nullptr) {
      const QPointF pos = event->position();
      m_owner->emit_mouse_move(static_cast<int>(pos.x()), static_cast<int>(pos.y()));
    }
    event->accept();
  }

  void mousePressEvent(QMouseEvent* event) override {
    if (m_owner != nullptr) {
      const QPointF pos = event->position();
      m_owner->emit_mouse_button(QtMouseButtonCode(event->button()),
                                 true,
                                 static_cast<int>(pos.x()),
                                 static_cast<int>(pos.y()));
    }
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (m_owner != nullptr) {
      const QPointF pos = event->position();
      m_owner->emit_mouse_button(QtMouseButtonCode(event->button()),
                                 false,
                                 static_cast<int>(pos.x()),
                                 static_cast<int>(pos.y()));
    }
    event->accept();
  }

  void wheelEvent(QWheelEvent* event) override {
    if (m_owner != nullptr) {
      QPoint delta = event->angleDelta();
      if (delta.isNull()) {
        delta = event->pixelDelta();
      }
      const QPointF pos = event->position();
      m_owner->emit_mouse_wheel(static_cast<float>(delta.x()) / 120.0f,
                                static_cast<float>(delta.y()) / 120.0f,
                                static_cast<int>(pos.x()),
                                static_cast<int>(pos.y()));
    }
    event->accept();
  }

 private:
  void dropUnpresentedFrame() {
    if (!m_frame.isNull() && !m_framePresented && m_frameId != 0u && m_owner != nullptr) {
      m_owner->record_frame_dropped(m_frameId);
    }
    m_framePresented = true;
  }

  QtWindow* m_owner = nullptr;
  QImage m_frame;
  std::uint64_t m_frameId = 0u;
  std::size_t m_frameWidth = 0u;
  std::size_t m_frameHeight = 0u;
  bool m_framePresented = true;
  QString m_overlayText;
  std::vector<QtSelectionOverlayBox> m_overlayBoxes;
  bool m_dirty = false;
};

namespace {

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

void QtWindow::PendingFramebuffer::reset() {
  id = 0u;
  width = 0u;
  height = 0u;
  rgba.clear();
}

QtWindow::~QtWindow() {
  destroy();
}

bool QtWindow::initialize(std::size_t width, std::size_t height, std::string_view title) {
  if (width == 0u || height == 0u) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        kQtLogSubsystem,
        "refusing zero-sized Qt window",
        {{"width", std::to_string(width)}, {"height", std::to_string(height)}});
    return false;
  }

  if (QCoreApplication::instance() == nullptr) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error,
        kQtLogSubsystem,
        "cannot initialize Qt window without QApplication");
    return false;
  }

  if (m_widget == nullptr) {
    m_widget = new QtViewportWindow(this);
  } else {
    static_cast<QtViewportWindow*>(m_widget)->setOwner(this);
  }
  static_cast<QtViewportWindow*>(m_widget)->clearFrameOnUiThread();

  {
    std::scoped_lock lock(m_frameMutex);
    m_framebufferStats = {};
    m_nextFramebufferId = 1u;
    m_frameUpdateQueued = false;
    m_pendingFramebufferKind = PendingFramebufferKind::None;
    m_pendingFramebuffer.reset();
    m_frameAccepting = true;
  }

  m_events.clear();
  m_closeEventQueued = false;
  m_focused = false;
  m_title.assign(title);
  m_lastMouseX = 0;
  m_lastMouseY = 0;

  m_widget->resize(ClampToQtInt(width), ClampToQtInt(height));
  m_widget->setWindowTitle(ToQString(m_title));
  static_cast<QtViewportWindow*>(m_widget)->setOverlayText(ToQString(m_overlayText));
  m_widget->show();
  m_widget->raise();
  m_widget->activateWindow();
  m_widget->setFocus(Qt::OtherFocusReason);

  m_open = true;
  update_metrics_from_widget();
  queue_event(InputEventNormalizer::resize(static_cast<std::uint32_t>(std::max(0, m_metrics.width)),
                                           static_cast<std::uint32_t>(std::max(0, m_metrics.height))));
  emit_focus_change(true);

  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      kQtLogSubsystem,
      "Qt window initialized",
      {
        {"width", std::to_string(m_metrics.width)},
        {"height", std::to_string(m_metrics.height)},
        {"dpi_scale", std::to_string(m_metrics.dpiScale)},
        {"native_handle", native_handle() != nullptr ? "available" : "null"},
        {"viewport_native_handle_valid", native_handle() != nullptr ? "true" : "false"},
        {"viewport_device_pixel_ratio", std::to_string(m_metrics.dpiScale)}
      });
  return true;
}

bool QtWindow::is_open() const {
  return m_open;
}

void QtWindow::close() {
  if (!m_open && m_widget == nullptr) {
    return;
  }

  emit_close_requested();
  m_open = false;
  {
    std::scoped_lock lock(m_frameMutex);
    clear_frame_handoff_locked(false);
  }
  if (m_widget != nullptr) {
    m_widget->hide();
  }
}

WindowMetrics QtWindow::metrics() const {
  return m_metrics;
}

bool QtWindow::poll_events() {
  if (QCoreApplication::instance() != nullptr) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
  }

  update_metrics_from_widget();
  if (m_widget != nullptr && !m_widget->isVisible() && m_open) {
    mark_closed();
  }
  return m_open;
}

bool QtWindow::resize(std::size_t width, std::size_t height) {
  if (!m_open || m_widget == nullptr || width == 0u || height == 0u) {
    return false;
  }
  m_widget->resize(ClampToQtInt(width), ClampToQtInt(height));
  update_metrics_from_widget();
  return true;
}

void QtWindow::set_title(std::string_view title) {
  m_title.assign(title);
  if (m_widget != nullptr) {
    m_widget->setWindowTitle(ToQString(title));
  }
}

void QtWindow::set_overlay_text(std::string_view text) {
  m_overlayText.assign(text);
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setOverlayText(ToQString(text));
  }
}

void QtWindow::set_selection_overlay_boxes(const std::vector<QtSelectionOverlayBox>& boxes) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setSelectionOverlayBoxes(boxes);
  }
}

void QtWindow::set_framebuffer_rgba(const std::vector<std::uint8_t>& rgba,
                                    std::size_t width,
                                    std::size_t height) {
  // Render thread rule: callers may publish RGBA8 display frames here, but this
  // path only updates the handoff slot and posts a coalesced Qt-thread drain.
  {
    std::scoped_lock lock(m_frameMutex);
    if (!m_frameAccepting || m_widget == nullptr) {
      return;
    }
  }

  std::size_t expected = 0u;
  if (!CheckedRgbaByteCount(width, height, expected) ||
      !FitsQtImageDimensions(width, height)) {
    if (request_framebuffer_clear()) {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          kQtLogSubsystem,
          "cleared invalid Qt framebuffer dimensions",
          {{"width", std::to_string(width)}, {"height", std::to_string(height)}});
    }
    return;
  }
  if (rgba.size() < expected) {
    if (request_framebuffer_clear()) {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          kQtLogSubsystem,
          "cleared short Qt framebuffer upload",
          {
            {"width", std::to_string(width)},
            {"height", std::to_string(height)},
            {"expected_bytes", std::to_string(expected)},
            {"actual_bytes", std::to_string(rgba.size())}
          });
    }
    return;
  }

  std::vector<std::uint8_t> frame(expected);
  const std::size_t rowBytes = width * 4u;
  for (std::size_t y = 0u; y < height; ++y) {
    const auto* srcRow = rgba.data() + y * rowBytes;
    auto* dstRow = frame.data() + y * rowBytes;
    std::memcpy(dstRow, srcRow, rowBytes);
  }

  QtFramebufferStats stats{};
  std::uint64_t frameId = 0u;
  bool accepted = false;
  bool firstReceived = false;
  bool periodicStats = false;
  bool queueFailed = false;

  {
    std::scoped_lock lock(m_frameMutex);
    if (!m_frameAccepting || m_widget == nullptr) {
      return;
    }

    if (m_pendingFramebufferKind == PendingFramebufferKind::Frame) {
      ++m_framebufferStats.dropped;
      periodicStats = ShouldLogQtFramebufferStats(m_framebufferStats.dropped);
    }

    frameId = m_nextFramebufferId++;
    m_pendingFramebuffer.id = frameId;
    m_pendingFramebuffer.width = width;
    m_pendingFramebuffer.height = height;
    m_pendingFramebuffer.rgba = std::move(frame);
    m_pendingFramebufferKind = PendingFramebufferKind::Frame;

    ++m_framebufferStats.received;
    m_framebufferStats.latestPublishedId = frameId;
    m_framebufferStats.latestPublishedWidth = width;
    m_framebufferStats.latestPublishedHeight = height;

    firstReceived = m_framebufferStats.received == 1u;
    periodicStats = periodicStats || ShouldLogQtFramebufferStats(m_framebufferStats.received);
    queueFailed = !enqueue_frame_update_locked(m_widget);
    stats = m_framebufferStats;
    accepted = true;
  }

  if (!accepted) {
    return;
  }

  if (firstReceived) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "traceprobe",
        "first Qt display frame received",
        {
          {"frame_id", std::to_string(frameId)},
          {"width", std::to_string(width)},
          {"height", std::to_string(height)},
          {"bytes", std::to_string(expected)}
        });
  }

  if (periodicStats) {
    LogQtFramebufferStats("Qt display frame handoff counters", stats);
  }

  if (queueFailed) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        kQtLogSubsystem,
        "Qt framebuffer UI update queue failed",
        {{"frame_id", std::to_string(frameId)}});
  }
}

void QtWindow::clear_framebuffer() {
  (void)request_framebuffer_clear();
}

QtFramebufferStats QtWindow::framebuffer_stats() const {
  std::scoped_lock lock(m_frameMutex);
  return m_framebufferStats;
}

bool QtWindow::request_framebuffer_clear() {
  QtFramebufferStats stats{};
  bool periodicStats = false;
  bool queueFailed = false;

  {
    std::scoped_lock lock(m_frameMutex);
    if (!m_frameAccepting || m_widget == nullptr) {
      return false;
    }

    if (m_pendingFramebufferKind == PendingFramebufferKind::Frame) {
      ++m_framebufferStats.dropped;
      periodicStats = ShouldLogQtFramebufferStats(m_framebufferStats.dropped);
    }

    m_pendingFramebuffer.reset();
    m_pendingFramebufferKind = PendingFramebufferKind::Clear;
    queueFailed = !enqueue_frame_update_locked(m_widget);
    stats = m_framebufferStats;
  }

  if (periodicStats) {
    LogQtFramebufferStats("Qt display frame handoff counters", stats);
  }

  if (queueFailed) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        kQtLogSubsystem,
        "Qt framebuffer clear update queue failed");
  }

  return true;
}

bool QtWindow::enqueue_frame_update_locked(QWidget* widget) {
  if (m_frameUpdateQueued) {
    return true;
  }
  if (widget == nullptr) {
    return false;
  }

  m_frameUpdateQueued = true;
  const bool queued = QMetaObject::invokeMethod(
      widget,
      [this, widget]() {
        deliver_pending_frame_to_widget(widget);
      },
      Qt::QueuedConnection);
  if (!queued) {
    m_frameUpdateQueued = false;
  }
  return queued;
}

void QtWindow::deliver_pending_frame_to_widget(QWidget* widget) {
  PendingFramebufferKind kind = PendingFramebufferKind::None;
  PendingFramebuffer frame;

  {
    std::scoped_lock lock(m_frameMutex);
    m_frameUpdateQueued = false;
    if (!m_frameAccepting || widget != m_widget) {
      return;
    }

    kind = m_pendingFramebufferKind;
    if (kind == PendingFramebufferKind::Frame) {
      frame = std::move(m_pendingFramebuffer);
    }
    m_pendingFramebufferKind = PendingFramebufferKind::None;
    m_pendingFramebuffer.reset();
  }

  auto* viewport = static_cast<QtViewportWindow*>(widget);
  if (kind == PendingFramebufferKind::Clear) {
    viewport->clearFrameOnUiThread();
    return;
  }
  if (kind != PendingFramebufferKind::Frame) {
    return;
  }

  if (!viewport->deliverFrameOnUiThread(frame.rgba.data(),
                                        frame.rgba.size(),
                                        frame.width,
                                        frame.height,
                                        frame.id)) {
    record_frame_dropped(frame.id);
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        kQtLogSubsystem,
        "Qt display frame present failed",
        {
          {"frame_id", std::to_string(frame.id)},
          {"width", std::to_string(frame.width)},
          {"height", std::to_string(frame.height)}
        });
  }
}

void QtWindow::record_frame_presented(std::uint64_t frameId,
                                      std::size_t width,
                                      std::size_t height) {
  QtFramebufferStats stats{};
  bool firstPresented = false;
  bool periodicStats = false;

  {
    std::scoped_lock lock(m_frameMutex);
    ++m_framebufferStats.presented;
    m_framebufferStats.latestPresentedId = frameId;
    m_framebufferStats.latestPresentedWidth = width;
    m_framebufferStats.latestPresentedHeight = height;
    firstPresented = m_framebufferStats.presented == 1u;
    periodicStats = ShouldLogQtFramebufferStats(m_framebufferStats.presented);
    stats = m_framebufferStats;
  }

  if (firstPresented) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        "traceprobe",
        "first Qt display frame presented",
        {
          {"frame_id", std::to_string(frameId)},
          {"width", std::to_string(width)},
          {"height", std::to_string(height)}
        });
  }

  if (periodicStats) {
    LogQtFramebufferStats("Qt display frame handoff counters", stats);
  }
}

void QtWindow::record_frame_dropped(std::uint64_t frameId) {
  QtFramebufferStats stats{};
  bool periodicStats = false;

  {
    std::scoped_lock lock(m_frameMutex);
    ++m_framebufferStats.dropped;
    periodicStats = ShouldLogQtFramebufferStats(m_framebufferStats.dropped);
    stats = m_framebufferStats;
  }

  if (periodicStats) {
    LogQtFramebufferStats("Qt display frame handoff counters", stats);
  }
  (void)frameId;
}

void QtWindow::clear_frame_handoff_locked(bool accepting) {
  if (m_pendingFramebufferKind == PendingFramebufferKind::Frame) {
    ++m_framebufferStats.dropped;
  }
  m_pendingFramebuffer.reset();
  m_pendingFramebufferKind = PendingFramebufferKind::None;
  m_frameUpdateQueued = false;
  m_frameAccepting = accepting;
}

void* QtWindow::native_handle() const {
  if (m_widget == nullptr) {
    return nullptr;
  }
  return WindowIdToHandle(m_widget->winId());
}

void QtWindow::on_native_resize(std::size_t width, std::size_t height) {
  const int oldWidth = m_metrics.width;
  const int oldHeight = m_metrics.height;
  const float oldDpi = m_metrics.dpiScale;
  update_metrics_from_widget();

  const bool changed = oldWidth != m_metrics.width ||
                       oldHeight != m_metrics.height ||
                       oldDpi != m_metrics.dpiScale;
  if (changed) {
    queue_event(InputEventNormalizer::resize(static_cast<std::uint32_t>(width),
                                             static_cast<std::uint32_t>(height)));
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        kQtLogSubsystem,
        "Qt window metrics changed",
        {
          {"width", std::to_string(m_metrics.width)},
          {"height", std::to_string(m_metrics.height)},
          {"dpi_scale", std::to_string(m_metrics.dpiScale)}
        });
  }
}

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

void QtWindow::emit_mouse_button(std::int32_t button, bool pressed, int x, int y) {
  queue_event(InputEventNormalizer::mouse_button(button,
                                                 pressed,
                                                 static_cast<float>(x),
                                                 static_cast<float>(y)));
}

void QtWindow::emit_mouse_wheel(float delta_x, float delta_y, int x, int y) {
  InputEvent event = InputEventNormalizer::mouse_wheel(
      delta_y != 0.0f ? delta_y : delta_x,
      static_cast<float>(x),
      static_cast<float>(y));
  event.delta_x = delta_x;
  event.delta_y = delta_y;
  queue_event(event);
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

void QtWindow::mark_closed() {
  m_open = false;
  std::scoped_lock lock(m_frameMutex);
  clear_frame_handoff_locked(false);
}

void QtWindow::destroy() {
  m_open = false;
  m_closeEventQueued = false;
  m_events.clear();
  QWidget* widget = nullptr;
  {
    std::scoped_lock lock(m_frameMutex);
    clear_frame_handoff_locked(false);
    widget = m_widget;
    m_widget = nullptr;
  }
  if (widget != nullptr) {
    auto* viewport = static_cast<QtViewportWindow*>(widget);
    viewport->clearFrameOnUiThread();
    viewport->setOwner(nullptr);
    viewport->hide();
    delete viewport;
  }
}

void QtWindow::queue_event(InputEvent event) {
  m_events.push_back(event);
}

void QtWindow::update_metrics_from_widget() {
  if (m_widget == nullptr) {
    return;
  }

  m_metrics.width = std::max(0, m_widget->width());
  m_metrics.height = std::max(0, m_widget->height());

  qreal dpiScale = m_widget->devicePixelRatioF();
  if (const QWindow* window = m_widget->windowHandle()) {
    if (const QScreen* screen = window->screen()) {
      dpiScale = screen->devicePixelRatio();
    }
  }
  if (dpiScale <= 0.0) {
    dpiScale = 1.0;
  }
  m_metrics.dpiScale = static_cast<float>(dpiScale);
}

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

QtPlatform::QtPlatform(std::string_view name) : m_name(name) {}

vkpt::core::Result<void> QtPlatform::initialize() {
  if (m_initialized) {
    return vkpt::core::Result<void>::ok();
  }

  InstallQtDiagnostics();

  auto& runtime = AppRuntime();
  if (QCoreApplication::instance() == nullptr) {
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
