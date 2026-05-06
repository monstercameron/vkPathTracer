#include "editor/UiModelsInternal.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vkpt::editor {

std::vector<InspectorFieldValue> BuildInspectorFieldStates(
    std::string_view field_id,
    const std::vector<std::string>& values) {
  InspectorFieldValue field;
  field.field_id = std::string(field_id);
  if (values.empty()) {
    field.unsupported = true;
    field.value.clear();
    return {field};
  }
  if (values.size() == 1) {
    field.value = values[0];
    return {field};
  }
  field.value = values[0];
  for (std::size_t i = 1; i < values.size(); ++i) {
    if (values[i] != field.value) {
      field.mixed = true;
      break;
    }
  }
  return {field};
}

AssetDropValidation ValidateAssetDrop(std::string_view asset_path,
                                     std::string_view target_slot) {
  AssetDropValidation result;
  if (asset_path.empty()) {
    result.reason = "empty asset path";
    return result;
  }

  const std::filesystem::path path(asset_path);
  result.normalized_path = std::filesystem::absolute(path).string();
  const auto ext = ToLower(path.extension().string());
  const std::string_view ext_no_dot = ext.empty() ? std::string_view{} : std::string_view(ext).substr(1);

  static const std::vector<std::string_view> texture_extensions{
    "png", "jpg", "jpeg", "bmp", "tga", "tiff", "tif", "exr", "hdr"
  };
  static const std::vector<std::string_view> model_extensions{
    "obj", "fbx", "gltf", "glb", "ply", "abc", "usd", "usdc", "usda"
  };
  static const std::vector<std::string_view> scene_extensions{
    "json", "ptscene"
  };
  static const std::vector<std::string_view> lua_extensions{
    "lua"
  };
  static const std::vector<std::string_view> benchmark_extensions{
    "json", "csv", "txt"
  };

  const bool is_texture = IsIn(texture_extensions, ext_no_dot);
  const bool is_model = IsIn(model_extensions, ext_no_dot);
  const bool is_scene = IsIn(scene_extensions, ext_no_dot);
  const bool is_lua = IsIn(lua_extensions, ext_no_dot);
  const bool is_benchmark = IsIn(benchmark_extensions, ext_no_dot);
  result.extension_supported = is_texture || is_model || is_scene || is_lua || is_benchmark;

  if (!result.extension_supported) {
    result.reason = "unsupported extension";
    return result;
  }

  if (target_slot == "texture_slot" || target_slot == "material_slot" || target_slot == "material") {
    if (!is_texture) {
      result.reason = "target expects texture/texture-like asset";
      return result;
    }
    result.accepted = true;
    result.asset_type = "texture";
    result.target_slot = std::string(target_slot);
    return result;
  }
  if (target_slot == "script_slot" || target_slot == "script" || target_slot == "lua_slot") {
    if (!is_lua) {
      result.reason = "target expects lua script";
      return result;
    }
    result.accepted = true;
    result.asset_type = "script";
    result.target_slot = std::string(target_slot);
    return result;
  }
  if (target_slot == "scene_slot" || target_slot == "scene" || target_slot == "scene_open") {
    if (!is_scene) {
      result.reason = "target expects scene asset";
      return result;
    }
    result.accepted = true;
    result.asset_type = "scene";
    result.target_slot = std::string(target_slot);
    return result;
  }
  if (target_slot == "benchmark_slot" || target_slot == "benchmark_reference") {
    if (!is_benchmark) {
      result.reason = "target expects benchmark descriptor";
      return result;
    }
    result.accepted = true;
    result.asset_type = "benchmark_descriptor";
    result.target_slot = std::string(target_slot);
    return result;
  }

  if (target_slot == "model_slot" || target_slot == "asset_browser" || target_slot.empty()) {
    result.accepted = true;
    if (is_model) result.asset_type = "model";
    else if (is_texture) result.asset_type = "texture";
    else if (is_scene) result.asset_type = "scene";
    else if (is_lua) result.asset_type = "script";
    else result.asset_type = "benchmark_descriptor";
    result.target_slot = std::string(target_slot);
    return result;
  }

  result.reason = "unknown drop target";
  return result;
}

std::vector<UiPanelDefinition> BuildDefaultPanelDefinitions() {
  return {
    {"menu_bar", "Menu Bar", "view.panel.menu_bar", true, false, false, false, 1280.0f, 28.0f,
     UiDockArea::Top, "chrome", 0, "chrome", "Application menus and command routing."},
    {"toolbar", "Toolbar", "view.panel.toolbar", true, false, false, false, 1280.0f, 36.0f,
     UiDockArea::Top, "chrome", 1, "chrome", "Primary viewport and editing commands."},
    {"viewport", "Viewport", "view.panel.viewport", true, true, false, false, 960.0f, 540.0f,
     UiDockArea::Center, "center", 10, "viewport", "Interactive render viewport."},
    {"scene_tree", "Scene Graph", "view.panel.scene_tree", true, true, true, true, 280.0f, 600.0f,
     UiDockArea::Left, "scene", 20, "scene_tree", "Hierarchy, visibility, lock, and selection state."},
    {"inspector", "Inspector", "view.panel.inspector", true, true, true, true, 360.0f, 600.0f,
     UiDockArea::Right, "properties", 30, "inspector", "Selected entity and component properties."},
    {"material_editor", "Materials", "view.panel.material_editor", false, true, true, true, 520.0f, 420.0f,
     UiDockArea::Right, "properties", 40, "materials", "Material assignment and shader parameters."},
    {"lights_panel", "Lights", "view.panel.lights", false, true, true, true, 360.0f, 360.0f,
     UiDockArea::Right, "properties", 50, "lights", "Light list and selected light controls."},
    {"camera_panel", "Camera", "view.panel.camera", false, true, true, true, 360.0f, 320.0f,
     UiDockArea::Right, "properties", 60, "camera", "Camera pose, lens, and navigation settings."},
    {"render_settings", "Render Settings", "view.panel.render_settings", true, true, true, true, 380.0f, 420.0f,
     UiDockArea::Right, "properties", 70, "render_settings", "Renderer backend, quality, accumulation, and tone mapping."},
    {"benchmark_panel", "Benchmark", "view.panel.benchmark", false, true, true, true, 560.0f, 480.0f,
     UiDockArea::Right, "benchmark", 80, "benchmark", "Benchmark descriptor and run controls."},
    {"benchmark_history", "Benchmark History", "view.panel.benchmark_history", false, true, true, true, 680.0f, 360.0f,
     UiDockArea::Bottom, "benchmark", 90, "benchmark_history", "Stored benchmark runs, artifacts, and regressions."},
    {"diagnostics", "Diagnostics", "view.panel.diagnostics", false, true, true, true, 520.0f, 300.0f,
     UiDockArea::Bottom, "debug", 100, "diagnostics", "Warnings, errors, import diagnostics, and event log filters."},
    {"performance", "Performance", "view.panel.performance", false, true, true, true, 520.0f, 300.0f,
     UiDockArea::Bottom, "debug", 110, "performance", "Frame timing, job pressure, memory, and UI budget."},
    {"debug_views", "Debug Views", "view.panel.debug_views", false, true, true, true, 420.0f, 320.0f,
     UiDockArea::Right, "debug", 120, "debug_views", "Renderer debug channels and visualization toggles."},
    {"asset_browser", "Asset Browser", "view.panel.asset_browser", true, true, true, true, 720.0f, 260.0f,
     UiDockArea::Bottom, "assets", 130, "asset_browser", "Asset search, validation, import, and assignment."},
    {"timeline", "Timeline", "view.panel.timeline", false, true, true, true, 720.0f, 220.0f,
     UiDockArea::Bottom, "animation", 140, "timeline", "Animation timeline and keyframe controls."},
    {"script_panel", "Scripts", "view.panel.script_panel", false, true, true, true, 520.0f, 420.0f,
     UiDockArea::Right, "scripting", 150, "scripts", "Script attachment, lifecycle events, and sandbox settings."},
    {"physics", "Physics", "view.panel.physics", false, true, true, true, 420.0f, 360.0f,
     UiDockArea::Right, "simulation", 160, "physics", "Physics bodies, colliders, and simulation properties."},
    {"console", "Console", "view.panel.console", true, true, true, true, 720.0f, 260.0f,
     UiDockArea::Bottom, "debug", 170, "console", "Command log and textual diagnostics."},
    {"status_bar", "Status Bar", "view.panel.status_bar", true, false, false, false, 1280.0f, 28.0f,
     UiDockArea::Status, "chrome", 180, "status", "Scene, renderer, performance, and selection summary."},
  };
}

std::vector<InspectorFieldSchema> BuildDefaultInspectorSchemas() {
  return {
    {"transform.position", "Position", "Transform", InspectorControlKind::Vector3, true, true, -100000.0f, 100000.0f, 0.01f, {}},
    {"transform.rotation", "Rotation", "Transform", InspectorControlKind::Vector3, true, true, -360.0f, 360.0f, 0.1f, {}},
    {"transform.scale", "Scale", "Transform", InspectorControlKind::Vector3, true, true, 0.0001f, 10000.0f, 0.01f, {}},
    {"transform.visible", "Visible", "Transform", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"transform.locked", "Locked", "Transform", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"material.id", "Material", "Material", InspectorControlKind::AssetPicker, true, true, 0.0f, 0.0f, 0.0f, {}},
    {"material.base_color", "Base Color", "Material", InspectorControlKind::Color, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.opacity", "Opacity", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.roughness", "Roughness", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.metallic", "Metallic", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.transmission", "Transmission", "Material", InspectorControlKind::Slider, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.ior", "Index of Refraction", "Material", InspectorControlKind::Number, true, true, 1.0f, 3.0f, 0.01f, {}},
    {"material.emission_color", "Emission Color", "Material", InspectorControlKind::Color, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"material.emission_strength", "Emission Strength", "Material", InspectorControlKind::Number, true, true, 0.0f, 100000.0f, 0.1f, {}},
    {"light.type", "Type", "Light", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"point", "sphere", "directional", "environment"}},
    {"light.intensity", "Intensity", "Light", InspectorControlKind::Number, true, true, 0.0f, 100000.0f, 0.1f, {}},
    {"light.color", "Color", "Light", InspectorControlKind::Color, true, true, 0.0f, 1.0f, 0.01f, {}},
    {"light.radius", "Radius", "Light", InspectorControlKind::Number, true, true, 0.0f, 10000.0f, 0.01f, {}},
    {"light.direction", "Direction", "Light", InspectorControlKind::Vector3, true, true, -1.0f, 1.0f, 0.01f, {}},
    {"light.enabled", "Enabled", "Light", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"camera.fov_y", "Vertical FOV", "Camera", InspectorControlKind::Slider, true, false, 1.0f, 179.0f, 0.1f, {}},
    {"camera.focal_length_mm", "Focal Length", "Camera", InspectorControlKind::Number, true, false, 8.0f, 300.0f, 0.1f, {}},
    {"camera.sensor_width_mm", "Sensor Width", "Camera", InspectorControlKind::Number, true, false, 4.0f, 70.0f, 0.1f, {}},
    {"camera.sensor_height_mm", "Sensor Height", "Camera", InspectorControlKind::Number, true, false, 4.0f, 70.0f, 0.1f, {}},
    {"camera.aperture", "Aperture", "Camera", InspectorControlKind::Number, true, false, 0.0f, 64.0f, 0.01f, {}},
    {"camera.focus_distance", "Focus Distance", "Camera", InspectorControlKind::Number, true, false, 0.001f, 100000.0f, 0.01f, {}},
    {"camera.f_stop", "F-stop", "Camera", InspectorControlKind::Number, true, false, 0.0f, 32.0f, 0.1f, {}},
    {"camera.controller", "Controller", "Camera", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"orbit", "fps", "turntable", "scripted_benchmark_path"}},
    {"camera.shutter_seconds", "Shutter", "Camera", InspectorControlKind::Number, true, false, 0.000125f, 1.0f, 0.000125f, {}},
    {"camera.iso", "ISO", "Camera", InspectorControlKind::Number, true, false, 25.0f, 12800.0f, 1.0f, {}},
    {"camera.exposure", "Exposure", "Camera", InspectorControlKind::Number, true, false, -16.0f, 16.0f, 0.1f, {}},
    {"camera.white_balance", "White Balance", "Camera", InspectorControlKind::Number, true, false, 1000.0f, 40000.0f, 50.0f, {}},
    {"camera.iris_blade_count", "Iris Blades", "Camera", InspectorControlKind::Number, true, false, 0.0f, 16.0f, 1.0f, {}},
    {"camera.iris_rotation_degrees", "Iris Rotation", "Camera", InspectorControlKind::Number, true, false, -180.0f, 180.0f, 1.0f, {}},
    {"camera.iris_roundness", "Iris Roundness", "Camera", InspectorControlKind::Slider, true, false, 0.0f, 1.0f, 0.01f, {}},
    {"camera.anamorphic_squeeze", "Anamorphic Squeeze", "Camera", InspectorControlKind::Number, true, false, 0.25f, 4.0f, 0.01f, {}},
    {"render.backend", "Backend", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"cpu_scalar", "cpu_simd", "vulkan", "d3d12"}},
    {"render.renderer_path", "Renderer Path", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"pathtracer", "hybrid", "reference"}},
    {"render.samples_per_pixel", "Samples Per Pixel", "Render", InspectorControlKind::Number, true, false, 1.0f, 65536.0f, 1.0f, {}},
    {"render.max_depth", "Max Bounces", "Render", InspectorControlKind::Number, true, false, 1.0f, 256.0f, 1.0f, {}},
    {"render.accumulation", "Accumulate", "Render", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"render.denoiser", "Denoiser", "Render", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"render.tone_mapping", "Tone Mapping", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"filmic", "aces", "linear", "reinhard"}},
    {"render.output_transform", "Output Transform", "Render", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"gamma", "linear"}},
    {"render.debug_channel", "Debug Channel", "Debug", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"beauty", "albedo", "normal", "depth", "variance", "heatmap"}},
    {"render.reset_accumulation", "Reset Accumulation", "Render", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"benchmark.scene", "Scene", "Benchmark", InspectorControlKind::AssetPicker, true, false, 0.0f, 0.0f, 0.0f, {}},
    {"benchmark.duration", "Duration", "Benchmark", InspectorControlKind::Number, true, false, 1.0f, 3600.0f, 1.0f, {}},
    {"benchmark.warmup_frames", "Warmup Frames", "Benchmark", InspectorControlKind::Number, true, false, 0.0f, 10000.0f, 1.0f, {}},
    {"benchmark.seed", "Seed", "Benchmark", InspectorControlKind::Number, true, false, 0.0f, 4294967295.0f, 1.0f, {}},
    {"diagnostics.severity", "Severity", "Diagnostics", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"trace", "info", "warning", "error"}},
    {"diagnostics.subsystem", "Subsystem", "Diagnostics", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"all", "render", "scene", "assets", "ui", "scripts"}},
    {"diagnostics.auto_scroll", "Auto Scroll", "Diagnostics", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"performance.ui_frame_budget_ms", "UI Frame Budget", "Performance", InspectorControlKind::Number, true, false, 0.1f, 33.0f, 0.1f, {}},
    {"performance.panel_build_budget_ms", "Panel Build Budget", "Performance", InspectorControlKind::Number, true, false, 0.1f, 16.0f, 0.1f, {}},
    {"performance.thumbnail_jobs", "Thumbnail Jobs", "Performance", InspectorControlKind::Number, true, false, 0.0f, 64.0f, 1.0f, {}},
    {"asset.query", "Search", "Asset", InspectorControlKind::Text, true, false, 0.0f, 0.0f, 0.0f, {}},
    {"asset.category", "Category", "Asset", InspectorControlKind::Enum, true, false, 0.0f, 0.0f, 0.0f, {"all", "mesh", "texture", "material", "script", "scene"}},
    {"asset.show_missing", "Show Missing", "Asset", InspectorControlKind::Toggle, true, false, 0.0f, 1.0f, 1.0f, {}},
    {"script.path", "Script", "Script", InspectorControlKind::AssetPicker, true, true, 0.0f, 0.0f, 0.0f, {}},
    {"script.enabled", "Enabled", "Script", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"script.reload_on_save", "Reload on Save", "Script", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"timeline.current_frame", "Current Frame", "Timeline", InspectorControlKind::Number, true, false, 0.0f, 1000000.0f, 1.0f, {}},
    {"timeline.playback_rate", "Playback Rate", "Timeline", InspectorControlKind::Number, true, false, 0.01f, 8.0f, 0.01f, {}},
    {"physics.enabled", "Physics Enabled", "Physics", InspectorControlKind::Toggle, true, true, 0.0f, 1.0f, 1.0f, {}},
    {"physics.body_type", "Body Type", "Physics", InspectorControlKind::Enum, true, true, 0.0f, 0.0f, 0.0f, {"static", "dynamic", "kinematic"}},
    {"physics.mass", "Mass", "Physics", InspectorControlKind::Number, true, true, 0.0f, 100000.0f, 0.01f, {}},
    {"physics.collision_shape", "Collision Shape", "Physics", InspectorControlKind::Enum, true, true, 0.0f, 0.0f, 0.0f, {"box", "sphere", "capsule", "mesh"}},
  };
}

std::vector<InspectorFieldSchema> BuildInspectorSchemasForPanel(std::string_view panel_id) {
  const auto schemas = BuildDefaultInspectorSchemas();
  if (panel_id == "inspector") {
    return schemas;
  }

  std::vector<std::string_view> components;
  if (panel_id == "material_editor") {
    components = {"Material"};
  } else if (panel_id == "lights_panel") {
    components = {"Light"};
  } else if (panel_id == "camera_panel") {
    components = {"Camera"};
  } else if (panel_id == "render_settings") {
    components = {"Render"};
  } else if (panel_id == "benchmark_panel" || panel_id == "benchmark_history") {
    components = {"Benchmark"};
  } else if (panel_id == "diagnostics" || panel_id == "console") {
    components = {"Diagnostics"};
  } else if (panel_id == "performance") {
    components = {"Performance"};
  } else if (panel_id == "debug_views") {
    components = {"Debug"};
  } else if (panel_id == "asset_browser") {
    components = {"Asset"};
  } else if (panel_id == "script_panel") {
    components = {"Script"};
  } else if (panel_id == "timeline") {
    components = {"Timeline"};
  } else if (panel_id == "physics") {
    components = {"Physics"};
  } else if (panel_id == "scene_tree" || panel_id == "viewport") {
    components = {"Transform"};
  }

  std::vector<InspectorFieldSchema> filtered;
  for (const auto& schema : schemas) {
    const std::string_view component(schema.component.data(), schema.component.size());
    if (std::find(components.begin(), components.end(), component) != components.end()) {
      filtered.push_back(schema);
    }
  }
  return filtered;
}

std::vector<UiPanelPropertyGroup> BuildDefaultPanelPropertyGroups() {
  const auto definitions = BuildDefaultPanelDefinitions();
  std::vector<UiPanelPropertyGroup> groups;
  groups.reserve(definitions.size());
  for (const auto& definition : definitions) {
    auto fields = BuildInspectorSchemasForPanel(definition.id);
    if (fields.empty()) {
      continue;
    }
    UiPanelPropertyGroup group;
    group.panel_id = definition.id;
    group.group_id = definition.property_group_id.empty() ? definition.id : definition.property_group_id;
    group.title = definition.title;
    group.description = definition.status_hint;
    group.fields = std::move(fields);
    groups.push_back(std::move(group));
  }
  return groups;
}

std::vector<ScriptLifecycleHookState> BuildDefaultScriptLifecycleHooks() {
  const std::vector<std::string_view> names = {
    "on_load", "on_spawn", "on_enable", "on_disable", "on_update",
    "on_fixed_update", "on_late_update", "on_collision", "on_trigger",
    "on_animation_event", "on_animation_loop", "on_keyframe_reached",
    "on_destroy", "on_unload"
  };
  std::vector<ScriptLifecycleHookState> hooks;
  hooks.reserve(names.size());
  for (const auto name : names) {
    ScriptLifecycleHookState hook;
    hook.hook_name = std::string(name);
    hooks.push_back(std::move(hook));
  }
  return hooks;
}

std::vector<SceneTreeRow> BuildSceneTreeRows(const std::vector<SceneTreeEntityModel>& entities,
                                             const SelectionState& selection,
                                             vkpt::core::StableId hovered_entity,
                                             std::size_t max_rows) {
  std::unordered_set<vkpt::core::StableId> known_ids;
  known_ids.reserve(entities.size());
  for (const auto& entity : entities) {
    if (entity.entity_id != 0) {
      known_ids.insert(entity.entity_id);
    }
  }

  std::unordered_map<vkpt::core::StableId, const SceneTreeEntityModel*> by_id;
  std::unordered_map<vkpt::core::StableId, std::vector<const SceneTreeEntityModel*>> children;
  by_id.reserve(entities.size());
  children.reserve(entities.size());
  std::vector<const SceneTreeEntityModel*> roots;
  roots.reserve(entities.size());
  for (const auto& entity : entities) {
    if (entity.entity_id == 0) {
      continue;
    }
    by_id[entity.entity_id] = &entity;
  }
  for (const auto& entity : entities) {
    if (entity.entity_id == 0) {
      continue;
    }
    if (entity.parent_id == 0 || !known_ids.contains(entity.parent_id) || entity.parent_id == entity.entity_id) {
      roots.push_back(&entity);
    } else {
      children[entity.parent_id].push_back(&entity);
    }
  }

  auto sort_rows = [](std::vector<const SceneTreeEntityModel*>& rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const SceneTreeEntityModel* lhs, const SceneTreeEntityModel* rhs) {
                       if (lhs->sibling_order != rhs->sibling_order) {
                         return lhs->sibling_order < rhs->sibling_order;
                       }
                       return lhs->entity_id < rhs->entity_id;
                     });
  };
  sort_rows(roots);
  for (auto& entry : children) {
    sort_rows(entry.second);
  }

  std::vector<SceneTreeRow> rows;
  rows.reserve(max_rows == 0 ? entities.size() : std::min(max_rows, entities.size()));
  const auto is_selected = [&](vkpt::core::StableId id) {
    return std::find(selection.selected_entity_ids.begin(), selection.selected_entity_ids.end(), id) !=
           selection.selected_entity_ids.end();
  };
  const auto is_hovered = [&](vkpt::core::StableId id) {
    if (hovered_entity != 0 && hovered_entity == id) {
      return true;
    }
    if (selection.hovered_entity == id) {
      return true;
    }
    return std::find(selection.hovered_entity_ids.begin(), selection.hovered_entity_ids.end(), id) !=
           selection.hovered_entity_ids.end();
  };

  std::unordered_set<vkpt::core::StableId> visiting;
  auto append_tree = [&](auto&& self, const SceneTreeEntityModel* entity, std::uint32_t depth) -> void {
    if (!entity || (max_rows != 0 && rows.size() >= max_rows)) {
      return;
    }
    const bool cycle = !visiting.insert(entity->entity_id).second;
    const auto childIt = children.find(entity->entity_id);
    const bool hasChildren = childIt != children.end() && !childIt->second.empty();
    SceneTreeRow row;
    row.entity_id = entity->entity_id;
    row.parent_id = entity->parent_id;
    row.depth = depth;
    row.sibling_order = entity->sibling_order;
    row.label = entity->name.empty()
        ? std::string("Entity ") + std::to_string(entity->entity_id)
        : entity->name;
    row.component_badges = entity->component_badges;
    if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "camera") !=
        entity->component_badges.end()) {
      row.icon = "camera";
    } else if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "light") !=
               entity->component_badges.end()) {
      row.icon = "light";
    } else if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "mesh") !=
               entity->component_badges.end()) {
      row.icon = "mesh";
    } else if (std::find(entity->component_badges.begin(), entity->component_badges.end(), "sdf") !=
               entity->component_badges.end()) {
      row.icon = "sdf";
    } else {
      row.icon = "entity";
    }
    row.expanded = entity->expanded;
    row.selected = is_selected(entity->entity_id);
    row.hovered = is_hovered(entity->entity_id);
    row.hidden = !entity->visible;
    row.locked = entity->locked;
    row.has_warning = cycle ||
                      (entity->parent_id != 0 && !known_ids.contains(entity->parent_id)) ||
                      entity->parent_id == entity->entity_id;
    row.has_children = hasChildren;
    rows.push_back(std::move(row));
    if (!cycle && entity->expanded && hasChildren) {
      for (const auto* child : childIt->second) {
        self(self, child, depth + 1u);
        if (max_rows != 0 && rows.size() >= max_rows) {
          break;
        }
      }
    }
    visiting.erase(entity->entity_id);
  };

  for (const auto* root : roots) {
    append_tree(append_tree, root, 0u);
    if (max_rows != 0 && rows.size() >= max_rows) {
      break;
    }
  }
  return rows;
}

std::vector<AssetPreviewCard> FilterAndSortAssetCards(const std::vector<AssetPreviewCard>& cards,
                                                      const AssetBrowserFilter& filter) {
  const std::string query = ToLower(filter.query);
  const std::string category = ToLower(filter.category);
  std::vector<AssetPreviewCard> out;
  for (const auto& card : cards) {
    if (!filter.show_missing && card.missing) {
      continue;
    }
    if (!filter.show_generated && ToLower(card.status) == "generated") {
      continue;
    }
    if (!category.empty() && ToLower(card.category) != category) {
      continue;
    }
    if (!query.empty()) {
      const std::string haystack = ToLower(card.display_name + " " + card.path + " " + card.asset_id);
      if (haystack.find(query) == std::string::npos) {
        continue;
      }
    }
    out.push_back(card);
  }

  const std::string sortKey = ToLower(filter.sort_key);
  std::sort(out.begin(), out.end(), [&](const AssetPreviewCard& a, const AssetPreviewCard& b) {
    std::string av;
    std::string bv;
    if (sortKey == "category") {
      av = a.category;
      bv = b.category;
    } else if (sortKey == "status") {
      av = a.status;
      bv = b.status;
    } else if (sortKey == "path") {
      av = a.path;
      bv = b.path;
    } else {
      av = a.display_name;
      bv = b.display_name;
    }
    if (filter.ascending) {
      return ToLower(av) < ToLower(bv);
    }
    return ToLower(av) > ToLower(bv);
  });
  return out;
}

StatusBarModel BuildStatusBarModel(const UiRuntimeState& runtime,
                                   const SelectionState& selection,
                                   const BenchmarkScoreModel* score) {
  StatusBarModel model;
  model.active_scene = runtime.active_scene;
  model.backend = runtime.active_renderer_backend;
  model.renderer_path = runtime.active_renderer_path.empty() ? runtime.selected_debug_view : runtime.active_renderer_path;
  model.spp = runtime.spp_accumulated;
  model.fps = runtime.fps;
  model.frame_ms = runtime.frame_ms;
  model.selected_entity_count = selection.selected_entity_ids.size();
  model.active_tool = ToString(runtime.active_viewport_tool);
  model.last_warning_or_error = runtime.last_warning_or_error.empty()
      ? runtime.status_message
      : runtime.last_warning_or_error;
  model.background_job_count = runtime.background_job_count;
  if (score != nullptr) {
    model.normalized_score = score->normalized_score;
  }
  return model;
}

}  // namespace vkpt::editor
