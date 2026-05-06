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
        << (row.selected ? '1' : '0')
        << (row.activatable ? '1' : '0')
        << (row.draggable ? '1' : '0') << ';';
    out << row.children.size() << '[';
    for (const auto& child : row.children) {
      appendRowSignature(out, child);
    }
    out << ']';
  }

  static std::string panelContentSignature(const QtDockPanel& panel) {
    std::ostringstream out;
    out << static_cast<int>(panel.content) << '|';
    out << (panel.tree_single_column ? '1' : '0') << '|'
        << panel.tree_stretch << '|'
        << panel.property_stretch << '|'
        << panel.property_preferred_height << '|';
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
    } else if (icon == "scene") {
      fill = QColor(88, 151, 97);
      glyph = 'S';
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
    if (row.activatable) {
      item->setData(0, Qt::UserRole + 2, true);
    }
    if (row.draggable) {
      item->setData(0, Qt::UserRole + 3, true);
    }
    if (row.entity_id != 0u) {
      item->setData(0, Qt::UserRole + 1, QVariant::fromValue<qulonglong>(
          static_cast<qulonglong>(row.entity_id)));
    } else if (!row.activatable) {
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
    const auto rowId = ToUtf8String(item->data(0, Qt::UserRole).toString());
    const bool activatable = item->data(0, Qt::UserRole + 2).toBool();
    if ((!ok || idValue == 0u) && (!activatable || rowId.empty())) {
      return;
    }
    const Qt::KeyboardModifiers modifiers = QApplication::keyboardModifiers();
    const bool append =
        modifiers.testFlag(Qt::ControlModifier) ||
        modifiers.testFlag(Qt::MetaModifier);
    const bool rangeMode = modifiers.testFlag(Qt::ShiftModifier);
    if (panelId == "asset_browser" &&
        rowId.starts_with("asset.model.") &&
        (append || rangeMode)) {
      return;
    }
    std::vector<std::string> rowIds;
    if (panelId == "asset_browser" && rowId.starts_with("asset.model.")) {
      if (QTreeWidget* tree = item->treeWidget()) {
        const auto selected = tree->selectedItems();
        for (QTreeWidgetItem* selectedItem : selected) {
          if (selectedItem == nullptr) {
            continue;
          }
          const auto selectedRow = ToUtf8String(selectedItem->data(0, Qt::UserRole).toString());
          if (selectedRow.starts_with("asset.model.") &&
              std::find(rowIds.begin(), rowIds.end(), selectedRow) == rowIds.end()) {
            rowIds.push_back(selectedRow);
          }
        }
      }
      if (rowIds.empty()) {
        rowIds.push_back(rowId);
      }
    }
    m_owner->emit_dock_row_activation(panelId,
                                      rowId,
                                      ok ? static_cast<vkpt::core::StableId>(idValue) : 0u,
                                      append,
                                      rangeMode,
                                      false,
                                      0.0f,
                                      0.0f,
                                      {},
                                      std::move(rowIds));
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
      QWidget* cell = table->cellWidget(row, valueColumn);
      const bool sliderEditPending =
          isSliderProperty(property) &&
          cell != nullptr &&
          cell->property(kQtPendingSliderEditProperty).toBool();
      if (auto* valueItem = table->item(row, valueColumn)) {
        if (!sliderEditPending) {
          valueItem->setText(ToQString(property.value));
          valueItem->setToolTip(ToQString(property.value));
        }
        valueItem->setData(Qt::UserRole, ToQString(property.id));
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
          if (!sliderEditPending) {
            slider->setValue(sliderPositionFromValue(value, minimum, maximum));
          }
          slider->setEnabled(property.enabled);
          slider->setToolTip(ToQString(property.name));
        }
        if (spin != nullptr) {
          QSignalBlocker spinBlocker(spin);
          spin->setRange(minimum, maximum);
          spin->setSingleStep(property.step > 0.0 ? property.step : 0.01);
          spin->setDecimals(decimalsForStep(property.step));
          if (!sliderEditPending) {
            spin->setValue(value);
          }
          spin->setEnabled(property.enabled);
          spin->setToolTip(ToQString(property.name));
        }
      } else if (isButtonProperty(property)) {
        auto* button = qobject_cast<QPushButton*>(cell);
        if (button != nullptr) {
