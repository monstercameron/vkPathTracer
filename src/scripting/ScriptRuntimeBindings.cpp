#include "scripting/ScriptRuntime.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
#ifdef PT_ENABLE_LUA
  return true;
#else
  return false;
#endif
}

std::unique_ptr<IScriptRuntime> CreateScriptRuntime() {
  return std::make_unique<EcsScriptRuntime>();
}


}  // namespace vkpt::scripting
