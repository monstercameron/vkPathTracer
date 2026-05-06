#include "editor/UiModels.h"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace vkpt::editor {

namespace {

MenuItem MenuItemNode(std::string_view id, std::string_view label, bool enabled = true) {
  MenuItem item;
  item.id = std::string(id);
  item.label = std::string(label);
  item.enabled = enabled;
  return item;
}

void DisableMenuItem(MenuItem& item, std::string_view reason) {
  item.enabled = false;
  item.disabled_reason = std::string(reason);
}

MenuItem* FindMenuItemRecursive(MenuItem& node, std::string_view item_id) {
  if (node.id == item_id) {
    return &node;
  }
  for (auto& child : node.children) {
    if (auto* found = FindMenuItemRecursive(child, item_id)) {
      return found;
    }
  }
  return nullptr;
}

const MenuItem* FindMenuItemRecursive(const MenuItem& node, std::string_view item_id) {
  if (node.id == item_id) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (auto* found = FindMenuItemRecursive(child, item_id)) {
      return found;
    }
  }
  return nullptr;
}

bool HasSelection(const SelectionState& selection) {
  return !selection.selected_entity_ids.empty();
}

void ApplyEditSelectionRules(MenuItem& edit_menu, const SelectionState& selection) {
  const bool hasSelection = HasSelection(selection);
  const std::array<std::string_view, 15> selectionItems{
    "edit.cut",
    "edit.copy",
    "edit.paste",
    "edit.duplicate",
    "edit.delete",
    "edit.rename",
    "edit.group_selection",
    "edit.ungroup_selection",
    "edit.merge_selection",
    "edit.split_merged_object",
    "edit.reparent_selection",
    "edit.reset_transform",
    "scene_tree.cut",
    "scene_tree.duplicate",
    "scene_tree.delete"
  };

  for (auto& child : edit_menu.children) {
    const bool dependsOnSelection =
        std::find(selectionItems.begin(), selectionItems.end(), child.id) != selectionItems.end();
    if (dependsOnSelection) {
      if (hasSelection) {
        child.enabled = true;
        child.disabled_reason.clear();
      } else {
        DisableMenuItem(child, "requires at least one selected entity");
      }
    }
  }
}

}  // namespace

MenuBar BuildDefaultMenuBar() {
  MenuBar result;
  auto& top = result.top_level_menus;
  top.push_back(MenuItemNode("file", "File"));
  top.back().children.push_back(MenuItemNode("file.new_scene", "New Scene"));
  top.back().children.push_back(MenuItemNode("file.open_scene", "Open Scene"));
  top.back().children.push_back(MenuItemNode("file.open_recent", "Open Recent"));
  top.back().children.push_back(MenuItemNode("file.save_scene", "Save Scene"));
  top.back().children.push_back(MenuItemNode("file.save_scene_as", "Save Scene As"));
  top.back().children.push_back(MenuItemNode("file.clone_scene", "Clone Scene"));
  top.back().children.push_back(MenuItemNode("file.import_asset", "Import Asset"));
  top.back().children.push_back(MenuItemNode("file.export_image", "Export Image"));
  top.back().children.push_back(MenuItemNode("file.export_exr", "Export EXR"));
  top.back().children.push_back(MenuItemNode("file.export_benchmark_artifacts", "Export Benchmark Artifacts"));
  top.back().children.push_back(MenuItemNode("file.export_scene_snapshot", "Export Scene Snapshot"));
  top.back().children.push_back(MenuItemNode("file.preferences", "Preferences"));
  top.back().children.push_back(MenuItemNode("file.reveal_artifacts_folder", "Reveal Artifacts Folder"));
  top.back().children.push_back(MenuItemNode("file.exit", "Exit"));

  top.push_back(MenuItemNode("edit", "Edit"));
  top.back().children.push_back(MenuItemNode("edit.undo", "Undo"));
  top.back().children.push_back(MenuItemNode("edit.redo", "Redo"));
  top.back().children.push_back(MenuItemNode("edit.cut", "Cut"));
  top.back().children.push_back(MenuItemNode("edit.copy", "Copy"));
  top.back().children.push_back(MenuItemNode("edit.paste", "Paste"));
  top.back().children.push_back(MenuItemNode("edit.duplicate", "Duplicate"));
  top.back().children.push_back(MenuItemNode("edit.delete", "Delete"));
  top.back().children.push_back(MenuItemNode("edit.rename", "Rename"));
  top.back().children.push_back(MenuItemNode("edit.select_all", "Select All"));
  top.back().children.push_back(MenuItemNode("edit.select_none", "Select None"));
  top.back().children.push_back(MenuItemNode("edit.invert_selection", "Invert Selection"));
  top.back().children.push_back(MenuItemNode("edit.group_selection", "Group Selection"));
  top.back().children.push_back(MenuItemNode("edit.ungroup_selection", "Ungroup Selection"));
  top.back().children.push_back(MenuItemNode("edit.merge_selection", "Merge Selection"));
  top.back().children.push_back(MenuItemNode("edit.split_merged_object", "Split Merged Object"));
  top.back().children.push_back(MenuItemNode("edit.reparent_selection", "Reparent Selection"));
  top.back().children.push_back(MenuItemNode("edit.reset_transform", "Reset Transform"));
  top.back().children.push_back(MenuItemNode("edit.command_history", "Command History"));

  top.push_back(MenuItemNode("view", "View"));
  MenuItem panelsMenu = MenuItemNode("view.panels", "Panels");
  for (const auto& panel : BuildDefaultPanelDefinitions()) {
    if (panel.can_close || panel.default_area == UiDockArea::Bottom || panel.default_area == UiDockArea::Right ||
        panel.default_area == UiDockArea::Left) {
      panelsMenu.children.push_back(MenuItemNode(panel.menu_action_id, panel.title));
    }
  }
  top.back().children.push_back(std::move(panelsMenu));
  MenuItem layoutsMenu = MenuItemNode("view.layouts", "Layouts");
  layoutsMenu.children.push_back(MenuItemNode("view.layout.default", "Default"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.benchmark", "Benchmark"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.material_authoring", "Material Authoring"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.scripting", "Scripting"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.asset_management", "Asset Management"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.debug_profiler", "Debug/Profiler"));
  layoutsMenu.children.push_back(MenuItemNode("view.layout.minimal_viewport", "Minimal Viewport"));
  top.back().children.push_back(std::move(layoutsMenu));
  top.back().children.push_back(MenuItemNode("view.overlays", "Overlays"));
  top.back().children.push_back(MenuItemNode("view.debug_views", "Debug Views"));
  top.back().children.push_back(MenuItemNode("view.fullscreen", "Fullscreen"));
  top.back().children.push_back(MenuItemNode("view.ui_scale", "UI Scale"));
  top.back().children.push_back(MenuItemNode("view.reset_layout", "Reset Layout"));

  top.push_back(MenuItemNode("create", "Create"));
  top.back().children.push_back(MenuItemNode("create.empty_entity", "Empty Entity"));
  top.back().children.push_back(MenuItemNode("create.group_entity", "Group Entity"));
  top.back().children.push_back(MenuItemNode("create.camera", "Camera"));
  top.back().children.push_back(MenuItemNode("create.light", "Light"));
  top.back().children.push_back(MenuItemNode("create.mesh_primitives", "Primitive Mesh"));
  top.back().children.push_back(MenuItemNode("create.sdf_primitives", "SDF Primitive"));
  top.back().children.push_back(MenuItemNode("create.material", "Material"));
  top.back().children.push_back(MenuItemNode("create.script", "Script"));
  top.back().children.push_back(MenuItemNode("create.physics_body", "Physics Body"));
  top.back().children.push_back(MenuItemNode("create.benchmark_marker", "Benchmark Marker"));

  top.push_back(MenuItemNode("scene", "Scene"));
  top.back().children.push_back(MenuItemNode("scene.validate_scene", "Validate Scene"));
  top.back().children.push_back(MenuItemNode("scene.freeze_benchmark_snapshot", "Freeze Benchmark Snapshot"));
  top.back().children.push_back(MenuItemNode("scene.reset_accumulation", "Reset Accumulation"));
  top.back().children.push_back(MenuItemNode("scene.reload_scene", "Reload Scene"));
  top.back().children.push_back(MenuItemNode("scene.reload_assets", "Reload Assets"));
  top.back().children.push_back(MenuItemNode("scene.hot_reload_scripts", "Hot Reload Scripts"));
  top.back().children.push_back(MenuItemNode("scene.scene_settings", "Scene Settings"));
  top.back().children.push_back(MenuItemNode("scene.lighting_settings", "Lighting Settings"));
  top.back().children.push_back(MenuItemNode("scene.environment_settings", "Environment Settings"));
  top.back().children.push_back(MenuItemNode("scene.camera_settings", "Camera Settings"));
  top.back().children.push_back(MenuItemNode("scene.physics_settings", "Physics Settings"));
  top.back().children.push_back(MenuItemNode("scene.script_settings", "Script Settings"));
  top.back().children.push_back(MenuItemNode("scene.animation_settings", "Animation Settings"));

  top.push_back(MenuItemNode("render", "Render"));
  top.back().children.push_back(MenuItemNode("render.start_render", "Start Render..."));
  top.back().children.push_back(MenuItemNode("render.backend", "Backend"));
  top.back().children.push_back(MenuItemNode("render.renderer_path", "Renderer Path"));
  top.back().children.push_back(MenuItemNode("render.quality_presets", "Quality Presets"));
  top.back().children.push_back(MenuItemNode("render.resolution", "Resolution"));
  top.back().children.push_back(MenuItemNode("render.spp", "SPP"));
  top.back().children.push_back(MenuItemNode("render.max_bounces", "Max Bounces"));
  top.back().children.push_back(MenuItemNode("render.denoiser", "Denoiser"));
  top.back().children.push_back(MenuItemNode("render.tone_mapping", "Tone Mapping"));
  top.back().children.push_back(MenuItemNode("render.exposure", "Exposure"));
  top.back().children.push_back(MenuItemNode("render.debug_channel", "Debug Channel"));
  top.back().children.push_back(MenuItemNode("render.shader_cache", "Shader Cache"));
  top.back().children.push_back(MenuItemNode("render.backend_capabilities", "Backend Capabilities"));

  top.push_back(MenuItemNode("benchmark", "Benchmark"));
  top.back().children.push_back(MenuItemNode("benchmark.run_current_scene", "Run Current Scene"));
  top.back().children.push_back(MenuItemNode("benchmark.run_scene_pack", "Run Scene Pack"));
  top.back().children.push_back(MenuItemNode("benchmark.run_cpu_calibration", "Run CPU Calibration"));
  top.back().children.push_back(MenuItemNode("benchmark.run_gpu_calibration", "Run GPU Calibration"));
  top.back().children.push_back(MenuItemNode("benchmark.run_simd_experiment", "Run SIMD Experiment"));
  top.back().children.push_back(MenuItemNode("benchmark.run_backend_experiment", "Run Backend Experiment"));
  top.back().children.push_back(MenuItemNode("benchmark.compare_against_reference", "Compare Against Reference"));
  top.back().children.push_back(MenuItemNode("benchmark.open_artifacts", "Open Benchmark Artifacts"));
  top.back().children.push_back(MenuItemNode("benchmark.export_csv_json", "Export CSV/JSON"));
  top.back().children.push_back(MenuItemNode("benchmark.history", "Benchmark History"));

  top.push_back(MenuItemNode("assets", "Assets"));
  top.back().children.push_back(MenuItemNode("assets.import_files", "Import Files"));
  top.back().children.push_back(MenuItemNode("assets.reimport_selected", "Reimport Selected"));
  top.back().children.push_back(MenuItemNode("assets.refresh_browser", "Refresh Asset Browser"));
  top.back().children.push_back(MenuItemNode("assets.show_missing_assets", "Show Missing Assets"));
  top.back().children.push_back(MenuItemNode("assets.show_import_diagnostics", "Show Import Diagnostics"));
  top.back().children.push_back(MenuItemNode("assets.clear_generated_cache", "Clear Generated Cache"));
  top.back().children.push_back(MenuItemNode("assets.clear_shader_cache", "Clear Shader Cache"));

  top.push_back(MenuItemNode("scripts", "Scripts"));
  top.back().children.push_back(MenuItemNode("scripts.new_lua_script", "New Lua Script"));
  top.back().children.push_back(MenuItemNode("scripts.attach_script_to_selection", "Attach Script To Selection"));
  top.back().children.push_back(MenuItemNode("scripts.detach_script_from_selection", "Detach Script From Selection"));
  top.back().children.push_back(MenuItemNode("scripts.reload_scripts", "Reload Scripts"));
  top.back().children.push_back(MenuItemNode("scripts.open_script_folder", "Open Script Folder"));
  top.back().children.push_back(MenuItemNode("scripts.show_script_lifecycle_events", "Show Script Lifecycle Events"));
  top.back().children.push_back(MenuItemNode("scripts.show_script_errors", "Show Script Errors"));
  top.back().children.push_back(MenuItemNode("scripts.show_script_profiler", "Show Script Profiler"));
  top.back().children.push_back(MenuItemNode("scripts.sandbox_settings", "Sandbox Settings"));

  top.push_back(MenuItemNode("tools", "Tools"));
  top.back().children.push_back(MenuItemNode("tools.doctor", "Doctor"));
  top.back().children.push_back(MenuItemNode("tools.crash_artifacts", "Crash Artifacts"));
  top.back().children.push_back(MenuItemNode("tools.profiler", "Profiler"));
  top.back().children.push_back(MenuItemNode("tools.frame_capture", "Frame Capture"));
  top.back().children.push_back(MenuItemNode("tools.shader_manifest", "Shader Manifest"));
  top.back().children.push_back(MenuItemNode("tools.asset_manifest", "Asset Manifest"));
  top.back().children.push_back(MenuItemNode("tools.scene_snapshot", "Scene Snapshot"));
  top.back().children.push_back(MenuItemNode("tools.capability_matrix", "Capability Matrix"));
  top.back().children.push_back(MenuItemNode("tools.settings_dump", "Settings Dump"));
  top.back().children.push_back(MenuItemNode("tools.startup_self_test", "Run Startup Self-Test"));

  top.push_back(MenuItemNode("help", "Help"));
  top.back().children.push_back(MenuItemNode("help.controls", "Controls"));
  top.back().children.push_back(MenuItemNode("help.shortcut_reference", "Shortcut Reference"));
  top.back().children.push_back(MenuItemNode("help.about", "About"));
  top.back().children.push_back(MenuItemNode("help.build_info", "Build Info"));
  top.back().children.push_back(MenuItemNode("help.feature_flags", "Feature Flags"));
  top.back().children.push_back(MenuItemNode("help.dependency_info", "Dependency Info"));
  return result;
}

MenuBar BuildDefaultMenuBar(const SelectionState& selection) {
  auto menu = BuildDefaultMenuBar();
  return ApplySelectionRulesToEditMenu(std::move(menu), selection);
}

MenuItem* FindMenuItem(MenuBar& menu, std::string_view item_id) {
  for (auto& item : menu.top_level_menus) {
    if (auto* found = FindMenuItemRecursive(item, item_id)) {
      return found;
    }
  }
  return nullptr;
}

const MenuItem* FindMenuItem(const MenuBar& menu, std::string_view item_id) {
  for (const auto& item : menu.top_level_menus) {
    if (auto* found = FindMenuItemRecursive(item, item_id)) {
      return found;
    }
  }
  return nullptr;
}

MenuBar ApplySelectionRulesToEditMenu(MenuBar menu, const SelectionState& selection) {
  auto* edit_menu = FindMenuItem(menu, "edit");
  if (!edit_menu) {
    return menu;
  }
  ApplyEditSelectionRules(*edit_menu, selection);
  return menu;
}

std::vector<MenuEnablement> GetEditMenuEnablements(const SelectionState& selection) {
  std::vector<MenuEnablement> result;
  const auto edit_menu = BuildDefaultMenuBar(selection);
  const auto* edit = FindMenuItem(edit_menu, "edit");
  if (!edit) {
    return result;
  }
  result.reserve(edit->children.size());
  for (const auto& item : edit->children) {
    result.push_back({item.id, item.enabled});
  }
  return result;
}

}  // namespace vkpt::editor
