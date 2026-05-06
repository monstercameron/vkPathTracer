#include "app/UiValidation.h"

#include "app/UiValidationInternal.h"
#include "assets/SceneAssetLoader.h"
#include "benchmark/BenchmarkSchema.h"
#include "editor/QtPanelModels.h"
#include "pathtracer/PathTracer.h"
#include "scene/Scene.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vkpt::app {

std::uint64_t UiValidationNowMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index,
                 std::string_view old_value,
                 std::string_view new_value,
                 std::string_view command_result) {
  vkpt::editor::UiEvent event;
  event.event_type = std::string(event_type);
  event.panel_id = std::string(panel_id);
  event.widget_id = std::string(widget_id);
  event.frame_index = frame_index;
  event.timestamp_ms = UiValidationNowMs();
  event.thread_id = "main";
  event.old_value = std::string(old_value);
  event.new_value = std::string(new_value);
  event.command_result = std::string(command_result);
  log.push(event);
}

std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence();

bool Check(std::string_view tag, bool cond) {
  if (!cond) {
    std::cerr << "ui-model-smoke: fail: " << tag << "\n";
  }
  return cond;
}

bool HasTopLevelMenu(const vkpt::editor::MenuBar& menu, std::string_view id) {
  for (const auto& item : menu.top_level_menus) {
    if (item.id == id) {
      return true;
    }
  }
  return false;
}

bool HasMenuItem(const vkpt::editor::MenuBar& menu, std::string_view top_level_id, std::string_view action_id) {
  for (const auto& top_level : menu.top_level_menus) {
    if (top_level.id != top_level_id) {
      continue;
    }
    for (const auto& item : top_level.children) {
      if (item.id == action_id) {
        return true;
      }
    }
    return false;
  }
  return false;
}

bool RunUiModelSmokeTests() {
  using namespace vkpt::editor;
  bool ok = true;
  auto check_true = [&](std::string_view tag, bool cond) {
    ok = ok && Check(tag, cond);
  };
  auto find_panel = [](const UiLayoutDocument& layout, std::string_view panel_id) -> const UiPanelState* {
    const auto it = std::find_if(layout.panels.begin(), layout.panels.end(),
                                 [panel_id](const UiPanelState& panel) {
                                   return panel.id == panel_id;
                                 });
    if (it == layout.panels.end()) {
      return nullptr;
    }
    return &(*it);
  };
  auto find_menu_enablement = [](const std::vector<MenuEnablement>& entries,
                                std::string_view item_id) -> std::optional<bool> {
    for (const auto& entry : entries) {
      if (entry.item_id == item_id) {
        return entry.enabled;
      }
    }
    return std::nullopt;
  };
  auto has_menu_items = [&](const MenuBar& menu,
                           std::string_view top_level,
                           std::initializer_list<std::string_view> items) {
    for (const auto item : items) {
      check_true(std::string("menu item missing: ") + std::string(top_level) + "." + std::string(item),
                 HasMenuItem(menu, top_level, item));
    }
  };
  auto has_shortcut = [](const std::vector<UiShortcut>& shortcuts,
                        std::string_view action_id,
                        std::uint32_t key_code,
                        bool ctrl,
                        bool shift,
                        bool alt) {
    return std::any_of(shortcuts.begin(), shortcuts.end(),
                       [&](const UiShortcut& shortcut) {
                         return shortcut.action_id == action_id &&
                                shortcut.key_code == key_code &&
                                shortcut.ctrl == ctrl &&
                                shortcut.shift == shift &&
                               shortcut.alt == alt;
                       });
  };

  RunUiCameraAndQtDockSmokeChecks(check_true);

  const auto menu = BuildDefaultMenuBar();
  const std::initializer_list<std::string_view> requiredTopLevels = {
    "file", "edit", "view", "create", "scene", "render", "benchmark",
    "assets", "scripts", "tools", "help"
  };
  for (const auto level : requiredTopLevels) {
    check_true(std::string("missing top-level menu: ") + std::string(level),
               HasTopLevelMenu(menu, level));
  }

  has_menu_items(menu, "file", {
    "file.new_scene", "file.open_scene", "file.open_recent", "file.save_scene",
    "file.save_scene_as", "file.clone_scene", "file.import_asset", "file.export_image",
    "file.export_exr", "file.export_benchmark_artifacts", "file.export_scene_snapshot",
    "file.preferences", "file.reveal_artifacts_folder", "file.exit"
  });
  has_menu_items(menu, "edit", {
    "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste", "edit.duplicate",
    "edit.delete", "edit.rename", "edit.select_all", "edit.select_none", "edit.invert_selection",
    "edit.group_selection", "edit.ungroup_selection", "edit.merge_selection",
    "edit.split_merged_object", "edit.reparent_selection", "edit.reset_transform",
    "edit.command_history"
  });
  has_menu_items(menu, "view", {
    "view.panels", "view.layouts", "view.overlays", "view.debug_views",
    "view.fullscreen", "view.ui_scale", "view.reset_layout"
  });
  has_menu_items(menu, "create", {
    "create.empty_entity", "create.group_entity", "create.camera", "create.light",
    "create.mesh_primitives", "create.sdf_primitives", "create.material", "create.script",
    "create.physics_body", "create.benchmark_marker"
  });
  has_menu_items(menu, "benchmark", {
    "benchmark.run_current_scene", "benchmark.run_scene_pack", "benchmark.run_cpu_calibration",
    "benchmark.run_gpu_calibration", "benchmark.run_simd_experiment", "benchmark.run_backend_experiment",
    "benchmark.compare_against_reference", "benchmark.open_artifacts", "benchmark.export_csv_json",
    "benchmark.history"
  });
  has_menu_items(menu, "scene", {
    "scene.validate_scene", "scene.freeze_benchmark_snapshot", "scene.reset_accumulation",
    "scene.reload_scene", "scene.reload_assets", "scene.hot_reload_scripts", "scene.scene_settings",
    "scene.lighting_settings", "scene.environment_settings", "scene.camera_settings",
    "scene.physics_settings", "scene.script_settings", "scene.animation_settings"
  });
  has_menu_items(menu, "render", {
    "render.start_render", "render.backend", "render.renderer_path", "render.quality_presets", "render.resolution",
    "render.spp", "render.max_bounces", "render.denoiser", "render.tone_mapping",
    "render.exposure", "render.debug_channel", "render.shader_cache", "render.backend_capabilities"
  });
  has_menu_items(menu, "scripts", {
    "scripts.new_lua_script", "scripts.attach_script_to_selection", "scripts.detach_script_from_selection",
    "scripts.reload_scripts", "scripts.open_script_folder", "scripts.show_script_lifecycle_events",
    "scripts.show_script_errors", "scripts.show_script_profiler", "scripts.sandbox_settings"
  });
  has_menu_items(menu, "assets", {
    "assets.import_files", "assets.reimport_selected", "assets.refresh_browser", "assets.show_missing_assets",
    "assets.show_import_diagnostics", "assets.clear_generated_cache", "assets.clear_shader_cache"
  });
  has_menu_items(menu, "tools", {
    "tools.doctor", "tools.crash_artifacts", "tools.profiler", "tools.frame_capture",
    "tools.shader_manifest", "tools.asset_manifest", "tools.scene_snapshot",
    "tools.capability_matrix", "tools.settings_dump", "tools.startup_self_test"
  });
  has_menu_items(menu, "help", {
    "help.controls", "help.shortcut_reference", "help.about", "help.build_info",
    "help.feature_flags", "help.dependency_info"
  });
  auto mutable_menu = menu;
  check_true("find menu item non-const", FindMenuItem(mutable_menu, "edit.duplicate") != nullptr);
  check_true("find menu item const", FindMenuItem(std::as_const(menu), "edit.duplicate") != nullptr);

  const auto editEnablementsEmpty = GetEditMenuEnablements(CreateDefaultSelectionState());
  const auto editDuplicateDisabled = find_menu_enablement(editEnablementsEmpty, "edit.duplicate");
  check_true("edit.duplicate disabled without selection", editDuplicateDisabled.has_value() && !editDuplicateDisabled.value());
  const auto disabledEditMenu = BuildDefaultMenuBar(CreateDefaultSelectionState());
  const auto* disabledDuplicate = FindMenuItem(disabledEditMenu, "edit.duplicate");
  check_true("edit.duplicate disabled reason",
             disabledDuplicate != nullptr && !disabledDuplicate->disabled_reason.empty());
  SelectionState oneSelected = CreateDefaultSelectionState();
  oneSelected.selected_entity_ids = {11};
  const auto editEnablementsSelected = GetEditMenuEnablements(oneSelected);
  const auto editDuplicateEnabled = find_menu_enablement(editEnablementsSelected, "edit.duplicate");
  check_true("edit.duplicate enabled with selection", editDuplicateEnabled.has_value() && editDuplicateEnabled.value());
  check_true("file.exit remains enabled", FindMenuItem(menu, "file.exit")->enabled);

  const auto new_scene = MakeMenuCommand("file.new_scene", "menu");
  check_true("file.new_scene command kind", new_scene.kind == EditorCommandKind::kCreateEntity);
  check_true("file.new_scene mapping", std::holds_alternative<CreateEntityCommand>(new_scene.payload));

  const auto select_none = MakeMenuCommand("edit.select_none", "menu");
  check_true("edit.select_none command kind", select_none.kind == EditorCommandKind::kClearSelection);
  check_true("edit.select_none mapping", std::holds_alternative<ClearSelectionCommand>(select_none.payload));

  const auto open_scene = MakeMenuCommand("file.open_scene", "menu");
  check_true("file.open_scene unsupported command", open_scene.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto copy_action = MakeMenuCommand("edit.copy", "menu");
  check_true("edit.copy unsupported command", copy_action.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto save_scene = MakeMenuCommand("file.save_scene", "menu");
  check_true("file.save_scene command is explicit unsupported", save_scene.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto exit_action = MakeMenuCommand("file.exit", "menu");
  check_true("file.exit command is explicit unsupported", exit_action.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto run_benchmark = MakeMenuCommand("benchmark.run_current_scene", "menu");
  check_true("benchmark.run_current_scene command kind", run_benchmark.kind == EditorCommandKind::kRunBenchmark);
  check_true("benchmark.run_current_scene mapping", std::holds_alternative<RunBenchmarkCommand>(run_benchmark.payload));
  const auto run_scene_pack = MakeMenuCommand("benchmark.run_scene_pack", "menu");
  check_true("benchmark.run_scene_pack command kind", run_scene_pack.kind == EditorCommandKind::kRunBenchmark);
  const auto run_cpu_calibration = MakeMenuCommand("benchmark.run_cpu_calibration", "menu");
  check_true("benchmark.run_cpu_calibration command kind", run_cpu_calibration.kind == EditorCommandKind::kRunBenchmark);
  const auto run_gpu_calibration = MakeMenuCommand("benchmark.run_gpu_calibration", "menu");
  check_true("benchmark.run_gpu_calibration command kind", run_gpu_calibration.kind == EditorCommandKind::kRunBenchmark);
  const auto run_simd = MakeMenuCommand("benchmark.run_simd_experiment", "menu");
  check_true("benchmark.run_simd_experiment command kind", run_simd.kind == EditorCommandKind::kRunBenchmark);
  const auto run_backend = MakeMenuCommand("benchmark.run_backend_experiment", "menu");
  check_true("benchmark.run_backend_experiment command kind", run_backend.kind == EditorCommandKind::kRunBenchmark);
  const auto run_ref = MakeMenuCommand("benchmark.compare_against_reference", "menu");
  check_true("benchmark.compare_against_reference command kind", run_ref.kind == EditorCommandKind::kRunBenchmark);
  const auto benchmark_open_artifacts = MakeMenuCommand("benchmark.open_artifacts", "menu");
  check_true("benchmark.open_artifacts currently unsupported by model", benchmark_open_artifacts.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto benchmark_export_csv = MakeMenuCommand("benchmark.export_csv_json", "menu");
  check_true("benchmark.export_csv_json currently unsupported by model", benchmark_export_csv.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto benchmark_history = MakeMenuCommand("benchmark.history", "menu");
  check_true("benchmark.history currently unsupported by model", benchmark_history.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto scene_validate = MakeMenuCommand("scene.validate_scene", "menu");
  check_true("scene.validate_scene currently unsupported by model", scene_validate.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto scene_settings = MakeMenuCommand("scene.scene_settings", "menu");
  check_true("scene.scene_settings currently unsupported by model", scene_settings.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_backend = MakeMenuCommand("render.backend", "menu");
  check_true("render.backend currently unsupported by model", render_backend.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_start = MakeMenuCommand("render.start_render", "menu");
  check_true("render.start_render currently handled by Qt runtime", render_start.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_quality_presets = MakeMenuCommand("render.quality_presets", "menu");
  check_true("render.quality_presets currently unsupported by model", render_quality_presets.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto tools_doctor = MakeMenuCommand("tools.doctor", "menu");
  check_true("tools.doctor currently unsupported by model", tools_doctor.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto tools_profiler = MakeMenuCommand("tools.profiler", "menu");
  check_true("tools.profiler currently unsupported by model", tools_profiler.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto help_about = MakeMenuCommand("help.about", "menu");
  check_true("help.about currently unsupported by model", help_about.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto help_controls = MakeMenuCommand("help.controls", "menu");
  check_true("help.controls currently unsupported by model", help_controls.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto create_material = MakeMenuCommand("create.material", "menu");
  check_true("create.material command kind", create_material.kind == EditorCommandKind::kCreateEntity);
  const auto create_script = MakeMenuCommand("create.script", "menu");
  check_true("create.script command kind", create_script.kind == EditorCommandKind::kCreateEntity);
  const auto create_physics = MakeMenuCommand("create.physics_body", "menu");
  check_true("create.physics_body command kind", create_physics.kind == EditorCommandKind::kCreateEntity);
  const auto create_marker = MakeMenuCommand("create.benchmark_marker", "menu");
  check_true("create.benchmark_marker command kind", create_marker.kind == EditorCommandKind::kCreateEntity);
  const auto create_mat_template = std::get<CreateEntityCommand>(create_material.payload).template_name;
  check_true("create.material has template", create_mat_template == "material");
  const auto create_script_template = std::get<CreateEntityCommand>(create_script.payload).template_name;
  check_true("create.script has template", create_script_template == "script");
  const auto create_physics_template = std::get<CreateEntityCommand>(create_physics.payload).template_name;
  check_true("create.physics_body has template", create_physics_template == "physics_body");
  const auto create_marker_template = std::get<CreateEntityCommand>(create_marker.payload).template_name;
  check_true("create.benchmark_marker has template", create_marker_template == "benchmark_marker");
  const auto create_light = MakeMenuCommand("create.light", "menu");
  check_true("create.light command kind", create_light.kind == EditorCommandKind::kCreateEntity);
  check_true("create.light template", std::get<CreateEntityCommand>(create_light.payload).template_name == "create.light");
  const auto create_mesh = MakeMenuCommand("create.mesh_primitives", "menu");
  check_true("create.mesh_primitives command kind", create_mesh.kind == EditorCommandKind::kCreateEntity);
  check_true("create.mesh template", std::get<CreateEntityCommand>(create_mesh.payload).template_name == "create.mesh_primitives");
  const auto create_sdf = MakeMenuCommand("create.sdf_primitives", "menu");
  check_true("create.sdf_primitives command kind", create_sdf.kind == EditorCommandKind::kCreateEntity);
  check_true("create.sdf template", std::get<CreateEntityCommand>(create_sdf.payload).template_name == "create.sdf_primitives");

  const auto layout = CreateLayoutPreset(LayoutPreset::Benchmark);
  check_true("benchmark layout preset", layout.preset == LayoutPreset::Benchmark);
  check_true("benchmark layout panels", !layout.panels.empty());
  const auto defaultLayout = CreateDefaultLayout();
  const auto* defaultInspector = find_panel(defaultLayout, "inspector");
  const auto* defaultScripting = find_panel(defaultLayout, "script_panel");
  check_true("default layout shows inspector scripting tab",
             defaultInspector != nullptr &&
             defaultInspector->visible &&
             defaultScripting != nullptr &&
             defaultScripting->visible &&
             defaultScripting->docked &&
             defaultScripting->x == defaultInspector->x &&
             defaultScripting->y == defaultInspector->y);

  const auto tmp = std::filesystem::temp_directory_path() / "vkpt-ui-layout-smoke.json";
  std::string save_error;
  if (Check("layout save", SaveLayoutToFile(tmp.string(), layout, &save_error))) {
    UiLayoutDocument reloaded;
    check_true("layout load", LoadLayoutFromFile(tmp.string(), &reloaded));
    check_true("layout preset roundtrip", reloaded.preset == layout.preset);
    check_true("layout name roundtrip", reloaded.active_layout_name == layout.active_layout_name);
    check_true("layout ui scale roundtrip", reloaded.ui_scale == layout.ui_scale);
    std::filesystem::remove(tmp);
  }

  const std::vector<std::pair<LayoutPreset, std::string_view>> allPresets = {
    {LayoutPreset::Default, "Default"},
    {LayoutPreset::Benchmark, "Benchmark"},
    {LayoutPreset::MaterialAuthoring, "Material Authoring"},
    {LayoutPreset::Scripting, "Scripting"},
    {LayoutPreset::AssetManagement, "Asset Management"},
    {LayoutPreset::DebugProfiler, "Debug/Profiler"},
    {LayoutPreset::MinimalViewport, "Minimal Viewport"},
    {LayoutPreset::FullscreenViewportWithOverlay, "Fullscreen Viewport With Overlay"},
  };
  for (std::size_t i = 0; i < allPresets.size(); ++i) {
    const auto preset_layout = CreateLayoutPreset(allPresets[i].first);
    check_true(std::string("layout preset exists: ") + std::string(allPresets[i].second),
              !preset_layout.panels.empty());
    const auto preset_path = std::filesystem::temp_directory_path() / ("vkpt-ui-layout-smoke-" + std::to_string(i) + ".json");
    std::string preset_save_err;
    if (SaveLayoutToFile(preset_path.string(), preset_layout, &preset_save_err)) {
      UiLayoutDocument preset_roundtrip;
      check_true(std::string("layout preset roundtrip: ") + std::string(allPresets[i].second),
                LoadLayoutFromFile(preset_path.string(), &preset_roundtrip));
      check_true(std::string("layout preset restored: ") + std::string(allPresets[i].second),
                preset_roundtrip.preset == preset_layout.preset);
      std::filesystem::remove(preset_path);
    } else {
      check_true(std::string("layout preset save: ") + std::string(allPresets[i].second), false);
    }
  }

  SelectionState selection = CreateDefaultSelectionState();
  selection.selected_entity_ids = {7, 8, 9};
  selection.active_primary_entity = 7;
  selection.hovered_entity_ids = {9};
  selection.selection_source = SelectionSource::Viewport;
  const std::string selection_json = SerializeSelectionState(selection);
  check_true("selection json serialization",
             selection_json.find("\"selected_entity_ids\":[7,8,9]") != std::string::npos);
  selection.aggregate_bounds = {{-1.0f, -2.0f, -3.0f}, {4.0f, 5.0f, 6.0f}, true};
  selection.per_item_bounds = {
    {7, {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, true}},
    {8, {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true}},
    {9, {{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}, true}},
  };
  const auto selection_tmp = std::filesystem::temp_directory_path() / "vkpt-ui-selection-smoke.json";
  std::string selection_save_error;
  if (Check("selection save", SaveSelectionToFile(selection_tmp.string(), selection, &selection_save_error))) {
    SelectionState selection_reloaded;
    check_true("selection load", LoadSelectionFromFile(selection_tmp.string(), &selection_reloaded));
    check_true("selection selected ids roundtrip", selection_reloaded.selected_entity_ids == selection.selected_entity_ids);
    check_true("selection bounds roundtrip", selection_reloaded.aggregate_bounds.valid &&
               selection_reloaded.per_item_bounds.size() == selection.per_item_bounds.size());
    std::filesystem::remove(selection_tmp);
  }

  EditorCommand pick_entity_11;
  pick_entity_11.source_widget = "viewport";
  pick_entity_11.kind = EditorCommandKind::kSelectEntity;
  pick_entity_11.payload = SelectEntityCommand{11, false, false};
  SelectionState selected_once = ApplySelectionCommand(selection, pick_entity_11);
  check_true("select once sets single selection", selected_once.selected_entity_ids == std::vector<vkpt::core::StableId>{11});
  check_true("select once sets active primary", selected_once.active_primary_entity == 11);
  check_true("select once sets hovered entity", selected_once.hovered_entity == 11);
  check_true("select once source from viewport", selected_once.selection_source == SelectionSource::Viewport);

  EditorCommand append_entity_22;
  append_entity_22.source_widget = "scene_tree";
  append_entity_22.kind = EditorCommandKind::kSelectEntity;
  append_entity_22.payload = SelectEntityCommand{22, true, false};
  SelectionState selected_two = ApplySelectionCommand(selected_once, append_entity_22);
  check_true("append selection keeps two items", selected_two.selected_entity_ids == std::vector<vkpt::core::StableId>({11, 22}));
  check_true("append selection updates active", selected_two.active_primary_entity == 22);
  check_true("append source from scene tree", selected_two.selection_source == SelectionSource::SceneTree);

  EditorCommand toggle_entity_11;
  toggle_entity_11.source_widget = "inspector";
  toggle_entity_11.kind = EditorCommandKind::kToggleSelectEntity;
  toggle_entity_11.payload = ToggleSelectEntityCommand{11};
  SelectionState after_toggle = ApplySelectionCommand(selected_two, toggle_entity_11);
  check_true("toggle deselect one entity", after_toggle.selected_entity_ids == std::vector<vkpt::core::StableId>({22}));
  check_true("toggle keeps active primary", after_toggle.active_primary_entity == 22);
  check_true("toggle source from inspector", after_toggle.selection_source == SelectionSource::Inspector);

  EditorCommand append_range_selection;
  append_range_selection.source_widget = "scene_tree";
  append_range_selection.kind = EditorCommandKind::kSelectEntity;
  append_range_selection.payload = SelectEntityCommand{44, true, true};
  SelectionState range_add = ApplySelectionCommand(after_toggle, append_range_selection);
  check_true("append+range selection includes existing and new", range_add.selected_entity_ids == std::vector<vkpt::core::StableId>({22, 44}));
  check_true("append+range selection keeps hovered", range_add.hovered_entity == 44);
  check_true("append+range selection source from scene tree", range_add.selection_source == SelectionSource::SceneTree);

  EditorCommand range_selection;
  range_selection.source_widget = "scene_tree";
  range_selection.kind = EditorCommandKind::kSelectEntity;
  range_selection.payload = SelectEntityCommand{11, false, true};
  SelectionState range_replace = ApplySelectionCommand(after_toggle, range_selection);
  check_true("replace range selection keeps two markers", range_replace.selected_entity_ids.size() == 2);
  check_true("replace range selection sets active primary", range_replace.active_primary_entity == 11);
  check_true("replace range selection sets hovered", range_replace.hovered_entity == 11);

  EditorCommand clear_selection;
  clear_selection.source_widget = "menu";
  clear_selection.kind = EditorCommandKind::kClearSelection;
  clear_selection.payload = ClearSelectionCommand{};
  SelectionState cleared = ApplySelectionCommand(after_toggle, clear_selection);
  check_true("clear selection empties entities", cleared.selected_entity_ids.empty());
  check_true("clear selection resets hovered entity", cleared.hovered_entity == 0);
  check_true("clear selection keeps source from last command", cleared.selection_source == SelectionSource::Inspector);
  check_true("clear selection removes hovered_entity", cleared.hovered_entity == 0);

  EditorCommand escape_clear_selection;
  escape_clear_selection.source_widget = "keyboard";
  escape_clear_selection.kind = EditorCommandKind::kClearSelection;
  escape_clear_selection.payload = ClearSelectionCommand{};
  SelectionState escape_cleared = ApplySelectionCommand(selected_two, escape_clear_selection);
  check_true("escape clear selection empties entities", escape_cleared.selected_entity_ids.empty());
  check_true("escape clear selection resets active primary", escape_cleared.active_primary_entity == 0);

  auto mixed_field = BuildInspectorFieldStates("material.roughness", {"0.4", "0.8"});
  check_true("mixed inspector value marked mixed", mixed_field.size() == 1 && mixed_field[0].mixed);
  auto exact_field = BuildInspectorFieldStates("material.roughness", {"0.5", "0.5"});
  check_true("uniform inspector value not mixed", exact_field.size() == 1 && !exact_field[0].mixed && exact_field[0].value == "0.5");
  auto unsupported_field = BuildInspectorFieldStates("missing.field", {});
  check_true("missing inspector field unsupported", unsupported_field.size() == 1 && unsupported_field[0].unsupported);

  const auto tree_reorder = MakeReorderSiblingCommand(44, 10, 12, 7, "scene_tree");
  check_true("tree reorder command kind", tree_reorder.kind == EditorCommandKind::kReorderSibling);
  check_true("tree reorder command id", tree_reorder.command_id == "scene_tree.reorder_sibling");
  const auto reorder_payload = std::get<ReorderSiblingCommand>(tree_reorder.payload);
  check_true("tree reorder payload moved", reorder_payload.moved_entity == 44);
  check_true("tree reorder payload sibling before", reorder_payload.sibling_before == 10);
  check_true("tree reorder payload sibling after", reorder_payload.sibling_after == 12);
  check_true("tree reorder command source", tree_reorder.source_widget == "scene_tree");
  const std::string reorder_line = SerializeEditorCommand(tree_reorder);
  check_true("tree reorder serialized", reorder_line.find("\"moved_entity\":44") != std::string::npos);
  RunUiSceneTreeSmokeChecks(check_true);

  RunUiAssetDropSmokeChecks(check_true);

  const auto script_attachment = MakeMenuCommand("scripts.attach_script_to_selection", "menu");
  check_true("scripts.attach_script_to_selection command", script_attachment.kind == EditorCommandKind::kAttachScript);
  const auto script_detach = MakeMenuCommand("scripts.detach_script_from_selection", "menu");
  check_true("scripts.detach_script_from_selection command", script_detach.kind == EditorCommandKind::kDetachScript);
  const auto script_new = MakeMenuCommand("scripts.new_lua_script", "menu");
  check_true("scripts.new_lua_script currently unsupported by model", script_new.kind == EditorCommandKind::kUnsupportedUiAction);

  RunUiBenchmarkStatusSmokeChecks(check_true, selection);

  EditorCommandHistory commandHistory(4);
  commandHistory.push(new_scene);
  commandHistory.push(select_none);
  const std::string command_lines = SerializeEditorCommandsJsonl(commandHistory.history(), 4);
  check_true("command history serialization", command_lines.find("file.new_scene") != std::string::npos);

  UiEventLog eventLog(4);
  PushUiEvent(eventLog, "menu_click", "menu", "file.new_scene");
  PushUiEvent(eventLog, "menu_click", "menu", "edit.select_none");
  check_true("ui event log", eventLog.events().size() == 2);

  const auto shortcut_conflict_true = DetectShortcutConflicts(std::vector<UiShortcut>{
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open"},
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_recent", "Open"}
  });
  const auto shortcut_conflict_false = DetectShortcutConflicts(std::vector<UiShortcut>{
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open"},
    {static_cast<std::uint32_t>('S'), true, false, false, "file.save_scene", "Save"}
  });
  check_true("ui shortcut conflict true", shortcut_conflict_true);
  check_true("ui shortcut conflict false", !shortcut_conflict_false);

  const auto action_conflict_true = DetectShortcutConflicts(std::vector<UiShortcutAction>{
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_current_scene", "Run"},
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_cpu_calibration", "Run2"}
  });
  const auto action_conflict_false = DetectShortcutConflicts(std::vector<UiShortcutAction>{
    {static_cast<std::uint32_t>('B'), true, false, false, "benchmark.run_current_scene", "Run"},
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_cpu_calibration", "Run2"}
  });
  check_true("ui shortcut action conflict true", action_conflict_true);
  check_true("ui shortcut action conflict false", !action_conflict_false);

  const auto shortcuts = BuildDefaultUiShortcuts();
  check_true("shortcut ctrl+s", has_shortcut(shortcuts, "file.save_scene", 'S', true, false, false));
  check_true("shortcut ctrl+o", has_shortcut(shortcuts, "file.open_scene", 'O', true, false, false));
  check_true("shortcut ctrl+z", has_shortcut(shortcuts, "edit.undo", 'Z', true, false, false));
  check_true("shortcut ctrl+y", has_shortcut(shortcuts, "edit.redo", 'Y', true, false, false));
  check_true("shortcut ctrl+d", has_shortcut(shortcuts, "edit.duplicate", 'D', true, false, false));
  check_true("shortcut delete", has_shortcut(shortcuts, "edit.delete", 127, false, false, false));
  check_true("shortcut f", has_shortcut(shortcuts, "view.focus_selected", 'F', false, false, false));
  check_true("shortcut w", has_shortcut(shortcuts, "gizmo.translate", 'W', false, false, false));
  check_true("shortcut e", has_shortcut(shortcuts, "gizmo.rotate", 'E', false, false, false));
  check_true("shortcut r", has_shortcut(shortcuts, "gizmo.scale", 'R', false, false, false));
  check_true("shortcut q", has_shortcut(shortcuts, "gizmo.select", 'Q', false, false, false));
  check_true("shortcut ctrl+g", has_shortcut(shortcuts, "edit.group_selection", 'G', true, false, false));
  check_true("shortcut ctrl+shift+g", has_shortcut(shortcuts, "edit.ungroup_selection", 'G', true, true, false));
  check_true("shortcut ctrl+b", has_shortcut(shortcuts, "benchmark.run_current_scene", 'B', true, false, false));
  check_true("shortcut f11", has_shortcut(shortcuts, "view.fullscreen", 122, false, false, false));

  UiLayoutDocument panelLayout = CreateLayoutPreset(LayoutPreset::Default);
  const auto inspectorPanelBefore = find_panel(panelLayout, "inspector");
  check_true("panel state mutation target exists", inspectorPanelBefore != nullptr);
  if (inspectorPanelBefore) {
    const auto beforeX = inspectorPanelBefore->x;
    const auto beforeY = inspectorPanelBefore->y;
    const auto beforeWidth = inspectorPanelBefore->width;
    const auto beforeHeight = inspectorPanelBefore->height;
    const auto hideInspector = SetPanelVisible(panelLayout, "inspector", false);
    check_true("set panel visibility", hideInspector.changed && !find_panel(panelLayout, "inspector")->visible);
    const auto showInspector = SetPanelVisible(panelLayout, "inspector", true);
    check_true("restore panel visibility", showInspector.changed && find_panel(panelLayout, "inspector")->visible);
    const auto* restoredPanel = find_panel(panelLayout, "inspector");
    check_true("panel restore preserves position", restoredPanel != nullptr &&
              restoredPanel->x == beforeX &&
              restoredPanel->y == beforeY &&
              restoredPanel->width == beforeWidth &&
              restoredPanel->height == beforeHeight);
    const auto collapseInspector = SetPanelCollapsed(panelLayout, "inspector", true);
    check_true("set panel collapsed", collapseInspector.changed && find_panel(panelLayout, "inspector")->collapsed);
    const auto moveInspector = MovePanel(panelLayout, "inspector", 64.0f, 88.0f);
    const auto* movedPanel = find_panel(panelLayout, "inspector");
    check_true("move panel updates position", moveInspector.changed &&
              movedPanel != nullptr &&
              movedPanel->x == 64.0f &&
              movedPanel->y == 88.0f);
    const auto resizeInspector = ResizePanel(panelLayout, "inspector", 333.0f, 444.0f);
    const auto* resizedPanel = find_panel(panelLayout, "inspector");
    check_true("resize panel updates size", resizeInspector.changed &&
              resizedPanel != nullptr &&
              resizedPanel->width == 333.0f &&
              resizedPanel->height == 444.0f);
    const auto dockInspector = SetPanelDockState(panelLayout, "inspector", false, true);
    const auto* dockedPanel = find_panel(panelLayout, "inspector");
    check_true("set panel floating", dockInspector.changed && dockedPanel != nullptr &&
              dockedPanel->floating && !dockedPanel->docked);
    const auto commandMove = ApplyPanelStateCommand(panelLayout, "view.panel.move", true, 120.0f, "inspector");
    const auto* commandPanel = find_panel(panelLayout, "inspector");
    check_true("apply panel move command", commandMove.changed &&
              commandPanel != nullptr &&
              commandPanel->x == 120.0f);
    const auto commandResize = ApplyPanelStateCommand(panelLayout, "view.panel.resize", false, 512.0f, "inspector");
    const auto* commandResizePanel = find_panel(panelLayout, "inspector");
    check_true("apply panel resize command", commandResize.changed &&
              commandResizePanel != nullptr &&
              commandResizePanel->width == 512.0f);
    const auto commandToggleVisible = ApplyPanelStateCommand(panelLayout, "view.panel.toggle_visible", false, 0.0f, "inspector");
    const auto* commandVisiblePanel = find_panel(panelLayout, "inspector");
    check_true("apply panel toggle visible", commandToggleVisible.changed &&
              commandVisiblePanel != nullptr &&
              !commandVisiblePanel->visible);
  }
  const auto restoreBenchmarkLayout = RestoreLayoutPreset(panelLayout, LayoutPreset::Benchmark);
  check_true("restore benchmark layout", restoreBenchmarkLayout);
  check_true("restore layout changed preset", panelLayout.preset == LayoutPreset::Benchmark);
  const auto unknownPanelCommand = ApplyPanelStateCommand(panelLayout, "does_not_exist", false, 0.0f, "inspector");
  check_true("unknown panel command rejected", !unknownPanelCommand.changed);

  const auto release_gate = BuildUiReleaseGateEvidence();
  const std::string release_gate_json = SerializeUiReleaseGateChecklist(release_gate);
  check_true("release gate has no pending items",
             release_gate_json.find("\"pending_count\":0") != std::string::npos);
  check_true("release gate explicitly defers runtime UI gaps",
             release_gate_json.find("\"deferred_count\":") != std::string::npos &&
             release_gate_json.find("\"status\":\"deferred\"") != std::string::npos);

  std::cout << "ui model smoke: " << (ok ? "ok\n" : "failed\n");
  return ok;
}


}  // namespace vkpt::app
