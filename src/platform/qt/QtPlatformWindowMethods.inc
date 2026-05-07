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
  generation = 0u;
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
    QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
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

void QtWindow::set_script_overlay_panels(const std::vector<QtScriptOverlayPanel>& panels) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setScriptOverlayPanels(panels);
  }
}

void QtWindow::set_viewport_cursor(QtViewportCursor cursor) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setViewportCursor(cursor);
  }
}

void QtWindow::set_viewport_asset_drops_enabled(bool enabled) {
  if (m_widget != nullptr) {
    static_cast<QtViewportWindow*>(m_widget)->setAssetModelDropsEnabled(enabled);
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

bool QtWindow::request_render_settings(QtRenderDialogSettings& settings) {
  if (m_shell == nullptr) {
    return false;
  }

  QDialog dialog(static_cast<QWidget*>(m_shell));
  dialog.setWindowTitle(QStringLiteral("Start Render"));
  dialog.setModal(true);

  auto* layout = new QVBoxLayout(&dialog);
  auto* form = new QFormLayout();

  auto* width = new QSpinBox(&dialog);
  width->setRange(16, 16384);
  width->setSingleStep(64);
  width->setValue(static_cast<int>(settings.width));

  auto* height = new QSpinBox(&dialog);
  height->setRange(16, 16384);
  height->setSingleStep(64);
  height->setValue(static_cast<int>(settings.height));

  auto* imageSize = new QComboBox(&dialog);
  imageSize->addItems(QStringList{QStringLiteral("HD 1280x720"),
                                  QStringLiteral("Full HD 1920x1080"),
                                  QStringLiteral("QHD 2560x1440"),
                                  QStringLiteral("4K UHD 3840x2160"),
                                  QStringLiteral("Custom")});
  imageSize->setCurrentIndex(1);

  auto* aspect = new QComboBox(&dialog);
  aspect->addItems(QStringList{QStringLiteral("16:9"),
                               QStringLiteral("4:3"),
                               QStringLiteral("3:2"),
                               QStringLiteral("1:1"),
                               QStringLiteral("21:9"),
                               QStringLiteral("Custom")});
  const int aspectIndex = aspect->findText(ToQString(settings.aspect_ratio));
  aspect->setCurrentIndex(aspectIndex >= 0 ? aspectIndex : 0);

  auto* budget = new QComboBox(&dialog);
  budget->addItems(QStringList{QStringLiteral("Quick"),
                               QStringLiteral("Normal"),
                               QStringLiteral("Quality")});
  const int budgetIndex = budget->findText(ToQString(settings.budget));
  budget->setCurrentIndex(budgetIndex >= 0 ? budgetIndex : 1);

  auto* format = new QComboBox(&dialog);
  format->addItems(QStringList{QStringLiteral("png"), QStringLiteral("exr")});
  const int formatIndex = format->findText(ToQString(settings.format));
  format->setCurrentIndex(formatIndex >= 0 ? formatIndex : 0);

  auto* maxRays = new QLineEdit(ToQString(std::to_string(settings.max_rays)), &dialog);
  maxRays->setPlaceholderText(QStringLiteral("67108864"));

  auto* outputRow = new QWidget(&dialog);
  auto* outputLayout = new QHBoxLayout(outputRow);
  outputLayout->setContentsMargins(0, 0, 0, 0);
  auto* outputPath = new QLineEdit(ToQString(settings.output_path), outputRow);
  auto* browse = new QPushButton(QStringLiteral("Browse..."), outputRow);
  outputLayout->addWidget(outputPath, 1);
  outputLayout->addWidget(browse);

  const auto syncImageSizePreset = [&]() {
    const int w = width->value();
    const int h = height->value();
    QSignalBlocker blocker(imageSize);
    if (w == 1280 && h == 720) {
      imageSize->setCurrentIndex(0);
    } else if (w == 1920 && h == 1080) {
      imageSize->setCurrentIndex(1);
    } else if (w == 2560 && h == 1440) {
      imageSize->setCurrentIndex(2);
    } else if (w == 3840 && h == 2160) {
      imageSize->setCurrentIndex(3);
    } else {
      imageSize->setCurrentIndex(4);
    }
  };

  const auto applyImageSizePreset = [&]() {
    const QString preset = imageSize->currentText();
    int presetWidth = 0;
    int presetHeight = 0;
    if (preset == QStringLiteral("HD 1280x720")) {
      presetWidth = 1280;
      presetHeight = 720;
    } else if (preset == QStringLiteral("Full HD 1920x1080")) {
      presetWidth = 1920;
      presetHeight = 1080;
    } else if (preset == QStringLiteral("QHD 2560x1440")) {
      presetWidth = 2560;
      presetHeight = 1440;
    } else if (preset == QStringLiteral("4K UHD 3840x2160")) {
      presetWidth = 3840;
      presetHeight = 2160;
    }
    if (presetWidth <= 0 || presetHeight <= 0) {
      return;
    }
    const QSignalBlocker widthBlocker(width);
    const QSignalBlocker heightBlocker(height);
    width->setValue(presetWidth);
    height->setValue(presetHeight);
  };

  const auto applyAspect = [&]() {
    const QString ratio = aspect->currentText();
    if (ratio == QStringLiteral("Custom")) {
      syncImageSizePreset();
      return;
    }
    const auto parts = ratio.split(QStringLiteral(":"));
    if (parts.size() != 2) {
      return;
    }
    bool okA = false;
    bool okB = false;
    const double a = parts[0].toDouble(&okA);
    const double b = parts[1].toDouble(&okB);
    if (!okA || !okB || a <= 0.0 || b <= 0.0) {
      return;
    }
    const int adjustedHeight = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(width->value()) * b / a)),
        height->minimum(),
        height->maximum());
    const QSignalBlocker blocker(height);
    height->setValue(adjustedHeight);
    syncImageSizePreset();
  };

  const auto applyBudgetRays = [&]() {
    const QString q = budget->currentText();
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(width->value()) * static_cast<std::uint64_t>(height->value());
    std::uint64_t samples = 64ull;
    if (q == QStringLiteral("Quick")) {
      samples = 16ull;
    } else if (q == QStringLiteral("Quality")) {
      samples = 256ull;
    }
    maxRays->setText(ToQString(std::to_string(std::max<std::uint64_t>(pixels, pixels * samples))));
  };

  const auto normalizedOutputWithFormat = [&]() {
    std::filesystem::path path(outputPath->text().toStdString());
    const auto extension = path.extension().string();
    if (extension.empty() || extension == ".png" || extension == ".exr") {
      path.replace_extension(format->currentText().toStdString());
      outputPath->setText(ToQString(path.string()));
    }
  };

  QObject::connect(imageSize, &QComboBox::currentTextChanged, &dialog, applyImageSizePreset);
  QObject::connect(width, qOverload<int>(&QSpinBox::valueChanged), &dialog, applyAspect);
  QObject::connect(height, qOverload<int>(&QSpinBox::valueChanged), &dialog, syncImageSizePreset);
  QObject::connect(aspect, &QComboBox::currentTextChanged, &dialog, applyAspect);
  QObject::connect(budget, &QComboBox::currentTextChanged, &dialog, applyBudgetRays);
  QObject::connect(format, &QComboBox::currentTextChanged, &dialog, normalizedOutputWithFormat);
  QObject::connect(browse, &QPushButton::clicked, &dialog, [&]() {
    const QString filter = format->currentText() == QStringLiteral("exr")
        ? QStringLiteral("OpenEXR (*.exr)")
        : QStringLiteral("PNG Image (*.png)");
    const QString selected = QFileDialog::getSaveFileName(&dialog,
                                                          QStringLiteral("Save Render"),
                                                          outputPath->text(),
                                                          filter);
    if (!selected.isEmpty()) {
      outputPath->setText(selected);
    }
  });

  syncImageSizePreset();

  form->addRow(QStringLiteral("Image Size"), imageSize);
  form->addRow(QStringLiteral("Width"), width);
  form->addRow(QStringLiteral("Height"), height);
  form->addRow(QStringLiteral("Aspect Ratio"), aspect);
  form->addRow(QStringLiteral("Render Budget"), budget);
  form->addRow(QStringLiteral("Image Format"), format);
  form->addRow(QStringLiteral("Max Rays"), maxRays);
  form->addRow(QStringLiteral("File Path"), outputRow);
  layout->addLayout(form);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Render"));
  layout->addWidget(buttons);

  QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
    bool raysOk = false;
    const auto rays = maxRays->text().trimmed().toULongLong(&raysOk);
    if (!raysOk || rays == 0ull) {
      QMessageBox::warning(&dialog,
                           QStringLiteral("Start Render"),
                           QStringLiteral("Max rays must be a positive integer."));
      return;
    }
    if (outputPath->text().trimmed().isEmpty()) {
      QMessageBox::warning(&dialog,
                           QStringLiteral("Start Render"),
                           QStringLiteral("Choose a file path for the render."));
      return;
    }
    settings.width = static_cast<std::uint32_t>(width->value());
    settings.height = static_cast<std::uint32_t>(height->value());
    settings.max_rays = static_cast<std::uint64_t>(rays);
    settings.aspect_ratio = aspect->currentText().toStdString();
    settings.budget = budget->currentText().toStdString();
    settings.format = format->currentText().toStdString();
    settings.output_path = outputPath->text().trimmed().toStdString();
    dialog.accept();
  });

  return dialog.exec() == QDialog::Accepted;
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
  set_framebuffer_rgba_impl(std::vector<std::uint8_t>(rgba.begin(), rgba.end()), width, height, 0u);
}

void QtWindow::set_framebuffer_rgba(std::vector<std::uint8_t>&& rgba,
                                    std::size_t width,
                                    std::size_t height) {
  set_framebuffer_rgba_impl(std::move(rgba), width, height, 0u);
}

void QtWindow::set_framebuffer_rgba(const std::vector<std::uint8_t>& rgba,
                                    std::size_t width,
                                    std::size_t height,
                                    std::uint64_t generation) {
  set_framebuffer_rgba_impl(std::vector<std::uint8_t>(rgba.begin(), rgba.end()),
                            width,
                            height,
                            generation);
}

void QtWindow::set_framebuffer_rgba(std::vector<std::uint8_t>&& rgba,
                                    std::size_t width,
                                    std::size_t height,
                                    std::uint64_t generation) {
  set_framebuffer_rgba_impl(std::move(rgba), width, height, generation);
}

void QtWindow::set_framebuffer_rgba_impl(std::vector<std::uint8_t> frame,
                                         std::size_t width,
                                         std::size_t height,
                                         std::uint64_t generation) {
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
    m_pendingFramebuffer.generation = generation;
    m_pendingFramebuffer.width = width;
    m_pendingFramebuffer.height = height;
    m_pendingFramebuffer.rgba = std::move(frame);
    m_pendingFramebufferKind = PendingFramebufferKind::Frame;

    ++m_framebufferStats.received;
    m_framebufferStats.latestPublishedId = frameId;
    m_framebufferStats.latestPublishedGeneration = generation;
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
                                        frame.generation,
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
                                      std::uint64_t generation,
                                      std::size_t width,
                                      std::size_t height) {
  QtFramebufferStats stats{};
  bool firstPresented = false;
  bool periodicStats = false;

  {
    std::scoped_lock lock(m_frameMutex);
    ++m_framebufferStats.presented;
    m_framebufferStats.latestPresentedId = frameId;
    m_framebufferStats.latestPresentedGeneration = generation;
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

void QtWindow::record_canvas_painted(std::uint64_t frameId,
                                     std::uint64_t generation,
                                     std::size_t width,
                                     std::size_t height) {
  QtFramebufferStats stats{};
  bool periodicStats = false;

  {
    std::scoped_lock lock(m_frameMutex);
    ++m_framebufferStats.painted;
    m_framebufferStats.latestPaintedId = frameId;
    m_framebufferStats.latestPaintedGeneration = generation;
    m_framebufferStats.latestPaintedWidth = width;
    m_framebufferStats.latestPaintedHeight = height;
    periodicStats = ShouldLogQtFramebufferStats(m_framebufferStats.painted);
    stats = m_framebufferStats;
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
