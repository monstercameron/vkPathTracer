#include "editor/UiModels.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace vkpt::editor {

std::vector<UiShortcut> BuildDefaultUiShortcuts() {
  return {
    {static_cast<std::uint32_t>('S'), true, false, false, "file.save_scene", "Save Scene"},
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open Scene"},
    {static_cast<std::uint32_t>('Z'), true, false, false, "edit.undo", "Undo"},
    {static_cast<std::uint32_t>('Y'), true, false, false, "edit.redo", "Redo"},
    {static_cast<std::uint32_t>('D'), true, false, false, "edit.duplicate", "Duplicate"},
    {static_cast<std::uint32_t>(127), false, false, false, "edit.delete", "Delete"},
    {static_cast<std::uint32_t>('F'), false, false, false, "view.focus_selected", "Focus Selected"},
    {static_cast<std::uint32_t>('W'), false, false, false, "gizmo.translate", "Translate"},
    {static_cast<std::uint32_t>('E'), false, false, false, "gizmo.rotate", "Rotate"},
    {static_cast<std::uint32_t>('R'), false, false, false, "gizmo.scale", "Scale"},
    {static_cast<std::uint32_t>('Q'), false, false, false, "gizmo.select", "Select"},
    {static_cast<std::uint32_t>('G'), true, false, false, "edit.group_selection", "Group Selection"},
    {static_cast<std::uint32_t>('G'), true, true, false, "edit.ungroup_selection", "Ungroup Selection"},
    {static_cast<std::uint32_t>('B'), true, false, false, "benchmark.run_current_scene", "Run Benchmark"},
    {static_cast<std::uint32_t>(122), false, false, false, "view.fullscreen", "Fullscreen"},
  };
}

bool DetectShortcutConflicts(const std::vector<UiShortcut>& shortcuts) {
  std::vector<std::tuple<std::uint32_t, bool, bool, bool>> seen;
  for (const auto& shortcut : shortcuts) {
    const auto key = std::make_tuple(shortcut.key_code, shortcut.ctrl, shortcut.shift, shortcut.alt);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      return true;
    }
    seen.push_back(key);
  }
  return false;
}

bool DetectShortcutConflicts(const std::vector<UiShortcutAction>& shortcuts) {
  std::vector<std::tuple<std::uint32_t, bool, bool, bool>> seen;
  for (const auto& shortcut : shortcuts) {
    const auto key = std::make_tuple(shortcut.key_code, shortcut.ctrl, shortcut.shift, shortcut.alt);
    if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
      return true;
    }
    seen.push_back(key);
  }
  return false;
}

ShortcutResolution ResolveShortcut(const std::vector<UiShortcut>& shortcuts,
                                   std::uint32_t key_code,
                                   bool ctrl,
                                   bool shift,
                                   bool alt) {
  ShortcutResolution result;
  for (const auto& shortcut : shortcuts) {
    if (shortcut.key_code == key_code &&
        shortcut.ctrl == ctrl &&
        shortcut.shift == shift &&
        shortcut.alt == alt) {
      result.matched = true;
      result.conflicting_action_ids.push_back(shortcut.action_id);
    }
  }
  if (result.conflicting_action_ids.size() == 1u) {
    result.action_id = result.conflicting_action_ids.front();
  } else if (result.conflicting_action_ids.size() > 1u) {
    result.conflict = true;
  }
  return result;
}

EditorCommand MakeMenuCommand(std::string_view action_id, std::string_view source, vkpt::core::FrameIndex frame_index) {
  EditorCommand command;
  command.command_id = std::string(action_id);
  command.source_widget = std::string(source);
  command.frame_index = frame_index;

  const auto make_unsupported = [&](std::string_view reason) {
    command.kind = EditorCommandKind::kUnsupportedUiAction;
    command.undoable = false;
    command.redoable = false;
    command.validated = false;
    command.payload = UnsupportedUiActionCommand{
      std::string(action_id),
      std::string(reason)
    };
  };

  if (action_id == "file.new_scene") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "empty_scene";
    payload.entity_name = "New Scene";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "edit.select_none" || action_id == "edit.clear_selection") {
    command.kind = EditorCommandKind::kClearSelection;
    command.payload = ClearSelectionCommand{};
  } else if (action_id == "edit.duplicate") {
    command.kind = EditorCommandKind::kDuplicateEntity;
    command.payload = DuplicateEntityCommand{};
  } else if (action_id == "edit.delete") {
    command.kind = EditorCommandKind::kDeleteEntity;
    command.payload = DeleteEntityCommand{};
  } else if (action_id == "edit.group_selection") {
    command.kind = EditorCommandKind::kGroupEntities;
    command.payload = GroupEntitiesCommand{};
  } else if (action_id == "edit.ungroup_selection") {
    command.kind = EditorCommandKind::kUngroupEntities;
    command.payload = UngroupEntitiesCommand{};
  } else if (action_id == "edit.merge_selection") {
    command.kind = EditorCommandKind::kMergeEntities;
    command.payload = MergeEntitiesCommand{};
  } else if (action_id == "edit.split_merged_object") {
    make_unsupported("split merged object needs merged-object edit runtime");
  } else if (action_id == "create.empty_entity") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "empty";
    payload.entity_name = "New Entity";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.group_entity") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "group";
    payload.entity_name = "New Group";
    payload.requested_parent = 0;
    payload.as_group = true;
    command.payload = payload;
  } else if (action_id == "create.camera" || action_id == "create.light" ||
             action_id == "create.mesh_primitives" || action_id == "create.sdf_primitives") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = std::string(action_id);
    payload.entity_name = std::string(action_id);
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.material") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "material";
    payload.entity_name = "New Material";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.script") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "script";
    payload.entity_name = "New Script";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.physics_body") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "physics_body";
    payload.entity_name = "New Physics Body";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "create.benchmark_marker") {
    command.kind = EditorCommandKind::kCreateEntity;
    CreateEntityCommand payload;
    payload.template_name = "benchmark_marker";
    payload.entity_name = "New Benchmark Marker";
    payload.requested_parent = 0;
    payload.as_group = false;
    command.payload = payload;
  } else if (action_id == "benchmark.run_current_scene") {
    command.kind = EditorCommandKind::kRunBenchmark;
    command.payload = RunBenchmarkCommand{};
  } else if (action_id == "benchmark.run_scene_pack" ||
             action_id == "benchmark.run_cpu_calibration" ||
             action_id == "benchmark.run_gpu_calibration" ||
             action_id == "benchmark.run_simd_experiment" ||
             action_id == "benchmark.run_backend_experiment" ||
             action_id == "benchmark.compare_against_reference") {
    command.kind = EditorCommandKind::kRunBenchmark;
    RunBenchmarkCommand payload;
    payload.desc = MakeDefaultBenchmarkRunDesc("scene.json");
    command.payload = payload;
  } else if (action_id == "benchmark.history" ||
             action_id == "benchmark.open_artifacts" ||
             action_id == "benchmark.export_csv_json") {
    make_unsupported("benchmark artifact action requires benchmark runtime wiring");
  } else if (action_id == "scripts.attach_script_to_selection") {
    command.kind = EditorCommandKind::kAttachScript;
    command.payload = AttachScriptCommand{};
  } else if (action_id == "scripts.detach_script_from_selection") {
    command.kind = EditorCommandKind::kDetachScript;
    command.payload = DetachScriptCommand{};
  } else if (action_id == "edit.command_history") {
    command.kind = EditorCommandKind::kUnsupportedUiAction;
    command.payload = UnsupportedUiActionCommand{
      "edit.command_history",
      "editor command history panel is modeled but not yet wired to command execution"
    };
  } else if (action_id == "assets.import_files" || action_id == "assets.refresh_browser" ||
             action_id == "assets.show_missing_assets" || action_id == "assets.clear_generated_cache" ||
             action_id == "assets.clear_shader_cache" || action_id == "assets.reimport_selected" ||
             action_id == "assets.show_import_diagnostics") {
    make_unsupported("asset actions need import/picker runtime integration");
  } else if (action_id == "file.open_scene" || action_id == "file.open_recent" ||
             action_id == "file.save_scene" || action_id == "file.save_scene_as" ||
             action_id == "file.clone_scene" || action_id == "file.import_asset" ||
             action_id == "file.export_image" || action_id == "file.export_exr" ||
             action_id == "file.export_benchmark_artifacts" || action_id == "file.export_scene_snapshot" ||
             action_id == "file.preferences" || action_id == "file.reveal_artifacts_folder" ||
             action_id == "file.exit") {
    make_unsupported("file menu action requires scene/document runtime integration");
  } else if (action_id == "edit.undo" || action_id == "edit.redo") {
    make_unsupported("undo/redo command stack not wired in stub model");
  } else if (action_id == "edit.cut" || action_id == "edit.copy" || action_id == "edit.paste" ||
             action_id == "edit.rename" || action_id == "edit.select_all" || action_id == "edit.invert_selection" ||
             action_id == "edit.reset_transform" || action_id == "scene.validate_scene" ||
             action_id == "scene.freeze_benchmark_snapshot" || action_id == "scene.reset_accumulation" ||
             action_id == "scene.reload_scene" || action_id == "scene.reload_assets" ||
             action_id == "scene.hot_reload_scripts" || action_id == "scene.scene_settings" ||
             action_id == "scene.lighting_settings" || action_id == "scene.environment_settings" ||
             action_id == "scene.camera_settings" || action_id == "scene.physics_settings" ||
             action_id == "scene.script_settings" ||
             action_id == "edit.reparent_selection" ||
              action_id.starts_with("view.") || action_id.starts_with("render.") ||
              action_id.starts_with("tools.") || action_id.starts_with("help.") ||
              action_id.starts_with("scripts.") ||
              action_id.starts_with("scripts.new_lua_script")) {
    make_unsupported("action requires unimplemented editor runtime");
  } else {
    make_unsupported("unknown or unsupported menu action");
  }
  return command;
}

EditorCommand MakeClearSelectionCommand(vkpt::core::FrameIndex frame_index) {
  EditorCommand command;
  command.command_id = "edit.clear_selection";
  command.kind = EditorCommandKind::kClearSelection;
  command.source_widget = "edit_menu";
  command.frame_index = frame_index;
  command.payload = ClearSelectionCommand{};
  return command;
}

EditorCommand MakeCreateEntityCommand(std::string_view name, std::string_view template_name, vkpt::core::FrameIndex frame_index) {
  EditorCommand command;
  command.command_id = "create.empty_entity";
  command.kind = EditorCommandKind::kCreateEntity;
  command.source_widget = "create_menu";
  command.frame_index = frame_index;
  CreateEntityCommand payload;
  payload.entity_name = std::string(name);
  payload.template_name = std::string(template_name);
  payload.as_group = false;
  command.payload = payload;
  return command;
}

EditorCommand MakeReorderSiblingCommand(vkpt::core::StableId moved_entity,
                                       vkpt::core::StableId sibling_before,
                                       vkpt::core::StableId sibling_after,
                                       vkpt::core::FrameIndex frame_index,
                                       std::string_view source) {
  EditorCommand command;
  command.command_id = "scene_tree.reorder_sibling";
  command.kind = EditorCommandKind::kReorderSibling;
  command.source_widget = std::string(source);
  command.frame_index = frame_index;
  command.payload = ReorderSiblingCommand{
    moved_entity,
    sibling_before,
    sibling_after
  };
  return command;
}

}  // namespace vkpt::editor
