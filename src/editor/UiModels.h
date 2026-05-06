#pragma once

#include "editor/UiModelsCore.h"
#include "editor/UiModelsContracts.h"

namespace vkpt::editor {

// ----- Log/History helpers ------------------------------------------------

class UiEventLog {
 public:
  explicit UiEventLog(std::size_t max_events = 256);

  void push(UiEvent event);
  const std::deque<UiEvent>& events() const;

 private:
  std::size_t m_maxEvents;
  std::deque<UiEvent> m_events;
};

class EditorCommandHistory {
 public:
  explicit EditorCommandHistory(std::size_t max_events = 1024);

  void push(EditorCommand command);
  const std::vector<EditorCommand>& history() const;
  void clear();

 private:
  std::size_t m_maxEvents;
  std::vector<EditorCommand> m_commands;
};

// ----- Construction helpers ------------------------------------------------

UiLayoutDocument CreateDefaultLayout();
UiLayoutDocument CreateLayoutPreset(LayoutPreset preset);
std::vector<UiPanelState> BuildDefaultPanelStates(LayoutPreset preset);
MenuBar BuildDefaultMenuBar();
MenuBar BuildDefaultMenuBar(const SelectionState& selection);
MenuItem* FindMenuItem(MenuBar& menu, std::string_view item_id);
const MenuItem* FindMenuItem(const MenuBar& menu, std::string_view item_id);
MenuBar ApplySelectionRulesToEditMenu(MenuBar menu, const SelectionState& selection);
std::vector<MenuEnablement> GetEditMenuEnablements(const SelectionState& selection);
UiRuntimeState CreateDefaultRuntimeState();
SelectionState CreateDefaultSelectionState();
SelectionState ApplySelectionCommand(const SelectionState& state, const EditorCommand& command);
std::vector<InspectorFieldValue> BuildInspectorFieldStates(
    std::string_view field_id,
    const std::vector<std::string>& values);
AssetDropValidation ValidateAssetDrop(std::string_view asset_path,
                                     std::string_view target_slot);
vkpt::benchmark::BenchmarkRunDesc MakeDefaultBenchmarkRunDesc(std::string_view scene_path,
                                                             std::string_view backend = "cpu_scalar",
                                                             std::string_view renderer_path = "pathtracer",
                                                             std::uint32_t spp = 32,
                                                             std::uint32_t max_depth = 6,
                                                             std::uint64_t seed = 0xBEEFF00DULL,
                                                             std::uint32_t width = 1280,
                                                             std::uint32_t height = 720);
std::vector<UiShortcut> BuildDefaultUiShortcuts();
bool DetectShortcutConflicts(const std::vector<UiShortcut>& shortcuts);
bool DetectShortcutConflicts(const std::vector<UiShortcutAction>& shortcuts);
std::vector<UiPanelDefinition> BuildDefaultPanelDefinitions();
std::vector<InspectorFieldSchema> BuildDefaultInspectorSchemas();
std::vector<InspectorFieldSchema> BuildInspectorSchemasForPanel(std::string_view panel_id);
std::vector<UiPanelPropertyGroup> BuildDefaultPanelPropertyGroups();
std::vector<ScriptLifecycleHookState> BuildDefaultScriptLifecycleHooks();
std::vector<UiReleaseGateItem> BuildDefaultUiReleaseGateChecklist();
ShortcutResolution ResolveShortcut(const std::vector<UiShortcut>& shortcuts,
                                   std::uint32_t key_code,
                                   bool ctrl,
                                   bool shift,
                                   bool alt);
std::vector<AssetPreviewCard> FilterAndSortAssetCards(const std::vector<AssetPreviewCard>& cards,
                                                      const AssetBrowserFilter& filter);
BenchmarkScoreModel ComputeBenchmarkScore(double measured_units_per_second,
                                          double expected_units_per_second,
                                          double raw_samples_per_second,
                                          double workload_units,
                                          bool calibration_valid);
WorkloadComplexityModel EstimateWorkloadComplexity(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                                   std::uint32_t light_count = 0,
                                                   std::uint64_t triangle_count = 0,
                                                   std::uint64_t bvh_node_count = 0,
                                                   std::uint64_t texture_bytes = 0,
                                                   bool denoiser_enabled = false);
std::vector<BenchmarkCalibrationActionModel> BuildDefaultBenchmarkCalibrationActions(
    bool gpu_compute_available,
    bool hardware_rt_available);
BenchmarkPanelModel BuildBenchmarkPanelModel(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                            const BenchmarkRawMetricsModel& raw_metrics,
                                            const BenchmarkScoreModel& score,
                                            const WorkloadComplexityModel& workload,
                                            std::string_view artifact_location,
                                            std::string_view result_summary,
                                            bool can_run = true,
                                            std::string_view unavailable_reason = {});
StatusBarModel BuildStatusBarModel(const UiRuntimeState& runtime,
                                   const SelectionState& selection,
                                   const BenchmarkScoreModel* score = nullptr);
std::vector<SceneTreeRow> BuildSceneTreeRows(const std::vector<SceneTreeEntityModel>& entities,
                                             const SelectionState& selection,
                                             vkpt::core::StableId hovered_entity = 0,
                                             std::size_t max_rows = 0);

// ----- Serialization -------------------------------------------------------

std::string SerializeUiRuntimeState(const UiRuntimeState& state);
std::string SerializeSelectionState(const SelectionState& state);
std::string SerializeLayoutDocument(const UiLayoutDocument& layout);
std::string SerializeMenuBar(const MenuBar& menu);
std::string SerializePanelDefinitions(const std::vector<UiPanelDefinition>& panels);
std::string SerializeInspectorSchemas(const std::vector<InspectorFieldSchema>& schemas);
std::string SerializePanelPropertyGroups(const std::vector<UiPanelPropertyGroup>& groups);
std::string SerializeBenchmarkPanelModel(const BenchmarkPanelModel& model);
std::string SerializeUiReleaseGateChecklist(const std::vector<UiReleaseGateItem>& checklist);
std::string SerializeEditorCommand(const EditorCommand& command);
std::string SerializeEditorCommandsJsonl(const std::vector<EditorCommand>& commands, std::size_t max_lines = 256);
std::string SerializeUiEventsJsonl(const std::deque<UiEvent>& events, std::size_t max_lines = 256);

bool LoadSelectionFromFile(const std::string& path, SelectionState* out_selection);
bool SaveSelectionToFile(const std::string& path, const SelectionState& selection, std::string* error = nullptr);
bool LoadLayoutFromFile(const std::string& path, UiLayoutDocument* out_layout);
bool SaveLayoutToFile(const std::string& path, const UiLayoutDocument& layout, std::string* error = nullptr);
PanelMutationResult SetPanelVisible(UiLayoutDocument& layout, std::string_view panel_id, bool visible);
PanelMutationResult SetPanelCollapsed(UiLayoutDocument& layout, std::string_view panel_id, bool collapsed);
PanelMutationResult SetPanelDockState(UiLayoutDocument& layout, std::string_view panel_id, bool docked, bool floating);
PanelMutationResult MovePanel(UiLayoutDocument& layout, std::string_view panel_id, float x, float y);
PanelMutationResult ResizePanel(UiLayoutDocument& layout, std::string_view panel_id, float width, float height);
bool RestoreLayoutPreset(UiLayoutDocument& layout, LayoutPreset preset);

PanelMutationResult ApplyPanelStateCommand(UiLayoutDocument& layout,
                                          const std::string& command_id,
                                          bool value,
                                          float value_float,
                                          std::string_view target_panel_id);

// ----- Convenience factories for command generation ------------------------

EditorCommand MakeMenuCommand(std::string_view action_id,
                             std::string_view source = "menu",
                             vkpt::core::FrameIndex frame_index = 0);
EditorCommand MakeClearSelectionCommand(vkpt::core::FrameIndex frame_index = 0);
EditorCommand MakeCreateEntityCommand(std::string_view name,
                                     std::string_view template_name,
                                     vkpt::core::FrameIndex frame_index = 0);
EditorCommand MakeReorderSiblingCommand(vkpt::core::StableId moved_entity,
                                       vkpt::core::StableId sibling_before,
                                       vkpt::core::StableId sibling_after,
                                       vkpt::core::FrameIndex frame_index = 0,
                                       std::string_view source = "scene_tree");

}  // namespace vkpt::editor
