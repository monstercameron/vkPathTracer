#include "platform/qt/QtPlatformStyle.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QAbstractAnimation>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPropertyAnimation>
#include <QRect>
#include <QRectF>
#include <QScreen>
#include <QStringList>
#include <QTimer>
#include <QWidget>

namespace vkpt::platform {
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
namespace {
class QtStartupSplash final : public QWidget {
 public:
  explicit QtStartupSplash()
      : QtStartupSplash(QStringLiteral("vkPathTracer"),
                        QStringLiteral("Starting the renderer"),
                        QStringLiteral("The main viewport will appear after the first resolved frame is ready."),
                        QStringList{
                            QStringLiteral("Preparing renderer state"),
                            QStringLiteral("Loading scene assets"),
                            QStringLiteral("Building acceleration data"),
                            QStringLiteral("Compiling display pipeline"),
                            QStringLiteral("Resolving the first preview frame"),
                            QStringLiteral("Polishing the viewport handoff"),
                        }) {}

  QtStartupSplash(QString title, QString subtitle, QString footer, QStringList messages)
      : QWidget(nullptr,
                Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
        m_title(std::move(title)),
        m_subtitle(std::move(subtitle)),
        m_footer(std::move(footer)),
        m_messages(std::move(messages)) {
    setObjectName(QStringLiteral("vkpt.qt.startup_splash"));
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAutoFillBackground(false);
    resize(640, 360);

    if (m_messages.isEmpty()) {
      m_messages = QStringList{QStringLiteral("Preparing display handoff")};
    }

    auto* timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this]() {
      ++m_tick;
      if (m_tick % 26u == 0u && !m_messages.isEmpty()) {
        m_messageIndex = (m_messageIndex + 1) % m_messages.size();
      }
      if (m_displayProgress < m_targetProgress) {
        ++m_displayProgress;
      } else if (!m_finishing && m_targetProgress < 94 && (m_tick % 18u) == 0u) {
        ++m_targetProgress;
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
    updateProgressTargetForPhase(m_phase);
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
    m_targetProgress = 100;
    m_displayProgress = 100;
    update();
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

    const qreal contentLeft = panel.left() + 36.0;
    const qreal contentRight = panel.right() - 36.0;
    const qreal contentWidth = std::max<qreal>(120.0, contentRight - contentLeft);

    QFont titleFont = painter.font();
    titleFont.setPixelSize(28);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(244, 248, 252));
    const QFontMetricsF titleMetrics(titleFont);
    QRectF titleRect(contentLeft,
                     panel.top() + 34.0,
                     contentWidth,
                     titleMetrics.height() + 4.0);
    painter.drawText(titleRect,
                     Qt::AlignLeft | Qt::AlignTop,
                     m_title.isEmpty() ? QStringLiteral("vkPathTracer") : m_title);

    QFont subtitleFont = painter.font();
    subtitleFont.setPixelSize(13);
    subtitleFont.setBold(false);
    painter.setFont(subtitleFont);
    painter.setPen(QColor(164, 178, 194));
    const QFontMetricsF subtitleMetrics(subtitleFont);
    QRectF subtitleRect(contentLeft,
                        titleRect.bottom() + 12.0,
                        contentWidth,
                        subtitleMetrics.height() + 4.0);
    painter.drawText(subtitleRect,
                     Qt::AlignLeft | Qt::AlignTop,
                     m_subtitle.isEmpty() ? QStringLiteral("Loading") : m_subtitle);

    QFont progressFont = painter.font();
    progressFont.setPixelSize(13);
    progressFont.setBold(true);
    painter.setFont(progressFont);
    painter.setPen(QColor(175, 210, 238));
    QRectF progressTextRect(contentRight - 112.0,
                            subtitleRect.top() - 1.0,
                            112.0,
                            subtitleRect.height() + 4.0);
    painter.drawText(progressTextRect,
                     Qt::AlignRight | Qt::AlignTop,
                     QStringLiteral("Loading %1%").arg(m_displayProgress));

    const qreal statusTop = subtitleRect.bottom() + 42.0;
    const QPointF spinnerCenter(contentLeft + 22.0, statusTop + 34.0);
    const double spinnerRotation = static_cast<double>(m_tick) * 0.16;
    for (int i = 0; i < 12; ++i) {
      const int alpha = std::max(38, 230 - i * 15);
      const double angle = spinnerRotation + (static_cast<double>(i) / 12.0) * 6.283185307179586;
      const double inner = 15.0;
      const double outer = 24.0;
      QPen pen(QColor(86, 164, 220, alpha), 3.0);
      pen.setCapStyle(Qt::RoundCap);
      painter.setPen(pen);
      painter.drawLine(QPointF(spinnerCenter.x() + std::cos(angle) * inner,
                               spinnerCenter.y() + std::sin(angle) * inner),
                       QPointF(spinnerCenter.x() + std::cos(angle) * outer,
                               spinnerCenter.y() + std::sin(angle) * outer));
    }

    QFont phaseFont = painter.font();
    phaseFont.setPixelSize(16);
    phaseFont.setBold(true);
    painter.setFont(phaseFont);
    painter.setPen(QColor(232, 238, 246));
    const QString phase = m_phase.isEmpty() ? QStringLiteral("Preparing Qt shell") : m_phase;
    const qreal textLeft = contentLeft + 72.0;
    const qreal textWidth = std::max<qreal>(120.0, contentRight - textLeft);
    const QFontMetricsF phaseMetrics(phaseFont);
    QRectF phaseRect(textLeft,
                     statusTop + 4.0,
                     textWidth,
                     phaseMetrics.height() * 2.0 + 4.0);
    painter.drawText(phaseRect,
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     phase);

    QFont messageFont = painter.font();
    messageFont.setPixelSize(13);
    messageFont.setBold(false);
    painter.setFont(messageFont);
    painter.setPen(QColor(150, 166, 184));
    const QFontMetricsF messageMetrics(messageFont);
    QRectF messageRect(textLeft,
                       phaseRect.bottom() + 8.0,
                       textWidth,
                       messageMetrics.height() * 2.0 + 4.0);
    painter.drawText(messageRect,
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     currentLoadingMessage());

    const QRectF bar(contentLeft, panel.bottom() - 62.0, contentWidth, 8.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(24, 34, 45));
    painter.drawRoundedRect(bar, 4.0, 4.0);
    QRectF fill = bar;
    fill.setWidth(std::max<qreal>(8.0, bar.width() * static_cast<qreal>(m_displayProgress) / 100.0));
    painter.setBrush(QColor(70, 146, 208));
    painter.drawRoundedRect(fill, 4.0, 4.0);

    painter.setPen(QColor(106, 122, 140));
    QRectF footerRect(contentLeft,
                      bar.bottom() + 10.0,
                      contentWidth,
                      panel.bottom() - bar.bottom() - 16.0);
    painter.drawText(footerRect,
                     Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     m_footer);
  }

 private:
  QString currentLoadingMessage() const {
    if (m_messages.isEmpty()) {
      return QStringLiteral("Preparing display handoff");
    }
    const int messageCount = static_cast<int>(m_messages.size());
    const int index = std::clamp(m_messageIndex, 0, messageCount - 1);
    QString suffix;
    const int dotCount = static_cast<int>((m_tick / 10u) % 4u);
    for (int i = 0; i < dotCount; ++i) {
      suffix += QLatin1Char('.');
    }
    return m_messages.at(index) + suffix;
  }

  static int EstimateProgressForPhase(const QString& phase) {
    const QString lower = phase.toLower();
    if (lower.contains(QStringLiteral("preparing qt shell"))) {
      return 6;
    }
    if (lower.contains(QStringLiteral("qt window opened"))) {
      return 12;
    }
    if (lower.contains(QStringLiteral("initializing d3d12")) ||
        lower.contains(QStringLiteral("falling back"))) {
      return 22;
    }
    if (lower.contains(QStringLiteral("loading scene"))) {
      return 38;
    }
    if (lower.contains(QStringLiteral("scene loaded"))) {
      return 52;
    }
    if (lower.contains(QStringLiteral("configuring renderer")) ||
        lower.contains(QStringLiteral("acceleration"))) {
      return 72;
    }
    if (lower.contains(QStringLiteral("tracer initialized"))) {
      return 88;
    }
    if (lower.contains(QStringLiteral("resolved frame")) ||
        lower.contains(QStringLiteral("viewport handoff"))) {
      return 96;
    }
    return -1;
  }

  void updateProgressTargetForPhase(const QString& phase) {
    int nextTarget = EstimateProgressForPhase(phase);
    if (nextTarget < 0) {
      nextTarget = std::min(94, m_targetProgress + 6);
    }
    m_targetProgress = std::clamp(std::max(m_targetProgress, nextTarget), 0, 100);
  }

  QString m_title;
  QString m_subtitle;
  QString m_footer;
  QString m_phase;
  QStringList m_messages;
  int m_messageIndex = 0;
  int m_displayProgress = 0;
  int m_targetProgress = 0;
  unsigned m_tick = 0u;
  bool m_finishing = false;
  QTimer* m_animationTimer = nullptr;
};
}  // namespace

QWidget* CreateStartupSplash() {
  return new QtStartupSplash();
}

QWidget* CreateLoadingSplash(QString title, QString subtitle, QString footer, QStringList messages) {
  return new QtStartupSplash(std::move(title),
                             std::move(subtitle),
                             std::move(footer),
                             std::move(messages));
}

void SetLoadingSplashPhase(QWidget* splash, QString phase) {
  auto* startupSplash = dynamic_cast<QtStartupSplash*>(splash);
  if (startupSplash == nullptr) {
    return;
  }
  startupSplash->setPhase(std::move(phase));
}

void ShowLoadingSplashCentered(QWidget* splash, QWidget* reference) {
  auto* startupSplash = dynamic_cast<QtStartupSplash*>(splash);
  if (startupSplash == nullptr) {
    return;
  }
  startupSplash->showCentered(reference);
}

void FinishLoadingSplash(QWidget* splash, bool animated) {
  auto* startupSplash = dynamic_cast<QtStartupSplash*>(splash);
  if (startupSplash == nullptr) {
    return;
  }
  startupSplash->finish(animated);
}

void SetStartupSplashPhase(QWidget* splash, QString phase) {
  SetLoadingSplashPhase(splash, std::move(phase));
}

void ShowStartupSplashCentered(QWidget* splash, QWidget* reference) {
  ShowLoadingSplashCentered(splash, reference);
}

void FinishStartupSplash(QWidget* splash, bool animated) {
  FinishLoadingSplash(splash, animated);
}

}  // namespace vkpt::platform
