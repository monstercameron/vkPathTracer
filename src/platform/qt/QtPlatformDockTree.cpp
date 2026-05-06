#include "platform/qt/QtPlatformDockTree.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "platform/qt/QtPlatform.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPoint>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace vkpt::platform {

namespace {

std::string ToUtf8String(const QString& text) {
  const QByteArray bytes = text.toUtf8();
  return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

class QtDockTreeWidget final : public QTreeWidget {
 public:
  explicit QtDockTreeWidget(QtWindow* owner, std::string panelId, QWidget* parent = nullptr)
      : QTreeWidget(parent), m_owner(owner), m_panelId(std::move(panelId)) {}

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    m_dragAssetRows.clear();
    m_dragSceneEntityIds.clear();
    if (event != nullptr && event->button() == Qt::LeftButton) {
      m_dragStartPos = event->pos();
      QTreeWidgetItem* item = itemAt(m_dragStartPos);
      if (m_panelId == "asset_browser" && item != nullptr && item->isSelected()) {
        m_dragAssetRows = selectedAssetModelRows(item);
      } else if (isSceneTreePanel() && item != nullptr && item->isSelected()) {
        m_dragSceneEntityIds = selectedSceneEntityIds(item);
      }
    }
    QTreeWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (event == nullptr ||
        (event->buttons() & Qt::LeftButton) == 0 ||
        (event->pos() - m_dragStartPos).manhattanLength() < QApplication::startDragDistance()) {
      QTreeWidget::mouseMoveEvent(event);
      return;
    }

    QTreeWidgetItem* item = itemAt(m_dragStartPos);
    if (item == nullptr) {
      QTreeWidget::mouseMoveEvent(event);
      return;
    }

    if (m_panelId == "asset_browser") {
      const auto rows = !m_dragAssetRows.empty() ? m_dragAssetRows : selectedAssetModelRows(item);
      if (rows.empty()) {
        QTreeWidget::mouseMoveEvent(event);
        return;
      }

      auto* drag = new QDrag(this);
      auto* mime = new QMimeData();
      const auto payload = JoinLines(rows);
      if (payload.isEmpty()) {
        QTreeWidget::mouseMoveEvent(event);
        return;
      }
      mime->setData(QString::fromLatin1(kQtAssetMimeType), payload.toUtf8());
      mime->setText(rows.front().mid(QStringLiteral("asset.model.").size()));
      drag->setMimeData(mime);
      drag->exec(Qt::CopyAction);
      event->accept();
      return;
    }

    if (!isSceneTreePanel()) {
      QTreeWidget::mouseMoveEvent(event);
      return;
    }

    const auto entityIds = !m_dragSceneEntityIds.empty()
        ? m_dragSceneEntityIds
        : selectedSceneEntityIds(item);
    if (entityIds.empty()) {
      QTreeWidget::mouseMoveEvent(event);
      return;
    }
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData();
    mime->setData(QString::fromLatin1(kQtSceneEntityMimeType), JoinIds(entityIds).toUtf8());
    mime->setText(QStringLiteral("Scene graph item"));
    drag->setMimeData(mime);
    drag->exec(Qt::MoveAction);
    event->accept();
  }

  void dragEnterEvent(QDragEnterEvent* event) override {
    if (event != nullptr && acceptsSceneEntityDrop(event->mimeData())) {
      event->acceptProposedAction();
      return;
    }
    QTreeWidget::dragEnterEvent(event);
  }

  void dragMoveEvent(QDragMoveEvent* event) override {
    if (event != nullptr && acceptsSceneEntityDrop(event->mimeData()) &&
        hasSceneObjectId(itemAt(event->position().toPoint()))) {
      event->acceptProposedAction();
      return;
    }
    QTreeWidget::dragMoveEvent(event);
  }

  void dropEvent(QDropEvent* event) override {
    if (event == nullptr || m_owner == nullptr || !acceptsSceneEntityDrop(event->mimeData())) {
      QTreeWidget::dropEvent(event);
      return;
    }
    QTreeWidgetItem* targetItem = itemAt(event->position().toPoint());
    if (!hasSceneObjectId(targetItem)) {
      QTreeWidget::dropEvent(event);
      return;
    }
    const auto entityIds = ParseEntityIds(QString::fromUtf8(
        event->mimeData()->data(QString::fromLatin1(kQtSceneEntityMimeType))));
    if (entityIds.empty()) {
      QTreeWidget::dropEvent(event);
      return;
    }
    bool ok = false;
    const auto targetValue = targetItem->data(0, Qt::UserRole + 1).toULongLong(&ok);
    if (!ok || targetValue == 0u) {
      QTreeWidget::dropEvent(event);
      return;
    }
    const auto firstId = entityIds.empty() ? 0u : entityIds.front();
    if (firstId == 0u) {
      QTreeWidget::dropEvent(event);
      return;
    }
    m_owner->emit_dock_row_activation(m_panelId,
                                      "entity." + std::to_string(firstId),
                                      firstId,
                                      false,
                                      false,
                                      false,
                                      0.0f,
                                      0.0f,
                                      "reparent",
                                      {},
                                      entityIds,
                                      static_cast<vkpt::core::StableId>(targetValue));
    event->acceptProposedAction();
  }

  void keyPressEvent(QKeyEvent* event) override {
    if (event != nullptr &&
        (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) &&
        emitDeleteForSelection(currentItem())) {
      event->accept();
      return;
    }
    QTreeWidget::keyPressEvent(event);
  }

  void contextMenuEvent(QContextMenuEvent* event) override {
    if (event == nullptr || !isSceneTreePanel()) {
      QTreeWidget::contextMenuEvent(event);
      return;
    }

    QTreeWidgetItem* item = itemAt(event->pos());
    if (!hasSceneObjectId(item)) {
      QTreeWidget::contextMenuEvent(event);
      return;
    }

    setCurrentItem(item);
    QMenu menu(this);
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == deleteAction) {
      emitDeleteForSelection(item);
    }
    event->accept();
  }

 private:
  bool isSceneTreePanel() const {
    return m_panelId == "scene_graph" || m_panelId == "scene_tree";
  }

  static QString JoinLines(const std::vector<QString>& lines) {
    QStringList out;
    for (const auto& line : lines) {
      if (!line.isEmpty()) {
        out.push_back(line);
      }
    }
    return out.join(QStringLiteral("\n"));
  }

  static QString JoinIds(const std::vector<vkpt::core::StableId>& ids) {
    QStringList out;
    for (const auto id : ids) {
      if (id != 0u) {
        out.push_back(QString::number(static_cast<qulonglong>(id)));
      }
    }
    return out.join(QStringLiteral("\n"));
  }

  static std::vector<vkpt::core::StableId> ParseEntityIds(const QString& text) {
    std::vector<vkpt::core::StableId> ids;
    const auto lines = text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    for (const auto& line : lines) {
      bool ok = false;
      const auto value = line.trimmed().toULongLong(&ok);
      if (ok && value != 0u) {
        ids.push_back(static_cast<vkpt::core::StableId>(value));
      }
    }
    return ids;
  }

  static bool itemIsAssetModel(const QTreeWidgetItem* item) {
    if (item == nullptr || !item->data(0, Qt::UserRole + 3).toBool()) {
      return false;
    }
    return item->data(0, Qt::UserRole).toString().startsWith(QStringLiteral("asset.model."));
  }

  static bool hasSceneObjectId(const QTreeWidgetItem* item) {
    if (item == nullptr) {
      return false;
    }
    bool ok = false;
    const auto idValue = item->data(0, Qt::UserRole + 1).toULongLong(&ok);
    return ok && idValue != 0u;
  }

  std::vector<QString> selectedAssetModelRows(QTreeWidgetItem* fallback) const {
    std::vector<QString> rows;
    const auto selected = selectedItems();
    for (QTreeWidgetItem* item : selected) {
      if (itemIsAssetModel(item)) {
        const auto rowId = item->data(0, Qt::UserRole).toString();
        if (std::find(rows.begin(), rows.end(), rowId) == rows.end()) {
          rows.push_back(rowId);
        }
      }
    }
    if (rows.empty() && itemIsAssetModel(fallback)) {
      rows.push_back(fallback->data(0, Qt::UserRole).toString());
    }
    return rows;
  }

  std::vector<vkpt::core::StableId> selectedSceneEntityIds(QTreeWidgetItem* fallback) const {
    std::vector<vkpt::core::StableId> ids;
    auto addItem = [&](QTreeWidgetItem* item) {
      if (item == nullptr || !item->data(0, Qt::UserRole + 3).toBool()) {
        return;
      }
      bool ok = false;
      const auto value = item->data(0, Qt::UserRole + 1).toULongLong(&ok);
      if (!ok || value == 0u) {
        return;
      }
      const auto id = static_cast<vkpt::core::StableId>(value);
      if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
        ids.push_back(id);
      }
    };
    const auto selected = selectedItems();
    for (QTreeWidgetItem* item : selected) {
      addItem(item);
    }
    if (ids.empty()) {
      addItem(fallback);
    }
    return ids;
  }

  bool acceptsSceneEntityDrop(const QMimeData* mime) const {
    return isSceneTreePanel() &&
           mime != nullptr &&
           mime->hasFormat(QString::fromLatin1(kQtSceneEntityMimeType));
  }

  bool emitDeleteForSelection(QTreeWidgetItem* fallback) {
    if (m_owner == nullptr || !isSceneTreePanel()) {
      return false;
    }
    const auto ids = selectedSceneEntityIds(fallback);
    if (ids.empty()) {
      return false;
    }
    const auto firstId = ids.front();
    m_owner->emit_dock_row_activation(m_panelId,
                                      fallback == nullptr
                                          ? "entity." + std::to_string(firstId)
                                          : ToUtf8String(fallback->data(0, Qt::UserRole).toString()),
                                      firstId,
                                      false,
                                      false,
                                      false,
                                      0.0f,
                                      0.0f,
                                      "delete",
                                      {},
                                      ids);
    return true;
  }

  QtWindow* m_owner = nullptr;
  std::string m_panelId;
  QPoint m_dragStartPos;
  std::vector<QString> m_dragAssetRows;
  std::vector<vkpt::core::StableId> m_dragSceneEntityIds;
};

}  // namespace

QTreeWidget* CreateQtDockTreeWidget(QtWindow* owner,
                                    std::string panel_id,
                                    QWidget* parent) {
  return new QtDockTreeWidget(owner, std::move(panel_id), parent);
}

}  // namespace vkpt::platform
