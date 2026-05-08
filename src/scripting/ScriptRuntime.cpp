#include "scripting/ScriptRuntime.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "audio/AudioSystem.h"
#include "core/Logging.h"
#ifdef PT_ENABLE_LUA
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#endif

namespace vkpt::scripting {
namespace {

bool IsSupportedLanguage(std::string_view language) {
  return language.empty() || language == "lua";
}

std::string TrimCopy(std::string_view text) {
  std::size_t begin = 0u;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

std::string LowerSnakeCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool lastSeparator = false;
  for (const unsigned char c : text) {
    if (std::isalnum(c) != 0) {
      out.push_back(static_cast<char>(std::tolower(c)));
      lastSeparator = false;
    } else if (!lastSeparator && !out.empty()) {
      out.push_back('_');
      lastSeparator = true;
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::string ScriptVariableOverrideKey(vkpt::core::StableEntityId entity,
                                      std::string_view scope,
                                      std::string_view name) {
  return std::to_string(entity) + "|" + std::string(scope) + "|" + std::string(name);
}

const ScriptVariableOverride* FindVariableOverride(
    const std::unordered_map<std::string, ScriptVariableOverride>& overrides,
    vkpt::core::StableEntityId entity,
    std::string_view scope,
    std::string_view name) {
  const auto it = overrides.find(ScriptVariableOverrideKey(entity, scope, name));
  return it == overrides.end() ? nullptr : &it->second;
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

#ifdef PT_ENABLE_LUA

constexpr int kScriptKeyLeft = 0x01000012;
constexpr int kScriptKeyUp = 0x01000013;
constexpr int kScriptKeyRight = 0x01000014;
constexpr int kScriptKeyDown = 0x01000015;
constexpr int kScriptKeyShift = 0x01000020;
constexpr int kScriptKeyControl = 0x01000021;
constexpr int kScriptKeySpace = 0x20;

struct LuaHostContext {
  const vkpt::scene::SceneWorld* world = nullptr;
  vkpt::scene::WorldCommandBuffer* commands = nullptr;
  const ScriptExecutionContext* context = nullptr;
  const ScriptBinding* binding = nullptr;
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnUpdate;
  std::vector<ScriptDiagnostic>* dispatch_diagnostics = nullptr;
  std::vector<ScriptDiagnostic>* runtime_diagnostics = nullptr;
  vkpt::core::StableEntityId next_entity_id = 1;
  std::unordered_set<vkpt::core::StableEntityId> reserved_entity_ids;
  std::unordered_set<std::string> ensured_script_keys;
};

struct LuaMemoryBudget {
  std::size_t used = 0;
  std::size_t peak = 0;
  std::size_t limit = 0;
};

void* LuaBudgetAllocator(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* budget = static_cast<LuaMemoryBudget*>(ud);
  if (nsize == 0) {
    if (ptr != nullptr && budget != nullptr) {
      budget->used -= std::min(budget->used, osize);
    }
    std::free(ptr);
    return nullptr;
  }
  const auto current = budget == nullptr ? 0u : budget->used;
  const auto next = current - std::min(current, osize) + nsize;
  if (budget != nullptr && budget->limit != 0 && next > budget->limit) {
    return nullptr;
  }
  void* out = std::realloc(ptr, nsize);
  if (out != nullptr && budget != nullptr) {
    budget->used = next;
    budget->peak = std::max(budget->peak, budget->used);
  }
  return out;
}

void LuaInstructionBudgetHook(lua_State* lua, lua_Debug*) {
  luaL_error(lua, "instruction budget exceeded");
}

LuaHostContext* Host(lua_State* lua) {
  return static_cast<LuaHostContext*>(lua_touserdata(lua, lua_upvalueindex(1)));
}

int LuaBytecodeWriter(lua_State*, const void* data, std::size_t size, void* user_data) {
  auto* out = static_cast<std::string*>(user_data);
  out->append(static_cast<const char*>(data), size);
  return 0;
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

void AddLuaDiagnostic(LuaHostContext& host,
                      ScriptDiagnosticSeverity severity,
                      std::string message) {
  if (host.binding == nullptr || host.context == nullptr) {
    return;
  }
  auto diagnostic = MakeDiagnostic(severity, host.hook, *host.binding, *host.context, std::move(message));
  LogDiagnostic(diagnostic);
  if (host.dispatch_diagnostics != nullptr) {
    host.dispatch_diagnostics->push_back(diagnostic);
  }
  if (host.runtime_diagnostics != nullptr) {
    host.runtime_diagnostics->push_back(std::move(diagnostic));
  }
}

std::string LuaErrorText(lua_State* lua) {
  const char* message = lua_tostring(lua, -1);
  return message == nullptr ? "unknown Lua error" : std::string(message);
}

std::string LuaScalarValueText(lua_State* lua, int index) {
  if (lua_isboolean(lua, index)) {
    return lua_toboolean(lua, index) ? "true" : "false";
  }
  if (lua_isinteger(lua, index)) {
    return std::to_string(static_cast<long long>(lua_tointeger(lua, index)));
  }
  if (lua_isnumber(lua, index)) {
    std::ostringstream out;
    out << lua_tonumber(lua, index);
    return out.str();
  }
  if (lua_isstring(lua, index)) {
    return std::string("\"") + lua_tostring(lua, index) + "\"";
  }
  if (lua_isnil(lua, index)) {
    return "nil";
  }
  return {};
}

void PushLuaOverrideValue(lua_State* lua, std::string_view text) {
  const std::string trimmed = TrimCopy(text);
  std::string lower = trimmed;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "true" || lower == "false") {
    lua_pushboolean(lua, lower == "true" ? 1 : 0);
    return;
  }
  if (lower == "nil" || lower == "null") {
    lua_pushnil(lua);
    return;
  }
  if (trimmed.find('\n') != std::string::npos) {
    lua_newtable(lua);
    std::size_t begin = 0u;
    lua_Integer index = 1;
    while (begin <= trimmed.size()) {
      const auto end = trimmed.find('\n', begin);
      const auto item = TrimCopy(trimmed.substr(begin, end == std::string::npos
                                                         ? std::string_view::npos
                                                         : end - begin));
      if (!item.empty()) {
        PushLuaOverrideValue(lua, item);
        lua_seti(lua, -2, index++);
      }
      if (end == std::string::npos) {
        break;
      }
      begin = end + 1u;
    }
    return;
  }
  char* parseEnd = nullptr;
  const double parsed = std::strtod(trimmed.c_str(), &parseEnd);
  if (parseEnd != trimmed.c_str() && parseEnd != nullptr && *parseEnd == '\0' &&
      std::isfinite(parsed)) {
    lua_pushnumber(lua, parsed);
    return;
  }
  if (trimmed.size() >= 2u && trimmed.front() == '"' && trimmed.back() == '"') {
    lua_pushlstring(lua, trimmed.data() + 1, trimmed.size() - 2u);
    return;
  }
  lua_pushlstring(lua, trimmed.data(), trimmed.size());
}

bool LuaInspectableValue(lua_State* lua, int index) {
  return lua_isboolean(lua, index) || lua_isinteger(lua, index) || lua_isnumber(lua, index) ||
         lua_isstring(lua, index) || lua_isnil(lua, index) || lua_istable(lua, index);
}

std::string LuaTableValueText(lua_State* lua, int table_index, std::size_t max_fields = 6u) {
  if (!lua_istable(lua, table_index)) {
    return {};
  }
  const int table = lua_absindex(lua, table_index);
  std::vector<std::string> fields;
  fields.reserve(max_fields);
  lua_pushnil(lua);
  while (lua_next(lua, table) != 0) {
    const int value_index = lua_gettop(lua);
    const int key_index = value_index - 1;
    const auto value = LuaScalarValueText(lua, value_index);
    if (!value.empty()) {
      std::string key;
      if (lua_type(lua, key_index) == LUA_TSTRING) {
        key = lua_tostring(lua, key_index);
      } else if (lua_isinteger(lua, key_index)) {
        key = std::to_string(static_cast<long long>(lua_tointeger(lua, key_index)));
      }
      if (!key.empty()) {
        fields.push_back(key + "=" + value);
      }
    }
    lua_pop(lua, 1);
    if (fields.size() >= max_fields) {
      break;
    }
  }
  if (fields.empty()) {
    return "{}";
  }
  std::ostringstream out;
  out << "{";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i != 0u) {
      out << ", ";
    }
    out << fields[i];
  }
  out << "}";
  return out.str();
}

std::string LuaValueText(lua_State* lua, int index) {
  if (lua_istable(lua, index)) {
    return LuaTableValueText(lua, index);
  }
  return LuaScalarValueText(lua, index);
}

void AddLuaVariableSnapshot(std::vector<ScriptVariableSnapshot>& snapshots,
                            const ScriptBinding& binding,
                            ScriptLifecycleHook hook,
                            const ScriptExecutionContext& context,
                            std::string scope,
                            std::string name,
                            std::string value,
                            bool editable) {
  if (name.empty() || value.empty()) {
    return;
  }
  ScriptVariableSnapshot snapshot;
  snapshot.entity = binding.entity;
  snapshot.frame = context.frame;
  snapshot.hook = hook;
  snapshot.source = binding.source;
  snapshot.scope = std::move(scope);
  snapshot.name = std::move(name);
  snapshot.value = std::move(value);
  snapshot.editable = editable;
  snapshots.push_back(std::move(snapshot));
}

void CaptureLuaScriptVariables(lua_State* lua,
                               int script_table_index,
                               int hook_function_index,
                               const ScriptBinding& binding,
                               ScriptLifecycleHook hook,
                               const ScriptExecutionContext& context,
                               std::vector<ScriptVariableSnapshot>& snapshots) {
  constexpr std::size_t kMaxSnapshotsPerHook = 48u;
  const auto before_count = snapshots.size();
  const int script_table = lua_absindex(lua, script_table_index);
  if (lua_istable(lua, script_table)) {
    lua_pushnil(lua);
    while (lua_next(lua, script_table) != 0) {
      const int value_index = lua_gettop(lua);
      const int key_index = value_index - 1;
      if (lua_type(lua, key_index) == LUA_TSTRING && LuaInspectableValue(lua, value_index) &&
          !lua_isfunction(lua, value_index)) {
        AddLuaVariableSnapshot(snapshots,
                               binding,
                               hook,
                               context,
                               "script",
                               lua_tostring(lua, key_index),
                               LuaValueText(lua, value_index),
                               true);
      }
      lua_pop(lua, 1);
      if (snapshots.size() - before_count >= kMaxSnapshotsPerHook) {
        break;
      }
    }
  }

  if (!lua_isfunction(lua, hook_function_index)) {
    return;
  }
  const int hook_function = lua_absindex(lua, hook_function_index);
  for (int i = 1; snapshots.size() - before_count < kMaxSnapshotsPerHook; ++i) {
    const char* name = lua_getupvalue(lua, hook_function, i);
    if (name == nullptr) {
      break;
    }
    if (std::string_view{name} != "_ENV" && LuaInspectableValue(lua, -1)) {
      AddLuaVariableSnapshot(snapshots,
                             binding,
                             hook,
                             context,
                             "upvalue",
                             name,
                             LuaValueText(lua, -1),
                             true);
    }
    lua_pop(lua, 1);
  }
}

void OpenSafeLuaLibraries(lua_State* lua) {
  // Expose pure computation helpers but remove filesystem/loading/printing entry points from scripts.
  luaL_requiref(lua, "_G", luaopen_base, 1);
  lua_pop(lua, 1);
  luaL_requiref(lua, LUA_MATHLIBNAME, luaopen_math, 1);
  lua_pop(lua, 1);
  luaL_requiref(lua, LUA_TABLIBNAME, luaopen_table, 1);
  lua_pop(lua, 1);
  luaL_requiref(lua, LUA_STRLIBNAME, luaopen_string, 1);
  lua_pop(lua, 1);

  const char* blocked_globals[] = {
      "collectgarbage", "dofile", "load", "loadfile", "print", "require", nullptr};
  for (const char** name = blocked_globals; *name != nullptr; ++name) {
    lua_pushnil(lua);
    lua_setglobal(lua, *name);
  }
}

float LuaNumberField(lua_State* lua,
                     int table_index,
                     const char* field,
                     lua_Integer array_index,
                     float fallback) {
  const int absolute = lua_absindex(lua, table_index);
  lua_getfield(lua, absolute, field);
  if (lua_isnumber(lua, -1)) {
    const auto value = static_cast<float>(lua_tonumber(lua, -1));
    lua_pop(lua, 1);
    return std::isfinite(value) ? value : fallback;
  }
  lua_pop(lua, 1);

  lua_geti(lua, absolute, array_index);
  if (lua_isnumber(lua, -1)) {
    const auto value = static_cast<float>(lua_tonumber(lua, -1));
    lua_pop(lua, 1);
    return std::isfinite(value) ? value : fallback;
  }
  lua_pop(lua, 1);
  return fallback;
}

std::string LuaStringField(lua_State* lua,
                           int table_index,
                           const char* field,
                           std::string fallback = {}) {
  const int absolute = lua_absindex(lua, table_index);
  lua_getfield(lua, absolute, field);
  if (lua_isstring(lua, -1)) {
    std::string value = lua_tostring(lua, -1);
    lua_pop(lua, 1);
    return value;
  }
  lua_pop(lua, 1);
  return fallback;
}

bool LuaBoolField(lua_State* lua, int table_index, const char* field, bool fallback) {
  const int absolute = lua_absindex(lua, table_index);
  lua_getfield(lua, absolute, field);
  if (lua_isboolean(lua, -1)) {
    const bool value = lua_toboolean(lua, -1) != 0;
    lua_pop(lua, 1);
    return value;
  }
  lua_pop(lua, 1);
  return fallback;
}

std::unordered_map<std::string, std::string> LuaParamsTable(lua_State* lua, int table_index) {
  std::unordered_map<std::string, std::string> params;
  if (!lua_istable(lua, table_index)) {
    return params;
  }
  const int absolute = lua_absindex(lua, table_index);
  lua_pushnil(lua);
  while (lua_next(lua, absolute) != 0) {
    if (lua_isstring(lua, -2) &&
        (lua_isstring(lua, -1) || lua_isnumber(lua, -1) || lua_isboolean(lua, -1))) {
      if (lua_isstring(lua, -1) && !lua_isnumber(lua, -1)) {
        params[lua_tostring(lua, -2)] = lua_tostring(lua, -1);
      } else {
        params[lua_tostring(lua, -2)] = LuaScalarValueText(lua, -1);
      }
    }
    lua_pop(lua, 1);
  }
  return params;
}

vkpt::core::StableEntityId LuaStableIdField(lua_State* lua,
                                            int table_index,
                                            const char* field,
                                            vkpt::core::StableEntityId fallback = 0) {
  const int absolute = lua_absindex(lua, table_index);
  lua_getfield(lua, absolute, field);
  if (lua_isinteger(lua, -1) || lua_isnumber(lua, -1)) {
    const auto value = lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    return value > 0 ? static_cast<vkpt::core::StableEntityId>(value) : fallback;
  }
  lua_pop(lua, 1);
  return fallback;
}

vkpt::scene::Vec3 LuaVec3(lua_State* lua, int table_index, vkpt::scene::Vec3 fallback = {}) {
  if (!lua_istable(lua, table_index)) {
    return fallback;
  }
  return {LuaNumberField(lua, table_index, "x", 1, fallback.x),
          LuaNumberField(lua, table_index, "y", 2, fallback.y),
          LuaNumberField(lua, table_index, "z", 3, fallback.z)};
}

vkpt::scene::Quat LuaQuat(lua_State* lua, int table_index, vkpt::scene::Quat fallback = {}) {
  if (!lua_istable(lua, table_index)) {
    return fallback;
  }
  return {LuaNumberField(lua, table_index, "x", 1, fallback.x),
          LuaNumberField(lua, table_index, "y", 2, fallback.y),
          LuaNumberField(lua, table_index, "z", 3, fallback.z),
          LuaNumberField(lua, table_index, "w", 4, fallback.w)};
}

void PushVec3(lua_State* lua, const vkpt::scene::Vec3& value) {
  lua_newtable(lua);
  lua_pushnumber(lua, value.x);
  lua_setfield(lua, -2, "x");
  lua_pushnumber(lua, value.y);
  lua_setfield(lua, -2, "y");
  lua_pushnumber(lua, value.z);
  lua_setfield(lua, -2, "z");
}

void PushQuat(lua_State* lua, const vkpt::scene::Quat& value) {
  lua_newtable(lua);
  lua_pushnumber(lua, value.x);
  lua_setfield(lua, -2, "x");
  lua_pushnumber(lua, value.y);
  lua_setfield(lua, -2, "y");
  lua_pushnumber(lua, value.z);
  lua_setfield(lua, -2, "z");
  lua_pushnumber(lua, value.w);
  lua_setfield(lua, -2, "w");
}

vkpt::scene::TransformComponent LuaTransform(lua_State* lua,
                                             int table_index,
                                             vkpt::scene::TransformComponent fallback = {}) {
  if (!lua_istable(lua, table_index)) {
    return fallback;
  }
  const int absolute = lua_absindex(lua, table_index);
  auto transform = fallback;
  lua_getfield(lua, absolute, "translation");
  transform.translation = LuaVec3(lua, -1, transform.translation);
  lua_pop(lua, 1);
  lua_getfield(lua, absolute, "rotation");
  transform.rotation = LuaQuat(lua, -1, transform.rotation);
  lua_pop(lua, 1);
  lua_getfield(lua, absolute, "scale");
  transform.scale = LuaVec3(lua, -1, transform.scale);
  lua_pop(lua, 1);
  transform.dirty = true;
  return transform;
}

void PushTransform(lua_State* lua, const vkpt::scene::TransformComponent& transform) {
  lua_newtable(lua);
  PushVec3(lua, transform.translation);
  lua_setfield(lua, -2, "translation");
  PushQuat(lua, transform.rotation);
  lua_setfield(lua, -2, "rotation");
  PushVec3(lua, transform.scale);
  lua_setfield(lua, -2, "scale");
}

vkpt::scene::LightComponent LuaLight(lua_State* lua,
                                     int table_index,
                                     vkpt::scene::LightComponent fallback = {}) {
  if (!lua_istable(lua, table_index)) {
    return fallback;
  }
  const int absolute = lua_absindex(lua, table_index);
  auto light = fallback;
  light.type = LuaStringField(lua, absolute, "type", light.type);
  lua_getfield(lua, absolute, "color");
  light.color = LuaVec3(lua, -1, light.color);
  lua_pop(lua, 1);
  lua_getfield(lua, absolute, "direction");
  light.direction = LuaVec3(lua, -1, light.direction);
  lua_pop(lua, 1);
  light.intensity = LuaNumberField(lua, absolute, "intensity", 0, light.intensity);
  light.radius = LuaNumberField(lua, absolute, "radius", 0, light.radius);
  light.beam_angle_degrees = LuaNumberField(lua, absolute, "beam_angle", 0, light.beam_angle_degrees);
  light.blend = LuaNumberField(lua, absolute, "blend", 0, light.blend);
  return light;
}

void PushLight(lua_State* lua, const vkpt::scene::LightComponent& light) {
  lua_newtable(lua);
  lua_pushstring(lua, light.type.c_str());
  lua_setfield(lua, -2, "type");
  PushVec3(lua, light.color);
  lua_setfield(lua, -2, "color");
  lua_pushnumber(lua, light.intensity);
  lua_setfield(lua, -2, "intensity");
  lua_pushnumber(lua, light.radius);
  lua_setfield(lua, -2, "radius");
  PushVec3(lua, light.direction);
  lua_setfield(lua, -2, "direction");
  lua_pushnumber(lua, light.beam_angle_degrees);
  lua_setfield(lua, -2, "beam_angle");
  lua_pushnumber(lua, light.blend);
  lua_setfield(lua, -2, "blend");
}

vkpt::scene::CameraComponent LuaCamera(lua_State* lua,
                                       int table_index,
                                       vkpt::scene::CameraComponent fallback = {}) {
  if (!lua_istable(lua, table_index)) {
    return fallback;
  }
  const int absolute = lua_absindex(lua, table_index);
  auto camera = fallback;
  camera.fov = LuaNumberField(lua, absolute, "fov", 0, camera.fov);
  camera.near_plane = LuaNumberField(lua, absolute, "near_plane", 0, camera.near_plane);
  camera.far_plane = LuaNumberField(lua, absolute, "far_plane", 0, camera.far_plane);
  camera.focal_length_mm = LuaNumberField(lua, absolute, "focal_length_mm", 0, camera.focal_length_mm);
  camera.sensor_width_mm = LuaNumberField(lua, absolute, "sensor_width_mm", 0, camera.sensor_width_mm);
  camera.sensor_height_mm = LuaNumberField(lua, absolute, "sensor_height_mm", 0, camera.sensor_height_mm);
  camera.aperture_radius = LuaNumberField(lua, absolute, "aperture_radius", 0, camera.aperture_radius);
  camera.focus_distance = LuaNumberField(lua, absolute, "focus_distance", 0, camera.focus_distance);
  camera.f_stop = LuaNumberField(lua, absolute, "f_stop", 0, camera.f_stop);
  camera.shutter_seconds = LuaNumberField(lua, absolute, "shutter_seconds", 0, camera.shutter_seconds);
  camera.iso = LuaNumberField(lua, absolute, "iso", 0, camera.iso);
  camera.exposure_compensation = LuaNumberField(lua, absolute, "exposure_compensation", 0, camera.exposure_compensation);
  camera.white_balance_kelvin = LuaNumberField(lua, absolute, "white_balance_kelvin", 0, camera.white_balance_kelvin);
  camera.iris_rotation_degrees = LuaNumberField(lua, absolute, "iris_rotation_degrees", 0, camera.iris_rotation_degrees);
  camera.iris_roundness = LuaNumberField(lua, absolute, "iris_roundness", 0, camera.iris_roundness);
  camera.anamorphic_squeeze = LuaNumberField(lua, absolute, "anamorphic_squeeze", 0, camera.anamorphic_squeeze);
  camera.iris_blade_count = static_cast<std::uint32_t>(
      std::max(0.0f, LuaNumberField(lua, absolute, "iris_blade_count", 0, static_cast<float>(camera.iris_blade_count))));
  return camera;
}

void PushCamera(lua_State* lua, const vkpt::scene::CameraComponent& camera) {
  lua_newtable(lua);
  lua_pushnumber(lua, camera.fov);
  lua_setfield(lua, -2, "fov");
  lua_pushnumber(lua, camera.near_plane);
  lua_setfield(lua, -2, "near_plane");
  lua_pushnumber(lua, camera.far_plane);
  lua_setfield(lua, -2, "far_plane");
  lua_pushnumber(lua, camera.focus_distance);
  lua_setfield(lua, -2, "focus_distance");
  lua_pushnumber(lua, camera.f_stop);
  lua_setfield(lua, -2, "f_stop");
  lua_pushnumber(lua, camera.exposure_compensation);
  lua_setfield(lua, -2, "exposure_compensation");
}

void PushPhysicsBody(lua_State* lua, const vkpt::scene::PhysicsBodyComponent& body) {
  lua_newtable(lua);
  lua_pushboolean(lua, body.enabled ? 1 : 0);
  lua_setfield(lua, -2, "enabled");
  lua_pushnumber(lua, body.mass);
  lua_setfield(lua, -2, "mass");
  lua_pushboolean(lua, body.dynamic ? 1 : 0);
  lua_setfield(lua, -2, "dynamic");
  lua_pushstring(lua, body.body_type.c_str());
  lua_setfield(lua, -2, "body_type");
  lua_pushstring(lua, body.shape.c_str());
  lua_setfield(lua, -2, "shape");
  lua_pushnumber(lua, body.friction);
  lua_setfield(lua, -2, "friction");
  lua_pushnumber(lua, body.restitution);
  lua_setfield(lua, -2, "restitution");
  lua_pushnumber(lua, body.gravity_scale);
  lua_setfield(lua, -2, "gravity_scale");
  lua_pushboolean(lua, body.trigger ? 1 : 0);
  lua_setfield(lua, -2, "trigger");
  lua_pushboolean(lua, body.allow_sleeping ? 1 : 0);
  lua_setfield(lua, -2, "allow_sleeping");
  lua_pushboolean(lua, body.continuous_collision ? 1 : 0);
  lua_setfield(lua, -2, "continuous_collision");
}

std::vector<std::string> LuaStringList(lua_State* lua, int table_index) {
  std::vector<std::string> out;
  if (!lua_istable(lua, table_index)) {
    return out;
  }
  const int absolute = lua_absindex(lua, table_index);
  const auto len = lua_rawlen(lua, absolute);
  out.reserve(static_cast<std::size_t>(len));
  for (lua_Integer i = 1; i <= static_cast<lua_Integer>(len); ++i) {
    lua_geti(lua, absolute, i);
    if (lua_isstring(lua, -1)) {
      out.emplace_back(lua_tostring(lua, -1));
    }
    lua_pop(lua, 1);
  }
  return out;
}

vkpt::scene::UiPanelComponent LuaUiPanel(lua_State* lua,
                                         int table_index,
                                         vkpt::scene::UiPanelComponent fallback = {}) {
  if (!lua_istable(lua, table_index)) {
    return fallback;
  }
  const int absolute = lua_absindex(lua, table_index);
  auto panel = fallback;
  panel.panel_id = LuaStringField(lua, absolute, "id", panel.panel_id);
  panel.panel_id = LuaStringField(lua, absolute, "panel_id", panel.panel_id);
  panel.title = LuaStringField(lua, absolute, "title", panel.title);
  panel.anchor = LuaStringField(lua, absolute, "anchor", panel.anchor);
  panel.enabled = LuaBoolField(lua, absolute, "enabled", panel.enabled);
  panel.visible = LuaBoolField(lua, absolute, "visible", panel.visible);
  panel.x = LuaNumberField(lua, absolute, "x", 0, panel.x);
  panel.y = LuaNumberField(lua, absolute, "y", 0, panel.y);
  panel.width = LuaNumberField(lua, absolute, "width", 0, panel.width);
  panel.height = LuaNumberField(lua, absolute, "height", 0, panel.height);
  panel.opacity = LuaNumberField(lua, absolute, "opacity", 0, panel.opacity);
  panel.font_size = LuaNumberField(lua, absolute, "font_size", 0, panel.font_size);
  lua_getfield(lua, absolute, "background");
  panel.background = LuaVec3(lua, -1, panel.background);
  lua_pop(lua, 1);
  lua_getfield(lua, absolute, "foreground");
  panel.foreground = LuaVec3(lua, -1, panel.foreground);
  lua_pop(lua, 1);
  lua_getfield(lua, absolute, "accent");
  panel.accent = LuaVec3(lua, -1, panel.accent);
  lua_pop(lua, 1);
  lua_getfield(lua, absolute, "lines");
  if (lua_istable(lua, -1)) {
    panel.lines = LuaStringList(lua, -1);
  }
  lua_pop(lua, 1);
  return panel;
}

void PushStringList(lua_State* lua, const std::vector<std::string>& lines) {
  lua_newtable(lua);
  lua_Integer index = 1;
  for (const auto& line : lines) {
    lua_pushstring(lua, line.c_str());
    lua_seti(lua, -2, index++);
  }
}

void PushScriptParams(lua_State* lua,
                      const ScriptBinding& binding,
                      const std::unordered_map<std::string, ScriptVariableOverride>& overrides) {
  lua_newtable(lua);
  for (const auto& param : binding.editor_params) {
    if (!param.default_value.empty()) {
      lua_pushstring(lua, param.default_value.c_str());
      lua_setfield(lua, -2, param.name.c_str());
    }
  }
  for (const auto& [key, value] : binding.params) {
    lua_pushstring(lua, value.c_str());
    lua_setfield(lua, -2, key.c_str());
  }
  for (const auto& [_, overrideValue] : overrides) {
    (void)_;
    if (overrideValue.entity != binding.entity || overrideValue.name.empty()) {
      continue;
    }
    const std::string key = LowerSnakeCopy(overrideValue.name);
    if (key.empty()) {
      continue;
    }
    lua_pushstring(lua, overrideValue.value.c_str());
    lua_setfield(lua, -2, key.c_str());
  }
}

void PushUiPanel(lua_State* lua, const vkpt::scene::UiPanelComponent& panel) {
  lua_newtable(lua);
  lua_pushstring(lua, panel.panel_id.c_str());
  lua_setfield(lua, -2, "id");
  lua_pushstring(lua, panel.panel_id.c_str());
  lua_setfield(lua, -2, "panel_id");
  lua_pushstring(lua, panel.title.c_str());
  lua_setfield(lua, -2, "title");
  lua_pushstring(lua, panel.anchor.c_str());
  lua_setfield(lua, -2, "anchor");
  lua_pushboolean(lua, panel.enabled ? 1 : 0);
  lua_setfield(lua, -2, "enabled");
  lua_pushboolean(lua, panel.visible ? 1 : 0);
  lua_setfield(lua, -2, "visible");
  lua_pushnumber(lua, panel.x);
  lua_setfield(lua, -2, "x");
  lua_pushnumber(lua, panel.y);
  lua_setfield(lua, -2, "y");
  lua_pushnumber(lua, panel.width);
  lua_setfield(lua, -2, "width");
  lua_pushnumber(lua, panel.height);
  lua_setfield(lua, -2, "height");
  lua_pushnumber(lua, panel.opacity);
  lua_setfield(lua, -2, "opacity");
  lua_pushnumber(lua, panel.font_size);
  lua_setfield(lua, -2, "font_size");
  PushVec3(lua, panel.background);
  lua_setfield(lua, -2, "background");
  PushVec3(lua, panel.foreground);
  lua_setfield(lua, -2, "foreground");
  PushVec3(lua, panel.accent);
  lua_setfield(lua, -2, "accent");
  PushStringList(lua, panel.lines);
  lua_setfield(lua, -2, "lines");
}

vkpt::core::StableEntityId LuaSelfEntity(lua_State* lua, int self_index) {
  if (!lua_istable(lua, self_index)) {
    return 0;
  }
  const int absolute = lua_absindex(lua, self_index);
  lua_getfield(lua, absolute, "_entity_id");
  const auto id = lua_isinteger(lua, -1) || lua_isnumber(lua, -1)
      ? static_cast<vkpt::core::StableEntityId>(lua_tointeger(lua, -1))
      : 0u;
  lua_pop(lua, 1);
  return id;
}

vkpt::scene::TransformComponent EntityTransformOrDefault(const vkpt::scene::SceneWorld* world,
                                                         vkpt::core::StableEntityId entity_id) {
  if (world != nullptr) {
    if (const auto* entity = world->get_entity(entity_id); entity != nullptr && entity->transform.has_value()) {
      return *entity->transform;
    }
  }
  return {};
}

int LuaEntityId(lua_State* lua) {
  lua_pushinteger(lua, static_cast<lua_Integer>(LuaSelfEntity(lua, 1)));
  return 1;
}

int LuaEntityGetName(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  lua_pushstring(lua, entity->identity.name.c_str());
  return 1;
}

int LuaEntityGetTransform(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr || !entity->transform.has_value()) {
    lua_pushnil(lua);
    return 1;
  }
  PushTransform(lua, *entity->transform);
  return 1;
}

int LuaEntitySetTransform(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0 || !lua_istable(lua, 2)) {
    return 0;
  }
  const auto transform = LuaTransform(lua, 2, EntityTransformOrDefault(host->world, entity_id));
  host->commands->add_set_transform(entity_id,
                                    transform,
                                    vkpt::scene::TransformAuthority::ScriptControlled,
                                    host->binding == nullptr ? "lua" : host->binding->source,
                                    host->context == nullptr ? 0 : host->context->frame);
  return 0;
}

int LuaEntitySetName(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || host->world == nullptr || entity_id == 0 || !lua_isstring(lua, 2)) {
    return 0;
  }
  vkpt::scene::IdentityComponent identity;
  if (const auto* entity = host->world->get_entity(entity_id)) {
    identity = entity->identity;
  } else {
    identity.stable_id = entity_id;
  }
  identity.name = lua_tostring(lua, 2);
  host->commands->add_set_component(entity_id, vkpt::scene::ComponentKind::Identity, identity);
  return 0;
}

int LuaEntityLog(lua_State* lua) {
  auto* host = Host(lua);
  if (host != nullptr && lua_isstring(lua, 2)) {
    AddLuaDiagnostic(*host, ScriptDiagnosticSeverity::Info, lua_tostring(lua, 2));
  }
  return 0;
}

int LuaEntitySetDebugValue(lua_State* lua) {
  auto* host = Host(lua);
  if (host != nullptr && lua_isstring(lua, 2)) {
    AddLuaDiagnostic(*host, ScriptDiagnosticSeverity::Info,
                     std::string("debug ") + lua_tostring(lua, 2));
  }
  return 0;
}

int LuaEntityGetLight(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr || !entity->light.has_value()) {
    lua_pushnil(lua);
    return 1;
  }
  PushLight(lua, *entity->light);
  return 1;
}

int LuaEntitySetLight(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0 || !lua_istable(lua, 2)) {
    return 0;
  }
  vkpt::scene::LightComponent fallback;
  if (host->world != nullptr) {
    if (const auto* entity = host->world->get_entity(entity_id); entity != nullptr && entity->light.has_value()) {
      fallback = *entity->light;
    }
  }
  host->commands->add_assign_light(entity_id, LuaLight(lua, 2, fallback));
  return 0;
}

int LuaEntityGetCamera(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr || !entity->camera.has_value()) {
    lua_pushnil(lua);
    return 1;
  }
  PushCamera(lua, *entity->camera);
  return 1;
}

int LuaEntitySetCamera(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0 || !lua_istable(lua, 2)) {
    return 0;
  }
  vkpt::scene::CameraComponent fallback;
  if (host->world != nullptr) {
    if (const auto* entity = host->world->get_entity(entity_id); entity != nullptr && entity->camera.has_value()) {
      fallback = *entity->camera;
    }
  }
  host->commands->add_assign_camera(entity_id, LuaCamera(lua, 2, fallback));
  return 0;
}

int LuaEntityGetPhysics(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr || !entity->physics_body.has_value()) {
    lua_pushnil(lua);
    return 1;
  }
  PushPhysicsBody(lua, *entity->physics_body);
  return 1;
}

int LuaEntityGetMaterialId(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushinteger(lua, 0);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr) {
    lua_pushinteger(lua, 0);
    return 1;
  }
  if (entity->material_override.has_value() && entity->material_override->material_id != 0u) {
    lua_pushinteger(lua, static_cast<lua_Integer>(entity->material_override->material_id));
    return 1;
  }
  if (entity->mesh_renderer.has_value() && entity->mesh_renderer->material_id != 0u) {
    lua_pushinteger(lua, static_cast<lua_Integer>(entity->mesh_renderer->material_id));
    return 1;
  }
  lua_pushinteger(lua, 0);
  return 1;
}

int LuaEntityGetUiPanel(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  const auto* entity = host->world->get_entity(entity_id);
  if (entity == nullptr || !entity->ui_panel.has_value()) {
    lua_pushnil(lua);
    return 1;
  }
  PushUiPanel(lua, *entity->ui_panel);
  return 1;
}

int LuaEntitySetUiPanel(lua_State* lua) {
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0 || !lua_istable(lua, 2)) {
    return 0;
  }
  vkpt::scene::UiPanelComponent fallback;
  if (host->world != nullptr) {
    if (const auto* entity = host->world->get_entity(entity_id); entity != nullptr && entity->ui_panel.has_value()) {
      fallback = *entity->ui_panel;
    }
  }
  host->commands->add_set_component(entity_id, vkpt::scene::ComponentKind::UiPanel, LuaUiPanel(lua, 2, fallback));
  return 0;
}

void PushHostClosure(lua_State* lua, LuaHostContext& host, lua_CFunction function) {
  lua_pushlightuserdata(lua, &host);
  lua_pushcclosure(lua, function, 1);
}

void PushEntityObject(lua_State* lua, LuaHostContext& host, vkpt::core::StableEntityId entity_id) {
  // Entity handles are lightweight tables closed over the host context for this hook dispatch only.
  lua_newtable(lua);
  lua_pushinteger(lua, static_cast<lua_Integer>(entity_id));
  lua_setfield(lua, -2, "_entity_id");
  PushHostClosure(lua, host, LuaEntityId);
  lua_setfield(lua, -2, "id");
  PushHostClosure(lua, host, LuaEntityGetName);
  lua_setfield(lua, -2, "get_name");
  PushHostClosure(lua, host, LuaEntityGetTransform);
  lua_setfield(lua, -2, "get_transform");
  PushHostClosure(lua, host, LuaEntitySetTransform);
  lua_setfield(lua, -2, "set_transform");
  PushHostClosure(lua, host, LuaEntitySetName);
  lua_setfield(lua, -2, "set_name");
  PushHostClosure(lua, host, LuaEntityLog);
  lua_setfield(lua, -2, "log");
  PushHostClosure(lua, host, LuaEntitySetDebugValue);
  lua_setfield(lua, -2, "set_debug_value");
  PushHostClosure(lua, host, LuaEntityGetLight);
  lua_setfield(lua, -2, "get_light");
  PushHostClosure(lua, host, LuaEntitySetLight);
  lua_setfield(lua, -2, "set_light");
  PushHostClosure(lua, host, LuaEntityGetCamera);
  lua_setfield(lua, -2, "get_camera");
  PushHostClosure(lua, host, LuaEntitySetCamera);
  lua_setfield(lua, -2, "set_camera");
  PushHostClosure(lua, host, LuaEntityGetPhysics);
  lua_setfield(lua, -2, "get_physics");
  PushHostClosure(lua, host, LuaEntityGetMaterialId);
  lua_setfield(lua, -2, "get_material_id");
  PushHostClosure(lua, host, LuaEntityGetUiPanel);
  lua_setfield(lua, -2, "get_ui_panel");
  PushHostClosure(lua, host, LuaEntitySetUiPanel);
  lua_setfield(lua, -2, "set_ui_panel");
}

vkpt::core::StableEntityId AllocateScriptEntityId(LuaHostContext& host) {
  while ((host.world != nullptr && host.world->entity_exists(host.next_entity_id)) ||
         host.reserved_entity_ids.contains(host.next_entity_id) ||
         host.next_entity_id == 0) {
    ++host.next_entity_id;
  }
  const auto id = host.next_entity_id++;
  host.reserved_entity_ids.insert(id);
  return id;
}

int LuaWorldFindEntity(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  if (lua_isinteger(lua, 2) || lua_isnumber(lua, 2)) {
    const auto entity_id = static_cast<vkpt::core::StableEntityId>(lua_tointeger(lua, 2));
    if (host->world->entity_exists(entity_id)) {
      PushEntityObject(lua, *host, entity_id);
      return 1;
    }
    lua_pushnil(lua);
    return 1;
  }
  if (lua_isstring(lua, 2)) {
    const std::string name = lua_tostring(lua, 2);
    for (const auto entity_id : host->world->all_entities()) {
      const auto* entity = host->world->get_entity(entity_id);
      if (entity != nullptr && entity->identity.name == name) {
        PushEntityObject(lua, *host, entity_id);
        return 1;
      }
    }
  }
  lua_pushnil(lua);
  return 1;
}

int LuaWorldChildrenOf(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->world == nullptr) {
    lua_newtable(lua);
    return 1;
  }
  vkpt::core::StableEntityId parent = 0;
  if (lua_isinteger(lua, 2) || lua_isnumber(lua, 2)) {
    parent = static_cast<vkpt::core::StableEntityId>(lua_tointeger(lua, 2));
  } else if (lua_istable(lua, 2)) {
    parent = LuaSelfEntity(lua, 2);
  }
  const auto children = host->world->children_of(parent);
  lua_newtable(lua);
  lua_Integer index = 1;
  for (const auto child : children) {
    PushEntityObject(lua, *host, child);
    lua_seti(lua, -2, index++);
  }
  return 1;
}

int LuaWorldDestroyEntity(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr) {
    return 0;
  }
  vkpt::core::StableEntityId entity_id = 0;
  if (lua_istable(lua, 2)) {
    entity_id = LuaSelfEntity(lua, 2);
  } else if (lua_isinteger(lua, 2) || lua_isnumber(lua, 2)) {
    entity_id = static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 2)));
  } else if (lua_isstring(lua, 2) && host->world != nullptr) {
    const std::string_view name{lua_tostring(lua, 2)};
    for (const auto candidate : host->world->all_entities()) {
      const auto* entity = host->world->get_entity(candidate);
      if (entity != nullptr && entity->identity.name == name) {
        entity_id = candidate;
        break;
      }
    }
  }
  if (entity_id != 0) {
    host->commands->add_destroy_entity(entity_id);
  }
  return 0;
}

int LuaWorldHasComponent(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->world == nullptr) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  vkpt::core::StableEntityId entity_id = 0;
  if (lua_istable(lua, 2)) {
    entity_id = LuaSelfEntity(lua, 2);
  } else if (lua_isinteger(lua, 2) || lua_isnumber(lua, 2)) {
    entity_id = static_cast<vkpt::core::StableEntityId>(lua_tointeger(lua, 2));
  }
  const auto* entity = host->world->get_entity(entity_id);
  bool has = false;
  if (entity != nullptr) {
    std::string name;
    if (lua_isstring(lua, 3)) {
      name = lua_tostring(lua, 3);
    } else if (lua_istable(lua, 2)) {
      name = LuaStringField(lua, 2, "component");
    }
    has = (name == "transform" && entity->transform.has_value()) ||
          (name == "camera" && entity->camera.has_value()) ||
          (name == "light" && entity->light.has_value()) ||
          (name == "physics" && entity->physics_body.has_value()) ||
          (name == "script" && entity->script.has_value()) ||
          (name == "ui_panel" && entity->ui_panel.has_value());
  }
  lua_pushboolean(lua, has ? 1 : 0);
  return 1;
}

int LuaWorldReparentEntity(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr) {
    return 0;
  }
  const auto child = lua_istable(lua, 2)
                         ? LuaSelfEntity(lua, 2)
                         : static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 2)));
  const auto parent = lua_istable(lua, 3)
                          ? LuaSelfEntity(lua, 3)
                          : static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 3)));
  const bool preserve = lua_isnoneornil(lua, 4) ? true : lua_toboolean(lua, 4) != 0;
  if (child != 0) {
    host->commands->add_reparent_entity(child, parent, preserve);
  }
  return 0;
}

int LuaWorldReorderEntity(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr) {
    return 0;
  }
  const auto moved = static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 2)));
  const auto before = static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 3)));
  const auto after = static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 4)));
  if (moved != 0) {
    host->commands->add_reorder_sibling(moved, before, after);
  }
  return 0;
}

int LuaWorldRemoveComponent(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr || !lua_isstring(lua, 3)) {
    return 0;
  }
  const auto entity_id = lua_istable(lua, 2)
                             ? LuaSelfEntity(lua, 2)
                             : static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 2)));
  const std::string component = lua_tostring(lua, 3);
  if (entity_id == 0) {
    return 0;
  }
  if (component == "script") {
    host->commands->add_remove_component(entity_id, vkpt::scene::ComponentKind::Script);
  } else if (component == "ui_panel") {
    host->commands->add_remove_component(entity_id, vkpt::scene::ComponentKind::UiPanel);
  } else if (component == "light") {
    host->commands->add_remove_component(entity_id, vkpt::scene::ComponentKind::Light);
  } else if (component == "camera") {
    host->commands->add_remove_component(entity_id, vkpt::scene::ComponentKind::Camera);
  }
  return 0;
}

int LuaWorldAssignMaterial(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr) {
    return 0;
  }
  const auto entity_id = lua_istable(lua, 2)
                             ? LuaSelfEntity(lua, 2)
                             : static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 2)));
  const auto material_id = static_cast<vkpt::core::StableEntityId>(std::max<lua_Integer>(0, lua_tointeger(lua, 3)));
  if (entity_id != 0 && material_id != 0) {
    host->commands->add_assign_material(entity_id, material_id);
  }
  return 0;
}

int LuaWorldSpawnEntity(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr || !lua_istable(lua, 2)) {
    lua_pushnil(lua);
    return 1;
  }
  const int def = lua_absindex(lua, 2);
  auto entity_id = LuaStableIdField(lua, def, "id");
  if (entity_id == 0) {
    entity_id = AllocateScriptEntityId(*host);
  } else {
    // Reserve explicit IDs immediately so multiple spawns in one hook cannot collide.
    host->reserved_entity_ids.insert(entity_id);
  }
  const auto name = LuaStringField(lua, def, "name", "Script Entity");
  const auto parent = LuaStableIdField(lua, def, "parent");
  host->commands->add_create_entity(name, entity_id, parent);

  lua_getfield(lua, def, "transform");
  if (lua_istable(lua, -1)) {
    host->commands->add_set_transform(entity_id,
                                      LuaTransform(lua, -1),
                                      vkpt::scene::TransformAuthority::ScriptControlled,
                                      host->binding == nullptr ? "lua" : host->binding->source,
                                      host->context == nullptr ? 0 : host->context->frame);
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "mesh");
  if (lua_istable(lua, -1)) {
    vkpt::scene::MeshRendererComponent mesh;
    mesh.mesh_id = LuaStableIdField(lua, -1, "mesh_id");
    mesh.material_id = LuaStableIdField(lua, -1, "material_id");
    host->commands->add_set_component(entity_id, vkpt::scene::ComponentKind::MeshRenderer, mesh);
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "sdf_primitive");
  if (lua_istable(lua, -1)) {
    vkpt::scene::SdfPrimitiveComponent sdf;
    sdf.shape = LuaStringField(lua, -1, "shape", sdf.shape);
    sdf.radius = LuaNumberField(lua, -1, "radius", 0, sdf.radius);
    sdf.param_a = LuaNumberField(lua, -1, "param_a", 0, sdf.param_a);
    sdf.param_b = LuaNumberField(lua, -1, "param_b", 0, sdf.param_b);
    host->commands->add_set_component(entity_id, vkpt::scene::ComponentKind::SdfPrimitive, sdf);
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "material");
  if (lua_istable(lua, -1)) {
    vkpt::scene::MaterialOverrideComponent material;
    material.material_id = LuaStableIdField(lua, -1, "id");
    if (material.material_id != 0) {
      host->commands->add_set_component(entity_id, vkpt::scene::ComponentKind::MaterialOverride, material);
    }
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "script");
  if (lua_istable(lua, -1)) {
    vkpt::scene::ScriptComponent script;
    script.script = LuaStringField(lua, -1, "source", LuaStringField(lua, -1, "path"));
    script.language = LuaStringField(lua, -1, "language", "lua");
    script.entry = LuaStringField(lua, -1, "entry", "default");
    script.enabled = LuaBoolField(lua, -1, "enabled", true);
    script.reload_on_save = LuaBoolField(lua, -1, "reload_on_save", true);
    if (!script.script.empty()) {
      host->commands->add_set_component(entity_id, vkpt::scene::ComponentKind::Script, script);
    }
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "ui_panel");
  if (lua_istable(lua, -1)) {
    host->commands->add_set_component(entity_id,
                                      vkpt::scene::ComponentKind::UiPanel,
                                      LuaUiPanel(lua, -1));
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "light");
  if (lua_istable(lua, -1)) {
    host->commands->add_assign_light(entity_id, LuaLight(lua, -1));
  }
  lua_pop(lua, 1);

  lua_getfield(lua, def, "camera");
  if (lua_istable(lua, -1)) {
    host->commands->add_assign_camera(entity_id, LuaCamera(lua, -1));
  }
  lua_pop(lua, 1);

  PushEntityObject(lua, *host, entity_id);
  return 1;
}

vkpt::core::StableEntityId LuaEntityArgument(lua_State* lua, LuaHostContext& host, int index) {
  if (lua_istable(lua, index)) {
    return LuaSelfEntity(lua, index);
  }
  if (lua_isinteger(lua, index) || lua_isnumber(lua, index)) {
    return static_cast<vkpt::core::StableEntityId>(
        std::max<lua_Integer>(0, lua_tointeger(lua, index)));
  }
  if (lua_isstring(lua, index) && host.world != nullptr) {
    const std::string name = lua_tostring(lua, index);
    for (const auto entity_id : host.world->all_entities()) {
      const auto* entity = host.world->get_entity(entity_id);
      if (entity != nullptr && entity->identity.name == name) {
        return entity_id;
      }
    }
  }
  return 0;
}

bool ComponentNameMatches(const vkpt::scene::SceneWorld::EntityRecord& entity,
                          std::string_view component) {
  return (component == "transform" && entity.transform.has_value()) ||
         (component == "camera" && entity.camera.has_value()) ||
         (component == "light" && entity.light.has_value()) ||
         (component == "mesh" && entity.mesh_renderer.has_value()) ||
         (component == "mesh_renderer" && entity.mesh_renderer.has_value()) ||
         (component == "sdf" && entity.sdf_primitive.has_value()) ||
         (component == "sdf_primitive" && entity.sdf_primitive.has_value()) ||
         (component == "physics" && entity.physics_body.has_value()) ||
         (component == "physics_body" && entity.physics_body.has_value()) ||
         (component == "script" && entity.script.has_value()) ||
         (component == "audio_listener" && entity.audio_listener.has_value()) ||
         (component == "audio_emitter" && entity.audio_emitter.has_value()) ||
         (component == "ui_panel" && entity.ui_panel.has_value()) ||
         (component == "benchmark_tag" && entity.benchmark_tag.has_value());
}

std::string BuiltinSystemSource(std::string_view module_name) {
  if (module_name == "systems.generic_fps_camera" ||
      module_name == "generic_fps_camera") {
    return "assets/scripts/systems/generic_fps_camera.lua";
  }
  return {};
}

void PushSystemDescriptor(lua_State* lua,
                          std::string_view module_name,
                          std::string_view source) {
  lua_newtable(lua);
  lua_pushlstring(lua, module_name.data(), module_name.size());
  lua_setfield(lua, -2, "module");
  lua_pushlstring(lua, source.data(), source.size());
  lua_setfield(lua, -2, "source");
}

int LuaInclude(lua_State* lua) {
  auto* host = Host(lua);
  const int source_index = lua_isstring(lua, 2) ? 2 : (lua_isstring(lua, 1) ? 1 : 0);
  if (host == nullptr || source_index == 0) {
    return luaL_error(lua, "include requires a script source path");
  }
  const std::string source = lua_tostring(lua, source_index);
  const auto path = ResolveScriptPath(source);
  const auto script_text = ReadTextFile(path);
  if (!script_text) {
    return luaL_error(lua, "included script source could not be read: %s",
                      path.generic_string().c_str());
  }
  const auto chunk_name = "@" + path.generic_string();
  if (luaL_loadbufferx(lua,
                       script_text->data(),
                       script_text->size(),
                       chunk_name.c_str(),
                       "t") != LUA_OK) {
    return lua_error(lua);
  }
  if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    return lua_error(lua);
  }
  return 1;
}

int LuaSceneMainCamera(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->world == nullptr) {
    lua_pushnil(lua);
    return 1;
  }
  for (const auto entity_id : host->world->all_entities()) {
    const auto* entity = host->world->get_entity(entity_id);
    if (entity != nullptr && entity->camera.has_value()) {
      PushEntityObject(lua, *host, entity_id);
      return 1;
    }
  }
  lua_pushnil(lua);
  return 1;
}

int LuaSceneFindEntity(lua_State* lua) {
  return LuaWorldFindEntity(lua);
}

int LuaSceneEntitiesWithComponent(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->world == nullptr || !lua_isstring(lua, 2)) {
    lua_newtable(lua);
    return 1;
  }
  const std::string component = lua_tostring(lua, 2);
  lua_newtable(lua);
  lua_Integer index = 1;
  for (const auto entity_id : host->world->all_entities()) {
    const auto* entity = host->world->get_entity(entity_id);
    if (entity != nullptr && ComponentNameMatches(*entity, component)) {
      PushEntityObject(lua, *host, entity_id);
      lua_seti(lua, -2, index++);
    }
  }
  return 1;
}

int LuaSceneEnsureScript(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr || host->world == nullptr) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const auto entity_id = LuaEntityArgument(lua, *host, 2);
  if (entity_id == 0 || !lua_isstring(lua, 3)) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const std::string source = lua_tostring(lua, 3);
  if (source.empty()) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const std::string key = std::to_string(entity_id) + "|" + source;
  if (host->ensured_script_keys.contains(key)) {
    lua_pushboolean(lua, 1);
    return 1;
  }
  if (const auto* entity = host->world->get_entity(entity_id);
      entity != nullptr && entity->script.has_value() &&
      entity->script->script == source) {
    lua_pushboolean(lua, 1);
    return 1;
  }

  vkpt::scene::ScriptComponent script;
  script.script = source;
  script.language = "lua";
  script.entry = "default";
  script.module_id = source;
  script.enabled = true;
  script.reload_on_save = true;
  if (lua_istable(lua, 4)) {
    script.params = LuaParamsTable(lua, 4);
  }
  script.params.emplace("runtime_attached_by",
                        host->binding == nullptr ? "scene" : host->binding->source);
  host->commands->add_set_component(entity_id,
                                    vkpt::scene::ComponentKind::Script,
                                    script);
  host->ensured_script_keys.insert(key);
  lua_pushboolean(lua, 1);
  return 1;
}

int LuaSceneUseSystem(lua_State* lua) {
  auto* host = Host(lua);
  const int module_index = lua_isstring(lua, 2) ? 2 : (lua_isstring(lua, 1) ? 1 : 0);
  if (host == nullptr || module_index == 0) {
    lua_pushnil(lua);
    return 1;
  }
  const std::string module_name = lua_tostring(lua, module_index);
  const auto source = BuiltinSystemSource(module_name);
  if (source.empty()) {
    AddLuaDiagnostic(*host,
                     ScriptDiagnosticSeverity::Warning,
                     "script system is not registered: " + module_name);
    lua_pushnil(lua);
    return 1;
  }
  PushSystemDescriptor(lua, module_name, source);
  return 1;
}

int LuaSceneRegisterInteractable(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const auto entity_id = LuaEntityArgument(lua, *host, 2);
  if (entity_id == 0) {
    AddLuaDiagnostic(*host,
                     ScriptDiagnosticSeverity::Warning,
                     "scene:register_interactable skipped an invalid entity");
    lua_pushboolean(lua, 0);
    return 1;
  }
  AddLuaDiagnostic(*host,
                   ScriptDiagnosticSeverity::Info,
                   "registered interactable entity " + std::to_string(entity_id));
  lua_pushboolean(lua, 1);
  return 1;
}

int LuaContextDiagnostic(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr) {
    return 0;
  }
  const int level_index = lua_istable(lua, 1) ? 2 : 1;
  const int message_index = level_index + 1;
  std::string level = lua_isstring(lua, level_index) ? lua_tostring(lua, level_index) : "info";
  std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Info;
  if (level == "warning" || level == "warn") {
    severity = ScriptDiagnosticSeverity::Warning;
  } else if (level == "error") {
    severity = ScriptDiagnosticSeverity::Error;
  }
  const std::string message =
      lua_isstring(lua, message_index) ? lua_tostring(lua, message_index) : "script diagnostic";
  AddLuaDiagnostic(*host, severity, message);
  return 0;
}

std::vector<int> LuaKeyCandidates(lua_State* lua, int index) {
  std::vector<int> candidates;
  if (lua_isinteger(lua, index) || lua_isnumber(lua, index)) {
    candidates.push_back(static_cast<int>(lua_tointeger(lua, index)));
    return candidates;
  }
  if (!lua_isstring(lua, index)) {
    return candidates;
  }
  std::string key = lua_tostring(lua, index);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (key.size() == 1) {
    candidates.push_back(static_cast<int>(std::toupper(static_cast<unsigned char>(key.front()))));
    candidates.push_back(static_cast<int>(key.front()));
  } else if (key == "space") {
    candidates.push_back(kScriptKeySpace);
  } else if (key == "shift" || key == "run") {
    candidates.push_back(kScriptKeyShift);
    candidates.push_back(16);
  } else if (key == "control" || key == "ctrl") {
    candidates.push_back(kScriptKeyControl);
    candidates.push_back(17);
  } else if (key == "left") {
    candidates.push_back(kScriptKeyLeft);
  } else if (key == "right") {
    candidates.push_back(kScriptKeyRight);
  } else if (key == "up") {
    candidates.push_back(kScriptKeyUp);
  } else if (key == "down") {
    candidates.push_back(kScriptKeyDown);
  }
  return candidates;
}

int LuaInputKeyDown(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr || host->context == nullptr) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const auto candidates = LuaKeyCandidates(lua, 2);
  bool down = false;
  for (const auto candidate : candidates) {
    if (std::find(host->context->input.active_keys.begin(),
                  host->context->input.active_keys.end(),
                  candidate) != host->context->input.active_keys.end()) {
      down = true;
      break;
    }
  }
  lua_pushboolean(lua, down ? 1 : 0);
  return 1;
}

int LuaInputMouseDelta(lua_State* lua) {
  auto* host = Host(lua);
  lua_newtable(lua);
  const float dx = host == nullptr || host->context == nullptr ? 0.0f : host->context->input.mouse_delta_x;
  const float dy = host == nullptr || host->context == nullptr ? 0.0f : host->context->input.mouse_delta_y;
  lua_pushnumber(lua, dx);
  lua_setfield(lua, -2, "x");
  lua_pushnumber(lua, dy);
  lua_setfield(lua, -2, "y");
  return 1;
}

void PushWorldTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  PushHostClosure(lua, host, LuaWorldFindEntity);
  lua_setfield(lua, -2, "find_entity");
  PushHostClosure(lua, host, LuaWorldFindEntity);
  lua_setfield(lua, -2, "entity");
  PushHostClosure(lua, host, LuaWorldChildrenOf);
  lua_setfield(lua, -2, "children_of");
  PushHostClosure(lua, host, LuaWorldSpawnEntity);
  lua_setfield(lua, -2, "spawn_entity");
  PushHostClosure(lua, host, LuaWorldDestroyEntity);
  lua_setfield(lua, -2, "destroy_entity");
  PushHostClosure(lua, host, LuaWorldHasComponent);
  lua_setfield(lua, -2, "has_component");
  PushHostClosure(lua, host, LuaWorldReparentEntity);
  lua_setfield(lua, -2, "reparent_entity");
  PushHostClosure(lua, host, LuaWorldReorderEntity);
  lua_setfield(lua, -2, "reorder_entity");
  PushHostClosure(lua, host, LuaWorldRemoveComponent);
  lua_setfield(lua, -2, "remove_component");
  PushHostClosure(lua, host, LuaWorldAssignMaterial);
  lua_setfield(lua, -2, "assign_material");
}

void PushSceneTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  PushHostClosure(lua, host, LuaSceneMainCamera);
  lua_setfield(lua, -2, "main_camera");
  PushHostClosure(lua, host, LuaSceneFindEntity);
  lua_setfield(lua, -2, "find_entity");
  PushHostClosure(lua, host, LuaSceneEntitiesWithComponent);
  lua_setfield(lua, -2, "entities_with_component");
  PushHostClosure(lua, host, LuaSceneEnsureScript);
  lua_setfield(lua, -2, "ensure_script");
  PushHostClosure(lua, host, LuaSceneUseSystem);
  lua_setfield(lua, -2, "use_system");
  PushHostClosure(lua, host, LuaSceneRegisterInteractable);
  lua_setfield(lua, -2, "register_interactable");
}

std::string ResolvedRuntimeMode(const ScriptExecutionContext& context) {
  if (!context.runtime.mode.empty()) {
    return context.runtime.mode;
  }
  return context.game_mode ? "play" : "edit";
}

bool ResolvedRuntimeScriptsRunning(const ScriptExecutionContext& context) {
  if (context.runtime.scripts_running) {
    return true;
  }
  const auto mode = ResolvedRuntimeMode(context);
  return context.game_mode || mode == "play" || mode == "live_edit";
}

void PushRuntimeTable(lua_State* lua, const ScriptExecutionContext& context) {
  lua_newtable(lua);
  const auto mode = ResolvedRuntimeMode(context);
  lua_pushlstring(lua, mode.data(), mode.size());
  lua_setfield(lua, -2, "mode");
  lua_pushboolean(lua, ResolvedRuntimeScriptsRunning(context) ? 1 : 0);
  lua_setfield(lua, -2, "scripts_running");
}

void PushInputTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  PushHostClosure(lua, host, LuaInputKeyDown);
  lua_setfield(lua, -2, "key_down");
  PushHostClosure(lua, host, LuaInputMouseDelta);
  lua_setfield(lua, -2, "mouse_delta");
  const auto& input = host.context == nullptr ? ScriptExecutionContext::InputState{} : host.context->input;
  lua_pushboolean(lua, input.enabled ? 1 : 0);
  lua_setfield(lua, -2, "enabled");
  lua_pushnumber(lua, input.mouse_delta_x);
  lua_setfield(lua, -2, "mouse_delta_x");
  lua_pushnumber(lua, input.mouse_delta_y);
  lua_setfield(lua, -2, "mouse_delta_y");
  lua_pushnumber(lua, input.mouse_wheel_delta);
  lua_setfield(lua, -2, "mouse_wheel_delta");
  lua_pushboolean(lua, input.viewport_focused ? 1 : 0);
  lua_setfield(lua, -2, "viewport_focused");
}

void PushEditorTable(lua_State* lua, const ScriptExecutionContext& context) {
  lua_newtable(lua);
  lua_pushboolean(lua, context.editor.canvas_enabled ? 1 : 0);
  lua_setfield(lua, -2, "canvas_enabled");
  lua_pushboolean(lua, context.editor.is_editing ? 1 : 0);
  lua_setfield(lua, -2, "is_editing");
  lua_pushinteger(lua, static_cast<lua_Integer>(context.editor.edited_entity_id));
  lua_setfield(lua, -2, "edited_entity_id");
  lua_pushlstring(lua,
                  context.editor.edited_component.data(),
                  context.editor.edited_component.size());
  lua_setfield(lua, -2, "edited_component");
}

int LuaAudioPostEvent(lua_State* lua) {
  auto* host = Host(lua);
  const int eventIndex = lua_isstring(lua, 2) ? 2 : (lua_isstring(lua, 1) ? 1 : 0);
  if (host == nullptr || eventIndex == 0) {
    lua_pushnil(lua);
    return 1;
  }

  auto* audio = vkpt::audio::GlobalAudioSystem();
  if (audio == nullptr) {
    AddLuaDiagnostic(*host, ScriptDiagnosticSeverity::Warning, "audio system is not available");
    lua_pushnil(lua);
    return 1;
  }

  vkpt::audio::AudioPostEventDesc desc;
  desc.event_name = lua_tostring(lua, eventIndex);
  desc.entity = host->binding == nullptr ? 0u : host->binding->entity;
  if (host->world != nullptr && desc.entity != 0u) {
    if (const auto* entity = host->world->get_entity(desc.entity);
        entity != nullptr && entity->transform.has_value()) {
      desc.position = entity->transform->translation;
      desc.has_position = true;
    }
  }

  const int optionsIndex = eventIndex + 1;
  if (lua_istable(lua, optionsIndex)) {
    const int options = lua_absindex(lua, optionsIndex);
    desc.entity = LuaStableIdField(lua, options, "entity", desc.entity);
    desc.volume = LuaNumberField(lua, options, "volume", 0, desc.volume);
    desc.pitch = LuaNumberField(lua, options, "pitch", 0, desc.pitch);
    desc.spatial = LuaBoolField(lua, options, "spatial", desc.spatial);
    desc.loop = LuaBoolField(lua, options, "loop", desc.loop);

    lua_getfield(lua, options, "position");
    if (lua_istable(lua, -1)) {
      desc.position = LuaVec3(lua, -1, desc.position);
      desc.has_position = true;
    }
    lua_pop(lua, 1);

    if (!desc.has_position && host->world != nullptr && desc.entity != 0u) {
      if (const auto* entity = host->world->get_entity(desc.entity);
          entity != nullptr && entity->transform.has_value()) {
        desc.position = entity->transform->translation;
        desc.has_position = true;
      }
    }
  }

  const auto handle = audio->post_event(desc);
  if (!handle) {
    lua_pushnil(lua);
    return 1;
  }
  lua_newtable(lua);
  lua_pushinteger(lua, static_cast<lua_Integer>(handle.slot));
  lua_setfield(lua, -2, "slot");
  lua_pushinteger(lua, static_cast<lua_Integer>(handle.generation));
  lua_setfield(lua, -2, "generation");
  return 1;
}

int LuaAudioStop(lua_State* lua) {
  auto* host = Host(lua);
  if (host == nullptr) {
    return 0;
  }
  auto* audio = vkpt::audio::GlobalAudioSystem();
  if (audio == nullptr) {
    AddLuaDiagnostic(*host, ScriptDiagnosticSeverity::Warning, "audio system is not available");
    return 0;
  }

  vkpt::audio::AudioVoiceHandle handle;
  const int handleIndex = lua_istable(lua, 2) ? 2 : (lua_istable(lua, 1) ? 1 : 0);
  if (handleIndex != 0) {
    const int table = lua_absindex(lua, handleIndex);
    lua_getfield(lua, table, "slot");
    handle.slot = static_cast<std::uint32_t>(std::max<lua_Integer>(0, lua_tointeger(lua, -1)));
    lua_pop(lua, 1);
    lua_getfield(lua, table, "generation");
    handle.generation = static_cast<std::uint32_t>(std::max<lua_Integer>(0, lua_tointeger(lua, -1)));
    lua_pop(lua, 1);
  }
  audio->stop(handle);
  return 0;
}

void PushAudioTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  PushHostClosure(lua, host, LuaAudioPostEvent);
  lua_setfield(lua, -2, "post_event");
  PushHostClosure(lua, host, LuaAudioStop);
  lua_setfield(lua, -2, "stop");
}

void PushContextTable(lua_State* lua,
                      LuaHostContext& host,
                      const std::unordered_map<std::string, ScriptVariableOverride>& overrides) {
  lua_newtable(lua);
  const auto& context = *host.context;
  lua_pushinteger(lua, static_cast<lua_Integer>(host.binding == nullptr ? 0u : host.binding->entity));
  lua_setfield(lua, -2, "entity_id");
  lua_pushinteger(lua, static_cast<lua_Integer>(context.frame));
  lua_setfield(lua, -2, "frame");
  lua_pushnumber(lua, context.elapsed_seconds);
  lua_setfield(lua, -2, "elapsed_seconds");
  lua_pushnumber(lua, context.delta_seconds);
  lua_setfield(lua, -2, "delta_seconds");
  lua_pushnumber(lua, context.delta_seconds);
  lua_setfield(lua, -2, "dt");
  lua_pushnumber(lua, context.fixed_delta_seconds);
  lua_setfield(lua, -2, "fixed_delta_seconds");
  lua_pushboolean(lua, context.deterministic ? 1 : 0);
  lua_setfield(lua, -2, "deterministic");
  PushRuntimeTable(lua, context);
  lua_setfield(lua, -2, "runtime");
  if (host.binding != nullptr) {
    PushScriptParams(lua, *host.binding, overrides);
  } else {
    lua_newtable(lua);
  }
  lua_setfield(lua, -2, "params");
  PushWorldTable(lua, host);
  lua_setfield(lua, -2, "world");
  PushSceneTable(lua, host);
  lua_setfield(lua, -2, "scene");
  PushInputTable(lua, host);
  lua_setfield(lua, -2, "input");
  PushEditorTable(lua, context);
  lua_setfield(lua, -2, "editor");
  PushAudioTable(lua, host);
  lua_setfield(lua, -2, "audio");
  PushHostClosure(lua, host, LuaContextDiagnostic);
  lua_setfield(lua, -2, "diagnostic");
  PushHostClosure(lua, host, LuaInclude);
  lua_setfield(lua, -2, "include");
}

vkpt::core::StableEntityId NextEntityId(const vkpt::scene::SceneWorld& world) {
  vkpt::core::StableEntityId next = 1;
  for (const auto entity_id : world.all_entities()) {
    next = std::max(next, entity_id + 1);
  }
  return next;
}

bool ExecuteLuaHook(const ScriptBinding& binding,
                    const vkpt::scene::SceneWorld& world,
                    ScriptLifecycleHook hook,
                    const ScriptExecutionContext& context,
                    vkpt::scene::WorldCommandBuffer& commands,
                    std::vector<ScriptDiagnostic>& dispatch_diagnostics,
                    std::vector<ScriptDiagnostic>& runtime_diagnostics,
                    std::vector<ScriptVariableSnapshot>& variable_snapshots,
                    ScriptBindingRuntimeState& runtime_state,
                    std::unordered_map<std::string, std::string>& bytecode_cache,
                    const std::unordered_map<std::string, ScriptVariableOverride>& variable_overrides) {
  LuaMemoryBudget memory_budget;
  memory_budget.limit = context.memory_budget_bytes;
  auto lua = lua_newstate(LuaBudgetAllocator, &memory_budget);
  if (lua == nullptr) {
    LuaHostContext host;
    host.world = &world;
    host.commands = &commands;
    host.context = &context;
    host.binding = &binding;
    host.hook = hook;
    host.dispatch_diagnostics = &dispatch_diagnostics;
    host.runtime_diagnostics = &runtime_diagnostics;
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "could not create Lua state");
    return false;
  }

  LuaHostContext host;
  host.world = &world;
  host.commands = &commands;
  host.context = &context;
  host.binding = &binding;
  host.hook = hook;
  host.dispatch_diagnostics = &dispatch_diagnostics;
  host.runtime_diagnostics = &runtime_diagnostics;
  host.next_entity_id = NextEntityId(world);
  runtime_state.entity = binding.entity;
  runtime_state.source = binding.source;
  runtime_state.last_hook = hook;
  runtime_state.last_frame = context.frame;
  runtime_state.command_count = commands.commands().size();
  runtime_state.last_error.clear();
  runtime_state.skip_reason.clear();
  const auto hook_start = std::chrono::steady_clock::now();

  OpenSafeLuaLibraries(lua);
  PushHostClosure(lua, host, LuaInclude);
  lua_setglobal(lua, "include");
  if (context.instruction_budget > 0) {
    lua_sethook(lua,
                LuaInstructionBudgetHook,
                LUA_MASKCOUNT,
                static_cast<int>(std::min<std::size_t>(context.instruction_budget, 1000000u)));
  }
  const auto script_path = ResolveScriptPath(binding.source);
  const auto chunk_name = "@" + script_path.generic_string();
  const auto cache_key = script_path.generic_string();
  bool chunk_loaded = false;
  // Cache dumped Lua bytecode by resolved source path; stale cache entries are discarded on load failure.
  if (auto cached = bytecode_cache.find(cache_key); cached != bytecode_cache.end()) {
    if (luaL_loadbufferx(lua, cached->second.data(), cached->second.size(), chunk_name.c_str(), "b") == LUA_OK) {
      chunk_loaded = true;
    } else {
      lua_pop(lua, 1);
      bytecode_cache.erase(cached);
    }
  }
  if (!chunk_loaded) {
    const auto script_text = ReadTextFile(script_path);
    if (!script_text) {
      AddLuaDiagnostic(host,
                       ScriptDiagnosticSeverity::Error,
                       "script source could not be read: " + script_path.generic_string());
      runtime_state.last_error = "script source could not be read";
      lua_close(lua);
      return false;
    }

    if (luaL_loadbufferx(lua, script_text->data(), script_text->size(), chunk_name.c_str(), "t") != LUA_OK) {
      runtime_state.last_error = "script compile failed: " + LuaErrorText(lua);
      AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
      lua_close(lua);
      return false;
    }
    std::string bytecode;
    if (lua_dump(lua, LuaBytecodeWriter, &bytecode, 0) == 0 && !bytecode.empty()) {
      bytecode_cache[cache_key] = std::move(bytecode);
    }
  }
  if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    runtime_state.last_error = "script load failed: " + LuaErrorText(lua);
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    lua_close(lua);
    return false;
  }
  if (!lua_istable(lua, -1)) {
    runtime_state.last_error = "script must return a table";
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    lua_close(lua);
    return false;
  }

  const int script_table = lua_absindex(lua, -1);
  for (const auto& [_, overrideValue] : variable_overrides) {
    (void)_;
    if (overrideValue.entity != binding.entity || overrideValue.scope != "script" ||
        overrideValue.name.empty()) {
      continue;
    }
    PushLuaOverrideValue(lua, overrideValue.value);
    lua_setfield(lua, script_table, overrideValue.name.c_str());
  }

  lua_getfield(lua, -1, std::string(to_string(hook)).c_str());
  if (lua_isnil(lua, -1)) {
    // Missing hook functions are normal and are counted as non-calls, not diagnostics.
    lua_close(lua);
    return false;
  }
  if (!lua_isfunction(lua, -1)) {
    runtime_state.last_error = "script hook is not a function";
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    lua_close(lua);
    return false;
  }
  const int hook_function = lua_absindex(lua, -1);
  for (int i = 1;; ++i) {
    const char* name = lua_getupvalue(lua, hook_function, i);
    if (name == nullptr) {
      break;
    }
    const bool hasOverride = std::string_view{name} != "_ENV" &&
        FindVariableOverride(variable_overrides, binding.entity, "upvalue", name) != nullptr;
    lua_pop(lua, 1);
    if (!hasOverride) {
      continue;
    }
    const auto* overrideValue = FindVariableOverride(variable_overrides, binding.entity, "upvalue", name);
    PushLuaOverrideValue(lua, overrideValue->value);
    (void)lua_setupvalue(lua, hook_function, i);
  }
  lua_pushvalue(lua, -1);
  const int hook_function_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  PushEntityObject(lua, host, binding.entity);
  PushContextTable(lua, host, variable_overrides);
  if (lua_pcall(lua, 2, 0, 0) != LUA_OK) {
    runtime_state.last_error = "script hook failed: " + LuaErrorText(lua);
    if (runtime_state.last_error.find("instruction budget exceeded") != std::string::npos) {
      runtime_state.disabled_until_reload = true;
    }
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    luaL_unref(lua, LUA_REGISTRYINDEX, hook_function_ref);
    runtime_state.memory_estimate_bytes = memory_budget.peak;
    lua_close(lua);
    return false;
  }
  lua_sethook(lua, nullptr, 0, 0);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, hook_function_ref);
  CaptureLuaScriptVariables(lua, -2, -1, binding, hook, context, variable_snapshots);
  lua_pop(lua, 1);
  luaL_unref(lua, LUA_REGISTRYINDEX, hook_function_ref);
  const auto hook_end = std::chrono::steady_clock::now();
  runtime_state.hook_duration_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(hook_end - hook_start).count());
  runtime_state.command_count = commands.commands().size() - runtime_state.command_count;
  runtime_state.memory_estimate_bytes = memory_budget.peak;
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Debug,
      "scripts",
      "Lua hook executed",
      {{"entity", std::to_string(binding.entity)},
       {"entity_name", binding.entity_name},
       {"hook", std::string(to_string(hook))},
       {"source", binding.source},
       {"commands_after", std::to_string(commands.commands().size())},
       {"active_keys", std::to_string(context.input.active_keys.size())}},
      context.frame);
  lua_close(lua);
  return true;
}

#endif  // PT_ENABLE_LUA

std::string BindingSignature(const std::vector<ScriptBinding>& bindings) {
  std::ostringstream out;
  for (const auto& binding : bindings) {
    out << binding.entity << "|" << binding.source << "|" << binding.language << "|" << binding.entry << "|"
        << binding.module_id << "|" << (binding.enabled ? "1" : "0") << "|"
        << (binding.reload_on_save ? "1" : "0") << ";";
    for (const auto& [key, value] : binding.params) {
      out << key << "=" << value << ";";
    }
  }
  return out.str();
}

}  // namespace

ScriptDispatchSummary EcsScriptRuntime::dispatch_hook(const vkpt::scene::SceneWorld& world,
                                                      ScriptLifecycleHook hook,
                                                      const ScriptExecutionContext& context,
                                                      vkpt::scene::WorldCommandBuffer& commands) {
  const auto current_bindings = BuildScriptBindings(world);
  if (BindingSignature(current_bindings) != BindingSignature(m_bindings)) {
    // Entity/script topology changes invalidate stable dispatch order and compiled bytecode assumptions.
    m_bindings = current_bindings;
    ApplyScriptEditorAnnotations(m_bindings);
    m_lua_bytecode_cache.clear();
    m_disabled_until_reload.clear();
  }

  ScriptDispatchSummary summary;
  summary.hook = hook;
  summary.frame = context.frame;
  summary.binding_count = m_bindings.size();
  summary.command_count_before = commands.commands().size();
  summary.lua_compiled_in = lua_compiled_in();
  summary.execution_available = execution_available();
  summary.scripts_disabled = !context.scripts_enabled;
  summary.game_mode_blocked = !ResolvedRuntimeScriptsRunning(context);
  summary.benchmark_blocked = context.benchmark_mode && !context.allow_benchmark_scripts;
  m_variable_snapshots.clear();
  m_runtime_states.clear();

  for (const auto& binding : m_bindings) {
    ScriptBindingRuntimeState runtime_state;
    runtime_state.entity = binding.entity;
    runtime_state.source = binding.source;
    runtime_state.last_hook = hook;
    runtime_state.last_frame = context.frame;
    const auto disabled_key = std::to_string(binding.entity) + ":" + binding.source;
    if (!binding.enabled || !IsSupportedLanguage(binding.language)) {
      runtime_state.skip_reason = !binding.enabled ? "disabled" : "unsupported language";
      ++summary.skipped_count;
      m_runtime_states.push_back(std::move(runtime_state));
      continue;
    }

    ++summary.runnable_count;

    if (summary.scripts_disabled) {
      runtime_state.skip_reason = "scripts disabled";
      ++summary.skipped_count;
      m_runtime_states.push_back(std::move(runtime_state));
      continue;
    }
    if (summary.game_mode_blocked) {
      runtime_state.skip_reason = "game mode required";
      ++summary.skipped_count;
      m_runtime_states.push_back(std::move(runtime_state));
      continue;
    }
    if (summary.benchmark_blocked) {
      runtime_state.skip_reason = "benchmark blocked";
      ++summary.skipped_count;
      m_runtime_states.push_back(std::move(runtime_state));
      continue;
    }
    if (m_disabled_until_reload.contains(disabled_key)) {
      runtime_state.skip_reason = "disabled until reload";
      runtime_state.disabled_until_reload = true;
      ++summary.skipped_count;
      m_runtime_states.push_back(std::move(runtime_state));
      continue;
    }
    if (!summary.execution_available) {
      runtime_state.skip_reason = "Lua execution unavailable";
      ++summary.skipped_count;
      auto diagnostic = MakeDiagnostic(ScriptDiagnosticSeverity::Warning,
                                       hook,
                                       binding,
                                       context,
                                       "script hook skipped because Lua execution is not available");
      LogDiagnostic(diagnostic);
      summary.diagnostics.push_back(diagnostic);
      m_diagnostics.push_back(std::move(diagnostic));
      m_runtime_states.push_back(std::move(runtime_state));
      continue;
    }

#ifdef PT_ENABLE_LUA
    if (ExecuteLuaHook(binding,
                       world,
                       hook,
                       context,
                       commands,
                       summary.diagnostics,
                       m_diagnostics,
                       m_variable_snapshots,
                       runtime_state,
                       m_lua_bytecode_cache,
                       m_variable_overrides)) {
      ++summary.hook_call_count;
    }
    if (runtime_state.disabled_until_reload) {
      m_disabled_until_reload.insert(disabled_key);
    }
    m_runtime_states.push_back(std::move(runtime_state));
#else
    ++summary.hook_call_count;
    m_runtime_states.push_back(std::move(runtime_state));
#endif
  }

  summary.command_count_after = commands.commands().size();
  return summary;
}

}  // namespace vkpt::scripting
