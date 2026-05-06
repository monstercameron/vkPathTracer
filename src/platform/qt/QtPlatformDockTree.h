#pragma once

#include <string>

class QTreeWidget;
class QWidget;

namespace vkpt::platform {

class QtWindow;

constexpr const char* kQtAssetMimeType = "application/x-vkpt-asset-row";
constexpr const char* kQtSceneEntityMimeType = "application/x-vkpt-scene-entity-row";

QTreeWidget* CreateQtDockTreeWidget(QtWindow* owner,
                                    std::string panel_id,
                                    QWidget* parent);

}  // namespace vkpt::platform
