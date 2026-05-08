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
    setAcceptDrops(true);

    auto* timer = new QTimer(this);
    QObject::connect(timer, &QTimer::timeout, this, [this]() {
      if (m_dirty) {
        update();
      }
    });
    timer->start(8);
  }

  void setOwner(QtWindow* owner) {
    m_owner = owner;
  }

  // Overlay setters set m_dirty and let the 8ms poll timer coalesce the
  // repaint. Calling update() here too forces Qt to schedule an immediate
  // paint per setter; with several overlays mutated per frame (FPS text,
  // selection boxes, script panels) that produces 4-7× the necessary paints
  // and pegs the UI thread on a 3ms paintEvent that doesn't render any new
  // frame data. The timer still fires every 8ms and will pick up m_dirty.
  void setOverlayText(QString text) {
    if (m_overlayText == text) {
      return;
    }
    m_overlayText = std::move(text);
    m_dirty = true;
  }

  void setSelectionOverlayBoxes(std::vector<QtSelectionOverlayBox> boxes) {
    m_overlayBoxes = std::move(boxes);
    m_dirty = true;
  }

  void setScriptOverlayPanels(std::vector<QtScriptOverlayPanel> panels) {
    m_scriptOverlayPanels = std::move(panels);
    m_dirty = true;
  }

  void setViewportCursor(QtViewportCursor cursor) {
    if (m_viewportCursor == cursor) {
      return;
    }
    m_viewportCursor = cursor;
    applyViewportCursor();
  }

  void setAssetModelDropsEnabled(bool enabled) {
    if (m_assetModelDropsEnabled == enabled) {
      return;
    }
    m_assetModelDropsEnabled = enabled;
    setAcceptDrops(enabled);
  }

  void setMouseLocked(bool locked) {
    if (m_mouseLocked == locked) {
      return;
    }
    m_mouseLocked = locked;
    m_ignoreNextLockedMouseMove = false;
    if (m_mouseLocked) {
      setFocus(Qt::MouseFocusReason);
      if (!m_mouseGrabbed) {
        grabMouse();
        m_mouseGrabbed = true;
      }
      m_forceClosedHandCursor = false;
      centerLockedMouse();
    } else if (m_mouseGrabbed && QGuiApplication::mouseButtons() == Qt::NoButton) {
      releaseMouse();
      m_mouseGrabbed = false;
    }
    applyViewportCursor();
  }

  void clearFrameOnUiThread() {
    dropUnpresentedFrame();
    m_frame = QImage();
    m_frameId = 0u;
    m_frameGeneration = 0u;
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
                              std::uint64_t generation,
                              std::uint64_t frameId) {
    const auto _vkp_deliver_t0_ns = vkpt::core::metrics::UiInputLatencyNowNs();
    if (src == nullptr || !FitsQtImageDimensions(width, height)) {
      clearFrameOnUiThread();
      VKP_METRIC_INC("vkp.ui.frame_dropped_by_ui_total");
      return false;
    }

    std::size_t required = 0u;
    if (!CheckedRgbaByteCount(width, height, required) ||
        srcBytes < required) {
      clearFrameOnUiThread();
      VKP_METRIC_INC("vkp.ui.frame_dropped_by_ui_total");
      return false;
    }

    const int imageWidth = static_cast<int>(width);
    const int imageHeight = static_cast<int>(height);
    if (m_frame.width() != imageWidth || m_frame.height() != imageHeight ||
        m_frame.format() != QImage::Format_RGBA8888) {
      m_frame = QImage(imageWidth, imageHeight, QImage::Format_RGBA8888);
    }
    if (m_frame.isNull()) {
      VKP_METRIC_INC("vkp.ui.frame_dropped_by_ui_total");
      return false;
    }

    const std::size_t srcStride = width * 4u;
    const auto dstStride = m_frame.bytesPerLine();
    if (dstStride < 0 || static_cast<std::size_t>(dstStride) < srcStride) {
      clearFrameOnUiThread();
      VKP_METRIC_INC("vkp.ui.frame_dropped_by_ui_total");
      return false;
    }

    dropUnpresentedFrame();
    for (std::size_t y = 0; y < height; ++y) {
      const auto* srcRow = src + static_cast<std::size_t>(y) * srcStride;
      auto* dstRow = m_frame.scanLine(static_cast<int>(y));
      std::memcpy(dstRow, srcRow, srcStride);
    }

    m_frameId = frameId;
    m_frameGeneration = generation;
    m_frameWidth = width;
    m_frameHeight = height;
    m_framePresented = false;
    m_dirty = true;
    update();
    const auto _vkp_deliver_t1_ns = vkpt::core::metrics::UiInputLatencyNowNs();
    VKP_METRIC_OBSERVE("vkp.ui.frame_deliver_us",
                       (_vkp_deliver_t1_ns - _vkp_deliver_t0_ns) / 1000u);
    VKP_METRIC_INC("vkp.ui.frame_delivered_total");
    return true;
  }

 protected:
  void paintEvent(QPaintEvent* /*event*/) override {
    const auto _vkp_paint_t0_ns = vkpt::core::metrics::UiInputLatencyNowNs();
    VKP_METRIC_INC("vkp.ui.repaint_total");
    QPainter painter(this);
    painter.fillRect(rect(), QColor(6, 8, 12));

    // UI thread rule: painting only draws the most recent immutable display image
    // and overlay. Rendering, resolving, and handoff publication happen elsewhere.
    if (!m_frame.isNull()) {
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

    if (!m_scriptOverlayPanels.empty()) {
      painter.setRenderHint(QPainter::Antialiasing, true);
      painter.setRenderHint(QPainter::TextAntialiasing, true);
      drawScriptOverlayPanels(painter);
    }

    if (!m_frame.isNull() && !m_framePresented) {
      m_framePresented = true;
      if (m_owner != nullptr) {
        m_owner->record_frame_presented(m_frameId, m_frameGeneration, m_frameWidth, m_frameHeight);
      }
    }
    if (m_owner != nullptr) {
      m_owner->record_canvas_painted(m_frameId, m_frameGeneration, m_frameWidth, m_frameHeight);
    }

    m_dirty = false;
    const auto _vkp_paint_t1_ns = vkpt::core::metrics::UiInputLatencyNowNs();
    VKP_METRIC_OBSERVE("vkp.ui.paint_us", (_vkp_paint_t1_ns - _vkp_paint_t0_ns) / 1000u);
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
      if (m_mouseLocked) {
        const QPoint center = lockedMouseCenter();
        const float dx = static_cast<float>(pos.x() - center.x());
        const float dy = static_cast<float>(pos.y() - center.y());
        if (m_ignoreNextLockedMouseMove && std::fabs(dx) < 0.5f && std::fabs(dy) < 0.5f) {
          m_ignoreNextLockedMouseMove = false;
          event->accept();
          return;
        }
        m_ignoreNextLockedMouseMove = false;
        if (std::fabs(dx) >= 0.5f || std::fabs(dy) >= 0.5f) {
          m_owner->emit_mouse_move_delta(center.x(), center.y(), dx, dy);
          centerLockedMouse();
        }
        event->accept();
        return;
      }
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
    if (!m_mouseLocked && (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton)) {
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
    if (!m_mouseLocked && event->buttons() == Qt::NoButton) {
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

  void dragEnterEvent(QDragEnterEvent* event) override {
    if (m_assetModelDropsEnabled && event != nullptr &&
        acceptsAssetModelDrop(event->mimeData())) {
      event->acceptProposedAction();
      return;
    }
    QWidget::dragEnterEvent(event);
  }

  void dragMoveEvent(QDragMoveEvent* event) override {
    if (m_assetModelDropsEnabled && event != nullptr &&
        acceptsAssetModelDrop(event->mimeData())) {
      event->acceptProposedAction();
      return;
    }
    QWidget::dragMoveEvent(event);
  }

  void dropEvent(QDropEvent* event) override {
    if (!m_assetModelDropsEnabled || event == nullptr || m_owner == nullptr ||
        !acceptsAssetModelDrop(event->mimeData())) {
      QWidget::dropEvent(event);
      return;
    }
    const QString payload =
        QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kQtAssetMimeType)));
    QStringList rows = payload.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    if (rows.empty()) {
      QWidget::dropEvent(event);
      return;
    }
    std::vector<std::string> rowIds;
    rowIds.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows) {
      const auto text = ToUtf8String(row.trimmed());
      if (text.starts_with("asset.model.") &&
          std::find(rowIds.begin(), rowIds.end(), text) == rowIds.end()) {
        rowIds.push_back(text);
      }
    }
    if (rowIds.empty()) {
      QWidget::dropEvent(event);
      return;
    }
    const auto primaryRowId = rowIds.front();
    if (primaryRowId.empty()) {
      QWidget::dropEvent(event);
      return;
    }
    const QPointF pos = event->position();
    m_owner->emit_mouse_move(static_cast<int>(pos.x()), static_cast<int>(pos.y()));
    m_owner->emit_dock_row_activation("asset_browser",
                                      primaryRowId,
                                      0u,
                                      false,
                                      false,
                                      true,
                                      static_cast<float>(pos.x()),
                                      static_cast<float>(pos.y()),
                                      {},
                                      std::move(rowIds));
    event->acceptProposedAction();
  }

 private:
  static bool acceptsAssetModelDrop(const QMimeData* mime) {
    if (mime == nullptr || !mime->hasFormat(QString::fromLatin1(kQtAssetMimeType))) {
      return false;
    }
    const QString payload = QString::fromUtf8(mime->data(QString::fromLatin1(kQtAssetMimeType)));
    const auto rows = payload.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    return std::any_of(rows.begin(), rows.end(), [](const QString& row) {
      return row.trimmed().startsWith(QStringLiteral("asset.model."));
    });
  }

  void releaseMouseGrab() {
    if (m_mouseGrabbed) {
      releaseMouse();
      m_mouseGrabbed = false;
    }
    m_mouseLocked = false;
    m_ignoreNextLockedMouseMove = false;
    m_forceClosedHandCursor = false;
    applyViewportCursor();
  }

  void applyViewportCursor() {
    if (m_mouseLocked) {
      setCursor(Qt::BlankCursor);
      return;
    }
    if (m_forceClosedHandCursor) {
      setCursor(Qt::ClosedHandCursor);
      return;
    }
    setCursor(CursorForViewportCursor(m_viewportCursor));
  }

  QPoint lockedMouseCenter() const {
    return QPoint(std::max(0, width() / 2), std::max(0, height() / 2));
  }

  void centerLockedMouse() {
    if (!isVisible()) {
      return;
    }
    m_ignoreNextLockedMouseMove = true;
    QCursor::setPos(mapToGlobal(lockedMouseCenter()));
  }

  void dropUnpresentedFrame() {
    if (!m_frame.isNull() && !m_framePresented && m_frameId != 0u && m_owner != nullptr) {
      m_owner->record_frame_dropped(m_frameId);
    }
    m_framePresented = true;
  }

  QRectF scriptPanelRect(const QtScriptOverlayPanel& panel, double panelHeight) const {
    const QRectF viewportRect = frameDisplayRect();
    const double margin = 8.0;
    const double width = std::clamp<double>(
        std::isfinite(panel.width) && panel.width > 0.0f ? panel.width : 320.0f,
        160.0,
        std::max(160.0, viewportRect.width() - margin * 2.0));
    const double height = std::clamp<double>(
        panelHeight,
        44.0,
        std::max(44.0, viewportRect.height() - margin * 2.0));
    const double offsetX = std::isfinite(panel.x) ? std::max(0.0f, panel.x) : 16.0f;
    const double offsetY = std::isfinite(panel.y) ? std::max(0.0f, panel.y) : 16.0f;
    double x = viewportRect.left() + offsetX;
    double y = viewportRect.top() + offsetY;
    const std::string anchor = panel.anchor.empty() ? std::string("top_left") : panel.anchor;
    if (anchor == "top_right") {
      x = viewportRect.right() - width - offsetX;
    } else if (anchor == "bottom_left") {
      y = viewportRect.bottom() - height - offsetY;
    } else if (anchor == "bottom_right") {
      x = viewportRect.right() - width - offsetX;
      y = viewportRect.bottom() - height - offsetY;
    } else if (anchor == "center") {
      const double centerOffsetX = std::isfinite(panel.x) ? panel.x : 0.0f;
      const double centerOffsetY = std::isfinite(panel.y) ? panel.y : 0.0f;
      x = viewportRect.left() + (viewportRect.width() - width) * 0.5 + centerOffsetX;
      y = viewportRect.top() + (viewportRect.height() - height) * 0.5 + centerOffsetY;
    }
    const double minX = viewportRect.left() + margin;
    const double minY = viewportRect.top() + margin;
    const double maxX = std::max(minX, viewportRect.right() - width - margin);
    const double maxY = std::max(minY, viewportRect.bottom() - height - margin);
    x = std::clamp<double>(x, minX, maxX);
    y = std::clamp<double>(y, minY, maxY);
    return QRectF(x, y, width, height);
  }

  void drawScriptOverlayPanels(QPainter& painter) {
    for (const auto& panel : m_scriptOverlayPanels) {
      const double fontSize = std::clamp<double>(
          std::isfinite(panel.font_size) ? panel.font_size : 13.0f,
          8.0,
          32.0);
      QFont titleFont = painter.font();
      titleFont.setPointSizeF(fontSize + 1.0);
      titleFont.setBold(true);
      QFont bodyFont = painter.font();
      bodyFont.setPointSizeF(fontSize);

      QFontMetricsF titleMetrics(titleFont);
      QFontMetricsF bodyMetrics(bodyFont);
      const double padding = 12.0;
      const double titleHeight = panel.title.empty() ? 0.0 : titleMetrics.height();
      const double titleGap = panel.title.empty() || panel.lines.empty() ? 0.0 : 7.0;
      const double lineHeight = std::max(15.0, bodyMetrics.lineSpacing());
      const double autoHeight = padding * 2.0 + titleHeight + titleGap +
                                static_cast<double>(panel.lines.size()) * lineHeight;
      const double requestedHeight =
          std::isfinite(panel.height) && panel.height > 0.0f ? panel.height : autoHeight;
      const QRectF panelRect = scriptPanelRect(panel, requestedHeight);
      if (!panelRect.isValid() || panelRect.width() < 24.0 || panelRect.height() < 24.0) {
        continue;
      }

      const float opacity = std::isfinite(panel.opacity) ? panel.opacity : 0.84f;
      const int alpha = std::clamp<int>(
          static_cast<int>(std::round(std::clamp<float>(opacity, 0.0f, 1.0f) * 255.0f)),
          0,
          255);
      QPainterPath shadowPath;
      shadowPath.addRoundedRect(panelRect.translated(0.0, 2.0), 8.0, 8.0);
      painter.fillPath(shadowPath, QColor(0, 0, 0, std::min(90, alpha)));

      QPainterPath panelPath;
      panelPath.addRoundedRect(panelRect, 8.0, 8.0);
      painter.fillPath(panelPath,
                       QColor(panel.background_r,
                              panel.background_g,
                              panel.background_b,
                              alpha));
      painter.setPen(QPen(QColor(panel.accent_r,
                                 panel.accent_g,
                                 panel.accent_b,
                                 std::min(220, alpha + 35)),
                          1.2));
      painter.drawPath(panelPath);
      painter.fillRect(QRectF(panelRect.left(),
                              panelRect.top() + 8.0,
                              3.0,
                              std::max(0.0, panelRect.height() - 16.0)),
                       QColor(panel.accent_r, panel.accent_g, panel.accent_b, std::min(230, alpha + 45)));

      const QRectF inner = panelRect.adjusted(padding, padding, -padding, -padding);
      double cursorY = inner.top();
      if (!panel.title.empty()) {
        painter.setFont(titleFont);
        painter.setPen(QColor(panel.foreground_r, panel.foreground_g, panel.foreground_b, 245));
        const auto title = titleMetrics.elidedText(ToQString(panel.title), Qt::ElideRight, inner.width());
        painter.drawText(QRectF(inner.left(), cursorY, inner.width(), titleHeight + 2.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         title);
        cursorY += titleHeight + titleGap;
      }

      painter.setFont(bodyFont);
      painter.setPen(QColor(panel.foreground_r, panel.foreground_g, panel.foreground_b, 226));
      for (const auto& line : panel.lines) {
        if (cursorY + lineHeight > inner.bottom() + 1.0) {
          break;
        }
        const auto text = bodyMetrics.elidedText(ToQString(line), Qt::ElideRight, inner.width());
        painter.drawText(QRectF(inner.left(), cursorY, inner.width(), lineHeight + 2.0),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         text);
        cursorY += lineHeight;
      }
    }
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
  std::uint64_t m_frameGeneration = 0u;
  std::size_t m_frameWidth = 0u;
  std::size_t m_frameHeight = 0u;
  bool m_framePresented = true;
  QString m_overlayText;
  std::vector<QtSelectionOverlayBox> m_overlayBoxes;
  std::vector<QtScriptOverlayPanel> m_scriptOverlayPanels;
  QtViewportCursor m_viewportCursor = QtViewportCursor::Default;
  bool m_dirty = false;
  bool m_assetModelDropsEnabled = true;
  bool m_mouseGrabbed = false;
  bool m_mouseLocked = false;
  bool m_ignoreNextLockedMouseMove = false;
  bool m_forceClosedHandCursor = false;
};
