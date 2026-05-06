#include "editor/UiModelsInternal.h"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vkpt::editor {

std::vector<UiReleaseGateItem> BuildDefaultUiReleaseGateChecklist() {
  const std::vector<std::pair<std::string_view, std::string_view>> items = {
    {"window.opens", "window opens"},
    {"menu.works", "menu bar works"},
    {"layout.persists", "layout persists"},
    {"panels.dock_float", "panels dock/float"},
    {"tree.hierarchy", "scene tree displays hierarchy"},
    {"viewport.selection", "viewport selection works"},
    {"viewport.bounds", "bounding boxes display"},
    {"gizmo.trs", "translate/rotate/scale gizmos work"},
    {"inspector.edits", "inspector edits selected entity"},
    {"selection.multi", "multi-select works"},
    {"grouping", "group/ungroup works"},
    {"merge.split", "merge/split works"},
    {"assets.import", "asset browser imports valid files"},
    {"assets.reject", "invalid file drops are rejected"},
    {"lua.attach", "Lua script attach UI works"},
    {"benchmark.desc", "benchmark panel runs benchmark descriptor"},
    {"benchmark.score", "normalized score displays"},
    {"logs.errors", "logs panel shows errors"},
    {"crash.ui_state", "crash snapshot includes UI state"},
  };
  std::vector<UiReleaseGateItem> checklist;
  checklist.reserve(items.size());
  for (const auto& [id, label] : items) {
    UiReleaseGateItem item;
    item.id = std::string(id);
    item.label = std::string(label);
    item.required = true;
    checklist.push_back(std::move(item));
  }
  return checklist;
}

std::string SerializeUiReleaseGateChecklist(const std::vector<UiReleaseGateItem>& checklist) {
  std::ostringstream out;
  std::size_t passed = 0;
  std::size_t deferred = 0;
  std::size_t pending = 0;
  for (const auto& item : checklist) {
    if (item.passed) {
      ++passed;
    } else if (item.deferred) {
      ++deferred;
    } else {
      ++pending;
    }
  }

  out << "{";
  out << "\"total\":" << checklist.size() << ",";
  out << "\"passed_count\":" << passed << ",";
  out << "\"deferred_count\":" << deferred << ",";
  out << "\"pending_count\":" << pending << ",";
  out << "\"items\":[";
  for (std::size_t i = 0; i < checklist.size(); ++i) {
    const auto& item = checklist[i];
    if (i > 0) {
      out << ",";
    }
    out << "{";
    out << "\"id\":\"" << EscapeJson(item.id) << "\",";
    out << "\"label\":\"" << EscapeJson(item.label) << "\",";
    out << "\"required\":" << (item.required ? "true" : "false") << ",";
    out << "\"passed\":" << (item.passed ? "true" : "false") << ",";
    out << "\"deferred\":" << (item.deferred ? "true" : "false") << ",";
    out << "\"status\":\"" << (item.passed ? "passed" : (item.deferred ? "deferred" : "pending")) << "\",";
    out << "\"evidence\":\"" << EscapeJson(item.evidence) << "\",";
    out << "\"deferred_reason\":\"" << EscapeJson(item.deferred_reason) << "\"";
    out << "}";
  }
  out << "]}";
  return out.str();
}

}  // namespace vkpt::editor
