#include "scripting/ScriptRuntime.h"

#include <algorithm>
#include <utility>

#include "core/Logging.h"

namespace vkpt::scripting {
namespace {

bool IsLuaCompiledIn() {
#ifdef PT_ENABLE_LUA
  return true;
#else
  return false;
#endif
}

bool IsSupportedLanguage(std::string_view language) {
  return language.empty() || language == "lua";
}

ScriptDiagnostic MakeDiagnostic(ScriptDiagnosticSeverity severity,
                                ScriptLifecycleHook hook,
                                const ScriptBinding& binding,
                                const ScriptExecutionContext& context,
                                std::string message) {
  ScriptDiagnostic diagnostic;
  diagnostic.severity = severity;
  diagnostic.hook = hook;
  diagnostic.entity = binding.entity;
  diagnostic.frame = context.frame;
  diagnostic.source = binding.source;
  diagnostic.message = std::move(message);
  return diagnostic;
}

void LogDiagnostic(const ScriptDiagnostic& diagnostic) {
  vkpt::log::Severity severity = vkpt::log::Severity::Info;
  switch (diagnostic.severity) {
    case ScriptDiagnosticSeverity::Warning:
      severity = vkpt::log::Severity::Warning;
      break;
    case ScriptDiagnosticSeverity::Error:
      severity = vkpt::log::Severity::Error;
      break;
    case ScriptDiagnosticSeverity::Info:
    default:
      severity = vkpt::log::Severity::Info;
      break;
  }

  vkpt::log::Logger::instance().log(
      severity,
      "scripts",
      diagnostic.message,
      {{"entity", std::to_string(diagnostic.entity)},
       {"hook", std::string(to_string(diagnostic.hook))},
       {"source", diagnostic.source}},
      diagnostic.frame);
}

}  // namespace

std::string_view to_string(ScriptLifecycleHook hook) {
  switch (hook) {
    case ScriptLifecycleHook::OnLoad:
      return "on_load";
    case ScriptLifecycleHook::OnSpawn:
      return "on_spawn";
    case ScriptLifecycleHook::OnEnable:
      return "on_enable";
    case ScriptLifecycleHook::OnDisable:
      return "on_disable";
    case ScriptLifecycleHook::OnUpdate:
      return "on_update";
    case ScriptLifecycleHook::OnFixedUpdate:
      return "on_fixed_update";
    case ScriptLifecycleHook::OnLateUpdate:
      return "on_late_update";
    case ScriptLifecycleHook::OnDestroy:
      return "on_destroy";
    case ScriptLifecycleHook::OnUnload:
      return "on_unload";
    default:
      return "unknown";
  }
}

std::string_view to_string(ScriptDiagnosticSeverity severity) {
  switch (severity) {
    case ScriptDiagnosticSeverity::Info:
      return "info";
    case ScriptDiagnosticSeverity::Warning:
      return "warning";
    case ScriptDiagnosticSeverity::Error:
      return "error";
    default:
      return "info";
  }
}

std::vector<ScriptBinding> BuildScriptBindings(const vkpt::scene::SceneWorld& world) {
  std::vector<ScriptBinding> bindings;
  bindings.reserve(world.query(vkpt::scene::ComponentKind::Script).size());

  std::size_t stable_order = 0;
  for (const auto entity_id : world.all_entities()) {
    const auto* entity = world.get_entity(entity_id);
    if (entity == nullptr || !entity->script.has_value()) {
      continue;
    }
    const auto& script = *entity->script;
    if (script.script.empty()) {
      continue;
    }

    ScriptBinding binding;
    binding.entity = entity_id;
    binding.stable_order = stable_order++;
    binding.entity_name = entity->identity.name;
    binding.source = script.script;
    binding.language = script.language.empty() ? "lua" : script.language;
    binding.entry = script.entry.empty() ? "default" : script.entry;
    binding.enabled = script.enabled;
    binding.reload_on_save = script.reload_on_save;
    bindings.push_back(std::move(binding));
  }

  return bindings;
}

ScriptBindingSummary SummarizeScriptBindings(const std::vector<ScriptBinding>& bindings,
                                             bool lua_compiled_in,
                                             bool execution_available) {
  ScriptBindingSummary summary;
  summary.binding_count = bindings.size();
  summary.lua_compiled_in = lua_compiled_in;
  summary.execution_available = execution_available;

  for (const auto& binding : bindings) {
    if (!binding.enabled) {
      ++summary.disabled_count;
      continue;
    }
    if (!IsSupportedLanguage(binding.language)) {
      ++summary.unsupported_language_count;
      continue;
    }
    ++summary.runnable_count;
  }

  return summary;
}

ScriptBindingSummary EcsScriptRuntime::reload_bindings(const vkpt::scene::SceneWorld& world) {
  m_bindings = BuildScriptBindings(world);
  const auto summary = SummarizeScriptBindings(m_bindings, lua_compiled_in(), execution_available());
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      "scripts",
      "script bindings reloaded",
      {{"bindings", std::to_string(summary.binding_count)},
       {"runnable", std::to_string(summary.runnable_count)},
       {"disabled", std::to_string(summary.disabled_count)},
       {"unsupported_language", std::to_string(summary.unsupported_language_count)},
       {"lua_compiled_in", summary.lua_compiled_in ? "true" : "false"},
       {"execution_available", summary.execution_available ? "true" : "false"}});
  return summary;
}

ScriptDispatchSummary EcsScriptRuntime::dispatch_hook(const vkpt::scene::SceneWorld& world,
                                                      ScriptLifecycleHook hook,
                                                      const ScriptExecutionContext& context,
                                                      vkpt::scene::WorldCommandBuffer& commands) {
  const auto current_bindings = BuildScriptBindings(world);
  if (current_bindings.size() != m_bindings.size()) {
    m_bindings = current_bindings;
  }

  ScriptDispatchSummary summary;
  summary.hook = hook;
  summary.frame = context.frame;
  summary.binding_count = m_bindings.size();
  summary.command_count_before = commands.commands().size();
  summary.lua_compiled_in = lua_compiled_in();
  summary.execution_available = execution_available();
  summary.scripts_disabled = !context.scripts_enabled;
  summary.benchmark_blocked = context.benchmark_mode && !context.allow_benchmark_scripts;

  for (const auto& binding : m_bindings) {
    if (!binding.enabled || !IsSupportedLanguage(binding.language)) {
      ++summary.skipped_count;
      continue;
    }

    ++summary.runnable_count;

    if (summary.scripts_disabled) {
      ++summary.skipped_count;
      continue;
    }
    if (summary.benchmark_blocked) {
      ++summary.skipped_count;
      continue;
    }
    if (!summary.execution_available) {
      ++summary.skipped_count;
      auto diagnostic = MakeDiagnostic(ScriptDiagnosticSeverity::Warning,
                                       hook,
                                       binding,
                                       context,
                                       "script hook skipped because Lua execution is not available");
      LogDiagnostic(diagnostic);
      summary.diagnostics.push_back(diagnostic);
      m_diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    ++summary.hook_call_count;
  }

  summary.command_count_after = commands.commands().size();
  return summary;
}

const std::vector<ScriptBinding>& EcsScriptRuntime::bindings() const {
  return m_bindings;
}

const std::vector<ScriptDiagnostic>& EcsScriptRuntime::diagnostics() const {
  return m_diagnostics;
}

bool EcsScriptRuntime::lua_compiled_in() const {
  return IsLuaCompiledIn();
}

bool EcsScriptRuntime::execution_available() const {
  return false;
}

std::unique_ptr<IScriptRuntime> CreateScriptRuntime() {
  return std::make_unique<EcsScriptRuntime>();
}

}  // namespace vkpt::scripting
