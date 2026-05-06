#pragma once

#include <vector>

#include "core/Types.h"
#include "editor/UiModels.h"

namespace vkpt::app {

std::vector<vkpt::core::StableId> SortedUniqueEntityIds(
    std::vector<vkpt::core::StableId> ids);
std::vector<vkpt::core::StableId> ResolveCommandEntityIds(
    const vkpt::editor::EditorCommand& command,
    const vkpt::editor::SelectionState& selection_state);

}  // namespace vkpt::app
