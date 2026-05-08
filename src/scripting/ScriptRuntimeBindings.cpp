#include "scripting/ScriptRuntime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
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

std::string TrimCopy(std::string_view value) {
  auto begin = value.begin();
  auto end = value.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
    ++begin;
  }
  while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
    --end;
  }
  return std::string(begin, end);
}

std::string LowerCopy(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool ValidEditorParamName(std::string_view name) {
  if (name.empty() || name.find('=') != std::string_view::npos) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
  });
}

std::string NormalizeEditorParamType(std::string_view type) {
  const auto lower = LowerCopy(type);
  if (lower == "bool" || lower == "boolean" || lower == "toggle") {
    return "bool";
  }
  if (lower == "number" || lower == "float" || lower == "double" ||
      lower == "int" || lower == "integer" || lower == "slider") {
    return "number";
  }
  return "text";
}

std::vector<std::string> TokenizeAnnotation(std::string_view text) {
  std::vector<std::string> tokens;
  std::string token;
  char quote = '\0';
  bool escaped = false;
  for (const char c : text) {
    if (quote != '\0') {
      token.push_back(c);
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == quote) {
        quote = '\0';
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      token.push_back(c);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!token.empty()) {
        tokens.push_back(std::move(token));
        token.clear();
      }
      continue;
    }
    token.push_back(c);
  }
  if (!token.empty()) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

std::string UnquoteAnnotationValue(std::string value) {
  if (value.size() >= 2u && ((value.front() == '"' && value.back() == '"') ||
                             (value.front() == '\'' && value.back() == '\''))) {
    const char quote = value.front();
    std::string out;
    out.reserve(value.size() - 2u);
    bool escaped = false;
    for (std::size_t i = 1u; i + 1u < value.size(); ++i) {
      const char c = value[i];
      if (escaped) {
        out.push_back(c);
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else {
        out.push_back(c);
      }
    }
    (void)quote;
    return out;
  }
  return value;
}

std::optional<double> ParseAnnotationDouble(std::string_view value) {
  const std::string text(value);
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0') {
    return std::nullopt;
  }
  return parsed;
}

std::filesystem::path ResolveScriptPath(std::string_view source) {
  const std::filesystem::path requested{std::string(source)};
  std::error_code ec;
  if (requested.is_absolute() || (std::filesystem::exists(requested, ec) && !ec)) {
    return requested.lexically_normal();
  }
  auto current = std::filesystem::current_path();
  for (int i = 0; i < 8; ++i) {
    const auto candidate = (current / requested).lexically_normal();
    ec.clear();
    if (std::filesystem::exists(candidate, ec) && !ec) {
      return candidate;
    }
    if (!current.has_parent_path() || current.parent_path() == current) {
      break;
    }
    current = current.parent_path();
  }
  return requested.lexically_normal();
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::optional<ScriptEditorParam> ParseEditorAnnotationLine(std::string_view line) {
  std::string trimmed = TrimCopy(line);
  if (!trimmed.starts_with("--")) {
    return std::nullopt;
  }
  trimmed = TrimCopy(std::string_view(trimmed).substr(2u));
  std::size_t markerLength = 0u;
  if (trimmed.starts_with("@editor")) {
    markerLength = 7u;
  } else if (trimmed.starts_with("[editor]")) {
    markerLength = 8u;
  } else {
    return std::nullopt;
  }

  const auto tokens = TokenizeAnnotation(TrimCopy(std::string_view(trimmed).substr(markerLength)));
  if (tokens.empty() || !ValidEditorParamName(tokens.front())) {
    return std::nullopt;
  }

  ScriptEditorParam param;
  param.name = tokens.front();
  param.label = param.name;
  std::size_t index = 1u;
  if (index < tokens.size() && tokens[index].find('=') == std::string::npos) {
    param.type = NormalizeEditorParamType(tokens[index]);
    ++index;
  }

  for (; index < tokens.size(); ++index) {
    const auto equals = tokens[index].find('=');
    if (equals == std::string::npos || equals == 0u) {
      continue;
    }
    const auto key = LowerCopy(std::string_view(tokens[index]).substr(0u, equals));
    const auto value = UnquoteAnnotationValue(tokens[index].substr(equals + 1u));
    if (key == "type") {
      param.type = NormalizeEditorParamType(value);
    } else if (key == "default" || key == "value") {
      param.default_value = value;
    } else if (key == "label") {
      param.label = value.empty() ? param.name : value;
    } else if (key == "min" || key == "minimum") {
      if (const auto parsed = ParseAnnotationDouble(value)) {
        param.minimum = *parsed;
        param.has_minimum = true;
      }
    } else if (key == "max" || key == "maximum") {
      if (const auto parsed = ParseAnnotationDouble(value)) {
        param.maximum = *parsed;
        param.has_maximum = true;
      }
    } else if (key == "step") {
      if (const auto parsed = ParseAnnotationDouble(value)) {
        param.step = *parsed;
        param.has_step = true;
      }
    }
  }
  return param;
}

std::vector<ScriptEditorParam> ParseScriptEditorAnnotations(std::string_view source) {
  std::vector<ScriptEditorParam> params;
  std::unordered_set<std::string> seen;
  std::size_t offset = 0u;
  while (offset <= source.size()) {
    const auto newline = source.find('\n', offset);
    const auto count = newline == std::string_view::npos ? source.size() - offset : newline - offset;
    const auto line = source.substr(offset, count);
    if (auto param = ParseEditorAnnotationLine(line)) {
      if (seen.insert(param->name).second) {
        params.push_back(std::move(*param));
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    offset = newline + 1u;
  }
  return params;
}

std::string ScriptVariableOverrideKey(vkpt::core::StableEntityId entity,
                                      std::string_view scope,
                                      std::string_view name) {
  return std::to_string(entity) + "|" + std::string(scope) + "|" + std::string(name);
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
  bindings.reserve(world.all_entities().size());

  std::size_t stable_order = 0;
  // Dispatch order follows SceneWorld stable entity order and skips empty ScriptComponent sources.
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
    binding.module_id = script.module_id.empty() ? binding.entry : script.module_id;
    binding.enabled = script.enabled;
    binding.reload_on_save = script.reload_on_save;
    binding.params = script.params;
    bindings.push_back(std::move(binding));
  }

  return bindings;
}

void ApplyScriptEditorAnnotations(std::vector<ScriptBinding>& bindings) {
  for (auto& binding : bindings) {
    binding.editor_params.clear();
    if (!IsSupportedLanguage(binding.language) || binding.source.empty()) {
      continue;
    }
    const auto path = ResolveScriptPath(binding.source);
    const auto source = ReadTextFile(path);
    if (!source) {
      continue;
    }
    binding.editor_params = ParseScriptEditorAnnotations(*source);
  }
}

ScriptBindingSummary SummarizeScriptBindings(const std::vector<ScriptBinding>& bindings,
                                             bool lua_compiled_in,
                                             bool execution_available) {
  ScriptBindingSummary summary;
  summary.binding_count = bindings.size();
  summary.lua_compiled_in = lua_compiled_in;
  summary.execution_available = execution_available;

  // Counts describe binding eligibility only; global runtime gates are applied during dispatch.
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
  ApplyScriptEditorAnnotations(m_bindings);
  // Source paths can resolve differently after reload, so bytecode is rebuilt on demand.
  m_lua_bytecode_cache.clear();
  m_variable_snapshots.clear();
  m_runtime_states.clear();
  m_disabled_until_reload.clear();
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

const std::vector<ScriptVariableSnapshot>& EcsScriptRuntime::variable_snapshots() const {
  return m_variable_snapshots;
}

bool EcsScriptRuntime::set_variable_override(vkpt::core::StableEntityId entity,
                                             std::string_view scope,
                                             std::string_view name,
                                             std::string_view value) {
  if (entity == 0u || scope.empty() || name.empty()) {
    return false;
  }
  ScriptVariableOverride overrideValue;
  overrideValue.entity = entity;
  overrideValue.scope = std::string(scope);
  overrideValue.name = std::string(name);
  overrideValue.value = std::string(value);
  m_variable_overrides[ScriptVariableOverrideKey(entity, scope, name)] = std::move(overrideValue);
  return true;
}

void EcsScriptRuntime::clear_variable_overrides(vkpt::core::StableEntityId entity) {
  if (entity == 0u) {
    m_variable_overrides.clear();
    return;
  }
  for (auto it = m_variable_overrides.begin(); it != m_variable_overrides.end();) {
    if (it->second.entity == entity) {
      it = m_variable_overrides.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<ScriptVariableOverride> EcsScriptRuntime::variable_overrides() const {
  std::vector<ScriptVariableOverride> out;
  out.reserve(m_variable_overrides.size());
  for (const auto& [_, overrideValue] : m_variable_overrides) {
    (void)_;
    out.push_back(overrideValue);
  }
  return out;
}

const std::vector<ScriptBindingRuntimeState>& EcsScriptRuntime::runtime_states() const {
  return m_runtime_states;
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
