#include "editor/UiModels.h"

#include <utility>

namespace vkpt::editor {

UiEventLog::UiEventLog(std::size_t max_events)
    : m_maxEvents(max_events) {}

void UiEventLog::push(UiEvent event) {
  if (m_events.size() >= m_maxEvents) {
    m_events.pop_front();
  }
  m_events.push_back(std::move(event));
}

const std::deque<UiEvent>& UiEventLog::events() const {
  return m_events;
}

EditorCommandHistory::EditorCommandHistory(std::size_t max_events)
    : m_maxEvents(max_events) {}

void EditorCommandHistory::push(EditorCommand command) {
  if (m_commands.size() >= m_maxEvents) {
    m_commands.erase(m_commands.begin());
  }
  m_commands.push_back(std::move(command));
}

const std::vector<EditorCommand>& EditorCommandHistory::history() const {
  return m_commands;
}

void EditorCommandHistory::clear() {
  m_commands.clear();
}

}  // namespace vkpt::editor
