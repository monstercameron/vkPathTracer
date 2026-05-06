#include "scripting/ScriptRuntime.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>
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
};

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
}

void PushInputTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  PushHostClosure(lua, host, LuaInputKeyDown);
  lua_setfield(lua, -2, "key_down");
  PushHostClosure(lua, host, LuaInputMouseDelta);
  lua_setfield(lua, -2, "mouse_delta");
  const auto& input = host.context == nullptr ? ScriptExecutionContext::InputState{} : host.context->input;
  lua_pushnumber(lua, input.mouse_delta_x);
  lua_setfield(lua, -2, "mouse_delta_x");
  lua_pushnumber(lua, input.mouse_delta_y);
  lua_setfield(lua, -2, "mouse_delta_y");
  lua_pushnumber(lua, input.mouse_wheel_delta);
  lua_setfield(lua, -2, "mouse_wheel_delta");
  lua_pushboolean(lua, input.viewport_focused ? 1 : 0);
  lua_setfield(lua, -2, "viewport_focused");
}

void PushContextTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  const auto& context = *host.context;
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
  PushWorldTable(lua, host);
  lua_setfield(lua, -2, "world");
  PushInputTable(lua, host);
  lua_setfield(lua, -2, "input");
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
                    std::unordered_map<std::string, std::string>& bytecode_cache) {
  auto lua = luaL_newstate();
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

  OpenSafeLuaLibraries(lua);
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
      lua_close(lua);
      return false;
    }

    if (luaL_loadbufferx(lua, script_text->data(), script_text->size(), chunk_name.c_str(), "t") != LUA_OK) {
      AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "script compile failed: " + LuaErrorText(lua));
      lua_close(lua);
      return false;
    }
    std::string bytecode;
    if (lua_dump(lua, LuaBytecodeWriter, &bytecode, 0) == 0 && !bytecode.empty()) {
      bytecode_cache[cache_key] = std::move(bytecode);
    }
  }
  if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "script load failed: " + LuaErrorText(lua));
    lua_close(lua);
    return false;
  }
  if (!lua_istable(lua, -1)) {
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "script must return a table");
    lua_close(lua);
    return false;
  }

  lua_getfield(lua, -1, std::string(to_string(hook)).c_str());
  if (lua_isnil(lua, -1)) {
    // Missing hook functions are normal and are counted as non-calls, not diagnostics.
    lua_close(lua);
    return false;
  }
  if (!lua_isfunction(lua, -1)) {
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "script hook is not a function");
    lua_close(lua);
    return false;
  }
  PushEntityObject(lua, host, binding.entity);
  PushContextTable(lua, host);
  if (lua_pcall(lua, 2, 0, 0) != LUA_OK) {
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "script hook failed: " + LuaErrorText(lua));
    lua_close(lua);
    return false;
  }
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

}  // namespace

ScriptDispatchSummary EcsScriptRuntime::dispatch_hook(const vkpt::scene::SceneWorld& world,
                                                      ScriptLifecycleHook hook,
                                                      const ScriptExecutionContext& context,
                                                      vkpt::scene::WorldCommandBuffer& commands) {
  const auto current_bindings = BuildScriptBindings(world);
  if (current_bindings.size() != m_bindings.size()) {
    // Entity/script topology changes invalidate stable dispatch order and compiled bytecode assumptions.
    m_bindings = current_bindings;
    m_lua_bytecode_cache.clear();
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

#ifdef PT_ENABLE_LUA
    if (ExecuteLuaHook(binding,
                       world,
                       hook,
                       context,
                       commands,
                       summary.diagnostics,
                       m_diagnostics,
                       m_lua_bytecode_cache)) {
      ++summary.hook_call_count;
    }
#else
    ++summary.hook_call_count;
#endif
  }

  summary.command_count_after = commands.commands().size();
  return summary;
}

}  // namespace vkpt::scripting
