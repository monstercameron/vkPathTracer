#include "scripting/ScriptRuntime.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
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

bool IsKnownEditorParamType(std::string_view type) {
  const auto lower = LowerCopy(type);
  return lower == "bool" ||
         lower == "boolean" ||
         lower == "toggle" ||
         lower == "number" ||
         lower == "float" ||
         lower == "double" ||
         lower == "int" ||
         lower == "integer" ||
         lower == "slider" ||
         lower == "text" ||
         lower == "string";
}

bool IsValidAnnotationBool(std::string_view value) {
  const auto lower = LowerCopy(value);
  return lower == "true" ||
         lower == "false" ||
         lower == "1" ||
         lower == "0" ||
         lower == "yes" ||
         lower == "no" ||
         lower == "on" ||
         lower == "off";
}

void AddEditorAnnotationDiagnostic(std::vector<std::string>* diagnostics,
                                   std::size_t line_number,
                                   std::string message) {
  if (diagnostics == nullptr) {
    return;
  }
  diagnostics->push_back(
      "line " + std::to_string(line_number) + ": " + std::move(message));
}

struct AnnotationTokens {
  std::vector<std::string> tokens;
  std::string error;
};

struct EditorAnnotationParseResult {
  std::vector<ScriptEditorParam> params;
  std::vector<std::string> diagnostics;
  bool pure = false;
  bool requires_authoritative_reads = false;
};

AnnotationTokens TokenizeAnnotation(std::string_view text) {
  AnnotationTokens result;
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
        result.tokens.push_back(std::move(token));
        token.clear();
      }
      continue;
    }
    token.push_back(c);
  }
  if (!token.empty()) {
    result.tokens.push_back(std::move(token));
  }
  if (quote != '\0') {
    result.error = "unterminated quoted value";
  }
  return result;
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
  if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

std::size_t EditorAnnotationMarkerLength(std::string_view trimmed) {
  auto markerMatches = [](std::string_view text, std::string_view marker) {
    if (!text.starts_with(marker)) {
      return false;
    }
    return text.size() == marker.size() ||
           std::isspace(static_cast<unsigned char>(text[marker.size()])) != 0;
  };
  if (markerMatches(trimmed, "@editor")) {
    return 7u;
  }
  if (markerMatches(trimmed, "[editor]")) {
    return 8u;
  }
  return 0u;
}

bool LooksLikeMalformedEditorAnnotation(std::string_view line) {
  std::string trimmed = TrimCopy(line);
  if (!trimmed.starts_with("--")) {
    return false;
  }
  trimmed = TrimCopy(std::string_view(trimmed).substr(2u));
  return EditorAnnotationMarkerLength(trimmed) != 0u;
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

std::optional<ScriptEditorParam> ParseEditorAnnotationLine(std::string_view line,
                                                           std::size_t line_number,
                                                           std::vector<std::string>* diagnostics) {
  std::string trimmed = TrimCopy(line);
  if (!trimmed.starts_with("--")) {
    return std::nullopt;
  }
  trimmed = TrimCopy(std::string_view(trimmed).substr(2u));
  const std::size_t markerLength = EditorAnnotationMarkerLength(trimmed);
  if (markerLength == 0u) {
    return std::nullopt;
  }

  const auto token_result = TokenizeAnnotation(TrimCopy(std::string_view(trimmed).substr(markerLength)));
  if (!token_result.error.empty()) {
    AddEditorAnnotationDiagnostic(diagnostics, line_number, token_result.error);
    return std::nullopt;
  }
  const auto& tokens = token_result.tokens;
  if (tokens.empty()) {
    AddEditorAnnotationDiagnostic(diagnostics, line_number, "missing editor parameter name");
    return std::nullopt;
  }
  if (!ValidEditorParamName(tokens.front())) {
    AddEditorAnnotationDiagnostic(diagnostics,
                                  line_number,
                                  "invalid editor parameter name '" + tokens.front() + "'");
    return std::nullopt;
  }

  ScriptEditorParam param;
  param.name = tokens.front();
  param.label = param.name;
  std::size_t index = 1u;
  if (index < tokens.size() && tokens[index].find('=') == std::string::npos) {
    if (!IsKnownEditorParamType(tokens[index])) {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "unknown editor parameter type '" + tokens[index] +
                                        "' for '" + param.name + "'");
    }
    param.type = NormalizeEditorParamType(tokens[index]);
    ++index;
  }

  std::unordered_set<std::string> seen_keys;
  for (; index < tokens.size(); ++index) {
    const auto equals = tokens[index].find('=');
    if (equals == std::string::npos) {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "unexpected token '" + tokens[index] +
                                        "' in decorator for '" + param.name + "'");
      continue;
    }
    if (equals == 0u) {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "empty decorator field name for '" + param.name + "'");
      continue;
    }
    const auto key = LowerCopy(std::string_view(tokens[index]).substr(0u, equals));
    const auto value = UnquoteAnnotationValue(tokens[index].substr(equals + 1u));
    if (!seen_keys.insert(key).second) {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "duplicate decorator field '" + key +
                                        "' for '" + param.name + "'");
    }
    if (key == "type") {
      if (!IsKnownEditorParamType(value)) {
        AddEditorAnnotationDiagnostic(diagnostics,
                                      line_number,
                                      "unknown editor parameter type '" + value +
                                          "' for '" + param.name + "'");
      }
      param.type = NormalizeEditorParamType(value);
    } else if (key == "default" || key == "value") {
      param.default_value = value;
    } else if (key == "label") {
      param.label = value.empty() ? param.name : value;
    } else if (key == "min" || key == "minimum") {
      if (const auto parsed = ParseAnnotationDouble(value)) {
        param.minimum = *parsed;
        param.has_minimum = true;
      } else {
        AddEditorAnnotationDiagnostic(diagnostics,
                                      line_number,
                                      "invalid minimum value '" + value + "' for '" + param.name + "'");
      }
    } else if (key == "max" || key == "maximum") {
      if (const auto parsed = ParseAnnotationDouble(value)) {
        param.maximum = *parsed;
        param.has_maximum = true;
      } else {
        AddEditorAnnotationDiagnostic(diagnostics,
                                      line_number,
                                      "invalid maximum value '" + value + "' for '" + param.name + "'");
      }
    } else if (key == "step") {
      if (const auto parsed = ParseAnnotationDouble(value); parsed && *parsed > 0.0) {
        param.step = *parsed;
        param.has_step = true;
      } else {
        AddEditorAnnotationDiagnostic(diagnostics,
                                      line_number,
                                      "invalid step value '" + value + "' for '" + param.name + "'");
      }
    } else {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "unknown decorator field '" + key + "' for '" + param.name + "'");
    }
  }
  if (!param.default_value.empty()) {
    if (param.type == "number" && !ParseAnnotationDouble(param.default_value)) {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "invalid numeric default '" + param.default_value +
                                        "' for '" + param.name + "'");
      param.default_value.clear();
    } else if (param.type == "bool" && !IsValidAnnotationBool(param.default_value)) {
      AddEditorAnnotationDiagnostic(diagnostics,
                                    line_number,
                                    "invalid boolean default '" + param.default_value +
                                        "' for '" + param.name + "'");
      param.default_value.clear();
    }
  }
  if (param.type == "number" && param.has_minimum && param.has_maximum &&
      param.minimum > param.maximum) {
    AddEditorAnnotationDiagnostic(diagnostics,
                                  line_number,
                                  "minimum is greater than maximum for '" + param.name +
                                      "'; range was swapped");
    std::swap(param.minimum, param.maximum);
  }
  if (param.type == "bool" && (param.has_minimum || param.has_maximum || param.has_step)) {
    AddEditorAnnotationDiagnostic(diagnostics,
                                  line_number,
                                  "numeric range fields are ignored for bool param '" + param.name + "'");
    param.has_minimum = false;
    param.has_maximum = false;
    param.has_step = false;
  } else if (param.type == "text" &&
             (param.has_minimum || param.has_maximum || param.has_step)) {
    AddEditorAnnotationDiagnostic(diagnostics,
                                  line_number,
                                  "numeric range fields are ignored for text param '" + param.name + "'");
    param.has_minimum = false;
    param.has_maximum = false;
    param.has_step = false;
  }
  return param;
}

EditorAnnotationParseResult ParseScriptEditorAnnotations(std::string_view source,
                                                         std::string_view source_name) {
  EditorAnnotationParseResult result;
  std::unordered_set<std::string> seen;
  std::size_t offset = 0u;
  std::size_t line_number = 1u;
  while (offset <= source.size()) {
    const auto newline = source.find('\n', offset);
    const auto count = newline == std::string_view::npos ? source.size() - offset : newline - offset;
    const auto line = source.substr(offset, count);
    if (auto param = ParseEditorAnnotationLine(line, line_number, &result.diagnostics)) {
      if (seen.insert(param->name).second) {
        result.params.push_back(std::move(*param));
      } else {
        AddEditorAnnotationDiagnostic(&result.diagnostics,
                                      line_number,
                                      "duplicate editor param '" + param->name + "' ignored");
      }
    } else if (LooksLikeMalformedEditorAnnotation(line)) {
      AddEditorAnnotationDiagnostic(&result.diagnostics,
                                    line_number,
                                    "malformed script editor annotation ignored");
    }
    if (newline == std::string_view::npos) {
      break;
    }
    offset = newline + 1u;
    ++line_number;
  }
  for (const auto& diagnostic : result.diagnostics) {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Warning,
        "scripts",
        "script editor annotation diagnostic",
        {{"source", std::string(source_name)}, {"diagnostic", diagnostic}});
  }
  return result;
}

struct ScriptPreambleFlags {
  bool pure = false;
  bool requires_authoritative_reads = false;
};

ScriptPreambleFlags DetectScriptPreambleFlags(std::string_view source) {
  ScriptPreambleFlags flags;
  std::size_t offset = 0u;
  while (offset <= source.size()) {
    const auto newline = source.find('\n', offset);
    const auto count = newline == std::string_view::npos ? source.size() - offset : newline - offset;
    auto line = TrimCopy(source.substr(offset, count));
    if (line.starts_with("--")) {
      if (newline == std::string_view::npos) {
        break;
      }
      offset = newline + 1u;
      continue;
    }
    const auto comment = line.find("--");
    if (comment != std::string::npos) {
      line = TrimCopy(std::string_view(line).substr(0u, comment));
    }
    const auto equals = line.find('=');
    if (equals != std::string::npos) {
      const auto lhs = TrimCopy(std::string_view(line).substr(0u, equals));
      const auto rhs = LowerCopy(TrimCopy(std::string_view(line).substr(equals + 1u)));
      if (lhs == "pure") {
        flags.pure = rhs == "true" || rhs == "1";
      } else if (lhs == "fresh_reads" || lhs == "authoritative_reads") {
        flags.requires_authoritative_reads = rhs == "true" || rhs == "1";
      }
    }
    if (!line.empty() && !line.starts_with("local ")) {
      return flags;
    }
    if (newline == std::string::npos) {
      break;
    }
    offset = newline + 1u;
  }
  return flags;
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
  std::unordered_map<std::string, EditorAnnotationParseResult> annotation_cache;
  std::unordered_set<std::string> unreadable_sources;
  for (auto& binding : bindings) {
    binding.editor_params.clear();
    binding.editor_param_diagnostics.clear();
    if (!IsSupportedLanguage(binding.language) || binding.source.empty()) {
      continue;
    }
    const auto path = ResolveScriptPath(binding.source);
    const auto cache_key = path.generic_string();
    if (const auto cached = annotation_cache.find(cache_key); cached != annotation_cache.end()) {
      binding.editor_params = cached->second.params;
      binding.editor_param_diagnostics = cached->second.diagnostics;
      binding.pure = cached->second.pure;
      binding.requires_authoritative_reads = cached->second.requires_authoritative_reads;
      continue;
    }
    if (unreadable_sources.contains(cache_key)) {
      continue;
    }
    const auto source = ReadTextFile(path);
    if (!source) {
      unreadable_sources.insert(cache_key);
      binding.editor_param_diagnostics.push_back("source could not be read: " + cache_key);
      vkpt::log::Logger::instance().log(
          vkpt::log::Severity::Warning,
          "scripts",
          "script editor annotations skipped because source could not be read",
          {{"source", cache_key}});
      continue;
    }
    auto parsed = ParseScriptEditorAnnotations(*source, cache_key);
    const auto preamble = DetectScriptPreambleFlags(*source);
    parsed.pure = preamble.pure;
    parsed.requires_authoritative_reads = preamble.requires_authoritative_reads;
    binding.pure = parsed.pure;
    binding.requires_authoritative_reads = parsed.requires_authoritative_reads;
    binding.editor_params = parsed.params;
    binding.editor_param_diagnostics = parsed.diagnostics;
    annotation_cache.emplace(cache_key, std::move(parsed));
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
  std::unique_lock runtime_lock(m_runtime_mutex);
  vkpt::core::contracts::assert_state(
      "EcsScriptRuntime::reload_bindings",
      m_status.lifecycle,
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded,
       vkpt::core::contracts::ComponentLifecycle::Failed});
  m_status.lifecycle = vkpt::core::contracts::ComponentLifecycle::Initializing;
  m_bindings = BuildScriptBindings(world);
  ApplyScriptEditorAnnotations(m_bindings);
  // Source paths can resolve differently after reload, so bytecode is rebuilt on demand.
  {
    std::scoped_lock cache_lock(m_lua_cache_mutex);
    m_lua_bytecode_cache.clear();
  }
  m_lua_state_pool->clear();
  m_script_command_queue->clear();
  m_variable_snapshots.clear();
  m_runtime_states.clear();
  m_disabled_until_reload.clear();
  const auto summary = SummarizeScriptBindings(m_bindings, lua_compiled_in(), execution_available());
  m_status.lifecycle = vkpt::core::contracts::ComponentLifecycle::Ready;
  m_status.health = vkpt::core::contracts::SubsystemHealth::Ok;
  m_status.active_scripts = summary.runnable_count;
  m_status.last_error_script_id = 0u;
  m_status.current_flow_id = 0u;
  m_status.health_reason = "ok";
  m_status.last_error.clear();
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
  std::unique_lock runtime_lock(m_runtime_mutex);
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
  std::unique_lock runtime_lock(m_runtime_mutex);
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
  std::unique_lock runtime_lock(m_runtime_mutex);
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

ScriptingStatus EcsScriptRuntime::status() const {
  std::unique_lock runtime_lock(m_runtime_mutex);
  return m_status;
}

vkpt::core::health::Report EvaluateScriptingHealth(const ScriptingStatus& status) {
  using vkpt::core::health::Report;
  using vkpt::core::health::Status;

  if (status.lifecycle == vkpt::core::contracts::ComponentLifecycle::Failed) {
    return Report{Status::Failed,
                  status.last_error.empty() ? "scripting failed" : status.last_error};
  }
  if (!status.last_error.empty() || status.last_error_script_id != 0u) {
    return Report{Status::Degraded,
                  status.last_error.empty() ? "script_errors" : status.last_error};
  }
  return Report{Status::Ok,
                status.health_reason.empty() ? "ok" : status.health_reason};
}

vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const ScriptingStatus& status) {
  auto out = vkpt::core::contracts::MakeSubsystemStatus(status.name,
                                                       status.health);
  out.last_error = status.last_error;
  out.set_custom("lifecycle",
                 std::string(vkpt::core::contracts::ComponentLifecycleName(
                     status.lifecycle)));
  out.set_custom("health_reason", status.health_reason);
  out.set_custom("active_scripts", std::to_string(status.active_scripts));
  out.set_custom("hooks_fired_total", std::to_string(status.hooks_fired_total));
  out.set_custom("budget_kills_total", std::to_string(status.budget_kills_total));
  out.set_custom("last_error_script_id",
                 std::to_string(status.last_error_script_id));
  out.set_custom("last_frame", std::to_string(status.last_frame));
  out.set_custom("current_flow_id", std::to_string(status.current_flow_id));
  return out;
}

std::string FormatScriptingStatus(const ScriptingStatus& status) {
  std::ostringstream out;
  out << "scripting status: "
      << vkpt::core::contracts::SubsystemHealthName(status.health)
      << "\n  lifecycle: "
      << vkpt::core::contracts::ComponentLifecycleName(status.lifecycle)
      << "\n  active_scripts: " << status.active_scripts
      << "\n  hooks_fired_total: " << status.hooks_fired_total
      << "\n  budget_kills_total: " << status.budget_kills_total
      << "\n  last_error_script_id: " << status.last_error_script_id
      << "\n  last_frame: " << status.last_frame
      << "\n  current_flow_id: " << status.current_flow_id;
  if (!status.last_error.empty()) {
    out << "\n  last_error: " << status.last_error;
  }
  return out.str();
}

std::shared_ptr<vkpt::core::health::IHealthProbe>
EcsScriptRuntime::create_health_probe() const {
  class ScriptingHealthProbe final : public vkpt::core::health::IHealthProbe {
   public:
    explicit ScriptingHealthProbe(const EcsScriptRuntime* runtime)
        : m_runtime(runtime) {}

    std::string name() const override { return std::string(kScriptingSubsystemName); }

    vkpt::core::health::Report check() override {
      if (m_runtime == nullptr) {
        return {vkpt::core::health::Status::Failed,
                "scripting runtime unavailable"};
      }
      return EvaluateScriptingHealth(m_runtime->status());
    }

   private:
    const EcsScriptRuntime* m_runtime = nullptr;
  };

  return std::make_shared<ScriptingHealthProbe>(this);
}

std::size_t EcsScriptRuntime::lua_state_count() const {
  return m_lua_state_pool->state_count();
}

std::size_t EcsScriptRuntime::lua_states_created_total() const {
  return m_lua_state_pool->created_total();
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

std::string FormatScriptList(const IScriptRuntime& runtime) {
  std::ostringstream out;
  const auto status = runtime.status();
  out << "active_scripts=" << status.active_scripts
      << " hooks_fired_total=" << status.hooks_fired_total
      << " budget_kills_total=" << status.budget_kills_total
      << " last_error_script_id=" << status.last_error_script_id << "\n";

  const auto& bindings = runtime.bindings();
  const auto& states = runtime.runtime_states();
  for (const auto& binding : bindings) {
    const auto state_it = std::find_if(
        states.begin(),
        states.end(),
        [&](const ScriptBindingRuntimeState& state) {
          return state.entity == binding.entity && state.source == binding.source;
        });
    out << binding.entity << " " << binding.source
        << " enabled=" << (binding.enabled ? "true" : "false")
        << " pure=" << (binding.pure ? "true" : "false");
    if (state_it != states.end()) {
      out << " hook=" << to_string(state_it->last_hook)
          << " hook_us=" << (state_it->hook_duration_ns / 1000u)
          << " instructions=" << state_it->instruction_count
          << " mem_kb=" << (state_it->memory_estimate_bytes / 1024u)
          << " state_ptr=0x" << std::hex << state_it->state_ptr << std::dec;
      if (!state_it->last_error.empty()) {
        out << " last_error=" << state_it->last_error;
      }
      if (!state_it->skip_reason.empty()) {
        out << " skip=" << state_it->skip_reason;
      }
    }
    out << "\n";
  }
  return out.str();
}


}  // namespace vkpt::scripting
