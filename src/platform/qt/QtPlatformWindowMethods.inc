namespace {

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
  SetStartupSplashPhase(m_startupSplash, ToQString(text));
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

void QtWindow::set_viewport_mouse_locked(bool locked) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setMouseLocked(locked);
  }
}

bool QtWindow::confirm_scene_replacement(std::string_view scene_path) {
  if (m_shell == nullptr) {
    return true;
  }
  QMessageBox dialog(static_cast<QWidget*>(m_shell));
  dialog.setIcon(QMessageBox::Warning);
  dialog.setWindowTitle(QStringLiteral("Open Scene"));
  dialog.setText(QStringLiteral("Open this scene now?"));
  dialog.setInformativeText(
      QStringLiteral("The current runtime scene will be replaced. Unsaved edits in memory will not be preserved unless you saved them first.\n\n%1")
          .arg(ToQString(scene_path)));
  dialog.setStandardButtons(QMessageBox::Cancel | QMessageBox::Open);
  dialog.setDefaultButton(QMessageBox::Cancel);
  return dialog.exec() == QMessageBox::Open;
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
  set_framebuffer_rgba_impl(std::vector<std::uint8_t>(rgba.begin(), rgba.end()), width, height);
}

void QtWindow::set_framebuffer_rgba(std::vector<std::uint8_t>&& rgba,
                                    std::size_t width,
                                    std::size_t height) {
  set_framebuffer_rgba_impl(std::move(rgba), width, height);
}

void QtWindow::set_framebuffer_rgba_impl(std::vector<std::uint8_t> frame,
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
  if (frame.size() < expected) {
    if (request_framebuffer_clear()) {
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          kQtLogSubsystem,
          "cleared short Qt framebuffer upload",
          {
            {"width", std::to_string(width)},
            {"height", std::to_string(height)},
            {"expected_bytes", std::to_string(expected)},
            {"actual_bytes", std::to_string(frame.size())}
          });
    }
    return;
  }
  if (frame.size() != expected) {
    frame.resize(expected);
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

  auto* splash = CreateStartupSplash();
  m_startupSplash = splash;
  m_startupSplashActive = true;
  SetStartupSplashPhase(splash, QStringLiteral("Preparing Qt shell"));
  if (QObject* context = QCoreApplication::instance()) {
    QObject::connect(splash, &QObject::destroyed, context, [this, splash]() {
      if (m_startupSplash == splash) {
        m_startupSplash = nullptr;
      }
      m_startupSplashActive = false;
    });
  }
  ShowStartupSplashCentered(splash, m_shell);

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

  QWidget* splash = m_startupSplash;
  m_startupSplash = nullptr;
  m_startupSplashActive = false;
  FinishStartupSplash(splash, animated);
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

}  // namespace vkpt::platform
