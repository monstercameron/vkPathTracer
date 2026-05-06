#include "platform/qt/QtPlatform.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "core/Logging.h"
#include "platform/qt/QtPlatformDockTree.h"
#include "platform/qt/QtPlatformStyle.h"
#include <QAbstractItemView>
#include <QAbstractAnimation>
#include <QApplication>
#include <QByteArray>
#include <QBrush>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFocusEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QAction>
#include <QDockWidget>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QPoint>
#include <QPointF>
#include <QPen>
#include <QPointer>
#include <QPixmap>
#include <QPolygonF>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRect>
#include <QRectF>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScreen>
#include <QSettings>
#include <QSizePolicy>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QString>
#include <QStringList>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QtGlobal>
#include <QVariant>
#include <QVBoxLayout>
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
constexpr int kQtDockLayoutStateVersion = 2;
constexpr int kQtDockMinimumWidth = 168;
constexpr int kQtDockMinimumHeight = 96;
constexpr int kQtDockMaximumInitialWidth = 1400;
constexpr int kQtSliderEditDebounceMs = 120;
constexpr const char* kQtPendingSliderEditProperty = "vkpt.pending_slider_edit";

int ClampToQtInt(std::size_t value) {
  constexpr auto kMax = static_cast<std::size_t>(std::numeric_limits<int>::max());
  return static_cast<int>(std::min(value, kMax));
}

int ClampDockInitialWidth(int value) {
  if (value <= 0) {
    return 0;
  }
  return std::clamp(value, kQtDockMinimumWidth, kQtDockMaximumInitialWidth);
}

QString ToQString(std::string_view text) {
  const auto size = std::min<std::size_t>(
      text.size(),
      static_cast<std::size_t>(std::numeric_limits<int>::max()));
  return QString::fromUtf8(text.data(), static_cast<int>(size));
}

QCursor CursorForViewportCursor(QtViewportCursor cursor) {
  switch (cursor) {
    case QtViewportCursor::Translate:
      return QCursor(Qt::SizeAllCursor);
    case QtViewportCursor::Scale:
      return QCursor(Qt::SizeFDiagCursor);
    case QtViewportCursor::Rotate: {
      static const QCursor rotateCursor = [] {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF arcRect(4.5, 4.5, 15.0, 15.0);
        painter.setPen(QPen(QColor(0, 0, 0, 210), 4.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arcRect, 35 * 16, 285 * 16);
        painter.setPen(QPen(QColor(255, 255, 255, 235), 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arcRect, 35 * 16, 285 * 16);
        QPolygonF shadowHead;
        shadowHead << QPointF(18.0, 2.5) << QPointF(22.0, 7.5) << QPointF(15.5, 8.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 210));
        painter.drawPolygon(shadowHead);
        QPolygonF head;
        head << QPointF(18.0, 3.5) << QPointF(21.0, 7.0) << QPointF(16.5, 7.5);
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.drawPolygon(head);
        return QCursor(pixmap, 12, 12);
      }();
      return rotateCursor;
    }
    case QtViewportCursor::Default:
    default:
      return QCursor(Qt::ArrowCursor);
  }
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

void* WindowIdToHandle(WId id) {
#if defined(_WIN32) || defined(Q_OS_MACOS)
  return reinterpret_cast<void*>(id);
#else
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
#endif
}

}  // namespace

#include "QtPlatformMainWindowPanels.inc"
#include "QtPlatformMainWindowWidgets.inc"
#include "QtPlatformViewportWindow.inc"
#include "QtPlatformWindowMethods.inc"
