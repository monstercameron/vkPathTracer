#include "scene/Scene.h"
#include "scripting/ScriptRuntime.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "scripting runtime smoke failed: " << message << "\n";
    return false;
  }
  return true;
}

std::filesystem::path FindRepoFile(const std::filesystem::path& relative_path) {
  auto current = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const auto candidate = current / relative_path;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
    if (!current.has_parent_path() || current.parent_path() == current) {
      break;
    }
    current = current.parent_path();
  }
  return relative_path;
}

}  // namespace

int main() {
  vkpt::scene::SceneWorld world;

  const auto moving = world.create_entity("scripted_mover", 101);
  vkpt::scene::ScriptComponent mover_script;
  mover_script.script = "scripts/move.lua";
  mover_script.language = "lua";
  mover_script.entry = "default";
  mover_script.enabled = true;
  if (!world.set_component(moving, vkpt::scene::ComponentKind::Script, mover_script)) {
    std::cerr << "scripting runtime smoke failed: could not attach mover script\n";
    return 1;
  }

  const auto dormant = world.create_entity("dormant_script", 102);
  vkpt::scene::ScriptComponent dormant_script;
  dormant_script.script = "scripts/dormant.lua";
  dormant_script.enabled = false;
  if (!world.set_component(dormant, vkpt::scene::ComponentKind::Script, dormant_script)) {
    std::cerr << "scripting runtime smoke failed: could not attach dormant script\n";
    return 1;
  }

  auto runtime = vkpt::scripting::CreateScriptRuntime();
  const auto binding_summary = runtime->reload_bindings(world);
  if (!Check(binding_summary.binding_count == 2u, "expected two script bindings") ||
      !Check(binding_summary.runnable_count == 1u, "expected one runnable script") ||
      !Check(binding_summary.disabled_count == 1u, "expected one disabled script")) {
    return 1;
  }
#ifdef PT_ENABLE_LUA
  if (!Check(binding_summary.execution_available, "Lua runtime should execute when PT_ENABLE_LUA is enabled")) {
    return 1;
  }
#else
  if (!Check(!binding_summary.execution_available, "stub runtime should not execute Lua yet")) {
    return 1;
  }
#endif

  vkpt::scene::WorldCommandBuffer commands;
  vkpt::scripting::ScriptExecutionContext context;
  context.frame = 7;
  context.delta_seconds = 1.0 / 60.0;
  const auto dispatch_summary = runtime->dispatch_hook(
      world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context, commands);

  if (!Check(dispatch_summary.binding_count == 2u, "dispatch should see two bindings") ||
      !Check(dispatch_summary.runnable_count == 1u, "dispatch should see one runnable script") ||
      !Check(dispatch_summary.hook_call_count == 0u, "missing smoke script should not call hooks") ||
      !Check(dispatch_summary.command_count_after == 0u, "missing smoke script should not emit commands") ||
      !Check(!dispatch_summary.diagnostics.empty(), "dispatch should report the unavailable or missing smoke script")) {
    return 1;
  }
#ifdef PT_ENABLE_LUA
  if (!Check(dispatch_summary.skipped_count == 1u, "Lua dispatch should only skip the disabled script")) {
    return 1;
  }
#else
  if (!Check(dispatch_summary.skipped_count == 2u, "stub runtime should skip runnable and disabled scripts")) {
    return 1;
  }
#endif

  const auto demo_scene_path = FindRepoFile("assets/scenes/ecs_lifecycle_scripting_demo.json");
  if (!Check(std::filesystem::exists(demo_scene_path), "demo lifecycle scene should exist")) {
    return 1;
  }

  auto demo_document_result = vkpt::scene::SceneDocument::load_from_file(demo_scene_path.string());
  if (!Check(static_cast<bool>(demo_document_result), "demo lifecycle scene should load")) {
    return 1;
  }
  std::vector<std::string> scene_issues;
  if (!Check(demo_document_result.value().validate(&scene_issues), "demo lifecycle scene should validate")) {
    for (const auto& issue : scene_issues) {
      std::cerr << "  scene issue: " << issue << "\n";
    }
    return 1;
  }

  auto demo_world_result = demo_document_result.value().to_world();
  if (!Check(static_cast<bool>(demo_world_result), "demo lifecycle scene should convert to world")) {
    return 1;
  }

  auto demo_runtime = vkpt::scripting::CreateScriptRuntime();
  const auto demo_binding_summary = demo_runtime->reload_bindings(demo_world_result.value());
  if (!Check(demo_binding_summary.binding_count == 4u, "demo scene should expose four script bindings") ||
      !Check(demo_binding_summary.runnable_count == 3u, "demo scene should expose three runnable scripts") ||
      !Check(demo_binding_summary.disabled_count == 1u, "demo scene should expose one disabled script")) {
    return 1;
  }

  for (const auto& binding : demo_runtime->bindings()) {
    const auto source_path = FindRepoFile(binding.source);
    if (!Check(std::filesystem::exists(source_path), "demo script source should exist: " + binding.source)) {
      return 1;
    }
  }

  const std::array hooks = {
      vkpt::scripting::ScriptLifecycleHook::OnLoad,
      vkpt::scripting::ScriptLifecycleHook::OnSpawn,
      vkpt::scripting::ScriptLifecycleHook::OnEnable,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      vkpt::scripting::ScriptLifecycleHook::OnLateUpdate,
      vkpt::scripting::ScriptLifecycleHook::OnDisable,
      vkpt::scripting::ScriptLifecycleHook::OnDestroy,
      vkpt::scripting::ScriptLifecycleHook::OnUnload,
  };

  for (const auto hook : hooks) {
    vkpt::scene::WorldCommandBuffer lifecycle_commands;
    vkpt::scripting::ScriptExecutionContext lifecycle_context;
    lifecycle_context.frame = 17;
    lifecycle_context.elapsed_seconds = 0.25;
    lifecycle_context.delta_seconds = 1.0 / 60.0;

    const auto lifecycle_summary =
        demo_runtime->dispatch_hook(demo_world_result.value(), hook, lifecycle_context, lifecycle_commands);
    if (!Check(lifecycle_summary.binding_count == 4u, "lifecycle dispatch should see four bindings") ||
        !Check(lifecycle_summary.runnable_count == 3u, "lifecycle dispatch should see three runnable scripts")) {
      return 1;
    }
#ifdef PT_ENABLE_LUA
    if (!Check(lifecycle_summary.skipped_count == 1u, "Lua lifecycle dispatch should only skip disabled bindings")) {
      return 1;
    }
#else
    if (!Check(lifecycle_summary.skipped_count == 4u, "no-Lua lifecycle dispatch should skip all demo bindings") ||
        !Check(lifecycle_summary.hook_call_count == 0u, "no-Lua lifecycle dispatch should call no hooks") ||
        !Check(lifecycle_summary.command_count_after == 0u, "no-Lua lifecycle dispatch should emit no commands") ||
        !Check(lifecycle_summary.diagnostics.size() == 3u,
               "no-Lua lifecycle dispatch should diagnose the three runnable scripts")) {
      return 1;
    }
#endif
  }

  const auto third_person_scene_path = FindRepoFile("assets/scenes/third_person_action_demo.json");
  if (!Check(std::filesystem::exists(third_person_scene_path), "third-person scripted scene should exist")) {
    return 1;
  }
  auto third_person_document_result =
      vkpt::scene::SceneDocument::load_from_file(third_person_scene_path.string());
  if (!Check(static_cast<bool>(third_person_document_result), "third-person scripted scene should load")) {
    return 1;
  }
  std::vector<std::string> third_person_issues;
  if (!Check(third_person_document_result.value().validate(&third_person_issues),
             "third-person scripted scene should validate")) {
    for (const auto& issue : third_person_issues) {
      std::cerr << "  scene issue: " << issue << "\n";
    }
    return 1;
  }
  auto third_person_world_result = third_person_document_result.value().to_world();
  if (!Check(static_cast<bool>(third_person_world_result), "third-person scripted scene should convert to world")) {
    return 1;
  }
  auto third_person_runtime = vkpt::scripting::CreateScriptRuntime();
  const auto third_person_summary =
      third_person_runtime->reload_bindings(third_person_world_result.value());
  if (!Check(third_person_summary.binding_count == 2u, "third-person scene should expose two script bindings") ||
      !Check(third_person_summary.runnable_count == 2u, "third-person scene should expose two runnable scripts")) {
    return 1;
  }
  for (const auto& binding : third_person_runtime->bindings()) {
    const auto source_path = FindRepoFile(binding.source);
    if (!Check(std::filesystem::exists(source_path), "third-person script source should exist: " + binding.source)) {
      return 1;
    }
  }

  vkpt::scene::WorldCommandBuffer third_person_commands;
  vkpt::scripting::ScriptExecutionContext third_person_context;
  third_person_context.frame = 3;
  third_person_context.delta_seconds = 1.0 / 60.0;
  third_person_context.input.active_keys = {'W'};
  const auto third_person_dispatch = third_person_runtime->dispatch_hook(
      third_person_world_result.value(),
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      third_person_context,
      third_person_commands);
  if (!Check(third_person_dispatch.runnable_count == 2u, "third-person dispatch should see two runnable scripts")) {
    return 1;
  }
#ifdef PT_ENABLE_LUA
  bool moved_hero_root = false;
  bool moved_camera = false;
  bool assigned_camera = false;
  bool assigned_light = false;
  for (const auto& command : third_person_commands.commands()) {
    if (const auto* set_transform =
            std::get_if<vkpt::scene::WorldCommandBuffer::SetTransformCommand>(&command.payload)) {
      if (set_transform->id == 9110u && set_transform->transform.translation.z < -0.0001f) {
        moved_hero_root = true;
      }
      if (set_transform->id == 9101u) {
        moved_camera = true;
      }
    } else if (const auto* assign_camera =
                   std::get_if<vkpt::scene::WorldCommandBuffer::AssignCameraCommand>(&command.payload)) {
      if (assign_camera->id == 9101u && assign_camera->camera.fov > 0.0f) {
        assigned_camera = true;
      }
    } else if (const auto* assign_light =
                   std::get_if<vkpt::scene::WorldCommandBuffer::AssignLightCommand>(&command.payload)) {
      if (assign_light->id == 9103u && assign_light->light.intensity > 0.0f) {
        assigned_light = true;
      }
    }
  }
  if (!Check(third_person_dispatch.hook_call_count == 2u, "Lua third-person dispatch should call both update hooks") ||
      !Check(third_person_dispatch.command_count_after >= 8u,
             "Lua third-person dispatch should emit gameplay transform/camera/light commands") ||
      !Check(third_person_dispatch.diagnostics.empty(), "Lua third-person dispatch should not report diagnostics") ||
      !Check(moved_hero_root, "W input should move the hero root forward") ||
      !Check(moved_camera, "third-person script should move the action camera") ||
      !Check(assigned_camera, "third-person script should update the camera component") ||
      !Check(assigned_light, "third-person light script should update the key light")) {
    return 1;
  }
#else
  if (!Check(third_person_dispatch.command_count_after == 0u, "no-Lua third-person dispatch should emit no commands") ||
      !Check(third_person_dispatch.diagnostics.size() == 2u,
             "no-Lua third-person dispatch should diagnose both runnable scripts")) {
    return 1;
  }
#endif

  std::cout << "scripting runtime smoke: ok\n";
  return 0;
}
