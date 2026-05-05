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

#include <QAbstractItemView>
#include <QAbstractAnimation>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFocusEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QAction>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
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
#include <QResizeEvent>
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

QPalette StartupDarkPalette() {
  QPalette palette;
  palette.setColor(QPalette::Window, QColor(6, 8, 12));
  palette.setColor(QPalette::WindowText, QColor(232, 237, 245));
  palette.setColor(QPalette::Base, QColor(10, 15, 21));
  palette.setColor(QPalette::AlternateBase, QColor(16, 24, 32));
  palette.setColor(QPalette::ToolTipBase, QColor(20, 30, 40));
  palette.setColor(QPalette::ToolTipText, QColor(245, 248, 252));
  palette.setColor(QPalette::Text, QColor(232, 237, 245));
  palette.setColor(QPalette::Button, QColor(18, 27, 38));
  palette.setColor(QPalette::ButtonText, QColor(242, 246, 252));
  palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
  palette.setColor(QPalette::Highlight, QColor(54, 112, 166));
  palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
  return palette;
}

QString StartupDarkStyleSheet() {
  return QStringLiteral(
      "QMainWindow, QWidget#vkpt.qt.viewport { background-color: #06080c; color: #e8edf5; }"
      "QDockWidget { background-color: #0b1118; color: #e8edf5; }"
      "QDockWidget::title { background-color: #101820; padding: 4px; }"
      "QStatusBar, QMenuBar { background-color: #0b1118; color: #e8edf5; }"
      "QMenu { background-color: #0b1118; color: #e8edf5; border: 1px solid #263241; }"
      "QMenu::item:selected { background-color: #263b52; }"
      "QTableWidget, QTreeWidget, QTextEdit { background-color: #0a0f15; alternate-background-color: #101820; color: #e8edf5; gridline-color: #263241; }"
      "QHeaderView::section { background-color: #101820; color: #e8edf5; border: 1px solid #263241; padding: 3px; }"
      "QPushButton { background-color: #172331; color: #f3f6fb; border: 1px solid #344457; padding: 3px 8px; border-radius: 3px; }"
      "QPushButton:hover { background-color: #213249; }"
      "QComboBox, QDoubleSpinBox { background-color: #0d141d; color: #f3f6fb; border: 1px solid #344457; padding: 2px; }");
}

void ApplyStartupSurface(QWidget* widget) {
  if (widget == nullptr) {
    return;
  }
  widget->setAutoFillBackground(true);
  widget->setPalette(StartupDarkPalette());
  widget->setAttribute(Qt::WA_StyledBackground, true);
}

}  // namespace

class QtStartupSplash final : public QWidget {
 public:
  explicit QtStartupSplash()
      : QWidget(nullptr,
                Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setObjectName(QStringLiteral("vkpt.qt.startup_splash"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    resize(620, 340);

    m_messages = QStringList{
        QStringLiteral("Preparing renderer state"),
        QStringLiteral("Loading scene assets"),
        QStringLiteral("Building acceleration data"),
        QStringLiteral("Compiling display pipeline"),
        QStringLiteral("Resolving the first preview frame"),
        QStringLiteral("Polishing the viewport handoff"),
    };

    auto* timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this]() {
      ++m_tick;
      if (m_tick % 26u == 0u && !m_messages.isEmpty()) {
        m_messageIndex = (m_messageIndex + 1) % m_messages.size();
      }
      update();
    });
    timer->start(32);
    m_animationTimer = timer;
  }

  void setPhase(QString phase) {
    if (phase.isEmpty() || m_phase == phase) {
      return;
    }
    m_phase = std::move(phase);
    m_tick = 0u;
    update();
  }

  void showCentered(QWidget* reference) {
    QRect anchorRect;
    if (reference != nullptr && reference->screen() != nullptr) {
      anchorRect = reference->screen()->availableGeometry();
    } else if (QScreen* screen = QGuiApplication::primaryScreen()) {
      anchorRect = screen->availableGeometry();
    } else {
      anchorRect = QRect(0, 0, 1280, 720);
    }

    move(anchorRect.center() - rect().center());
    setWindowOpacity(1.0);
    show();
    raise();
    activateWindow();
  }

  void finish(bool animated) {
    if (m_finishing) {
      return;
    }
    m_finishing = true;
    if (m_animationTimer != nullptr) {
      m_animationTimer->stop();
    }

    if (!animated) {
      hide();
      deleteLater();
      return;
    }

    auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(220);
    fade->setStartValue(windowOpacity());
    fade->setEndValue(0.0);
    QObject::connect(fade, &QPropertyAnimation::finished, this, [this]() {
      hide();
      deleteLater();
    });
    fade->start(QAbstractAnimation::DeleteWhenStopped);
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor(5, 7, 11));

    const QRectF panel = QRectF(rect()).adjusted(26.0, 24.0, -26.0, -24.0);
    painter.setPen(QPen(QColor(37, 50, 66), 1.0));
    painter.setBrush(QColor(8, 12, 18));
    painter.drawRoundedRect(panel, 8.0, 8.0);

    const QRectF accent(panel.left(), panel.top(), panel.width(), 3.0);
    painter.fillRect(accent, QColor(76, 148, 210));

    QFont titleFont = painter.font();
    titleFont.setPointSize(22);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(244, 248, 252));
    painter.drawText(panel.adjusted(34.0, 34.0, -34.0, -238.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("vkPathTracer"));

    QFont subtitleFont = painter.font();
    subtitleFont.setPointSize(10);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(QColor(164, 178, 194));
    painter.drawText(panel.adjusted(36.0, 80.0, -36.0, -206.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Starting the renderer"));

    const QPointF spinnerCenter(panel.left() + 56.0, panel.top() + 168.0);
    for (int i = 0; i < 12; ++i) {
      const int active = static_cast<int>((m_tick / 2u + static_cast<unsigned>(i)) % 12u);
      const int alpha = 48 + active * 15;
      const double angle = (static_cast<double>(i) / 12.0) * 6.283185307179586;
      const double inner = 15.0;
      const double outer = 24.0;
      QPen pen(QColor(86, 164, 220, std::min(230, alpha)), 3.0);
      pen.setCapStyle(Qt::RoundCap);
      painter.setPen(pen);
      painter.drawLine(QPointF(spinnerCenter.x() + std::cos(angle) * inner,
                               spinnerCenter.y() + std::sin(angle) * inner),
                       QPointF(spinnerCenter.x() + std::cos(angle) * outer,
                               spinnerCenter.y() + std::sin(angle) * outer));
    }

    QFont phaseFont = painter.font();
    phaseFont.setPointSize(13);
    phaseFont.setBold(true);
    painter.setFont(phaseFont);
    painter.setPen(QColor(232, 238, 246));
    const QString phase = m_phase.isEmpty() ? QStringLiteral("Preparing Qt shell") : m_phase;
    painter.drawText(panel.adjusted(96.0, 134.0, -40.0, -158.0),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                     phase);

    QFont messageFont = painter.font();
    messageFont.setPointSize(10);
    messageFont.setBold(false);
    painter.setFont(messageFont);
    painter.setPen(QColor(150, 166, 184));
    painter.drawText(panel.adjusted(96.0, 174.0, -40.0, -120.0),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                     currentLoadingMessage());

    const QRectF bar(panel.left() + 36.0, panel.bottom() - 58.0, panel.width() - 72.0, 8.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(24, 34, 45));
    painter.drawRoundedRect(bar, 4.0, 4.0);
    const double progress = static_cast<double>(m_tick % 96u) / 96.0;
    QRectF fill = bar;
    fill.setWidth(std::max(18.0, bar.width() * (0.18 + progress * 0.82)));
    painter.setBrush(QColor(70, 146, 208));
    painter.drawRoundedRect(fill, 4.0, 4.0);

    painter.setPen(QColor(106, 122, 140));
    painter.drawText(panel.adjusted(36.0, 246.0, -36.0, -28.0),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("The main viewport will appear after the first resolved frame is ready."));
  }

 private:
  QString currentLoadingMessage() const {
    if (m_messages.isEmpty()) {
      return QStringLiteral("Preparing display handoff");
    }
    const int messageCount = static_cast<int>(m_messages.size());
    const int index = std::clamp(m_messageIndex, 0, messageCount - 1);
    return m_messages.at(index);
  }

  QString m_phase;
  QStringList m_messages;
  int m_messageIndex = 0;
  unsigned m_tick = 0u;
  bool m_finishing = false;
  QTimer* m_animationTimer = nullptr;
};

class QtMainWindow final : public QMainWindow {
 public:
  explicit QtMainWindow(QtWindow* owner) : QMainWindow(nullptr), m_owner(owner) {
    setObjectName(QStringLiteral("vkpt.qt.main_window"));
    setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    setAnimated(false);
    ApplyStartupSurface(this);
    setStyleSheet(StartupDarkStyleSheet());
    statusBar()->setObjectName(QStringLiteral("vkpt.qt.status_bar"));
  }

  void setOwner(QtWindow* owner) {
    m_owner = owner;
  }

  void setDockPanels(const std::vector<QtDockPanel>& panels) {
    const bool previousUpdatesEnabled = updatesEnabled();
    setUpdatesEnabled(false);
    std::unordered_set<std::string> seen;
    for (const auto& panel : panels) {
      if (panel.id.empty()) {
        continue;
      }
      seen.insert(panel.id);
      auto* dock = ensureDock(panel);
      dock->setWindowTitle(ToQString(panel.title.empty() ? panel.id : panel.title));
      dock->setEnabled(panel.enabled);
      dock->setFeatures(dockFeatures(panel));
      const std::string signature = panelContentSignature(panel);
      const auto existingSignature = m_panelContentSignatures.find(panel.id);
      if (existingSignature == m_panelContentSignatures.end() ||
          existingSignature->second != signature ||
          dock->widget() == nullptr) {
        QPointer<QWidget> oldWidget = dock->widget();
        dock->setWidget(buildDockWidget(panel));
        m_panelContentSignatures[panel.id] = signature;
        if (!oldWidget.isNull()) {
          oldWidget->deleteLater();
        }
      } else {
        updateDockWidgetValues(dock->widget(), panel);
      }
      dock->setVisible(panel.visible);
    }

    for (auto it = m_docks.begin(); it != m_docks.end();) {
      if (seen.find(it->first) == seen.end()) {
        removeDockWidget(it->second);
        it->second->deleteLater();
        m_panelContentSignatures.erase(it->first);
        it = m_docks.erase(it);
      } else {
        ++it;
      }
    }

    if (!m_layoutRestored && !m_docks.empty()) {
      const bool restoredLayout = restoreDockLayout();
      if (!restoredLayout) {
        tabifyDefaultDockGroups();
        applyInitialSideDockWidths(panels);
      }
      m_layoutRestored = true;
    }
    setUpdatesEnabled(previousUpdatesEnabled);
    update();
  }

  void setStatusText(QString text) {
    QtStatusBarText status;
    status.message = ToUtf8String(text);
    setStatusText(status);
  }

  void setStatusText(const QtStatusBarText& status) {
    auto* bar = statusBar();
    if (status.message.empty()) {
      bar->clearMessage();
    } else if (status.timeout_ms > 0) {
      bar->showMessage(ToQString(status.message), status.timeout_ms);
    } else {
      bar->showMessage(ToQString(status.message));
    }

    for (auto& entry : m_statusFields) {
      bar->removeWidget(entry.second);
      delete entry.second;
    }
    m_statusFields.clear();

    for (const auto& field : status.fields) {
      if (field.id.empty() && field.text.empty()) {
        continue;
      }
      const std::string id = field.id.empty() ? field.text : field.id;
      auto* label = new QLabel(ToQString(field.text), bar);
      label->setObjectName(ToQString(std::string("vkpt.status.") + id));
      label->setContentsMargins(8, 0, 8, 0);
      bar->addPermanentWidget(label, std::max(0, field.stretch));
      m_statusFields.emplace(id, label);
    }
  }

  void saveDockLayout() {
    QSettings settings(QStringLiteral("vkPathTracer"), QStringLiteral("QtShell"));
    settings.setValue(QStringLiteral("main_window_geometry"), saveGeometry());
    settings.setValue(QStringLiteral("main_window_state"), saveState(kQtDockLayoutStateVersion));
  }

  bool restoreDockLayout() {
    QSettings settings(QStringLiteral("vkPathTracer"), QStringLiteral("QtShell"));
    const auto geometry = settings.value(QStringLiteral("main_window_geometry")).toByteArray();
    if (!geometry.isEmpty()) {
      restoreGeometry(geometry);
    }
    const auto state = settings.value(QStringLiteral("main_window_state")).toByteArray();
    if (!state.isEmpty()) {
      return restoreState(state, kQtDockLayoutStateVersion);
    }
    return false;
  }

 protected:
  void closeEvent(QCloseEvent* event) override {
    saveDockLayout();
    if (m_owner != nullptr) {
      m_owner->emit_close_requested();
      m_owner->mark_closed();
    }
    event->accept();
    QMainWindow::closeEvent(event);
  }

 private:
  static Qt::DockWidgetArea toQtArea(QtDockArea area) {
    switch (area) {
      case QtDockArea::Left:
        return Qt::LeftDockWidgetArea;
      case QtDockArea::Bottom:
        return Qt::BottomDockWidgetArea;
      case QtDockArea::Top:
        return Qt::TopDockWidgetArea;
      case QtDockArea::Right:
      default:
        return Qt::RightDockWidgetArea;
    }
  }

  QDockWidget* ensureDock(const QtDockPanel& panel) {
    if (auto existing = m_docks.find(panel.id); existing != m_docks.end()) {
      return existing->second;
    }
    auto* dock = new QDockWidget(ToQString(panel.title.empty() ? panel.id : panel.title), this);
    ApplyStartupSurface(dock);
    dock->setObjectName(ToQString(std::string("vkpt.dock.") + panel.id));
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setMinimumWidth(kQtDockMinimumWidth);
    dock->setMinimumHeight(kQtDockMinimumHeight);
    dock->setMaximumWidth(QWIDGETSIZE_MAX);
    dock->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    addDockWidget(toQtArea(panel.area), dock);
    m_docks.emplace(panel.id, dock);
    return dock;
  }

  static QDockWidget::DockWidgetFeatures dockFeatures(const QtDockPanel& panel) {
    QDockWidget::DockWidgetFeatures features = QDockWidget::NoDockWidgetFeatures;
    if (panel.closable) {
      features |= QDockWidget::DockWidgetClosable;
    }
    if (panel.movable) {
      features |= QDockWidget::DockWidgetMovable;
    }
    if (panel.floatable) {
      features |= QDockWidget::DockWidgetFloatable;
    }
    return features;
  }

  static void appendSignatureField(std::ostringstream& out, std::string_view value) {
    out << value.size() << ':' << value << ';';
  }

  static void appendRowSignature(std::ostringstream& out, const QtDockRow& row) {
    appendSignatureField(out, row.id);
    appendSignatureField(out, row.label);
    appendSignatureField(out, row.value);
    appendSignatureField(out, row.icon);
    out << row.entity_id << ','
        << (row.selected ? '1' : '0') << ';';
    out << row.children.size() << '[';
    for (const auto& child : row.children) {
      appendRowSignature(out, child);
    }
    out << ']';
  }

  static std::string panelContentSignature(const QtDockPanel& panel) {
    std::ostringstream out;
    out << static_cast<int>(panel.content) << '|';
    appendSignatureField(out, panel.text);
    out << panel.rows.size() << '|';
    for (const auto& row : panel.rows) {
      appendSignatureField(out, row);
    }
    out << panel.tree_rows.size() << '|';
    for (const auto& row : panel.tree_rows) {
      appendRowSignature(out, row);
    }
    out << panel.properties.size() << '|';
    for (const auto& property : panel.properties) {
      appendSignatureField(out, property.id);
      appendSignatureField(out, property.group);
      appendSignatureField(out, property.name);
      appendSignatureField(out, std::string_view{});
      appendSignatureField(out, property.unit);
      appendSignatureField(out, property.editor);
      out << property.options.size() << '[';
      for (const auto& option : property.options) {
        appendSignatureField(out, option);
      }
      out << ']';
      out << property.minimum << ',' << property.maximum << ','
          << property.step << ',' << property.default_value << ','
          << (property.has_numeric_range ? '1' : '0')
          << (property.has_default ? '1' : '0');
      out << (property.editable ? '1' : '0') << (property.enabled ? '1' : '0') << ';';
    }
    return out.str();
  }

  QDockWidget* dockById(std::string_view id) const {
    const auto it = m_docks.find(std::string(id));
    return it == m_docks.end() ? nullptr : it->second;
  }

  void tabifyDockIds(std::initializer_list<std::string_view> ids) {
    QDockWidget* base = nullptr;
    for (const auto id : ids) {
      auto* dock = dockById(id);
      if (dock == nullptr) {
        continue;
      }
      if (base == nullptr) {
        base = dock;
      } else {
        tabifyDockWidget(base, dock);
      }
    }
    if (base != nullptr) {
      base->raise();
    }
  }

  void tabifyDefaultDockGroups() {
    tabifyDockIds({"scene_graph", "asset_browser"});
    tabifyDockIds({"inspector",
                   "materials",
                   "lights",
                   "camera",
                   "render_settings",
                   "benchmark_panel",
                   "debug_views",
                   "script_panel",
                   "physics"});
    tabifyDockIds({"diagnostics", "performance", "timeline"});
  }

  void applyInitialSideDockWidths(const std::vector<QtDockPanel>& panels) {
    applyInitialSideDockWidth(panels, QtDockArea::Left);
    applyInitialSideDockWidth(panels, QtDockArea::Right);
  }

  void applyInitialSideDockWidth(const std::vector<QtDockPanel>& panels, QtDockArea area) {
    QDockWidget* target = nullptr;
    int width = 0;
    for (const auto& panel : panels) {
      if (!panel.visible || panel.area != area) {
        continue;
      }
      target = dockById(panel.id);
      width = ClampDockInitialWidth(panel.preferred_width);
      if (target != nullptr && width > 0) {
        break;
      }
    }
    if (target == nullptr || width <= 0) {
      return;
    }

    QList<QDockWidget*> docks;
    QList<int> sizes;
    docks.push_back(target);
    sizes.push_back(width);
    resizeDocks(docks, sizes, Qt::Horizontal);
  }

  static QIcon dockRowIcon(const std::string& icon) {
    if (icon.empty()) {
      return {};
    }

    QColor fill(82, 100, 126);
    char glyph = 'E';
    if (icon == "model" || icon == "mesh" || icon == "geometry") {
      fill = QColor(56, 124, 184);
      glyph = 'M';
    } else if (icon == "light") {
      fill = QColor(201, 142, 41);
      glyph = 'L';
    } else if (icon == "camera") {
      fill = QColor(93, 152, 97);
      glyph = 'C';
    } else if (icon == "sdf") {
      fill = QColor(145, 97, 188);
      glyph = 'S';
    } else if (icon == "physics") {
      fill = QColor(181, 86, 70);
      glyph = 'P';
    } else if (icon == "script") {
      fill = QColor(88, 151, 167);
      glyph = 'S';
    } else if (icon == "animation") {
      fill = QColor(168, 114, 61);
      glyph = 'A';
    } else if (icon == "folder" || icon == "group") {
      fill = QColor(111, 122, 139);
      glyph = 'G';
    }

    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 0, 0, 100), 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(1.0, 1.0, 14.0, 14.0), 3.0, 3.0);
    QFont font = painter.font();
    font.setPixelSize(9);
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(QColor(255, 255, 255, 235));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QString::fromLatin1(&glyph, 1));
    return QIcon(pixmap);
  }

  static QTreeWidgetItem* buildTreeItem(const QtDockRow& row) {
    auto* item = new QTreeWidgetItem();
    item->setText(0, ToQString(row.label));
    item->setText(1, ToQString(row.value));
    if (!row.icon.empty()) {
      item->setIcon(0, dockRowIcon(row.icon));
    }
    if (!row.id.empty()) {
      item->setData(0, Qt::UserRole, ToQString(row.id));
    }
    if (row.entity_id != 0u) {
      item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(
          static_cast<qulonglong>(row.entity_id)));
    } else {
      Qt::ItemFlags flags = item->flags();
      flags &= ~Qt::ItemIsSelectable;
      item->setFlags(flags);
    }
    item->setSelected(row.selected);
    if (!row.value.empty()) {
      item->setToolTip(0, ToQString(row.label + "\n" + row.value));
    }
    for (const auto& child : row.children) {
      item->addChild(buildTreeItem(child));
    }
    return item;
  }

  void emitDockTreeActivation(const std::string& panelId, QTreeWidgetItem* item) {
    if (m_owner == nullptr || item == nullptr) {
      return;
    }
    bool ok = false;
    const auto idValue = item->data(0, Qt::UserRole + 1).toULongLong(&ok);
    if (!ok || idValue == 0u) {
      return;
    }
    const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    const bool append =
        modifiers.testFlag(Qt::ControlModifier) ||
        modifiers.testFlag(Qt::MetaModifier);
    const bool rangeMode = modifiers.testFlag(Qt::ShiftModifier);
    m_owner->emit_dock_row_activation(panelId,
                                      ToUtf8String(item->data(0, Qt::UserRole).toString()),
                                      static_cast<vkpt::core::StableId>(idValue),
                                      append,
                                      rangeMode);
  }

  static void applyReadOnlyFlags(QTableWidgetItem* item, bool editable, bool enabled) {
    if (item == nullptr) {
      return;
    }
    Qt::ItemFlags flags = item->flags();
    if (!editable) {
      flags &= ~Qt::ItemIsEditable;
    }
    if (!enabled) {
      flags &= ~Qt::ItemIsEnabled;
    }
    item->setFlags(flags);
  }

  static bool isDropdownProperty(const QtDockProperty& property) {
    return property.editable && property.editor == "dropdown";
  }

  static bool isToggleProperty(const QtDockProperty& property) {
    return property.editable && property.editor == "toggle";
  }

  static bool isSliderProperty(const QtDockProperty& property) {
    return property.editable && property.editor == "slider" && property.has_numeric_range;
  }

  static bool isButtonProperty(const QtDockProperty& property) {
    return property.editable && property.editor == "button";
  }

  static bool usesCompactPropertyTable(const QtDockPanel& panel) {
    return panel.id == "inspector" ||
           panel.id == "camera" ||
           panel.id == "device" ||
           panel.id == "render_settings";
  }

  static int sliderSteps() {
    return 1000;
  }

  static bool parseTogglePropertyValue(const std::string& value) {
    std::string normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return normalized == "1" || normalized == "true" || normalized == "yes" ||
           normalized == "on" || normalized == "checked";
  }

  static double clampSliderValue(double value, double minimum, double maximum) {
    if (!std::isfinite(value)) {
      return minimum;
    }
    if (maximum < minimum) {
      std::swap(minimum, maximum);
    }
    return std::clamp(value, minimum, maximum);
  }

  static int sliderPositionFromValue(double value, double minimum, double maximum) {
    if (maximum <= minimum) {
      return 0;
    }
    const double normalized =
        (clampSliderValue(value, minimum, maximum) - minimum) / (maximum - minimum);
    return static_cast<int>(std::round(normalized * static_cast<double>(sliderSteps())));
  }

  static double valueFromSliderPosition(int position, double minimum, double maximum) {
    if (maximum <= minimum) {
      return minimum;
    }
    const double normalized =
        std::clamp(static_cast<double>(position) / static_cast<double>(sliderSteps()), 0.0, 1.0);
    return minimum + (maximum - minimum) * normalized;
  }

  static int decimalsForStep(double step) {
    if (!std::isfinite(step) || step <= 0.0) {
      return 3;
    }
    if (step >= 1.0) {
      return 0;
    }
    int decimals = 0;
    double value = step;
    while (value < 1.0 && decimals < 6) {
      value *= 10.0;
      ++decimals;
    }
    return std::clamp(decimals, 1, 6);
  }

  static QString formatSliderValue(double value, int decimals) {
    return QString::number(value, 'f', decimals);
  }

  static double parseNumericPropertyValue(const QtDockProperty& property) {
    bool ok = false;
    const double parsed = ToQString(property.value).toDouble(&ok);
    if (ok && std::isfinite(parsed)) {
      return parsed;
    }
    return property.has_default ? property.default_value : property.minimum;
  }

  void updateDockWidgetValues(QWidget* root, const QtDockPanel& panel) {
    if (root == nullptr || panel.properties.empty()) {
      return;
    }
    auto* table = root->findChild<QTableWidget*>();
    if (table == nullptr) {
      return;
    }
    const bool compactProperties = usesCompactPropertyTable(panel);
    const bool hasGroups = !compactProperties &&
                           std::any_of(panel.properties.begin(),
                                       panel.properties.end(),
                                       [](const QtDockProperty& property) {
                                         return !property.group.empty();
                                       });
    const bool hasUnits = !compactProperties &&
                          std::any_of(panel.properties.begin(),
                                      panel.properties.end(),
                                      [](const QtDockProperty& property) {
                                        return !property.unit.empty();
                                      });
    const int propertyColumn = hasGroups ? 1 : 0;
    const int valueColumn = propertyColumn + 1;
    const int unitColumn = hasUnits ? valueColumn + 1 : -1;
    const int rowCount = std::min(table->rowCount(), static_cast<int>(panel.properties.size()));
    QSignalBlocker tableBlocker(table);
    for (int row = 0; row < rowCount; ++row) {
      const auto& property = panel.properties[static_cast<std::size_t>(row)];
      if (auto* valueItem = table->item(row, valueColumn)) {
        valueItem->setText(ToQString(property.value));
        valueItem->setData(Qt::UserRole, ToQString(property.id));
        valueItem->setToolTip(ToQString(property.value));
      }
      if (auto* nameItem = table->item(row, propertyColumn)) {
        nameItem->setText(ToQString(property.name));
        nameItem->setToolTip(ToQString(property.name));
      }
      if (unitColumn >= 0) {
        if (auto* unitItem = table->item(row, unitColumn)) {
          unitItem->setText(ToQString(property.unit));
          unitItem->setToolTip(ToQString(property.unit));
        }
      }

      QWidget* cell = table->cellWidget(row, valueColumn);
      if (isDropdownProperty(property)) {
        auto* combo = qobject_cast<QComboBox*>(cell);
        if (combo != nullptr) {
          QSignalBlocker comboBlocker(combo);
          const QString current = ToQString(property.value);
          if (combo->findText(current) < 0 && !current.isEmpty()) {
            combo->addItem(current);
          }
          combo->setCurrentText(current);
          combo->setToolTip(ToQString(property.name));
        }
      } else if (isToggleProperty(property)) {
        auto* checkbox = qobject_cast<QCheckBox*>(cell);
        if (checkbox != nullptr) {
          QSignalBlocker checkboxBlocker(checkbox);
          checkbox->setChecked(parseTogglePropertyValue(property.value));
          checkbox->setEnabled(property.enabled);
          checkbox->setToolTip(ToQString(property.name));
        }
      } else if (isSliderProperty(property) && cell != nullptr) {
        auto* slider = cell->findChild<QSlider*>();
        auto* spin = cell->findChild<QDoubleSpinBox*>();
        const double minimum = property.minimum;
        const double maximum = std::max(property.minimum, property.maximum);
        const double value =
            clampSliderValue(parseNumericPropertyValue(property), minimum, maximum);
        if (slider != nullptr) {
          QSignalBlocker sliderBlocker(slider);
          slider->setValue(sliderPositionFromValue(value, minimum, maximum));
          slider->setToolTip(ToQString(property.name));
        }
        if (spin != nullptr) {
          QSignalBlocker spinBlocker(spin);
          spin->setRange(minimum, maximum);
          spin->setSingleStep(property.step > 0.0 ? property.step : 0.01);
          spin->setDecimals(decimalsForStep(property.step));
          spin->setValue(value);
          spin->setToolTip(ToQString(property.name));
        }
      } else if (isButtonProperty(property)) {
        auto* button = qobject_cast<QPushButton*>(cell);
        if (button != nullptr) {
          button->setText(ToQString(property.value.empty() ? property.name : property.value));
          button->setEnabled(property.enabled);
          button->setToolTip(ToQString(property.name));
        }
      }
    }
  }

  QWidget* buildDockWidget(const QtDockPanel& panel) {
    auto* root = new QWidget();
    root->setMinimumWidth(kQtDockMinimumWidth);
    root->setMinimumHeight(kQtDockMinimumHeight);
    root->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    const bool wantsText = panel.content == QtDockPanelContent::Text || !panel.text.empty();
    const bool wantsTree = panel.content == QtDockPanelContent::Tree ||
                           !panel.rows.empty() ||
                           !panel.tree_rows.empty();
    const bool wantsProperties = panel.content == QtDockPanelContent::Properties ||
                                 !panel.properties.empty();
    const bool treePrimaryPanel = !panel.tree_rows.empty() &&
        (panel.id == "scene_graph" || panel.id == "scene_tree");
    const int sectionCount =
        (wantsText && !panel.text.empty() ? 1 : 0) +
        (wantsTree && (!panel.rows.empty() || !panel.tree_rows.empty()) ? 1 : 0) +
        (wantsProperties && !panel.properties.empty() ? 1 : 0);
    auto* splitter = sectionCount > 1 ? new QSplitter(Qt::Vertical, root) : nullptr;
    if (splitter != nullptr) {
      splitter->setChildrenCollapsible(false);
      splitter->setHandleWidth(6);
      splitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    auto addSection = [&](QWidget* section, int stretch) {
      if (section == nullptr) {
        return;
      }
      if (splitter != nullptr) {
        splitter->addWidget(section);
        splitter->setStretchFactor(splitter->count() - 1, std::max(0, stretch));
      } else {
        layout->addWidget(section, std::max(0, stretch));
      }
    };

    if (wantsText && !panel.text.empty()) {
      auto* text = new QTextEdit(root);
      text->setReadOnly(true);
      text->setPlainText(ToQString(panel.text));
      text->setMinimumHeight(72);
      text->setMinimumWidth(kQtDockMinimumWidth - 24);
      text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      addSection(text, 1);
    }

    if (wantsTree && (!panel.rows.empty() || !panel.tree_rows.empty())) {
      auto* tree = new QTreeWidget(root);
      tree->setMinimumWidth(kQtDockMinimumWidth - 24);
      tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
      tree->setAlternatingRowColors(true);
      tree->setSelectionBehavior(QAbstractItemView::SelectRows);
      tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
      tree->setUniformRowHeights(true);
      tree->setColumnCount(panel.tree_rows.empty() ? 1 : 2);
      if (panel.tree_rows.empty()) {
        tree->setHeaderHidden(true);
      } else {
        tree->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
        tree->header()->setStretchLastSection(true);
      }
      for (const auto& row : panel.rows) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, ToQString(row));
        tree->addTopLevelItem(item);
      }
      for (const auto& row : panel.tree_rows) {
        tree->addTopLevelItem(buildTreeItem(row));
      }
      tree->expandAll();
      const auto selectedItems = tree->selectedItems();
      if (!selectedItems.isEmpty()) {
        tree->scrollToItem(selectedItems.front(), QAbstractItemView::PositionAtCenter);
      }
      QObject::connect(tree,
                       &QTreeWidget::itemClicked,
                       tree,
                       [this, panelId = panel.id](QTreeWidgetItem* item, int /*column*/) {
                         emitDockTreeActivation(panelId, item);
                       });
      addSection(tree, wantsProperties ? (treePrimaryPanel ? 1 : 0) : 1);
    }

    if (wantsProperties && !panel.properties.empty()) {
      const bool compactProperties = usesCompactPropertyTable(panel);
      const bool hasGroups = !compactProperties &&
                             std::any_of(panel.properties.begin(),
                                         panel.properties.end(),
                                         [](const QtDockProperty& property) {
                                           return !property.group.empty();
                                         });
      const bool hasUnits = !compactProperties &&
                            std::any_of(panel.properties.begin(),
                                        panel.properties.end(),
                                        [](const QtDockProperty& property) {
                                          return !property.unit.empty();
                                        });
      const bool anyEditable = std::any_of(panel.properties.begin(),
                                           panel.properties.end(),
                                           [](const QtDockProperty& property) {
                                             return property.editable;
                                           });
      const int groupColumn = hasGroups ? 0 : -1;
      const int propertyColumn = hasGroups ? 1 : 0;
      const int valueColumn = propertyColumn + 1;
      const int unitColumn = hasUnits ? valueColumn + 1 : -1;
      const int columnCount = valueColumn + 1 + (hasUnits ? 1 : 0);
      auto* table = new QTableWidget(static_cast<int>(panel.properties.size()), columnCount, root);
      table->setMinimumWidth(kQtDockMinimumWidth - 24);
      if (compactProperties) {
        table->setMinimumHeight(260);
      }
      table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
      table->setTextElideMode(Qt::ElideRight);
      table->setWordWrap(compactProperties);
      QStringList headers;
      if (compactProperties) {
        headers.push_back(QStringLiteral("Setting"));
        headers.push_back(QStringLiteral("Control"));
      } else if (hasGroups) {
        headers.push_back(QStringLiteral("Group"));
        headers.push_back(QStringLiteral("Metric"));
        headers.push_back(QStringLiteral("Value"));
        if (hasUnits) {
          headers.push_back(QStringLiteral("Unit"));
        }
      } else {
        headers.push_back(QStringLiteral("Metric"));
        headers.push_back(QStringLiteral("Value"));
        if (hasUnits) {
          headers.push_back(QStringLiteral("Unit"));
        }
      }
      table->setHorizontalHeaderLabels(headers);
      table->verticalHeader()->setVisible(false);
      table->verticalHeader()->setDefaultSectionSize(compactProperties ? 38 : 28);
      table->setEditTriggers(anyEditable
          ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
          : QAbstractItemView::NoEditTriggers);
      table->setSelectionBehavior(QAbstractItemView::SelectRows);
      table->setSelectionMode(QAbstractItemView::SingleSelection);
      table->horizontalHeader()->setMinimumSectionSize(48);
      table->horizontalHeader()->setStretchLastSection(true);
      if (hasGroups) {
        table->horizontalHeader()->setSectionResizeMode(groupColumn, QHeaderView::Interactive);
        table->setColumnWidth(groupColumn, 96);
      }
      table->horizontalHeader()->setSectionResizeMode(
          propertyColumn,
          compactProperties ? QHeaderView::ResizeToContents : QHeaderView::Interactive);
      table->setColumnWidth(propertyColumn, compactProperties ? 142 : (hasGroups ? 132 : 150));
      table->horizontalHeader()->setSectionResizeMode(valueColumn, QHeaderView::Stretch);
      if (hasUnits) {
        table->horizontalHeader()->setSectionResizeMode(unitColumn, QHeaderView::Interactive);
        table->setColumnWidth(unitColumn, 64);
      }
      for (std::size_t i = 0; i < panel.properties.size(); ++i) {
        const auto& property = panel.properties[i];
        const int row = static_cast<int>(i);
        const bool multilineValue =
            compactProperties && property.value.find('\n') != std::string::npos;
        const bool usesValueWidget = isDropdownProperty(property) ||
                                     isToggleProperty(property) ||
                                     isSliderProperty(property) ||
                                     isButtonProperty(property);
        auto* nameItem = new QTableWidgetItem(ToQString(property.name));
        auto* valueItem = new QTableWidgetItem(ToQString(property.value));
        valueItem->setData(Qt::UserRole, ToQString(property.id));
        applyReadOnlyFlags(nameItem, false, property.enabled);
        applyReadOnlyFlags(valueItem, property.editable && !usesValueWidget, property.enabled);
        nameItem->setToolTip(ToQString(property.name));
        valueItem->setToolTip(ToQString(property.value));
        if (compactProperties) {
          valueItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (hasGroups) {
          auto* groupItem = new QTableWidgetItem(ToQString(property.group));
          applyReadOnlyFlags(groupItem, false, property.enabled);
          groupItem->setToolTip(ToQString(property.group));
          table->setItem(row, groupColumn, groupItem);
        }
        table->setItem(row, propertyColumn, nameItem);
        table->setItem(row, valueColumn, valueItem);
        if (isDropdownProperty(property)) {
          auto* combo = new QComboBox(table);
          combo->setEnabled(property.enabled);
          bool hasCurrent = false;
          for (const auto& option : property.options) {
            combo->addItem(ToQString(option));
            hasCurrent = hasCurrent || option == property.value;
          }
          if (!hasCurrent && !property.value.empty()) {
            combo->addItem(ToQString(property.value));
          }
          combo->setCurrentText(ToQString(property.value));
          combo->setToolTip(ToQString(property.name));
          QObject::connect(combo,
                           &QComboBox::currentTextChanged,
                           combo,
                           [owner = m_owner,
                            panelId = panel.id,
                            propertyId = property.id](const QString& value) {
                             if (owner != nullptr && !propertyId.empty()) {
                               owner->emit_dock_property_edit(panelId,
                                                              propertyId,
                                                              ToUtf8String(value));
                             }
          });
          table->setCellWidget(row, valueColumn, combo);
        } else if (isToggleProperty(property)) {
          auto* checkbox = new QCheckBox(table);
          checkbox->setChecked(parseTogglePropertyValue(property.value));
          checkbox->setEnabled(property.enabled);
          checkbox->setToolTip(ToQString(property.name));
          checkbox->setText(QStringLiteral("Enabled"));
          QObject::connect(checkbox,
                           &QCheckBox::toggled,
                           checkbox,
                           [owner = m_owner,
                            panelId = panel.id,
                            propertyId = property.id,
                            valueItem](bool checked) {
                             const char* text = checked ? "true" : "false";
                             if (valueItem != nullptr) {
                               valueItem->setText(QString::fromLatin1(text));
                               valueItem->setToolTip(QString::fromLatin1(text));
                             }
                             if (owner != nullptr && !propertyId.empty()) {
                               owner->emit_dock_property_edit(panelId, propertyId, text);
                             }
                           });
          table->setCellWidget(row, valueColumn, checkbox);
          table->setRowHeight(row, compactProperties ? 38 : 32);
        } else if (isSliderProperty(property)) {
          const double minimum = property.minimum;
          const double maximum = std::max(property.minimum, property.maximum);
          const double step = property.step > 0.0 ? property.step : 0.01;
          const int decimals = decimalsForStep(step);
          const double initialValue =
              clampSliderValue(parseNumericPropertyValue(property), minimum, maximum);
          const double resetValue = clampSliderValue(
              property.has_default ? property.default_value : initialValue,
              minimum,
              maximum);

          auto* editor = new QWidget(table);
          auto* editorLayout = new QHBoxLayout(editor);
          editorLayout->setContentsMargins(0, 0, 0, 0);
          editorLayout->setSpacing(4);

          auto* slider = new QSlider(Qt::Horizontal, editor);
          slider->setRange(0, sliderSteps());
          slider->setValue(sliderPositionFromValue(initialValue, minimum, maximum));
          slider->setEnabled(property.enabled);
          slider->setToolTip(ToQString(property.name));
          slider->setMinimumWidth(compactProperties ? 112 : 96);

          auto* spin = new QDoubleSpinBox(editor);
          spin->setRange(minimum, maximum);
          spin->setSingleStep(step);
          spin->setDecimals(decimals);
          spin->setValue(initialValue);
          spin->setEnabled(property.enabled);
          spin->setKeyboardTracking(false);
          spin->setMinimumWidth(74);
          spin->setToolTip(ToQString(property.name));

          auto* reset = new QPushButton(QStringLiteral("Reset"), editor);
          reset->setEnabled(property.enabled && property.has_default);
          reset->setMinimumWidth(compactProperties ? 54 : 48);
          reset->setToolTip(QStringLiteral("Reset to safe default"));

          editorLayout->addWidget(slider, 1);
          editorLayout->addWidget(spin, 0);
          editorLayout->addWidget(reset, 0);

          auto emitValue = [owner = m_owner,
                            panelId = panel.id,
                            propertyId = property.id,
                            decimals,
                            valueItem](double value) {
            const QString text = formatSliderValue(value, decimals);
            if (valueItem != nullptr) {
              valueItem->setText(text);
            }
            if (owner != nullptr && !propertyId.empty()) {
              owner->emit_dock_property_edit(panelId,
                                             propertyId,
                                             ToUtf8String(text));
            }
          };

          QObject::connect(slider,
                           &QSlider::valueChanged,
                           slider,
                           [spin, emitValue, minimum, maximum](int position) {
                             const double value = valueFromSliderPosition(position, minimum, maximum);
                             {
                               QSignalBlocker blocker(spin);
                               spin->setValue(value);
                             }
                             emitValue(value);
                           });
          QObject::connect(spin,
                           QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                           spin,
                           [slider, emitValue, minimum, maximum](double value) {
                             {
                               QSignalBlocker blocker(slider);
                               slider->setValue(sliderPositionFromValue(value, minimum, maximum));
                             }
                             emitValue(value);
                           });
          QObject::connect(reset, &QPushButton::clicked, reset, [slider, spin, resetValue, minimum, maximum]() {
            {
              QSignalBlocker blocker(slider);
              slider->setValue(sliderPositionFromValue(resetValue, minimum, maximum));
            }
            spin->setValue(resetValue);
          });

          table->setCellWidget(row, valueColumn, editor);
          table->setRowHeight(row, compactProperties ? 38 : 32);
        } else if (isButtonProperty(property)) {
          auto* button = new QPushButton(ToQString(property.value.empty() ? property.name : property.value), table);
          button->setEnabled(property.enabled);
          button->setToolTip(ToQString(property.name));
          QObject::connect(button,
                           &QPushButton::clicked,
                           button,
                           [owner = m_owner,
                            panelId = panel.id,
                            propertyId = property.id]() {
                             if (owner != nullptr && !propertyId.empty()) {
                               owner->emit_dock_property_edit(panelId,
                                                              propertyId,
                                                              "clicked");
                             }
                           });
          table->setCellWidget(row, valueColumn, button);
          table->setRowHeight(row, compactProperties ? 38 : 32);
        }
        if (hasUnits) {
          auto* unitItem = new QTableWidgetItem(ToQString(property.unit));
          applyReadOnlyFlags(unitItem, false, property.enabled);
          unitItem->setToolTip(ToQString(property.unit));
          table->setItem(row, unitColumn, unitItem);
        }
        if (multilineValue && !usesValueWidget) {
          const int lineCount = 1 + static_cast<int>(
              std::count(property.value.begin(), property.value.end(), '\n'));
          table->setRowHeight(row, std::min(118, 34 + lineCount * 20));
        }
      }
      QObject::connect(table,
                       &QTableWidget::itemChanged,
                       table,
                       [owner = m_owner,
                        panelId = panel.id,
                        valueColumn](QTableWidgetItem* item) {
                         if (owner == nullptr ||
                             item == nullptr ||
                             item->column() != valueColumn ||
                             (item->flags() & Qt::ItemIsEditable) == 0) {
                           return;
                         }
                         const auto propertyId = item->data(Qt::UserRole).toString();
                         if (propertyId.isEmpty()) {
                           return;
                         }
                         owner->emit_dock_property_edit(panelId,
                                                        ToUtf8String(propertyId),
                                                        ToUtf8String(item->text()));
                       });
      addSection(table, treePrimaryPanel ? 0 : 1);
    }

    if (splitter != nullptr) {
      layout->addWidget(splitter, 1);
    }

    if (panel.text.empty() &&
        panel.rows.empty() &&
        panel.tree_rows.empty() &&
        panel.properties.empty()) {
      auto* text = new QTextEdit(root);
      text->setReadOnly(true);
      text->setPlainText(QStringLiteral("No data"));
      text->setMinimumWidth(kQtDockMinimumWidth - 24);
      text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      layout->addWidget(text);
    }

    return root;
  }

  QtWindow* m_owner = nullptr;
  std::unordered_map<std::string, QDockWidget*> m_docks;
  std::unordered_map<std::string, std::string> m_panelContentSignatures;
  std::unordered_map<std::string, QLabel*> m_statusFields;
  bool m_layoutRestored = false;
};

class QtViewportWindow final : public QWidget {
 public:
  explicit QtViewportWindow(QtWindow* owner) : QWidget(nullptr), m_owner(owner) {
    setObjectName(QStringLiteral("vkpt.qt.viewport"));
    ApplyStartupSurface(this);
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

  void setViewportCursor(QtViewportCursor cursor) {
    if (m_viewportCursor == cursor) {
      return;
    }
    m_viewportCursor = cursor;
    applyViewportCursor();
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
      painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
      painter.drawImage(frameDisplayRect(), m_frame);
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
        const bool hasWireframe = !box.lines.empty();
        if (!hasWireframe &&
            (!clipped.isValid() || clipped.width() < 2.0 || clipped.height() < 2.0)) {
          continue;
        }

        const QColor stroke = box.primary ? QColor(255, 214, 64, 245)
                                          : QColor(102, 204, 255, 230);
        if (hasWireframe) {
          for (const auto& line : box.lines) {
            if (!std::isfinite(line.x0) || !std::isfinite(line.y0) ||
                !std::isfinite(line.x1) || !std::isfinite(line.y1)) {
              continue;
            }
            QPen shadowPen(QColor(0, 0, 0, 130));
            shadowPen.setWidthF(std::max(1.0f, line.width + 1.25f));
            painter.setPen(shadowPen);
            painter.drawLine(QPointF(line.x0, line.y0), QPointF(line.x1, line.y1));

            QPen linePen(QColor(line.r, line.g, line.b, line.a));
            linePen.setWidthF(std::max(1.0f, line.width));
            linePen.setCapStyle(Qt::RoundCap);
            painter.setPen(linePen);
            painter.drawLine(QPointF(line.x0, line.y0), QPointF(line.x1, line.y1));
          }

          for (const auto& point : box.points) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.radius)) {
              continue;
            }
            const float radius = std::max(2.0f, point.radius);
            const QRectF handle(point.x - radius, point.y - radius, radius * 2.0f, radius * 2.0f);
            painter.setPen(QPen(QColor(0, 0, 0, 150), 1.25));
            painter.setBrush(QColor(point.r, point.g, point.b, point.a));
            painter.drawEllipse(handle);
            if (!point.label.empty()) {
              painter.setPen(QColor(point.r, point.g, point.b, std::min<int>(point.a, 180)));
              painter.drawText(handle.adjusted(radius + 1.5f, -3.0f, 42.0f, 6.0f),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               ToQString(point.label));
            }
          }
        } else {
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
        }

        if (!box.label.empty()) {
          const QString label = ToQString(box.label);
          const double labelX = clipped.isValid()
              ? clipped.left() + 4.0
              : std::max(0.0f, box.x + 4.0f);
          const double labelY = clipped.isValid()
              ? std::max(0.0, clipped.top() - 20.0)
              : std::max(0.0f, box.y - 20.0f);
          const double labelWidth = clipped.isValid()
              ? std::max(48.0, clipped.width())
              : std::max(48.0f, box.width);
          const QRectF labelRect(labelX,
                                 labelY,
                                 labelWidth,
                                 18.0);
          painter.fillRect(labelRect.adjusted(-2.0, 0.0, 2.0, 0.0), QColor(0, 0, 0, 150));
          painter.setPen(stroke);
          painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label);
        }
      }
    }

    const QString text = m_overlayText;
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
    releaseMouseGrab();
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
    releaseMouseGrab();
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
    setFocus(Qt::MouseFocusReason);
    if (!m_mouseGrabbed) {
      grabMouse();
      m_mouseGrabbed = true;
    }
    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) {
      m_forceClosedHandCursor = true;
      applyViewportCursor();
    }
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
    if (event->buttons() == Qt::NoButton) {
      releaseMouseGrab();
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
  void releaseMouseGrab() {
    if (m_mouseGrabbed) {
      releaseMouse();
      m_mouseGrabbed = false;
    }
    m_forceClosedHandCursor = false;
    applyViewportCursor();
  }

  void applyViewportCursor() {
    if (m_forceClosedHandCursor) {
      setCursor(Qt::ClosedHandCursor);
      return;
    }
    setCursor(CursorForViewportCursor(m_viewportCursor));
  }

  void dropUnpresentedFrame() {
    if (!m_frame.isNull() && !m_framePresented && m_frameId != 0u && m_owner != nullptr) {
      m_owner->record_frame_dropped(m_frameId);
    }
    m_framePresented = true;
  }

  QRectF frameDisplayRect() const {
    const QRectF viewportRect(rect());
    if (m_frame.isNull() || m_frame.width() <= 0 || m_frame.height() <= 0 ||
        viewportRect.width() <= 0.0 || viewportRect.height() <= 0.0) {
      return viewportRect;
    }

    const double frameAspect =
        static_cast<double>(m_frame.width()) / static_cast<double>(m_frame.height());
    const double viewportAspect = viewportRect.width() / viewportRect.height();
    if (viewportAspect > frameAspect) {
      const double fittedWidth = viewportRect.height() * frameAspect;
      const double x = viewportRect.left() + (viewportRect.width() - fittedWidth) * 0.5;
      return QRectF(x, viewportRect.top(), fittedWidth, viewportRect.height());
    }

    const double fittedHeight = viewportRect.width() / frameAspect;
    const double y = viewportRect.top() + (viewportRect.height() - fittedHeight) * 0.5;
    return QRectF(viewportRect.left(), y, viewportRect.width(), fittedHeight);
  }

  QtWindow* m_owner = nullptr;
  QImage m_frame;
  std::uint64_t m_frameId = 0u;
  std::size_t m_frameWidth = 0u;
  std::size_t m_frameHeight = 0u;
  bool m_framePresented = true;
  QString m_overlayText;
  std::vector<QtSelectionOverlayBox> m_overlayBoxes;
  QtViewportCursor m_viewportCursor = QtViewportCursor::Default;
  bool m_dirty = false;
  bool m_mouseGrabbed = false;
  bool m_forceClosedHandCursor = false;
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

void AppendQtMenuSignature(std::ostringstream& out,
                           const std::vector<QtMenuItem>& items) {
  for (const auto& item : items) {
    out << item.command_id << '|'
        << (item.enabled ? '1' : '0') << '|'
        << item.label.size() << ':' << item.label << '[';
    AppendQtMenuSignature(out, item.children);
    out << ']';
  }
}

std::string QtMenuSignature(const std::vector<QtMenuItem>& menus) {
  std::ostringstream out;
  AppendQtMenuSignature(out, menus);
  return out.str();
}

void AppendQtMenuItems(QMenu* menu,
                       const std::vector<QtMenuItem>& items,
                       QtWindow* owner,
                       QWidget* focusTarget) {
  if (menu == nullptr) {
    return;
  }

  for (const auto& item : items) {
    if (!item.children.empty()) {
      auto* submenu = menu->addMenu(ToQString(item.label));
      submenu->setEnabled(item.enabled);
      AppendQtMenuItems(submenu, item.children, owner, focusTarget);
      continue;
    }

    auto* action = menu->addAction(ToQString(item.label));
    action->setEnabled(item.enabled);
    const std::uint32_t commandId = item.command_id;
    QObject::connect(action, &QAction::triggered, action, [owner, focusTarget, commandId]() {
      if (owner != nullptr) {
        owner->emit_menu_command(commandId);
      }
      if (focusTarget != nullptr) {
        focusTarget->setFocus(Qt::OtherFocusReason);
      }
    });
  }
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

  if (m_shell == nullptr) {
    m_shell = new QtMainWindow(this);
  } else {
    static_cast<QtMainWindow*>(m_shell)->setOwner(this);
  }

  if (m_widget == nullptr) {
    m_widget = new QtViewportWindow(this);
    static_cast<QtMainWindow*>(m_shell)->setCentralWidget(m_widget);
  } else {
    static_cast<QtViewportWindow*>(m_widget)->setOwner(this);
    if (m_widget->parentWidget() == nullptr) {
      static_cast<QtMainWindow*>(m_shell)->setCentralWidget(m_widget);
    }
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
  m_menuBarSignature.clear();
  m_lastMouseX = 0;
  m_lastMouseY = 0;
  m_mainWindowRevealed = false;

  m_shell->setUpdatesEnabled(false);
  m_shell->resize(ClampToQtInt(width), ClampToQtInt(height));
  m_shell->setWindowTitle(ToQString(m_title));
  m_widget->resize(ClampToQtInt(width), ClampToQtInt(height));
  if (m_overlayText.empty()) {
    m_overlayText = "vkPathTracer\nStarting Qt shell...\nPreparing renderer and UI panels.";
  }
  static_cast<QtViewportWindow*>(m_widget)->setOverlayText(ToQString(m_overlayText));
  m_shell->setUpdatesEnabled(true);
  (void)m_widget->winId();
  show_startup_splash();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 4);

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
  if (!m_open && m_shell == nullptr && m_widget == nullptr) {
    return;
  }

  emit_close_requested();
  m_open = false;
  {
    std::scoped_lock lock(m_frameMutex);
    clear_frame_handoff_locked(false);
  }
  if (m_shell != nullptr) {
    m_shell->hide();
  } else if (m_widget != nullptr) {
    m_widget->hide();
  }
  close_startup_splash(false);
}

WindowMetrics QtWindow::metrics() const {
  return m_metrics;
}

bool QtWindow::poll_events() {
  if (QCoreApplication::instance() != nullptr) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
  }

  update_metrics_from_widget();
  const bool hiddenForStartup = m_startupSplashActive && !m_mainWindowRevealed;
  if (m_shell != nullptr && !m_shell->isVisible() && m_open && !hiddenForStartup) {
    mark_closed();
  } else if (m_shell == nullptr && m_widget != nullptr && !m_widget->isVisible() && m_open) {
    mark_closed();
  }
  return m_open;
}

bool QtWindow::resize(std::size_t width, std::size_t height) {
  if (!m_open || width == 0u || height == 0u) {
    return false;
  }
  if (m_shell != nullptr) {
    m_shell->resize(ClampToQtInt(width), ClampToQtInt(height));
    if (!m_mainWindowRevealed && m_widget != nullptr) {
      m_widget->resize(ClampToQtInt(width), ClampToQtInt(height));
    }
  } else if (m_widget != nullptr) {
    m_widget->resize(ClampToQtInt(width), ClampToQtInt(height));
  } else {
    return false;
  }
  update_metrics_from_widget();
  return true;
}

void QtWindow::set_title(std::string_view title) {
  m_title.assign(title);
  if (m_shell != nullptr) {
    m_shell->setWindowTitle(ToQString(title));
  } else if (m_widget != nullptr) {
    m_widget->setWindowTitle(ToQString(title));
  }
}

void QtWindow::set_overlay_text(std::string_view text) {
  m_overlayText.assign(text);
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setOverlayText(ToQString(text));
  }
}

void QtWindow::set_startup_splash_text(std::string_view text) {
  if (m_startupSplash == nullptr) {
    return;
  }
  static_cast<QtStartupSplash*>(m_startupSplash)->setPhase(ToQString(text));
}

void QtWindow::finish_startup_splash() {
  reveal_main_window_from_splash();
}

void QtWindow::set_selection_overlay_boxes(const std::vector<QtSelectionOverlayBox>& boxes) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setSelectionOverlayBoxes(boxes);
  }
}

void QtWindow::set_viewport_cursor(QtViewportCursor cursor) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setViewportCursor(cursor);
  }
}

void QtWindow::set_menu_bar(const std::vector<QtMenuItem>& menus) {
  if (m_shell == nullptr) {
    return;
  }

  const std::string signature = QtMenuSignature(menus);
  if (signature == m_menuBarSignature) {
    return;
  }
  m_menuBarSignature = signature;

  auto* mainWindow = static_cast<QtMainWindow*>(m_shell);
  auto* menuBar = mainWindow->menuBar();
  if (menuBar == nullptr) {
    menuBar = new QMenuBar(mainWindow);
    mainWindow->setMenuBar(menuBar);
  }
  menuBar->setUpdatesEnabled(false);
  menuBar->clear();
  for (const auto& top : menus) {
    auto* menu = menuBar->addMenu(ToQString(top.label));
    menu->setEnabled(top.enabled);
    AppendQtMenuItems(menu, top.children, this, m_widget);
  }
  menuBar->setUpdatesEnabled(true);
  menuBar->updateGeometry();
  menuBar->update();
  if (m_widget != nullptr) {
    m_widget->setFocus(Qt::OtherFocusReason);
  }
}

void QtWindow::set_dock_panels(const std::vector<QtDockPanel>& panels) {
  if (m_shell == nullptr) {
    return;
  }
  static_cast<QtMainWindow*>(m_shell)->setDockPanels(panels);
  if (m_widget != nullptr) {
    m_widget->setFocus(Qt::OtherFocusReason);
  }
}

void QtWindow::set_status_bar_text(std::string_view text) {
  if (m_shell == nullptr) {
    return;
  }
  static_cast<QtMainWindow*>(m_shell)->setStatusText(ToQString(text));
}

void QtWindow::set_status_bar_text(const QtStatusBarText& status) {
  if (m_shell == nullptr) {
    return;
  }
  static_cast<QtMainWindow*>(m_shell)->setStatusText(status);
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
    return;
  }

  reveal_main_window_from_splash();
}

void QtWindow::show_startup_splash() {
  if (m_startupSplash != nullptr) {
    return;
  }

  auto* splash = new QtStartupSplash();
  m_startupSplash = splash;
  m_startupSplashActive = true;
  splash->setPhase(QStringLiteral("Preparing Qt shell"));
  if (QObject* context = QCoreApplication::instance()) {
    QObject::connect(splash, &QObject::destroyed, context, [this, splash]() {
      if (m_startupSplash == splash) {
        m_startupSplash = nullptr;
      }
      m_startupSplashActive = false;
    });
  }
  splash->showCentered(m_shell);

  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      kQtLogSubsystem,
      "Qt startup splash shown");
}

void QtWindow::reveal_main_window_from_splash() {
  if (m_mainWindowRevealed) {
    close_startup_splash(true);
    return;
  }

  if (m_shell != nullptr) {
    const bool animateReveal = m_startupSplashActive;
    m_shell->setUpdatesEnabled(true);
    m_shell->setWindowOpacity(animateReveal ? 0.0 : 1.0);
    m_shell->show();
    m_shell->raise();
    m_shell->activateWindow();
    if (m_widget != nullptr) {
      m_widget->setFocus(Qt::OtherFocusReason);
    }
    m_mainWindowRevealed = true;
    update_metrics_from_widget();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 4);

    if (animateReveal) {
      auto* fade = new QPropertyAnimation(m_shell, "windowOpacity", m_shell);
      fade->setDuration(180);
      fade->setStartValue(0.0);
      fade->setEndValue(1.0);
      fade->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
      m_shell->setWindowOpacity(1.0);
    }
  } else if (m_widget != nullptr) {
    m_widget->show();
    m_widget->raise();
    m_widget->activateWindow();
    m_mainWindowRevealed = true;
  }

  close_startup_splash(true);
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      kQtLogSubsystem,
      "Qt main window revealed after startup splash",
      {
        {"width", std::to_string(m_metrics.width)},
        {"height", std::to_string(m_metrics.height)}
      });
}

void QtWindow::close_startup_splash(bool animated) {
  if (m_startupSplash == nullptr) {
    m_startupSplashActive = false;
    return;
  }

  auto* splash = static_cast<QtStartupSplash*>(m_startupSplash);
  m_startupSplash = nullptr;
  m_startupSplashActive = false;
  splash->finish(animated);
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
                                        bool range_mode) {
  if (panel_id.empty() || entity_id == 0u) {
    return;
  }
  m_dockRowActivations.push_back(QtDockRowActivation{
      std::move(panel_id),
      std::move(row_id),
      entity_id,
      append,
      range_mode});
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

void QtWindow::mark_closed() {
  m_open = false;
  std::scoped_lock lock(m_frameMutex);
  clear_frame_handoff_locked(false);
}

void QtWindow::destroy() {
  m_open = false;
  m_closeEventQueued = false;
  m_events.clear();
  m_dockPropertyEdits.clear();
  m_dockRowActivations.clear();
  m_menuBarSignature.clear();
  close_startup_splash(false);
  m_mainWindowRevealed = false;
  QWidget* shell = nullptr;
  QWidget* widget = nullptr;
  {
    std::scoped_lock lock(m_frameMutex);
    clear_frame_handoff_locked(false);
    shell = m_shell;
    m_shell = nullptr;
    widget = m_widget;
    m_widget = nullptr;
  }
  if (shell != nullptr) {
    if (widget != nullptr) {
      auto* viewport = static_cast<QtViewportWindow*>(widget);
      viewport->clearFrameOnUiThread();
      viewport->setOwner(nullptr);
    }
    auto* mainWindow = static_cast<QtMainWindow*>(shell);
    mainWindow->setOwner(nullptr);
    mainWindow->hide();
    delete mainWindow;
  } else if (widget != nullptr) {
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
