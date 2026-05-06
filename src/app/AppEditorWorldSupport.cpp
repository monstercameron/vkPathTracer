#include "app/AppEditorWorldSupport.h"

#include <algorithm>
#include <variant>

namespace vkpt::app {

std::vector<vkpt::core::StableId> SortedUniqueEntityIds(
    std::vector<vkpt::core::StableId> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

std::vector<vkpt::core::StableId> ResolveCommandEntityIds(
    const vkpt::editor::EditorCommand& command,
    const vkpt::editor::SelectionState& selection_state) {
  std::vector<vkpt::core::StableId> ids;
  if (auto* duplicate = std::get_if<vkpt::editor::DuplicateEntityCommand>(&command.payload)) {
    ids = duplicate->entity_ids;
  } else if (auto* remove = std::get_if<vkpt::editor::DeleteEntityCommand>(&command.payload)) {
    ids = remove->entity_ids;
  } else if (auto* group = std::get_if<vkpt::editor::GroupEntitiesCommand>(&command.payload)) {
    ids = group->entity_ids;
  } else if (auto* merge = std::get_if<vkpt::editor::MergeEntitiesCommand>(&command.payload)) {
    ids = merge->entity_ids;
  } else if (auto* reparent = std::get_if<vkpt::editor::ReparentEntityCommand>(&command.payload)) {
    if (reparent->entity_id != 0u) {
      ids.push_back(reparent->entity_id);
    }
  }
  if (ids.empty()) {
    ids = selection_state.selected_entity_ids;
  }
  return SortedUniqueEntityIds(ids);
}

}  // namespace vkpt::app
