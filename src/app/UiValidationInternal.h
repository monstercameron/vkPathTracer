#pragma once

#include "editor/UiModels.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::scene {
class SceneWorld;
}

namespace vkpt::app {

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index = 0,
                 std::string_view old_value = {},
                 std::string_view new_value = {},
                 std::string_view command_result = {});
bool HasTopLevelMenu(const vkpt::editor::MenuBar& menu, std::string_view id);
bool HasMenuItem(const vkpt::editor::MenuBar& menu,
                 std::string_view top_level_id,
                 std::string_view action_id);

using UiSmokeCheckFn = std::function<void(std::string_view, bool)>;

std::vector<vkpt::editor::SceneTreeEntityModel> BuildSceneTreeEntitiesFromWorld(
    const vkpt::scene::SceneWorld& world);
void RunUiAssetDropSmokeChecks(const UiSmokeCheckFn& check_true);
void RunUiBenchmarkStatusSmokeChecks(const UiSmokeCheckFn& check_true,
                                     const vkpt::editor::SelectionState& selection);
void RunUiCameraAndQtDockSmokeChecks(const UiSmokeCheckFn& check_true);
void RunUiSceneTreeSmokeChecks(const UiSmokeCheckFn& check_true);
bool CheckEcsSceneTreeContracts(std::string* detail = nullptr);

}  // namespace vkpt::app
