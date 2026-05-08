#include "audio/AudioSystem.h"
#include "scene/Scene.h"
#include "scripting/ScriptRuntime.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace vkpt::audio {

IAudioSystem* GlobalAudioSystem() {
  return nullptr;
}

void SetGlobalAudioSystem(IAudioSystem*) {}

}  // namespace vkpt::audio

namespace vkpt::scene {

std::string_view to_string(ComponentKind kind) {
  switch (kind) {
    case ComponentKind::Identity:
      return "Identity";
    case ComponentKind::Transform:
      return "Transform";
    case ComponentKind::Hierarchy:
      return "Hierarchy";
    case ComponentKind::Camera:
      return "Camera";
    case ComponentKind::Light:
      return "Light";
    case ComponentKind::MeshRenderer:
      return "MeshRenderer";
    case ComponentKind::SdfPrimitive:
      return "SDFPrimitive";
    case ComponentKind::MaterialOverride:
      return "MaterialOverride";
    case ComponentKind::PhysicsBody:
      return "PhysicsBody";
    case ComponentKind::Script:
      return "Script";
    case ComponentKind::AudioListener:
      return "AudioListener";
    case ComponentKind::AudioEmitter:
      return "AudioEmitter";
    case ComponentKind::UiPanel:
      return "UiPanel";
    case ComponentKind::BenchmarkTag:
      return "BenchmarkTag";
    default:
      return "Unknown";
  }
}

std::string_view to_string(TransformAuthority authority) {
  switch (authority) {
    case TransformAuthority::BenchmarkFrozen:
      return "BenchmarkFrozen";
    case TransformAuthority::PhysicsControlled:
      return "PhysicsControlled";
    case TransformAuthority::ScriptControlled:
      return "ScriptControlled";
    case TransformAuthority::EditorControlled:
      return "EditorControlled";
    case TransformAuthority::Authored:
      return "Authored";
    default:
      return "Authored";
  }
}

}  // namespace vkpt::scene

namespace {

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "script dispatch contract smoke failed: " << message << "\n";
    return false;
  }
  return true;
}

void AttachScript(vkpt::scene::SceneWorld& world,
                  vkpt::core::StableEntityId entity,
                  std::string script_path) {
  vkpt::scene::ScriptComponent script;
  script.script = std::move(script_path);
  world.set_component(entity, vkpt::scene::ComponentKind::Script, script);
}

bool CheckScriptingNamingContract() {
  static_assert(std::is_same_v<vkpt::scripting::ScriptRuntimeStatus,
                               vkpt::scripting::ScriptingStatus>);

  vkpt::scripting::ScriptRuntimeStatus status;
  return Check(vkpt::scripting::ScriptingSubsystemName() == "scripting",
               "scripting subsystem should expose a stable canonical name") &&
         Check(std::string_view(status.name) ==
                   vkpt::scripting::kScriptingSubsystemName,
               "scripting status should carry the canonical subsystem name") &&
         Check(vkpt::scripting::kScriptingNamingContract.status_type_name ==
                   "ScriptingStatus",
               "scripting naming contract should expose the status type name") &&
         Check(vkpt::scripting::kScriptingNamingContract.health_probe_name ==
                   vkpt::scripting::kScriptingSubsystemName,
               "scripting health probe name should match the subsystem name") &&
         Check(vkpt::scripting::kScriptingNamingContract.flow_field_name ==
                   "current_flow_id",
               "scripting status should keep current_flow_id as the canonical flow field") &&
         Check(vkpt::scripting::kScriptingNamingContract.command_snapshot_contract ==
                   vkpt::scripting::kScriptingCommandSnapshotContractName,
               "scripting command snapshot contract should be source-proofable");
}

}  // namespace

int main() {
  if (!CheckScriptingNamingContract()) {
    return 1;
  }

#ifndef PT_ENABLE_LUA
  vkpt::scripting::ScriptExecutionContext context;
  context.game_mode = true;
  context.runtime.mode = "play";
  context.runtime.scripts_running = true;
  context.frame = 7u;

  vkpt::scene::SceneWorld world;
  const auto entity = world.create_entity("dispatch_no_lua", 50000u);
  AttachScript(world, entity, "assets/scripts/script_param_probe.lua");

  auto runtime = vkpt::scripting::CreateScriptRuntime();
  runtime->reload_bindings(world);
  vkpt::scene::WorldCommandBuffer commands;
  const auto dispatch = runtime->dispatch_hook(
      world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context, commands);
  const auto status = runtime->status();
  const auto probe = runtime->create_health_probe();
  const auto snapshot = runtime->dispatch_hook_snapshot(
      world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context);

  if (!Check(dispatch.execution_available == false,
             "no-Lua dispatch should report execution unavailable") ||
      !Check(dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Skipped,
             "no-Lua dispatch should skip runnable bindings") ||
      !Check(commands.flow_id() == context.frame &&
                 snapshot.commands.flow_id() == context.frame,
             "no-Lua dispatch should still propagate command flow ids") ||
      !Check(status.lifecycle == vkpt::core::contracts::ComponentLifecycle::Ready &&
                 status.current_flow_id == context.frame,
             "no-Lua scripting status should expose lifecycle and flow") ||
      !Check(probe->name() == "scripting" &&
                 probe->check().status == vkpt::core::health::Status::Ok,
             "no-Lua scripting health probe should report ok")) {
    return 1;
  }

  std::cout << "script dispatch contract smoke: ok (lua disabled)\n";
  return 0;
#else
  vkpt::scripting::ScriptExecutionContext context;
  context.game_mode = true;
  context.runtime.mode = "play";
  context.runtime.scripts_running = true;
  context.frame = 1u;

  vkpt::scene::SceneWorld ok_world;
  const auto ok_entity = ok_world.create_entity("dispatch_ok", 50001u);
  AttachScript(ok_world, ok_entity, "assets/scripts/script_param_probe.lua");
  auto ok_runtime = vkpt::scripting::CreateScriptRuntime();
  ok_runtime->reload_bindings(ok_world);
  vkpt::scene::WorldCommandBuffer ok_commands;
  const auto ok_dispatch = ok_runtime->dispatch_hook(
      ok_world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context, ok_commands);
  const auto ok_status = ok_runtime->status();
  const auto ok_probe = ok_runtime->create_health_probe();
  const auto ok_probe_report = ok_probe->check();
  if (!Check(ok_dispatch.hook_call_count == 1u, "expected successful hook call") ||
      !Check(ok_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Ok,
             "expected successful dispatch status") ||
      !Check(ok_commands.flow_id() == context.frame &&
                 ok_status.lifecycle == vkpt::core::contracts::ComponentLifecycle::Ready &&
                 ok_status.health == vkpt::core::contracts::SubsystemHealth::Ok &&
                 ok_status.current_flow_id == context.frame,
             "expected scripting status and command buffer to expose dispatch flow") ||
      !Check(ok_probe->name() == "scripting" &&
                 ok_probe_report.status == vkpt::core::health::Status::Ok,
             "expected scripting health probe to report ok after successful dispatch")) {
    return 1;
  }
  const auto ok_snapshot = ok_runtime->dispatch_hook_snapshot(
      ok_world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context);
  if (!Check(ok_snapshot.generation == context.frame,
             "expected command snapshot generation to match dispatch frame") ||
      !Check(ok_snapshot.hook == vkpt::scripting::ScriptLifecycleHook::OnUpdate,
             "expected command snapshot to retain hook identity") ||
      !Check(ok_snapshot.commands.flow_id() == context.frame,
             "expected command snapshot commands to retain dispatch flow id") ||
      !Check(ok_snapshot.diagnostics.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Ok,
             "expected command snapshot diagnostics to expose dispatch result")) {
    return 1;
  }

  vkpt::scene::SceneWorld partial_world;
  const auto partial_ok_entity = partial_world.create_entity("dispatch_partial_ok", 50002u);
  AttachScript(partial_world, partial_ok_entity, "assets/scripts/script_param_probe.lua");
  const auto partial_fail_entity = partial_world.create_entity("dispatch_partial_fail", 50003u);
  AttachScript(partial_world, partial_fail_entity, "assets/scripts/script_sandbox_probe.lua");
  auto partial_runtime = vkpt::scripting::CreateScriptRuntime();
  partial_runtime->reload_bindings(partial_world);
  vkpt::scene::WorldCommandBuffer partial_commands;
  const auto partial_dispatch = partial_runtime->dispatch_hook(
      partial_world, vkpt::scripting::ScriptLifecycleHook::OnUpdate, context, partial_commands);
  if (!Check(partial_dispatch.hook_call_count == 1u,
             "expected one healthy hook in partial dispatch") ||
      !Check(partial_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::PartialFailure,
             "expected partial failure status") ||
      !Check(partial_dispatch.result.errors.size() == 1u &&
                 partial_dispatch.result.errors.front().entity == partial_fail_entity,
             "expected partial failure error identity")) {
    return 1;
  }

  vkpt::scene::SceneWorld instruction_world;
  const auto instruction_entity = instruction_world.create_entity("dispatch_instruction_budget", 50004u);
  AttachScript(instruction_world, instruction_entity, "assets/scripts/script_budget_probe.lua");
  auto instruction_runtime = vkpt::scripting::CreateScriptRuntime();
  instruction_runtime->reload_bindings(instruction_world);
  vkpt::scripting::ScriptExecutionContext instruction_context = context;
  instruction_context.instruction_budget = 1000u;
  vkpt::scene::WorldCommandBuffer instruction_commands;
  const auto instruction_dispatch = instruction_runtime->dispatch_hook(
      instruction_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      instruction_context,
      instruction_commands);
  const auto instruction_status = instruction_runtime->status();
  const auto instruction_probe = instruction_runtime->create_health_probe();
  if (!Check(instruction_context.budget_policy ==
                 vkpt::scripting::BudgetPolicy::KillAndDisableUntilReload,
             "expected fail-closed default budget policy") ||
      !Check(instruction_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Failure,
             "expected instruction budget failure status") ||
      !Check(instruction_dispatch.result.budget_exceeded_count == 1u &&
                 instruction_dispatch.result.script_killed_count == 1u,
             "expected instruction budget kill counts") ||
      !Check(instruction_dispatch.result.errors.size() == 1u &&
                 instruction_dispatch.result.errors.front().entity == instruction_entity &&
                 instruction_dispatch.result.errors.front().budget_exceeded &&
                 instruction_dispatch.result.errors.front().budget == "instructions" &&
                 instruction_dispatch.result.errors.front().killed,
             "expected instruction budget error details") ||
      !Check(instruction_status.lifecycle ==
                     vkpt::core::contracts::ComponentLifecycle::Degraded &&
                 instruction_status.health ==
                     vkpt::core::contracts::SubsystemHealth::Degraded &&
                 instruction_status.last_error_script_id == instruction_entity,
             "expected scripting status to degrade after budget kill") ||
      !Check(instruction_probe->check().status ==
                 vkpt::core::health::Status::Degraded,
             "expected scripting health probe to degrade after budget kill")) {
    return 1;
  }

  const auto skipped_dispatch = instruction_runtime->dispatch_hook(
      instruction_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      instruction_context,
      instruction_commands);
  if (!Check(skipped_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Skipped,
             "expected killed binding to skip until reload") ||
      !Check(skipped_dispatch.result.script_killed_count == 0u,
             "expected skip result not to count a new kill")) {
    return 1;
  }
  instruction_runtime->reload_bindings(instruction_world);
  vkpt::scene::WorldCommandBuffer reloaded_instruction_commands;
  const auto reloaded_instruction_dispatch = instruction_runtime->dispatch_hook(
      instruction_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      instruction_context,
      reloaded_instruction_commands);
  if (!Check(reloaded_instruction_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Failure,
             "expected reload to clear disabled-until-reload skip state") ||
      !Check(reloaded_instruction_dispatch.result.script_killed_count == 1u,
             "expected reloaded script to execute and be killed again")) {
    return 1;
  }

  const auto memory_budget_path =
      std::filesystem::temp_directory_path() / "vkpt_script_dispatch_memory_budget.lua";
  {
    std::ofstream memory_probe(memory_budget_path, std::ios::trunc);
    memory_probe << "local script = {}\n"
                    "function script.on_update()\n"
                    "  local t = {}\n"
                    "  while true do\n"
                    "    t[#t + 1] = string.rep('x', 1024)\n"
                    "  end\n"
                    "end\n"
                    "return script\n";
  }

  vkpt::scene::SceneWorld memory_world;
  const auto memory_entity = memory_world.create_entity("dispatch_memory_budget", 50005u);
  AttachScript(memory_world, memory_entity, memory_budget_path.generic_string());
  auto memory_runtime = vkpt::scripting::CreateScriptRuntime();
  memory_runtime->reload_bindings(memory_world);
  vkpt::scripting::ScriptExecutionContext memory_context = context;
  memory_context.instruction_budget = 500000u;
  memory_context.memory_budget_bytes = 64u * 1024u;
  vkpt::scene::WorldCommandBuffer memory_commands;
  const auto memory_dispatch = memory_runtime->dispatch_hook(
      memory_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      memory_context,
      memory_commands);
  const bool memory_ok =
      Check(memory_dispatch.result.overall_status ==
                vkpt::scripting::ScriptDispatchResult::Status::Failure,
            "expected memory budget failure status") &&
      Check(memory_dispatch.result.budget_exceeded_count == 1u &&
                memory_dispatch.result.script_killed_count == 1u,
            "expected memory budget kill counts") &&
      Check(memory_dispatch.result.errors.size() == 1u &&
                memory_dispatch.result.errors.front().entity == memory_entity &&
                memory_dispatch.result.errors.front().budget_exceeded &&
                memory_dispatch.result.errors.front().budget == "memory" &&
                memory_dispatch.result.errors.front().killed,
            "expected memory budget error details");
  std::error_code remove_error;
  std::filesystem::remove(memory_budget_path, remove_error);
  if (!memory_ok) {
    return 1;
  }

  std::cout << "script dispatch contract smoke: ok\n";
  return 0;
#endif
}
