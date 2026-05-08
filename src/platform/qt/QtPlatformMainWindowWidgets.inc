          button->setText(ToQString(property.value.empty() ? property.name : property.value));
          button->setEnabled(property.enabled);
          button->setToolTip(ToQString(property.name));
        }
      } else if (isListProperty(property)) {
        auto* text = qobject_cast<QTextEdit*>(cell);
        if (text != nullptr && !text->hasFocus()) {
          QSignalBlocker textBlocker(text);
          text->setPlainText(ToQString(property.value));
          text->setEnabled(property.enabled);
          text->setToolTip(ToQString(property.name));
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
      auto* tree = CreateQtDockTreeWidget(m_owner, panel.id, root);
      tree->setMinimumWidth(kQtDockMinimumWidth - 24);
      tree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
      tree->setAlternatingRowColors(true);
      tree->setSelectionBehavior(QAbstractItemView::SelectRows);
      tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
      tree->setUniformRowHeights(true);
      const bool sceneTreePanel = panel.id == "scene_graph" || panel.id == "scene_tree";
      const bool hasVisibilityToggles = hasTreeRowVisibilityToggles(panel.tree_rows);
      tree->setDragEnabled(panel.id == "asset_browser" || sceneTreePanel);
      tree->setAcceptDrops(sceneTreePanel);
      tree->setDropIndicatorShown(sceneTreePanel);
      const bool singleColumnTree =
          (panel.tree_single_column && !hasVisibilityToggles) || panel.tree_rows.empty();
      tree->setColumnCount(singleColumnTree ? 1 : 2);
      if (panel.tree_single_column || panel.tree_rows.empty()) {
        tree->setHeaderHidden(true);
      } else {
        tree->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
        tree->header()->setStretchLastSection(true);
      }
      if (hasVisibilityToggles) {
        tree->header()->setStretchLastSection(false);
        tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
        tree->setColumnWidth(1, 32);
      }
      for (const auto& row : panel.rows) {
        auto* item = new QTreeWidgetItem();
        item->setText(0, ToQString(row));
        tree->addTopLevelItem(item);
      }
      for (const auto& row : panel.tree_rows) {
        auto* item = buildTreeItem(row, hasVisibilityToggles);
        tree->addTopLevelItem(item);
        installTreeItemWidgets(tree, row, item, panel.id);
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
      const int treeStretch = panel.tree_stretch >= 0
          ? panel.tree_stretch
          : (wantsProperties ? (treePrimaryPanel ? 1 : 0) : 1);
      addSection(tree, treeStretch);
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
      } else if (panel.property_preferred_height > 0) {
        table->setMinimumHeight(std::min(72, panel.property_preferred_height));
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
                                     isTextProperty(property) ||
                                     isButtonProperty(property) ||
                                     isListProperty(property);
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
          editor->setProperty(kQtPendingSliderEditProperty, false);
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

          auto* debounceTimer = new QTimer(editor);
          debounceTimer->setSingleShot(true);
          auto pendingText = std::make_shared<QString>(formatSliderValue(initialValue, decimals));
          auto pendingEdit = std::make_shared<bool>(false);

          auto updateVisibleValue = [valueItem](const QString& text) {
            if (valueItem != nullptr) {
              valueItem->setText(text);
              valueItem->setToolTip(text);
            }
          };

          auto commitValue = [owner = m_owner,
                              editor,
                              panelId = panel.id,
                              propertyId = property.id,
                              pendingText,
                              pendingEdit]() {
            if (!*pendingEdit) {
              return;
            }
            *pendingEdit = false;
            if (editor != nullptr) {
              editor->setProperty(kQtPendingSliderEditProperty, false);
            }
            if (owner != nullptr && !propertyId.empty()) {
              owner->emit_dock_property_edit(panelId,
                                             propertyId,
                                             ToUtf8String(*pendingText));
            }
          };
          QObject::connect(debounceTimer, &QTimer::timeout, debounceTimer, commitValue);

          auto scheduleValue = [debounceTimer,
                                pendingText,
                                pendingEdit,
                                editor,
                                updateVisibleValue,
                                decimals](double value) {
            const QString text = formatSliderValue(value, decimals);
            *pendingText = text;
            *pendingEdit = true;
            if (editor != nullptr) {
              editor->setProperty(kQtPendingSliderEditProperty, true);
            }
            updateVisibleValue(text);
            debounceTimer->start(kQtSliderEditDebounceMs);
          };

          auto commitNow = [debounceTimer, commitValue]() {
            if (debounceTimer->isActive()) {
              debounceTimer->stop();
            }
            commitValue();
          };

          QObject::connect(slider,
                           &QSlider::valueChanged,
                           slider,
                           [spin, scheduleValue, minimum, maximum](int position) {
                             const double value = valueFromSliderPosition(position, minimum, maximum);
                             {
                               QSignalBlocker blocker(spin);
                               spin->setValue(value);
                             }
                             scheduleValue(value);
                           });
          QObject::connect(slider,
                           &QSlider::sliderReleased,
                           slider,
                           commitNow);
          QObject::connect(spin,
                           QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                           spin,
                           [slider, scheduleValue, minimum, maximum](double value) {
                             {
                               QSignalBlocker blocker(slider);
                               slider->setValue(sliderPositionFromValue(value, minimum, maximum));
                             }
                             scheduleValue(value);
                           });
          QObject::connect(spin,
                           &QDoubleSpinBox::editingFinished,
                           spin,
                           commitNow);
          QObject::connect(reset,
                           &QPushButton::clicked,
                           reset,
                           [slider,
                            spin,
                            scheduleValue,
                            commitNow,
                            resetValue,
                            minimum,
                            maximum]() {
            {
              QSignalBlocker blocker(slider);
              slider->setValue(sliderPositionFromValue(resetValue, minimum, maximum));
            }
            {
              QSignalBlocker blocker(spin);
              spin->setValue(resetValue);
            }
            scheduleValue(resetValue);
            commitNow();
          });

          table->setCellWidget(row, valueColumn, editor);
          table->setRowHeight(row, compactProperties ? 38 : 32);
        } else if (isTextProperty(property)) {
          auto* lineEdit = new QLineEdit(ToQString(property.value), table);
          lineEdit->setEnabled(property.enabled);
          lineEdit->setClearButtonEnabled(true);
          lineEdit->setPlaceholderText(ToQString(property.name));
          lineEdit->setToolTip(ToQString(property.name));
          QObject::connect(lineEdit,
                           &QLineEdit::editingFinished,
                           lineEdit,
                           [owner = m_owner,
                            lineEdit,
                            valueItem,
                            panelId = panel.id,
                            propertyId = property.id]() {
                             const QString text = lineEdit != nullptr ? lineEdit->text() : QString{};
                             if (valueItem != nullptr) {
                               valueItem->setText(text);
                               valueItem->setToolTip(text);
                             }
                             if (owner != nullptr && !propertyId.empty()) {
                               owner->emit_dock_property_edit(panelId,
                                                              propertyId,
                                                              ToUtf8String(text));
                             }
                           });
          table->setCellWidget(row, valueColumn, lineEdit);
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
        } else if (isListProperty(property)) {
          auto* text = new QTextEdit(table);
          text->setReadOnly(!property.enabled);
          text->setPlainText(ToQString(property.value));
          text->setEnabled(property.enabled);
          text->setToolTip(ToQString(property.name));
          text->setFrameShape(QFrame::NoFrame);
          text->setMinimumHeight(compactProperties ? 72 : 64);
          auto* debounceTimer = new QTimer(text);
          debounceTimer->setSingleShot(true);
          QObject::connect(text,
                           &QTextEdit::textChanged,
                           text,
                           [owner = m_owner,
                            text,
                            valueItem,
                            debounceTimer,
                            panelId = panel.id,
                            propertyId = property.id]() {
                             if (owner == nullptr || propertyId.empty() || debounceTimer == nullptr) {
                               return;
                             }
                             debounceTimer->start(kQtSliderEditDebounceMs);
                             if (valueItem != nullptr && text != nullptr) {
                               valueItem->setText(text->toPlainText());
                               valueItem->setToolTip(text->toPlainText());
                             }
                           });
          QObject::connect(debounceTimer,
                           &QTimer::timeout,
                           debounceTimer,
                           [owner = m_owner,
                            text,
                            panelId = panel.id,
                            propertyId = property.id]() {
                             if (owner != nullptr && text != nullptr && !propertyId.empty()) {
                               owner->emit_dock_property_edit(panelId,
                                                              propertyId,
                                                              ToUtf8String(text->toPlainText()));
                             }
                           });
          table->setCellWidget(row, valueColumn, text);
          const int lineCount = 1 + static_cast<int>(
              std::count(property.value.begin(), property.value.end(), '\n'));
          table->setRowHeight(row, std::min(118, 34 + lineCount * 20));
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
      const int propertyStretch = panel.property_stretch >= 0
          ? panel.property_stretch
          : (treePrimaryPanel ? 0 : 1);
      addSection(table, propertyStretch);
    }

    if (splitter != nullptr) {
      layout->addWidget(splitter, 1);
      if (panel.property_preferred_height > 0 && splitter->count() == 2) {
        splitter->setSizes({
            std::max(kQtDockMinimumHeight, panel.preferred_height - panel.property_preferred_height),
            panel.property_preferred_height,
        });
      }
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
