#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "editor/UiModels.h"
#include "pathtracer/PathTracer.h"
#include "scene/Scene.h"

namespace vkpt::editor {

enum class QtPanelPropertyKind {
  Text,
  Number,
  Toggle,
  Vector3,
  Color,
  Asset,
  Entity,
  Enum,
  Command,
};

struct QtPanelProperty {
  std::string id;
  std::string label;
  std::string value;
  std::string group;
  std::string unit;
  QtPanelPropertyKind kind = QtPanelPropertyKind::Text;
  bool editable = false;
  bool mixed = false;
  bool warning = false;
  std::string tooltip;
};

struct QtPanelRow {
  std::string id;
  std::string label;
  std::string detail;
  std::string icon;
  std::uint32_t depth = 0;
  bool selected = false;
  bool expanded = false;
  bool visible = true;
  bool warning = false;
  std::vector<QtPanelProperty> properties;
};

struct QtPanelModel {
  std::string panel_id;
  std::string title;
  std::string summary;
  bool available = true;
  std::string empty_message;
  std::vector<QtPanelProperty> properties;
  std::vector<QtPanelRow> rows;
};

struct QtPanelBuildContext {
  const vkpt::scene::SceneDocument* document = nullptr;
  const vkpt::scene::SceneSnapshot* snapshot = nullptr;
  const vkpt::scene::RenderSceneProxy* render_proxy = nullptr;
  const vkpt::scene::SceneWorld* world = nullptr;
  const vkpt::pathtracer::RTSceneData* rt_scene = nullptr;
  const vkpt::pathtracer::RenderSettings* render_settings = nullptr;
  const UiRuntimeState* runtime = nullptr;
  const SelectionState* selection = nullptr;
  const BenchmarkPanelModel* benchmark = nullptr;
  const StatusBarModel* status_bar = nullptr;
  std::vector<AssetPreviewCard> asset_cards;
  std::vector<ScriptLifecycleHookState> script_hooks;
  std::vector<UiReleaseGateItem> release_gates;
  std::vector<std::string> diagnostics_log;
  std::vector<std::string> benchmark_history;
  vkpt::core::FrameIndex current_frame = 0;
  double delta_seconds = 0.0;
  bool timeline_playing = false;
};

std::string_view ToString(QtPanelPropertyKind kind);

std::vector<std::string> BuildDefaultQtPanelIds();
QtPanelModel BuildQtPanelModel(std::string_view panel_id, const QtPanelBuildContext& context);
std::vector<QtPanelModel> BuildAllQtPanelModels(const QtPanelBuildContext& context);

QtPanelModel BuildSceneGraphPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildInspectorPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildMaterialsPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildLightsPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildCameraPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildRenderSettingsPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildBenchmarkPanelTextModel(const QtPanelBuildContext& context);
QtPanelModel BuildDiagnosticsPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildPerformancePanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildDebugViewsPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildAssetBrowserPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildTimelinePanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildScriptingPanelModel(const QtPanelBuildContext& context);
QtPanelModel BuildPhysicsPanelModel(const QtPanelBuildContext& context);

}  // namespace vkpt::editor
