#include "audio/AudioSystem.h"
#include "scene/Scene.h"
#include "scripting/ScriptRuntime.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
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
  AttachScript(world, entity, "assets/scripts/test/script_param_probe.lua");

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
  AttachScript(ok_world, ok_entity, "assets/scripts/test/script_param_probe.lua");
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
  AttachScript(partial_world, partial_ok_entity, "assets/scripts/test/script_param_probe.lua");
  const auto partial_fail_entity = partial_world.create_entity("dispatch_partial_fail", 50003u);
  AttachScript(partial_world, partial_fail_entity, "assets/scripts/test/script_sandbox_probe.lua");
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
  AttachScript(instruction_world, instruction_entity, "assets/scripts/test/script_budget_probe.lua");
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
             "expected reloaded script to execute and be killed again") ||
      !Check(reloaded_instruction_dispatch.skipped_count == 0u,
             "expected reloaded script to actually be dispatched (not skipped)")) {
    return 1;
  }
  // E. disabled-until-reload regression: after reload the binding ran (then
  //    got killed again), so runtime_state.hook_fired must be true and the
  //    disabled flag must be set anew rather than carried over from before.
  {
    const auto& reloaded_states = instruction_runtime->runtime_states();
    if (!Check(!reloaded_states.empty() &&
                   reloaded_states.front().hook_fired,
               "reload should let the previously disabled script run again") ||
        !Check(!reloaded_states.empty() &&
                   reloaded_states.front().disabled_until_reload,
               "reloaded run should set disabled_until_reload from a fresh kill")) {
      return 1;
    }
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

  // A. Malformed-syntax probe: an inline file with a deliberate parse error must
  //    load without crashing the host, end up disabled / not invoked, and never
  //    interfere with a co-resident well-formed script in the same dispatch.
  const auto syntax_error_path =
      std::filesystem::temp_directory_path() / "vkpt_script_dispatch_syntax_error.lua";
  {
    std::ofstream bad_probe(syntax_error_path, std::ios::trunc);
    bad_probe << "function on_update(self) syntax error here\n";
  }
  vkpt::scene::SceneWorld syntax_world;
  const auto syntax_bad_entity = syntax_world.create_entity("dispatch_syntax_bad", 50006u);
  AttachScript(syntax_world, syntax_bad_entity, syntax_error_path.generic_string());
  const auto syntax_good_entity = syntax_world.create_entity("dispatch_syntax_good", 50007u);
  AttachScript(syntax_world, syntax_good_entity,
               "assets/scripts/test/script_param_probe.lua");
  auto syntax_runtime = vkpt::scripting::CreateScriptRuntime();
  syntax_runtime->reload_bindings(syntax_world);
  vkpt::scene::WorldCommandBuffer syntax_commands;
  const auto syntax_dispatch = syntax_runtime->dispatch_hook(
      syntax_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      context,
      syntax_commands);
  bool syntax_bad_disabled = false;
  bool syntax_good_fired = false;
  for (const auto& rs : syntax_runtime->runtime_states()) {
    if (rs.entity == syntax_bad_entity) {
      // Either disabled-until-reload, or simply never fired with last_error set.
      syntax_bad_disabled = rs.disabled_until_reload || !rs.last_error.empty() || !rs.hook_fired;
    }
    if (rs.entity == syntax_good_entity) {
      syntax_good_fired = rs.hook_fired;
    }
  }
  const bool syntax_ok =
      Check(syntax_dispatch.hook_call_count >= 1u,
            "malformed-syntax dispatch should still run the well-formed sibling") &&
      Check(syntax_good_fired,
            "well-formed sibling should fire on_update alongside malformed peer") &&
      Check(syntax_bad_disabled,
            "malformed-syntax script should not have a successful hook fire") &&
      Check(syntax_dispatch.result.overall_status ==
                vkpt::scripting::ScriptDispatchResult::Status::PartialFailure ||
                syntax_dispatch.result.overall_status ==
                    vkpt::scripting::ScriptDispatchResult::Status::Ok,
            "malformed-syntax dispatch should not crash; status is partial-failure or ok-with-error");
  std::filesystem::remove(syntax_error_path, remove_error);
  if (!syntax_ok) {
    return 1;
  }

  // B. Stack-overflow probe: unbounded recursion must come back through
  //    lua_resume cleanly and disable the binding until reload.
  vkpt::scene::SceneWorld stack_world;
  const auto stack_entity = stack_world.create_entity("dispatch_stack_overflow", 50008u);
  AttachScript(stack_world, stack_entity,
               "assets/scripts/test/script_stack_overflow_probe.lua");
  auto stack_runtime = vkpt::scripting::CreateScriptRuntime();
  stack_runtime->reload_bindings(stack_world);
  vkpt::scripting::ScriptExecutionContext stack_context = context;
  // Generous instruction budget so the recursion truly trips Lua's call-stack
  // limit rather than the per-script instruction cap.
  stack_context.instruction_budget = 5'000'000u;
  vkpt::scene::WorldCommandBuffer stack_commands;
  const auto stack_dispatch = stack_runtime->dispatch_hook(
      stack_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      stack_context,
      stack_commands);
  bool stack_disabled = false;
  for (const auto& rs : stack_runtime->runtime_states()) {
    if (rs.entity == stack_entity) {
      stack_disabled = rs.disabled_until_reload || !rs.last_error.empty();
    }
  }
  if (!Check(stack_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Failure,
             "stack overflow should yield a Failure dispatch status") ||
      !Check(stack_dispatch.result.errors.size() == 1u &&
                 stack_dispatch.result.errors.front().entity == stack_entity,
             "stack overflow should report the offending entity in errors") ||
      !Check(stack_disabled,
             "stack overflow should disable the binding (until reload or via last_error)")) {
    return 1;
  }

  // C. Wall-clock budget kill: drive instruction_budget high enough that the
  //    50 ms wall-clock deadline trips before the per-script instruction cap.
  vkpt::scene::SceneWorld wall_world;
  const auto wall_entity = wall_world.create_entity("dispatch_wall_clock", 50009u);
  AttachScript(wall_world, wall_entity,
               "assets/scripts/test/script_wall_clock_probe.lua");
  auto wall_runtime = vkpt::scripting::CreateScriptRuntime();
  wall_runtime->reload_bindings(wall_world);
  const auto wall_clock_kills_before = wall_runtime->status().wall_clock_kills_total;
  vkpt::scripting::ScriptExecutionContext wall_context = context;
  // 1 billion instructions so a 50 ms tight loop can never exhaust the
  // per-script instruction cap before the wall-clock deadline trips.
  wall_context.instruction_budget = 1'000'000'000u;
  vkpt::scene::WorldCommandBuffer wall_commands;
  const auto wall_dispatch = wall_runtime->dispatch_hook(
      wall_world,
      vkpt::scripting::ScriptLifecycleHook::OnUpdate,
      wall_context,
      wall_commands);
  const auto wall_status = wall_runtime->status();
  bool wall_disabled = false;
  std::string wall_kill_kind;
  for (const auto& rs : wall_runtime->runtime_states()) {
    if (rs.entity == wall_entity) {
      wall_disabled = rs.disabled_until_reload;
      wall_kill_kind = rs.budget_exceeded_type;
    }
  }
  if (!Check(wall_dispatch.result.overall_status ==
                 vkpt::scripting::ScriptDispatchResult::Status::Failure,
             "wall-clock probe should fail dispatch") ||
      !Check(wall_disabled,
             "wall-clock probe should disable the binding until reload") ||
      !Check(wall_kill_kind == "wall_clock",
             "wall-clock probe should record budget_exceeded_type=wall_clock") ||
      !Check(wall_status.wall_clock_kills_total == wall_clock_kills_before + 1u,
             "wall-clock probe should increment ScriptingStatus.wall_clock_kills_total") ||
      !Check(wall_dispatch.result.errors.size() == 1u &&
                 wall_dispatch.result.errors.front().budget == "wall_clock" &&
                 wall_dispatch.result.errors.front().killed,
             "wall-clock probe error should record budget=wall_clock and killed=true")) {
    return 1;
  }

  // D. Wrong-type binding-call survives: deliberately call ~6 representative
  //    bindings with the wrong Lua type. luaL_argerror must fire, lua_resume
  //    must catch it, the host stays alive, the offending script is
  //    disabled, and any well-formed sibling continues to dispatch.
  struct ArgErrorCase {
    const char* label;
    vkpt::core::StableEntityId entity;
    const char* body;
  };
  const std::array<ArgErrorCase, 6> argerror_cases{{
      {"entity_set_transform_string",
       50010u,
       "self:set_transform('not-a-table')"},                          // entity binding
      {"world_find_entity_table",
       50011u,
       "ctx.world:find_entity({})"},                                  // world binding
      {"scene_entities_with_component_table",
       50012u,
       "ctx.scene:entities_with_component({})"},                      // scene binding
      {"input_key_down_table",
       50013u,
       "ctx.input:key_down({})"},                                     // input binding
      {"audio_post_event_table",
       50014u,
       "ctx.audio:post_event({})"},                                   // audio binding
      {"context_diagnostic_table_message",
       50015u,
       "ctx:diagnostic('info', {})"},                                 // context binding
  }};

  for (const auto& test_case : argerror_cases) {
    const auto bad_path =
        std::filesystem::temp_directory_path() /
        (std::string("vkpt_script_dispatch_argerror_") + test_case.label + ".lua");
    {
      std::ofstream bad_script(bad_path, std::ios::trunc);
      bad_script << "local script = {}\n"
                    "function script.on_update(self, ctx)\n  "
                 << test_case.body
                 << "\nend\n"
                    "return script\n";
    }

    vkpt::scene::SceneWorld arg_world;
    const auto bad_entity =
        arg_world.create_entity(std::string("argerror_bad_") + test_case.label,
                                test_case.entity);
    AttachScript(arg_world, bad_entity, bad_path.generic_string());
    const auto good_entity =
        arg_world.create_entity(std::string("argerror_good_") + test_case.label,
                                test_case.entity + 100000u);
    AttachScript(arg_world, good_entity,
                 "assets/scripts/test/script_param_probe.lua");
    auto arg_runtime = vkpt::scripting::CreateScriptRuntime();
    arg_runtime->reload_bindings(arg_world);
    vkpt::scene::WorldCommandBuffer arg_commands;
    const auto arg_dispatch = arg_runtime->dispatch_hook(
        arg_world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        context,
        arg_commands);
    bool bad_failed = false;
    bool good_fired = false;
    for (const auto& rs : arg_runtime->runtime_states()) {
      if (rs.entity == bad_entity) {
        bad_failed = !rs.last_error.empty() || rs.disabled_until_reload;
      }
      if (rs.entity == good_entity) {
        good_fired = rs.hook_fired;
      }
    }
    const auto status_msg =
        std::string("argerror case should partial-fail: ") + test_case.label;
    const auto bad_msg =
        std::string("argerror case should leave bad script disabled/errored: ") +
        test_case.label;
    const auto good_msg =
        std::string("argerror case should still dispatch healthy sibling: ") +
        test_case.label;
    const auto count_msg =
        std::string("argerror case should fire exactly the healthy hook: ") +
        test_case.label;
    const bool case_ok =
        Check(arg_dispatch.result.overall_status ==
                  vkpt::scripting::ScriptDispatchResult::Status::PartialFailure,
              status_msg.c_str()) &&
        Check(bad_failed, bad_msg.c_str()) &&
        Check(good_fired, good_msg.c_str()) &&
        Check(arg_dispatch.hook_call_count == 1u, count_msg.c_str());
    std::filesystem::remove(bad_path, remove_error);
    if (!case_ok) {
      return 1;
    }
  }

  // F. Hot-reload state continuity: BindingIdentity-equal reloads must reuse
  //    the LuaStatePool entry (script-local counter persists), and any
  //    binding-set change must drop the pool (counter resets to 0).
  vkpt::scene::SceneWorld reload_world;
  const auto reload_entity = reload_world.create_entity("dispatch_state_continuity", 50020u);
  AttachScript(reload_world, reload_entity, "assets/scripts/test/script_counter_probe.lua");
  auto reload_runtime = vkpt::scripting::CreateScriptRuntime();
  reload_runtime->reload_bindings(reload_world);

  auto last_logged_tick = [](const vkpt::scripting::ScriptDispatchSummary& summary) -> int {
    int latest = -1;
    for (const auto& diag : summary.diagnostics) {
      const auto pos = diag.message.find("tick=");
      if (pos == std::string::npos) {
        continue;
      }
      try {
        latest = std::stoi(diag.message.substr(pos + 5));
      } catch (...) {
        // ignore
      }
    }
    return latest;
  };

  int observed_tick = 0;
  for (int i = 0; i < 5; ++i) {
    vkpt::scripting::ScriptExecutionContext tick_ctx = context;
    tick_ctx.frame = static_cast<vkpt::core::FrameIndex>(100u + i);
    vkpt::scene::WorldCommandBuffer tick_commands;
    const auto tick_dispatch = reload_runtime->dispatch_hook(
        reload_world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        tick_ctx,
        tick_commands);
    const auto seen = last_logged_tick(tick_dispatch);
    if (seen >= 0) {
      observed_tick = seen;
    }
  }
  if (!Check(observed_tick == 5,
             "counter probe should report tick=5 after 5 dispatches")) {
    return 1;
  }
  const auto state_count_before = reload_runtime->lua_state_count();
  const auto created_total_before = reload_runtime->lua_states_created_total();

  // Identical reload — BindingIdentity unchanged, state pool must be reused.
  reload_runtime->reload_bindings(reload_world);
  if (!Check(reload_runtime->lua_state_count() == state_count_before,
             "identical reload should keep LuaStatePool entries") ||
      !Check(reload_runtime->lua_states_created_total() == created_total_before,
             "identical reload should not create a new Lua state")) {
    return 1;
  }
  {
    vkpt::scripting::ScriptExecutionContext after_ctx = context;
    after_ctx.frame = 200u;
    vkpt::scene::WorldCommandBuffer after_commands;
    const auto after_dispatch = reload_runtime->dispatch_hook(
        reload_world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        after_ctx,
        after_commands);
    const auto seen = last_logged_tick(after_dispatch);
    if (!Check(seen == 6,
               "identical reload should preserve script-local counter (expected 6)")) {
      return 1;
    }
  }

  // Now mutate the binding set (add a second script). BindingIdentity differs,
  // state pool must be cleared, counter must reset to 0 (then increment to 1).
  const auto reload_extra_entity =
      reload_world.create_entity("dispatch_state_continuity_extra", 50021u);
  AttachScript(reload_world, reload_extra_entity,
               "assets/scripts/test/script_param_probe.lua");
  reload_runtime->reload_bindings(reload_world);
  if (!Check(reload_runtime->lua_state_count() == 0u,
             "binding-set change should clear LuaStatePool entries")) {
    return 1;
  }
  {
    vkpt::scripting::ScriptExecutionContext changed_ctx = context;
    changed_ctx.frame = 300u;
    vkpt::scene::WorldCommandBuffer changed_commands;
    const auto changed_dispatch = reload_runtime->dispatch_hook(
        reload_world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        changed_ctx,
        changed_commands);
    const auto seen = last_logged_tick(changed_dispatch);
    if (!Check(reload_runtime->lua_states_created_total() > created_total_before,
               "binding-set change should rebuild fresh LuaStatePool entries on next dispatch") ||
        !Check(seen == 1,
               "binding-set change should reset script-local counter to 1 on next tick")) {
      return 1;
    }
  }

  // G. Play->edit->play with reused lua_State must not leave a dangling host
  //    pointer in the registry. Regression for the crash that hit on a hot
  //    OnDisable -> reload (BindingIdentity match) -> OnLoad sequence: the
  //    old code stored a stack-allocated LuaHostContext pointer in the Lua
  //    registry, so the reused state's bindings would dereference dead stack
  //    memory on the next dispatch. The fix moves the host to a heap-owned
  //    member of LuaStatePool::Impl::State; this case verifies the sequence
  //    runs clean and that script-local state survives unchanged.
  vkpt::scene::SceneWorld toggle_world;
  const auto toggle_entity = toggle_world.create_entity("dispatch_play_toggle", 50030u);
  AttachScript(toggle_world, toggle_entity, "assets/scripts/test/script_counter_probe.lua");
  auto toggle_runtime = vkpt::scripting::CreateScriptRuntime();
  toggle_runtime->reload_bindings(toggle_world);

  // First "Play" session: a couple of OnUpdate ticks.
  for (int i = 0; i < 2; ++i) {
    vkpt::scripting::ScriptExecutionContext play_ctx = context;
    play_ctx.frame = static_cast<vkpt::core::FrameIndex>(400u + i);
    vkpt::scene::WorldCommandBuffer play_commands;
    toggle_runtime->dispatch_hook(
        toggle_world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        play_ctx,
        play_commands);
  }

  // Exit Play: OnDisable. The old bug stamped a stack pointer into the Lua
  // registry here that would go stale before the next dispatch.
  {
    vkpt::scripting::ScriptExecutionContext disable_ctx = context;
    disable_ctx.frame = 500u;
    vkpt::scene::WorldCommandBuffer disable_commands;
    toggle_runtime->dispatch_hook(
        toggle_world,
        vkpt::scripting::ScriptLifecycleHook::OnDisable,
        disable_ctx,
        disable_commands);
  }

  // Identity-unchanged reload (the toggle path): LuaStatePool must be reused.
  const auto toggle_state_count_before = toggle_runtime->lua_state_count();
  const auto toggle_created_before = toggle_runtime->lua_states_created_total();
  toggle_runtime->reload_bindings(toggle_world);
  if (!Check(toggle_runtime->lua_state_count() == toggle_state_count_before,
             "play->edit->play reload should keep LuaStatePool entries") ||
      !Check(toggle_runtime->lua_states_created_total() == toggle_created_before,
             "play->edit->play reload should not create a new Lua state")) {
    return 1;
  }

  // Re-enter Play: OnLoad. With the bug, this is where the crash hit because
  // the registry still pointed at the stack frame that owned the OnDisable
  // dispatch's host. With the fix, the registry points at a heap host owned
  // by the State, so this dispatches cleanly.
  {
    vkpt::scripting::ScriptExecutionContext load_ctx = context;
    load_ctx.frame = 600u;
    vkpt::scene::WorldCommandBuffer load_commands;
    const auto load_dispatch = toggle_runtime->dispatch_hook(
        toggle_world,
        vkpt::scripting::ScriptLifecycleHook::OnLoad,
        load_ctx,
        load_commands);
    if (!Check(load_dispatch.result.overall_status !=
                   vkpt::scripting::ScriptDispatchResult::Status::Failure,
               "OnLoad after play->edit->play must not fail")) {
      return 1;
    }
    bool toggle_load_fired = false;
    for (const auto& rs : toggle_runtime->runtime_states()) {
      if (rs.entity == toggle_entity && rs.hook_fired) {
        toggle_load_fired = true;
        break;
      }
    }
    if (!Check(toggle_load_fired,
               "OnLoad after play->edit->play must dispatch the reused script")) {
      return 1;
    }
  }

  // Subsequent OnUpdate must continue with state reused (counter at 4 since
  // we ran 2 OnUpdates earlier; the OnLoad hook in the counter probe doesn't
  // tick the counter, it just logs a load marker — the next OnUpdate logs 3).
  {
    vkpt::scripting::ScriptExecutionContext post_ctx = context;
    post_ctx.frame = 700u;
    vkpt::scene::WorldCommandBuffer post_commands;
    const auto post_dispatch = toggle_runtime->dispatch_hook(
        toggle_world,
        vkpt::scripting::ScriptLifecycleHook::OnUpdate,
        post_ctx,
        post_commands);
    const auto seen = last_logged_tick(post_dispatch);
    if (!Check(seen >= 3,
               "post-toggle OnUpdate should resume counter past the pre-toggle ticks")) {
      return 1;
    }
  }

  std::cout << "script dispatch contract smoke: ok\n";
  return 0;
#endif
}
