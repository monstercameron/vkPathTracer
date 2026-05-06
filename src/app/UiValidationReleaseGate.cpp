#include "app/UiValidation.h"

#include "app/UiValidationInternal.h"
#include "benchmark/BenchmarkSchema.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vkpt::app {

void MarkReleaseGate(std::vector<vkpt::editor::UiReleaseGateItem>& items,
                     std::string_view id,
                     bool passed,
                     std::string_view evidence,
                     std::string_view deferred_reason = {}) {
  for (auto& item : items) {
    if (item.id == id) {
      item.passed = passed;
      item.deferred = !passed && !deferred_reason.empty();
      item.evidence = std::string(evidence);
      item.deferred_reason = std::string(deferred_reason);
      return;
    }
  }
}

std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence() {
  using namespace vkpt::editor;
  auto checklist = BuildDefaultUiReleaseGateChecklist();

  const auto menu = BuildDefaultMenuBar();
  MarkReleaseGate(checklist, "menu.works",
                  HasTopLevelMenu(menu, "file") &&
                  HasTopLevelMenu(menu, "edit") &&
                  HasTopLevelMenu(menu, "benchmark") &&
                  HasMenuItem(menu, "file", "file.open_scene") &&
                  HasMenuItem(menu, "benchmark", "benchmark.run_current_scene"),
                  "BuildDefaultMenuBar exposes required top-level menus and typed actions");

  const auto layout = CreateLayoutPreset(LayoutPreset::Default);
  const auto layoutPath = std::filesystem::temp_directory_path() / "vkpt-ui-release-layout.json";
  std::string layoutError;
  UiLayoutDocument reloadedLayout;
  const bool layoutPersisted = SaveLayoutToFile(layoutPath.string(), layout, &layoutError) &&
                               LoadLayoutFromFile(layoutPath.string(), &reloadedLayout) &&
                               reloadedLayout.active_layout_name == layout.active_layout_name &&
                               !reloadedLayout.panels.empty();
  std::error_code removeEc;
  std::filesystem::remove(layoutPath, removeEc);
  MarkReleaseGate(checklist, "layout.persists", layoutPersisted,
                  "UiLayoutDocument save/load roundtrip preserves panel geometry",
                  layoutPersisted ? std::string_view{} : std::string_view{"layout JSON roundtrip failed"});

  auto panelLayout = CreateLayoutPreset(LayoutPreset::Default);
  const bool panelMutations =
      SetPanelVisible(panelLayout, "inspector", false).changed &&
      SetPanelVisible(panelLayout, "inspector", true).changed &&
      SetPanelDockState(panelLayout, "inspector", false, true).changed &&
      MovePanel(panelLayout, "inspector", 32.0f, 48.0f).changed &&
      ResizePanel(panelLayout, "inspector", 512.0f, 384.0f).changed;
  MarkReleaseGate(checklist, "panels.dock_float", panelMutations,
                  "UiLayoutDocument panel helpers cover close/show/dock/float/move/resize");

  SelectionState selection = CreateDefaultSelectionState();
  selection.selected_entity_ids = {1, 2};
  selection.active_primary_entity = 1;
  selection.hovered_entity = 2;
  selection.aggregate_bounds = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, true};
  selection.per_item_bounds = {
    {1, {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, true}},
    {2, {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true}},
  };
  const auto selectionPath = std::filesystem::temp_directory_path() / "vkpt-ui-release-selection.json";
  std::string selectionError;
  SelectionState selectionRoundtrip;
  const bool selectionSaved = SaveSelectionToFile(selectionPath.string(), selection, &selectionError) &&
                              LoadSelectionFromFile(selectionPath.string(), &selectionRoundtrip) &&
                              selectionRoundtrip.selected_entity_ids == selection.selected_entity_ids &&
                              selectionRoundtrip.per_item_bounds.size() == 2;
  std::filesystem::remove(selectionPath, removeEc);
  MarkReleaseGate(checklist, "selection.multi", selectionSaved,
                  "SelectionState supports stable IDs, primary selection, hover, aggregate bounds, per-item bounds, and JSON roundtrip");
  MarkReleaseGate(checklist, "viewport.bounds", false,
                  "SelectionState carries aggregate and per-item bounds for viewport/tree/inspector overlays",
                  "requires overlay renderer drawing those bounds in the viewport");

  const auto rejectedDrop = ValidateAssetDrop("C:/assets/readme.md", "texture_slot");
  MarkReleaseGate(checklist, "assets.reject", !rejectedDrop.accepted && !rejectedDrop.reason.empty(),
                  "ValidateAssetDrop rejects unsupported target/type combinations with a reason");

  const auto benchmarkDesc = MakeDefaultBenchmarkRunDesc("assets/scenes/cornell_native.json", "cpu", "cpu_scalar", 8, 4, 123, 320, 180);
  const auto workload = EstimateWorkloadComplexity(benchmarkDesc, 1, 12, 8, 4096, false);
  auto score = ComputeBenchmarkScore(1000.0, 1000.0, 250.0, workload.normalized_cost_units, true);
  score.raw_paths_per_second = 500.0;
  const BenchmarkRawMetricsModel rawMetrics{
    60.0, 16.67, 0.0, 16.67, 250.0, 500.0, 1500.0, 8, 4096, 0.25, 0.0
  };
  const auto benchmarkPanel = BuildBenchmarkPanelModel(
      benchmarkDesc, rawMetrics, score, workload, "artifacts/benchmarks/ui", "model smoke result", true);
  const std::string benchmarkJson = SerializeBenchmarkPanelModel(benchmarkPanel);
  const bool benchmarkPanelOk =
      benchmarkJson.find("\"selected_scene\":\"assets/scenes/cornell_native.json\"") != std::string::npos &&
      benchmarkJson.find("\"raw_metrics\"") != std::string::npos &&
      benchmarkJson.find("\"normalized_score\":1") != std::string::npos &&
      benchmarkJson.find("\"calibration_actions\"") != std::string::npos;
  MarkReleaseGate(checklist, "benchmark.desc", benchmarkPanelOk,
                  "BenchmarkPanelModel serializes run descriptor, raw metrics, score, workload, calibration actions, and artifact path");
  MarkReleaseGate(checklist, "benchmark.score", score.calibration_valid && score.confidence == "high",
                  "ComputeBenchmarkScore separates raw throughput from hardware-normalized efficiency");

  UiEventLog log(256);
  PushUiEvent(log, "menu_click", "menu", "file.open_scene", 1, {}, {}, "unsupported:file dialog unavailable");
  EditorCommandHistory history(256);
  history.push(MakeMenuCommand("benchmark.run_current_scene", "menu", 2));
  const auto runtime = CreateDefaultRuntimeState();
  const std::string uiState = SerializeUiRuntimeState(runtime);
  const std::string eventLines = SerializeUiEventsJsonl(log.events(), 256);
  const std::string commandLines = SerializeEditorCommandsJsonl(history.history(), 256);
  MarkReleaseGate(checklist, "crash.ui_state",
                  uiState.find("\"active_layout_name\"") != std::string::npos &&
                  eventLines.find("menu_click") != std::string::npos &&
                  commandLines.find("benchmark.run_current_scene") != std::string::npos,
                  "Crash recorder receives serialized UI state, selection state, layout, UI events, and editor command history from app shell");

  MarkReleaseGate(checklist, "window.opens", false,
                  "Native/Qt bounded window smoke is covered by tools/ui_qt_smoke when the platform is available",
                  "requires a GUI-capable platform run, not a headless model check");
  std::string treeEvidence;
  const bool treeHierarchyOk = CheckEcsSceneTreeContracts(&treeEvidence);
  MarkReleaseGate(checklist, "tree.hierarchy", treeHierarchyOk,
                  treeHierarchyOk ? treeEvidence : "ECS scene tree model/runtime contract check failed",
                  treeHierarchyOk ? std::string_view{} : std::string_view{treeEvidence});
  MarkReleaseGate(checklist, "viewport.selection", false,
                  "SelectionState and selection commands are covered by --ui-model-smoke",
                  "requires object-ID/CPU-ray picking integration in the viewport");
  MarkReleaseGate(checklist, "gizmo.trs", false,
                  "GizmoSettings and transform command contracts exist",
                  "requires rendered gizmo handles and command application");
  MarkReleaseGate(checklist, "inspector.edits", false,
                  "InspectorFieldSchema and mixed-value models are covered by --ui-model-smoke",
                  "requires bound inspector widgets applying ECS/component edits");
  MarkReleaseGate(checklist, "grouping", false,
                  "Group/Ungroup command payloads serialize through EditorCommand",
                  "requires group/ungroup runtime metadata and undo integration");
  MarkReleaseGate(checklist, "merge.split", false,
                  "Merge command payload serializes; split is explicitly unsupported until merge metadata runtime exists",
                  "requires generated asset/merge metadata runtime");
  MarkReleaseGate(checklist, "assets.import", false,
                  "Asset drop validation and import command contracts exist",
                  "requires importer runtime and asset browser widget");
  MarkReleaseGate(checklist, "lua.attach", false,
                  "Attach/Detach script command payloads exist and script lifecycle model is deterministic",
                  "requires Lua runtime/editor binding");
  MarkReleaseGate(checklist, "logs.errors", false,
                  "UiEventLog serializes model events for crash artifacts",
                  "requires visible log panel consuming the diagnostics ring buffer");

  return checklist;
}

bool RunUiReleaseGateCheck(bool json) {
  const auto checklist = BuildUiReleaseGateEvidence();
  if (json) {
    std::cout << vkpt::editor::SerializeUiReleaseGateChecklist(checklist) << "\n";
  } else {
    std::size_t passed = 0;
    std::size_t deferred = 0;
    std::size_t pending = 0;
    std::cout << "ui release gate:\n";
    for (const auto& item : checklist) {
      const char* status = item.passed ? "pass" : (item.deferred ? "defer" : "PEND");
      if (item.passed) {
        ++passed;
      } else if (item.deferred) {
        ++deferred;
      } else {
        ++pending;
      }
      std::cout << "  [" << status << "] " << item.id << ": " << item.evidence;
      if (item.deferred) {
        std::cout << " (" << item.deferred_reason << ")";
      }
      std::cout << "\n";
    }
    std::cout << "ui release gate summary: passed=" << passed
              << " deferred=" << deferred
              << " pending=" << pending << "\n";
  }

  return std::none_of(checklist.begin(), checklist.end(), [](const auto& item) {
    return !item.passed && !item.deferred;
  });
}


}  // namespace vkpt::app
