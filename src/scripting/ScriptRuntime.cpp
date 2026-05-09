#include "scripting/ScriptRuntime.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include "audio/AudioSystem.h"
#include "core/Logging.h"
#include "core/metrics/Metrics.h"
#include "jobs/JobSystem.h"
#include "scripting/Channels.h"
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

[[maybe_unused]] std::string TrimCopy(std::string_view text) {
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

[[maybe_unused]] std::string LowerSnakeCopy(std::string_view text) {
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

[[maybe_unused]] const ScriptVariableOverride* FindVariableOverride(
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

[[maybe_unused]] void LogScriptHookFired(const ScriptBinding& binding,
                                         ScriptLifecycleHook hook,
                                         const ScriptExecutionContext& context) {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Debug,
      "scripts",
      "script.hook_fired",
      {{"script_id", std::to_string(binding.entity)},
       {"hook", std::string(to_string(hook))},
       {"gen", std::to_string(context.frame)},
       {"source", binding.source}},
      context.frame);
}

[[maybe_unused]] void LogScriptBudgetExceeded(const ScriptBinding& binding,
                                              ScriptLifecycleHook hook,
                                              const ScriptExecutionContext& context,
                                              std::string_view budget,
                                              std::size_t actual,
                                              std::size_t limit) {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Warning,
      "scripts",
      "script.budget_exceeded",
      {{"script_id", std::to_string(binding.entity)},
       {"hook", std::string(to_string(hook))},
       {"gen", std::to_string(context.frame)},
       {"budget", std::string(budget)},
       {"actual", std::to_string(actual)},
       {"limit", std::to_string(limit)},
       {"source", binding.source}},
      context.frame);
}

[[maybe_unused]] void RecordScriptHookTiming(ScriptLifecycleHook hook, std::uint64_t hook_us) {
  auto& registry = vkpt::core::metrics::MetricsRegistry::instance();
  registry.histogram("vkp.script.hook_us").record(hook_us);
  registry.histogram("vkp.script.hook_us.hook." + std::string(to_string(hook))).record(hook_us);
}

void RecordScriptInstructionsPerFrame(std::size_t instructions) {
  vkpt::core::metrics::MetricsRegistry::instance()
      .histogram("vkp.script.instructions_per_frame")
      .record(static_cast<std::uint64_t>(instructions));
}

ScriptCmd::Type ScriptCmdTypeForSceneCommand(vkpt::scene::WorldCommandBuffer::CommandType type) {
  using CommandType = vkpt::scene::WorldCommandBuffer::CommandType;
  switch (type) {
    case CommandType::CreateEntity:
      return ScriptCmd::Type::SpawnEntity;
    case CommandType::DestroyEntity:
      return ScriptCmd::Type::DestroyEntity;
    case CommandType::SetTransform:
      return ScriptCmd::Type::SetTransform;
    case CommandType::AssignMaterial:
      return ScriptCmd::Type::SetMaterial;
    default:
      return ScriptCmd::Type::SceneCommand;
  }
}

void AppendSceneCommandPayload(const vkpt::scene::WorldCommandBuffer::Command& command,
                               vkpt::scene::WorldCommandBuffer& out) {
  std::visit([&](const auto& payload) {
    using T = std::decay_t<decltype(payload)>;
    if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::CreateEntityCommand>) {
      out.add_create_entity(payload.name, payload.requested_id, payload.requested_parent);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::DestroyEntityCommand>) {
      if (payload.destroy_children) {
        out.add_destroy_subtree(payload.id);
      } else {
        out.add_destroy_entity(payload.id);
      }
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetComponentCommand>) {
      out.add_set_component(payload.id, payload.kind, payload.component);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AddComponentCommand>) {
      out.add_add_component(payload.id, payload.kind, payload.component);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::RemoveComponentCommand>) {
      out.add_remove_component(payload.id, payload.kind);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::SetTransformCommand>) {
      out.add_set_transform(payload.id, payload.transform, payload.authority, payload.writer, payload.frame);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::ReparentEntityCommand>) {
      out.add_reparent_entity(payload.child, payload.parent, payload.preserve_world_transform);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::ReorderSiblingCommand>) {
      out.add_reorder_sibling(payload.moved, payload.sibling_before, payload.sibling_after);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignMaterialCommand>) {
      out.add_assign_material(payload.id, payload.material_id);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignLightCommand>) {
      out.add_assign_light(payload.id, payload.light);
    } else if constexpr (std::is_same_v<T, vkpt::scene::WorldCommandBuffer::AssignCameraCommand>) {
      out.add_assign_camera(payload.id, payload.camera);
    }
  }, command.payload);
}

class ScriptRingCommandQueue final {
 public:
  explicit ScriptRingCommandQueue(std::size_t capacity) : ring(capacity) {}

  ScriptCmdRing ring;
};

}  // namespace

ScriptCmd MakeScriptCmd(const vkpt::scene::WorldCommandBuffer::Command& command) {
  ScriptCmd cmd;
  cmd.type = ScriptCmdTypeForSceneCommand(command.type);
  cmd.payload = command;
  return cmd;
}

bool AppendScriptCmdToWorldCommands(const ScriptCmd& cmd, vkpt::scene::WorldCommandBuffer& commands) {
  if (const auto* scene_command = std::get_if<vkpt::scene::WorldCommandBuffer::Command>(&cmd.payload)) {
    AppendSceneCommandPayload(*scene_command, commands);
    return true;
  }
  return false;
}

struct ScriptCommandQueue::Impl {
  explicit Impl(std::size_t capacity) : queue(capacity) {}
  ScriptRingCommandQueue queue;
};

ScriptCommandQueue::ScriptCommandQueue(std::size_t capacity)
    : m_impl(std::make_unique<Impl>(capacity)) {}

ScriptCommandQueue::~ScriptCommandQueue() = default;

bool ScriptCommandQueue::push_scene_command(const vkpt::scene::WorldCommandBuffer::Command& command) {
  return m_impl->queue.ring.try_push(MakeScriptCmd(command));
}

std::size_t ScriptCommandQueue::drain_to(vkpt::scene::WorldCommandBuffer& commands) {
  std::size_t drained = 0u;
  ScriptCmd cmd;
  while (m_impl->queue.ring.try_pop(cmd)) {
    if (AppendScriptCmdToWorldCommands(cmd, commands)) {
      ++drained;
    }
  }
  return drained;
}

std::size_t ScriptCommandQueue::dropped_total() const {
  return m_impl->queue.ring.dropped_total();
}

void ScriptCommandQueue::clear() {
  ScriptCmd cmd;
  while (m_impl->queue.ring.try_pop(cmd)) {
  }
}

struct ScriptThread::Impl {
  Impl(std::size_t hook_capacity, std::size_t command_capacity)
      : hooks(hook_capacity),
        commands(command_capacity),
        runtime(CreateScriptRuntime()) {
    worker = std::thread([this]() {
      worker_loop();
    });
  }

  ~Impl() {
    stop();
  }

  void stop() {
    const bool already_stopping = stopping.exchange(true, std::memory_order_acq_rel);
    cv.notify_one();
    if (!already_stopping && worker.joinable()) {
      worker.join();
    } else if (worker.joinable()) {
      worker.join();
    }
  }

  void worker_loop() {
    while (!stopping.load(std::memory_order_acquire)) {
      ScriptHookRequest request;
      {
        std::unique_lock lock(wake_mutex);
        cv.wait_for(lock, std::chrono::milliseconds(1), [&]() {
          return stopping.load(std::memory_order_acquire) || hooks.depth() > 0u ||
                 reload_requested.load(std::memory_order_acquire);
        });
      }

      vkpt::scene::SceneWorld local_world;
      const bool should_reload = reload_requested.exchange(false, std::memory_order_acq_rel);
      {
        std::scoped_lock lock(world_mutex);
        local_world = world_snapshot;
      }
      if (should_reload) {
        runtime->reload_bindings(local_world);
      }

      while (hooks.try_pop(request)) {
        {
          std::scoped_lock lock(world_mutex);
          local_world = world_snapshot;
        }
        local_world.recompute_world_transforms();
        if (request.context.frame == 0u) {
          request.context.frame = request.frame;
        }
        vkpt::scene::WorldCommandBuffer hook_commands;
        (void)runtime->dispatch_hook(local_world, request.hook, request.context, hook_commands);
        for (const auto& command : hook_commands.commands()) {
          (void)commands.push_scene_command(command);
        }
      }
    }
  }

  ScriptHookRing hooks;
  ScriptCommandQueue commands;
  std::unique_ptr<IScriptRuntime> runtime;
  vkpt::scene::SceneWorld world_snapshot;
  std::mutex world_mutex;
  std::mutex wake_mutex;
  std::condition_variable cv;
  std::thread worker;
  std::atomic_bool stopping{false};
  std::atomic_bool reload_requested{false};
};

ScriptThread::ScriptThread(std::size_t hook_capacity, std::size_t command_capacity)
    : m_impl(std::make_unique<Impl>(hook_capacity, command_capacity)) {}

ScriptThread::~ScriptThread() = default;

void ScriptThread::publish_world_snapshot(vkpt::scene::SceneWorld world) {
  {
    std::scoped_lock lock(m_impl->world_mutex);
    m_impl->world_snapshot = std::move(world);
  }
  m_impl->reload_requested.store(true, std::memory_order_release);
  m_impl->cv.notify_one();
}

bool ScriptThread::enqueue_hook(ScriptHookRequest request) {
  if (!m_impl->hooks.try_push(std::move(request))) {
    return false;
  }
  m_impl->cv.notify_one();
  return true;
}

std::size_t ScriptThread::drain_commands(vkpt::scene::WorldCommandBuffer& commands) {
  return m_impl->commands.drain_to(commands);
}

void ScriptThread::stop() {
  m_impl->stop();
}

namespace {

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
  bool pure = false;
  struct LuaInstructionBudget* instruction_budget = nullptr;
};

struct LuaMemoryBudget {
  std::size_t used = 0;
  std::size_t peak = 0;
  std::size_t limit = 0;
  std::size_t requested = 0;
  bool exceeded = false;
};

struct LuaInstructionBudget {
  std::size_t limit = 0;
  std::size_t interval = 0;
  std::size_t executed = 0;
  bool exceeded = false;
  // Wall-clock deadline for this dispatch_hook invocation. Captured at hook
  // entry alongside the per-script instruction/memory budget state. The
  // count-hook checks std::chrono::steady_clock::now() against this deadline
  // every sample interval and raises luaL_error so the existing budget-kill
  // path disables the script via m_disabled_until_reload.
  std::chrono::steady_clock::time_point wall_clock_deadline{};
  std::chrono::milliseconds wall_clock_budget_ms{0};
  bool wall_clock_exceeded = false;
};

constexpr const char* kLuaHostContextRegistryKey = "vkpt.current_host_context";

LuaHostContext* Host(lua_State* lua);

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
    budget->requested = next;
    budget->exceeded = true;
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
  if (auto* host = Host(lua); host != nullptr && host->instruction_budget != nullptr) {
    auto& budget = *host->instruction_budget;
    budget.executed += budget.interval;
    const bool instructions_exceeded =
        budget.limit != 0 && budget.executed >= budget.limit;
    const bool wall_clock_active = budget.wall_clock_budget_ms.count() > 0;
    const bool wall_clock_exceeded =
        wall_clock_active &&
        std::chrono::steady_clock::now() >= budget.wall_clock_deadline;
    if (!instructions_exceeded && !wall_clock_exceeded) {
      return;
    }
    if (instructions_exceeded) {
      budget.exceeded = true;
      // Instruction budget takes priority over wall-clock when both trip on
      // the same sample so existing tests that exercise the infinite-loop
      // probe continue to see budget_exceeded_type == "instructions".
      luaL_error(lua, "instruction budget exceeded");
      return;
    }
    budget.wall_clock_exceeded = true;
    luaL_error(lua,
               "wall-clock budget exceeded (>%llums)",
               static_cast<unsigned long long>(budget.wall_clock_budget_ms.count()));
    return;
  }
  luaL_error(lua, "instruction budget exceeded");
}

LuaHostContext* Host(lua_State* lua) {
  lua_getfield(lua, LUA_REGISTRYINDEX, kLuaHostContextRegistryKey);
  auto* host = static_cast<LuaHostContext*>(lua_touserdata(lua, -1));
  lua_pop(lua, 1);
  return host;
}

void SetCurrentHost(lua_State* lua, LuaHostContext* host) {
  lua_pushlightuserdata(lua, host);
  lua_setfield(lua, LUA_REGISTRYINDEX, kLuaHostContextRegistryKey);
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
  luaL_requiref(lua, LUA_COLIBNAME, luaopen_coroutine, 1);
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
  lua_pushnumber(lua, camera.focal_length_mm);
  lua_setfield(lua, -2, "focal_length_mm");
  lua_pushnumber(lua, camera.sensor_width_mm);
  lua_setfield(lua, -2, "sensor_width_mm");
  lua_pushnumber(lua, camera.sensor_height_mm);
  lua_setfield(lua, -2, "sensor_height_mm");
  lua_pushnumber(lua, camera.aperture_radius);
  lua_setfield(lua, -2, "aperture_radius");
  lua_pushnumber(lua, camera.focus_distance);
  lua_setfield(lua, -2, "focus_distance");
  lua_pushnumber(lua, camera.f_stop);
  lua_setfield(lua, -2, "f_stop");
  lua_pushnumber(lua, camera.shutter_seconds);
  lua_setfield(lua, -2, "shutter_seconds");
  lua_pushnumber(lua, camera.iso);
  lua_setfield(lua, -2, "iso");
  lua_pushnumber(lua, camera.exposure_compensation);
  lua_setfield(lua, -2, "exposure_compensation");
  lua_pushnumber(lua, camera.white_balance_kelvin);
  lua_setfield(lua, -2, "white_balance_kelvin");
  lua_pushnumber(lua, camera.iris_rotation_degrees);
  lua_setfield(lua, -2, "iris_rotation_degrees");
  lua_pushnumber(lua, camera.iris_roundness);
  lua_setfield(lua, -2, "iris_roundness");
  lua_pushnumber(lua, camera.anamorphic_squeeze);
  lua_setfield(lua, -2, "anamorphic_squeeze");
  lua_pushinteger(lua, static_cast<lua_Integer>(camera.iris_blade_count));
  lua_setfield(lua, -2, "iris_blade_count");
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

// === Entity bindings ===========================================================
// Namespace: ctx.<entity>.* (methods on per-entity handle tables).
// Type errors: luaL_argerror — wrong Lua type for a user argument is a programming error.
// Missing object: returns nil (or 0 for material id) — entity destroyed or component absent.
// Returns: component table | string | integer | nil | void.
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
  if (!lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected transform table");
  }
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0) {
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
  if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected string name");
  }
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || host->world == nullptr || entity_id == 0) {
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
  if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected string message");
  }
  auto* host = Host(lua);
  if (host != nullptr) {
    AddLuaDiagnostic(*host, ScriptDiagnosticSeverity::Info, lua_tostring(lua, 2));
  }
  return 0;
}

int LuaEntitySetDebugValue(lua_State* lua) {
  if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected string value");
  }
  auto* host = Host(lua);
  if (host != nullptr) {
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
  if (!lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected light table");
  }
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0) {
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
  if (!lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected camera table");
  }
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0) {
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
  if (!lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected ui_panel table");
  }
  auto* host = Host(lua);
  const auto entity_id = LuaSelfEntity(lua, 1);
  if (host == nullptr || host->commands == nullptr || entity_id == 0) {
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
  (void)host;
  lua_pushcfunction(lua, function);
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
  PushHostClosure(lua, host, LuaEntityLog);
  lua_setfield(lua, -2, "log");
  PushHostClosure(lua, host, LuaEntityGetLight);
  lua_setfield(lua, -2, "get_light");
  PushHostClosure(lua, host, LuaEntityGetCamera);
  lua_setfield(lua, -2, "get_camera");
  PushHostClosure(lua, host, LuaEntityGetPhysics);
  lua_setfield(lua, -2, "get_physics");
  PushHostClosure(lua, host, LuaEntityGetMaterialId);
  lua_setfield(lua, -2, "get_material_id");
  PushHostClosure(lua, host, LuaEntityGetUiPanel);
  lua_setfield(lua, -2, "get_ui_panel");
  if (!host.pure) {
    PushHostClosure(lua, host, LuaEntitySetTransform);
    lua_setfield(lua, -2, "set_transform");
    PushHostClosure(lua, host, LuaEntitySetName);
    lua_setfield(lua, -2, "set_name");
    PushHostClosure(lua, host, LuaEntitySetDebugValue);
    lua_setfield(lua, -2, "set_debug_value");
    PushHostClosure(lua, host, LuaEntitySetLight);
    lua_setfield(lua, -2, "set_light");
    PushHostClosure(lua, host, LuaEntitySetCamera);
    lua_setfield(lua, -2, "set_camera");
    PushHostClosure(lua, host, LuaEntitySetUiPanel);
    lua_setfield(lua, -2, "set_ui_panel");
  }
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

// === World bindings ============================================================
// Namespace: ctx.world.*
// Type errors: luaL_argerror — wrong Lua type is a programming error.
// Missing object: returns nil / empty table / void — entity destroyed or absent.
// Returns: entity table | entity-list table | bool | nil | void.
int LuaWorldFindEntity(lua_State* lua) {
  if (lua_isnumber(lua, 2)) {
    const auto value = lua_tonumber(lua, 2);
    if (!std::isfinite(value)) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected entity id (number) or name (string)");
  }
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
  if (!lua_isnoneornil(lua, 2)) {
    if (lua_isnumber(lua, 2)) {
      if (!std::isfinite(lua_tonumber(lua, 2))) {
        return luaL_argerror(lua, 2, "expected finite number");
      }
    } else if (!lua_istable(lua, 2)) {
      return luaL_argerror(lua, 2, "expected entity table or numeric id");
    }
  }
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
  if (lua_isnumber(lua, 2)) {
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_istable(lua, 2) && !lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected entity table, numeric id, or name");
  }
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
  if (lua_isnumber(lua, 2)) {
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected entity table or numeric id");
  }
  if (!lua_isnoneornil(lua, 3) && !lua_isstring(lua, 3)) {
    return luaL_argerror(lua, 3, "expected component name string");
  }
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
  if (!lua_istable(lua, 2)) {
    if (!lua_isnumber(lua, 2)) {
      return luaL_argerror(lua, 2, "expected entity table or numeric id");
    }
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  }
  if (!lua_isnoneornil(lua, 3) && !lua_istable(lua, 3)) {
    if (!lua_isnumber(lua, 3)) {
      return luaL_argerror(lua, 3, "expected parent entity table or numeric id");
    }
    if (!std::isfinite(lua_tonumber(lua, 3))) {
      return luaL_argerror(lua, 3, "expected finite number");
    }
  }
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
  for (int idx = 2; idx <= 4; ++idx) {
    if (lua_isnoneornil(lua, idx)) {
      continue;
    }
    if (!lua_isnumber(lua, idx)) {
      return luaL_argerror(lua, idx, "expected numeric entity id");
    }
    if (!std::isfinite(lua_tonumber(lua, idx))) {
      return luaL_argerror(lua, idx, "expected finite number");
    }
  }
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
  if (!lua_istable(lua, 2)) {
    if (!lua_isnumber(lua, 2)) {
      return luaL_argerror(lua, 2, "expected entity table or numeric id");
    }
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  }
  if (!lua_isstring(lua, 3)) {
    return luaL_argerror(lua, 3, "expected component name string");
  }
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr) {
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
  if (!lua_istable(lua, 2)) {
    if (!lua_isnumber(lua, 2)) {
      return luaL_argerror(lua, 2, "expected entity table or numeric id");
    }
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  }
  if (!lua_isnumber(lua, 3)) {
    return luaL_argerror(lua, 3, "expected numeric material id");
  }
  if (!std::isfinite(lua_tonumber(lua, 3))) {
    return luaL_argerror(lua, 3, "expected finite number");
  }
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
  if (!lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected spawn definition table");
  }
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr) {
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

// === Context bindings ==========================================================
// Namespace: ctx.* (top-level callables: diagnostic, include).
// Type errors: luaL_argerror — wrong Lua type is a programming error.
// Missing object: returns nil / void — host context unavailable.
// Returns: chunk return value | void.
int LuaInclude(lua_State* lua) {
  const int source_index = lua_isstring(lua, 2) ? 2 : (lua_isstring(lua, 1) ? 1 : 0);
  if (source_index == 0) {
    return luaL_argerror(lua, lua_isnoneornil(lua, 2) ? 1 : 2,
                         "include requires a script source path string");
  }
  auto* host = Host(lua);
  if (host == nullptr) {
    return luaL_error(lua, "include requires a host context");
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

// === Scene bindings ============================================================
// Namespace: ctx.scene.*
// Type errors: luaL_argerror — wrong Lua type is a programming error.
// Missing object: returns nil / empty table / false — entity destroyed or system absent.
// Returns: entity table | entity-list table | bool | nil | void.
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
  if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected component name string");
  }
  auto* host = Host(lua);
  if (host == nullptr || host->world == nullptr) {
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
  if (lua_isnumber(lua, 2)) {
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_istable(lua, 2) && !lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected entity table, numeric id, or name");
  }
  if (!lua_isstring(lua, 3)) {
    return luaL_argerror(lua, 3, "expected script source string");
  }
  if (!lua_isnoneornil(lua, 4) && !lua_istable(lua, 4)) {
    return luaL_argerror(lua, 4, "expected params table or nil");
  }
  auto* host = Host(lua);
  if (host == nullptr || host->commands == nullptr || host->world == nullptr) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const auto entity_id = LuaEntityArgument(lua, *host, 2);
  if (entity_id == 0) {
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
  const int module_index = lua_isstring(lua, 2) ? 2 : (lua_isstring(lua, 1) ? 1 : 0);
  if (module_index == 0) {
    return luaL_argerror(lua, lua_isnoneornil(lua, 2) ? 1 : 2,
                         "expected module name string");
  }
  auto* host = Host(lua);
  if (host == nullptr) {
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
  if (lua_isnumber(lua, 2)) {
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_istable(lua, 2) && !lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected entity table, numeric id, or name");
  }
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
  const int level_index = lua_istable(lua, 1) ? 2 : 1;
  const int message_index = level_index + 1;
  if (!lua_isnoneornil(lua, level_index) && !lua_isstring(lua, level_index)) {
    return luaL_argerror(lua, level_index, "expected severity string (info|warning|error)");
  }
  if (!lua_isnoneornil(lua, message_index) && !lua_isstring(lua, message_index)) {
    return luaL_argerror(lua, message_index, "expected message string");
  }
  auto* host = Host(lua);
  if (host == nullptr) {
    return 0;
  }
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

std::vector<int> LuaMouseButtonCandidates(lua_State* lua, int index) {
  std::vector<int> candidates;
  if (lua_isinteger(lua, index) || lua_isnumber(lua, index)) {
    candidates.push_back(static_cast<int>(lua_tointeger(lua, index)));
    return candidates;
  }
  if (!lua_isstring(lua, index)) {
    return candidates;
  }

  std::string button = TrimCopy(lua_tostring(lua, index));
  std::transform(button.begin(), button.end(), button.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (button == "left" || button == "primary" || button == "fire" ||
      button == "mouse0" || button == "button0") {
    candidates.push_back(0);
  } else if (button == "right" || button == "secondary" || button == "ads" ||
             button == "aim" || button == "mouse1" || button == "button1") {
    candidates.push_back(1);
  } else if (button == "middle" || button == "mouse2" || button == "button2") {
    candidates.push_back(2);
  } else if (button == "back" || button == "mouse3" || button == "button3") {
    candidates.push_back(3);
  } else if (button == "forward" || button == "mouse4" || button == "button4") {
    candidates.push_back(4);
  }
  return candidates;
}

// === Input bindings ============================================================
// Namespace: ctx.input.* (callable methods only; static fields skip arg validation).
// Type errors: luaL_argerror — wrong Lua type is a programming error.
// Missing object: returns false (no host context); unknown keys/buttons match nothing.
// Returns: bool | table.
int LuaInputKeyDown(lua_State* lua) {
  if (lua_isnumber(lua, 2)) {
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected key code (number) or name (string)");
  }
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

int LuaInputMouseDown(lua_State* lua) {
  if (lua_isnumber(lua, 2)) {
    if (!std::isfinite(lua_tonumber(lua, 2))) {
      return luaL_argerror(lua, 2, "expected finite number");
    }
  } else if (!lua_isstring(lua, 2)) {
    return luaL_argerror(lua, 2, "expected button code (number) or name (string)");
  }
  auto* host = Host(lua);
  if (host == nullptr || host->context == nullptr) {
    lua_pushboolean(lua, 0);
    return 1;
  }
  const auto candidates = LuaMouseButtonCandidates(lua, 2);
  bool down = false;
  for (const auto candidate : candidates) {
    if (std::find(host->context->input.active_mouse_buttons.begin(),
                  host->context->input.active_mouse_buttons.end(),
                  candidate) != host->context->input.active_mouse_buttons.end()) {
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
  PushHostClosure(lua, host, LuaWorldChildrenOf);
  lua_setfield(lua, -2, "children_of");
  PushHostClosure(lua, host, LuaWorldHasComponent);
  lua_setfield(lua, -2, "has_component");
  if (!host.pure) {
    PushHostClosure(lua, host, LuaWorldSpawnEntity);
    lua_setfield(lua, -2, "spawn_entity");
    PushHostClosure(lua, host, LuaWorldDestroyEntity);
    lua_setfield(lua, -2, "destroy_entity");
    PushHostClosure(lua, host, LuaWorldReparentEntity);
    lua_setfield(lua, -2, "reparent_entity");
    PushHostClosure(lua, host, LuaWorldReorderEntity);
    lua_setfield(lua, -2, "reorder_entity");
    PushHostClosure(lua, host, LuaWorldRemoveComponent);
    lua_setfield(lua, -2, "remove_component");
    PushHostClosure(lua, host, LuaWorldAssignMaterial);
    lua_setfield(lua, -2, "assign_material");
  }
}

void PushSceneTable(lua_State* lua, LuaHostContext& host) {
  lua_newtable(lua);
  PushHostClosure(lua, host, LuaSceneMainCamera);
  lua_setfield(lua, -2, "main_camera");
  PushHostClosure(lua, host, LuaSceneFindEntity);
  lua_setfield(lua, -2, "find_entity");
  PushHostClosure(lua, host, LuaSceneEntitiesWithComponent);
  lua_setfield(lua, -2, "entities_with_component");
  PushHostClosure(lua, host, LuaSceneUseSystem);
  lua_setfield(lua, -2, "use_system");
  if (!host.pure) {
    PushHostClosure(lua, host, LuaSceneEnsureScript);
    lua_setfield(lua, -2, "ensure_script");
    PushHostClosure(lua, host, LuaSceneRegisterInteractable);
    lua_setfield(lua, -2, "register_interactable");
  }
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
  PushHostClosure(lua, host, LuaInputMouseDown);
  lua_setfield(lua, -2, "mouse_down");
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
  const bool left_down = std::find(input.active_mouse_buttons.begin(),
                                   input.active_mouse_buttons.end(),
                                   0) != input.active_mouse_buttons.end();
  const bool right_down = std::find(input.active_mouse_buttons.begin(),
                                    input.active_mouse_buttons.end(),
                                    1) != input.active_mouse_buttons.end();
  const bool middle_down = std::find(input.active_mouse_buttons.begin(),
                                     input.active_mouse_buttons.end(),
                                     2) != input.active_mouse_buttons.end();
  lua_pushboolean(lua, left_down ? 1 : 0);
  lua_setfield(lua, -2, "mouse_left_down");
  lua_pushboolean(lua, right_down ? 1 : 0);
  lua_setfield(lua, -2, "mouse_right_down");
  lua_pushboolean(lua, middle_down ? 1 : 0);
  lua_setfield(lua, -2, "mouse_middle_down");
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

// === Audio bindings ============================================================
// Namespace: ctx.audio.*
// Type errors: luaL_argerror — wrong Lua type is a programming error.
// Missing object: returns nil / void — audio system unavailable or handle invalid.
// Returns: voice handle table | nil | void.
int LuaAudioPostEvent(lua_State* lua) {
  const int eventIndex = lua_isstring(lua, 2) ? 2 : (lua_isstring(lua, 1) ? 1 : 0);
  if (eventIndex == 0) {
    return luaL_argerror(lua, lua_isnoneornil(lua, 2) ? 1 : 2,
                         "expected event name string");
  }
  const int optionsIndexArg = eventIndex + 1;
  if (!lua_isnoneornil(lua, optionsIndexArg) && !lua_istable(lua, optionsIndexArg)) {
    return luaL_argerror(lua, optionsIndexArg, "expected options table or nil");
  }
  auto* host = Host(lua);
  if (host == nullptr) {
    lua_pushnil(lua);
    return 1;
  }

  auto* audio = vkpt::audio::GlobalAudioSystem();
  if (audio == nullptr) {
    AddLuaDiagnostic(*host, ScriptDiagnosticSeverity::Warning, "audio system is not available");
    lua_pushnil(lua);
    return 1;
  }

  vkpt::audio::AudioTrackedEventDesc desc;
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

  const auto handle = audio->post_tracked_event(desc);
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
  // Accept :stop(handle) or .stop(handle). The user-supplied handle table may sit at
  // index 1 (dot-call) or index 2 (colon-call). nil is allowed as a no-op. Any other
  // type at index 2 is a programming error; we never argerror at index 1 because that
  // index is the audio self-table when called via :.
  if (!lua_isnoneornil(lua, 2) && !lua_istable(lua, 2)) {
    return luaL_argerror(lua, 2, "expected voice handle table or nil");
  }
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
  if (!host.pure) {
    PushHostClosure(lua, host, LuaAudioPostEvent);
    lua_setfield(lua, -2, "post_event");
    PushHostClosure(lua, host, LuaAudioStop);
    lua_setfield(lua, -2, "stop");
  }
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

}  // namespace

struct LuaStatePool::Impl {
  struct YieldedCoroutine {
    int thread_ref = LUA_NOREF;
    int hook_function_ref = LUA_NOREF;
    std::size_t yield_count = 0u;
  };

  struct State {
    lua_State* lua = nullptr;
    LuaMemoryBudget memory_budget;
    int script_table_ref = LUA_NOREF;
    std::string key;
    std::mutex call_mutex;
    std::unordered_map<int, YieldedCoroutine> yielded;

    ~State() {
      if (lua != nullptr) {
        for (const auto& [_, yielded_coroutine] : yielded) {
          (void)_;
          if (yielded_coroutine.thread_ref != LUA_NOREF) {
            luaL_unref(lua, LUA_REGISTRYINDEX, yielded_coroutine.thread_ref);
          }
          if (yielded_coroutine.hook_function_ref != LUA_NOREF) {
            luaL_unref(lua, LUA_REGISTRYINDEX, yielded_coroutine.hook_function_ref);
          }
        }
        if (script_table_ref != LUA_NOREF) {
          luaL_unref(lua, LUA_REGISTRYINDEX, script_table_ref);
        }
        lua_close(lua);
      }
    }
  };

  mutable std::mutex mutex;
  std::unordered_map<std::string, std::shared_ptr<State>> states;
  std::size_t created_total = 0u;
};

LuaStatePool::LuaStatePool() : m_impl(std::make_unique<Impl>()) {}
LuaStatePool::~LuaStatePool() = default;

void LuaStatePool::clear() {
  std::scoped_lock lock(m_impl->mutex);
  m_impl->states.clear();
}

std::size_t LuaStatePool::state_count() const {
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->states.size();
}

std::size_t LuaStatePool::created_total() const {
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->created_total;
}

LuaStatePool::Impl& LuaStatePool::impl() {
  return *m_impl;
}

const LuaStatePool::Impl& LuaStatePool::impl() const {
  return *m_impl;
}

namespace {

std::string LuaStateKey(const ScriptBinding& binding, const std::filesystem::path& script_path) {
  return std::to_string(binding.entity) + "|" + script_path.generic_string() + "|" + binding.module_id;
}

std::shared_ptr<LuaStatePool::Impl::State> GetOrCreateLuaState(
    LuaStatePool::Impl& pool,
    LuaHostContext& host,
    const ScriptBinding& binding,
    const ScriptExecutionContext& context,
    ScriptBindingRuntimeState& runtime_state,
    std::unordered_map<std::string, std::string>& bytecode_cache,
    std::mutex& bytecode_cache_mutex) {
  const auto script_path = ResolveScriptPath(binding.source);
  const auto key = LuaStateKey(binding, script_path);
  {
    std::scoped_lock lock(pool.mutex);
    if (const auto existing = pool.states.find(key); existing != pool.states.end()) {
      existing->second->memory_budget.limit = context.memory_budget_bytes;
      return existing->second;
    }
  }

  auto state = std::make_shared<LuaStatePool::Impl::State>();
  state->key = key;
  state->memory_budget.limit = context.memory_budget_bytes;
  state->lua = lua_newstate(LuaBudgetAllocator, &state->memory_budget);
  if (state->lua == nullptr) {
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, "could not create Lua state");
    return nullptr;
  }

  auto* lua = state->lua;
  SetCurrentHost(lua, &host);
  OpenSafeLuaLibraries(lua);
  PushHostClosure(lua, host, LuaInclude);
  lua_setglobal(lua, "include");

  const auto chunk_name = "@" + script_path.generic_string();
  const auto cache_key = script_path.generic_string();
  bool chunk_loaded = false;
  {
    std::scoped_lock cache_lock(bytecode_cache_mutex);
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
        runtime_state.last_error = "script source could not be read";
        AddLuaDiagnostic(host,
                         ScriptDiagnosticSeverity::Error,
                         "script source could not be read: " + script_path.generic_string());
        return nullptr;
      }

      if (luaL_loadbufferx(lua, script_text->data(), script_text->size(), chunk_name.c_str(), "t") != LUA_OK) {
        runtime_state.last_error = "script compile failed: " + LuaErrorText(lua);
        AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
        return nullptr;
      }
      std::string bytecode;
      if (lua_dump(lua, LuaBytecodeWriter, &bytecode, 0) == 0 && !bytecode.empty()) {
        bytecode_cache[cache_key] = std::move(bytecode);
      }
    }
  }
  if (lua_pcall(lua, 0, 1, 0) != LUA_OK) {
    runtime_state.last_error = "script load failed: " + LuaErrorText(lua);
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    return nullptr;
  }
  if (!lua_istable(lua, -1)) {
    runtime_state.last_error = "script must return a table";
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    lua_pop(lua, 1);
    return nullptr;
  }

  state->script_table_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
  std::shared_ptr<LuaStatePool::Impl::State> out = state;
  {
    std::scoped_lock lock(pool.mutex);
    auto [it, inserted] = pool.states.emplace(key, std::move(state));
    if (inserted) {
      ++pool.created_total;
    }
    out = it->second;
  }
  return out;
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
                    LuaStatePool::Impl& state_pool,
                    std::unordered_map<std::string, std::string>& bytecode_cache,
                    std::mutex& bytecode_cache_mutex,
                    const std::unordered_map<std::string, ScriptVariableOverride>& variable_overrides) {
  LuaHostContext host;
  host.world = &world;
  host.commands = &commands;
  host.context = &context;
  host.binding = &binding;
  host.hook = hook;
  host.dispatch_diagnostics = &dispatch_diagnostics;
  host.runtime_diagnostics = &runtime_diagnostics;
  host.next_entity_id = NextEntityId(world);
  host.pure = binding.pure;
  runtime_state.entity = binding.entity;
  runtime_state.source = binding.source;
  runtime_state.last_hook = hook;
  runtime_state.last_frame = context.frame;
  runtime_state.command_count = commands.commands().size();
  runtime_state.last_error.clear();
  runtime_state.skip_reason.clear();
  runtime_state.pure = binding.pure;
  runtime_state.hook_fired = false;
  runtime_state.budget_exceeded = false;
  runtime_state.budget_exceeded_type.clear();
  runtime_state.instruction_count = 0u;
  const auto hook_start = std::chrono::steady_clock::now();

  auto pooled_state = GetOrCreateLuaState(
      state_pool, host, binding, context, runtime_state, bytecode_cache, bytecode_cache_mutex);
  if (pooled_state == nullptr || pooled_state->lua == nullptr || pooled_state->script_table_ref == LUA_NOREF) {
    return false;
  }
  std::unique_lock state_call_lock(pooled_state->call_mutex);
  auto* lua = pooled_state->lua;
  SetCurrentHost(lua, &host);
  runtime_state.state_ptr = reinterpret_cast<std::uintptr_t>(lua);
  pooled_state->memory_budget.limit = context.memory_budget_bytes;
  pooled_state->memory_budget.exceeded = false;
  pooled_state->memory_budget.requested = 0u;
  PushHostClosure(lua, host, LuaInclude);
  lua_setglobal(lua, "include");
  constexpr std::size_t kInstructionSampleInterval = 1000u;
  // 50 ms per script per hook: at 60 Hz the per-tick budget is 16.67 ms, so
  // 50 ms is already 3x over budget. We could go tighter but the existing
  // infinite-loop fixture (assets/scripts/test/script_budget_probe.lua) needs
  // to trip the instruction budget first; with the default 250000-instruction
  // limit and 1000-step sample interval, an empty `while true do end` runs in
  // microseconds, well below 50 ms.
  constexpr std::chrono::milliseconds kWallClockBudgetMs{50};
  LuaInstructionBudget instruction_budget;
  instruction_budget.limit = context.instruction_budget;
  instruction_budget.interval = context.instruction_budget == 0u
                                    ? 0u
                                    : std::min(context.instruction_budget, kInstructionSampleInterval);
  instruction_budget.wall_clock_budget_ms = kWallClockBudgetMs;
  instruction_budget.wall_clock_deadline = hook_start + kWallClockBudgetMs;
  host.instruction_budget = &instruction_budget;

  lua_rawgeti(lua, LUA_REGISTRYINDEX, pooled_state->script_table_ref);
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

  const int hook_key = static_cast<int>(hook);
  int hook_function_ref = LUA_NOREF;
  int coroutine_ref = LUA_NOREF;
  int resume_status = LUA_OK;
  int result_count = 0;
  lua_State* active_coroutine = nullptr;
  bool resumed_yielded_hook = false;
  if (auto yielded = pooled_state->yielded.find(hook_key);
      yielded != pooled_state->yielded.end()) {
    resumed_yielded_hook = true;
    hook_function_ref = yielded->second.hook_function_ref;
    coroutine_ref = yielded->second.thread_ref;
    lua_rawgeti(lua, LUA_REGISTRYINDEX, coroutine_ref);
    auto* coroutine = lua_tothread(lua, -1);
    active_coroutine = coroutine;
    lua_pop(lua, 1);
    if (coroutine == nullptr) {
      runtime_state.last_error = "script yielded coroutine is invalid";
      AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
      luaL_unref(lua, LUA_REGISTRYINDEX, coroutine_ref);
      if (hook_function_ref != LUA_NOREF) {
        luaL_unref(lua, LUA_REGISTRYINDEX, hook_function_ref);
      }
      pooled_state->yielded.erase(yielded);
      lua_pop(lua, 1);
      lua_sethook(lua, nullptr, 0, 0);
      SetCurrentHost(lua, nullptr);
      return false;
    }
    if (context.instruction_budget > 0u) {
      lua_sethook(coroutine,
                  LuaInstructionBudgetHook,
                  LUA_MASKCOUNT,
                  static_cast<int>(instruction_budget.interval));
    }
    runtime_state.hook_fired = true;
    LogScriptHookFired(binding, hook, context);
    VKP_METRIC_INC("vkp.script.hooks_total");
    resume_status = lua_resume(coroutine, lua, 0, &result_count);
  } else {
    lua_getfield(lua, -1, std::string(to_string(hook)).c_str());
    if (lua_isnil(lua, -1)) {
      // Missing hook functions are normal and are counted as non-calls, not diagnostics.
      lua_pop(lua, 2);
      lua_sethook(lua, nullptr, 0, 0);
      SetCurrentHost(lua, nullptr);
      return false;
    }
    if (!lua_isfunction(lua, -1)) {
      runtime_state.last_error = "script hook is not a function";
      AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
      lua_pop(lua, 2);
      lua_sethook(lua, nullptr, 0, 0);
      SetCurrentHost(lua, nullptr);
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
    hook_function_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    auto* coroutine = lua_newthread(lua);
    active_coroutine = coroutine;
    coroutine_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
    lua_xmove(lua, coroutine, 1);
    PushEntityObject(lua, host, binding.entity);
    lua_xmove(lua, coroutine, 1);
    PushContextTable(lua, host, variable_overrides);
    lua_xmove(lua, coroutine, 1);
    if (context.instruction_budget > 0u) {
      lua_sethook(coroutine,
                  LuaInstructionBudgetHook,
                  LUA_MASKCOUNT,
                  static_cast<int>(instruction_budget.interval));
    }
    runtime_state.hook_fired = true;
    LogScriptHookFired(binding, hook, context);
    VKP_METRIC_INC("vkp.script.hooks_total");
    resume_status = lua_resume(coroutine, lua, 2, &result_count);
  }
  runtime_state.instruction_count = instruction_budget.executed;
  if (resume_status == LUA_YIELD) {
    if (active_coroutine != nullptr && result_count > 0) {
      lua_pop(active_coroutine, result_count);
    }
    auto& yielded = pooled_state->yielded[hook_key];
    if (!resumed_yielded_hook) {
      yielded.thread_ref = coroutine_ref;
      yielded.hook_function_ref = hook_function_ref;
    }
    ++yielded.yield_count;
    runtime_state.skip_reason = "yielded";
    runtime_state.memory_estimate_bytes = pooled_state->memory_budget.peak;
    if (context.yield_budget == 0u || yielded.yield_count > context.yield_budget) {
      runtime_state.last_error = "script hook killed: yield budget exceeded";
      runtime_state.disabled_until_reload = true;
      runtime_state.budget_exceeded = true;
      runtime_state.budget_exceeded_type = "yield";
      LogScriptBudgetExceeded(binding, hook, context, "yield", yielded.yield_count, context.yield_budget);
      AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Warning, runtime_state.last_error);
      luaL_unref(lua, LUA_REGISTRYINDEX, yielded.thread_ref);
      luaL_unref(lua, LUA_REGISTRYINDEX, yielded.hook_function_ref);
      pooled_state->yielded.erase(hook_key);
      runtime_state.skip_reason.clear();
      lua_pop(lua, 1);
      if (active_coroutine != nullptr) {
        lua_sethook(active_coroutine, nullptr, 0, 0);
      }
      lua_sethook(lua, nullptr, 0, 0);
      const auto hook_end = std::chrono::steady_clock::now();
      runtime_state.hook_duration_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(hook_end - hook_start).count());
      RecordScriptHookTiming(hook, runtime_state.hook_duration_ns / 1000u);
      SetCurrentHost(lua, nullptr);
      return false;
    }
    lua_pop(lua, 1);
    if (active_coroutine != nullptr) {
      lua_sethook(active_coroutine, nullptr, 0, 0);
    }
    lua_sethook(lua, nullptr, 0, 0);
    const auto hook_end = std::chrono::steady_clock::now();
    runtime_state.hook_duration_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(hook_end - hook_start).count());
    SetCurrentHost(lua, nullptr);
    return true;
  }
  if (resume_status != LUA_OK) {
    runtime_state.last_error = "script hook failed: " +
        LuaErrorText(active_coroutine != nullptr ? active_coroutine : lua);
    if (instruction_budget.exceeded ||
        runtime_state.last_error.find("instruction budget exceeded") != std::string::npos) {
      runtime_state.budget_exceeded = true;
      runtime_state.budget_exceeded_type = "instructions";
      runtime_state.instruction_count = std::max(instruction_budget.executed, context.instruction_budget);
      runtime_state.disabled_until_reload = true;
      LogScriptBudgetExceeded(binding,
                              hook,
                              context,
                              "instructions",
                              runtime_state.instruction_count,
                              context.instruction_budget);
    } else if (instruction_budget.wall_clock_exceeded ||
               runtime_state.last_error.find("wall-clock budget exceeded") != std::string::npos) {
      runtime_state.budget_exceeded = true;
      runtime_state.budget_exceeded_type = "wall_clock";
      runtime_state.disabled_until_reload = true;
      const auto elapsed_ms = static_cast<std::size_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - hook_start)
              .count());
      LogScriptBudgetExceeded(binding,
                              hook,
                              context,
                              "wall_clock",
                              elapsed_ms,
                              static_cast<std::size_t>(instruction_budget.wall_clock_budget_ms.count()));
    } else if (pooled_state->memory_budget.exceeded ||
               runtime_state.last_error.find("memory") != std::string::npos) {
      runtime_state.budget_exceeded = true;
      runtime_state.budget_exceeded_type = "memory";
      runtime_state.disabled_until_reload = true;
      const auto actual = std::max(pooled_state->memory_budget.requested,
                                   pooled_state->memory_budget.peak);
      LogScriptBudgetExceeded(binding,
                              hook,
                              context,
                              "memory",
                              actual,
                              context.memory_budget_bytes);
    }
    AddLuaDiagnostic(host, ScriptDiagnosticSeverity::Error, runtime_state.last_error);
    if (hook_function_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, hook_function_ref);
    }
    if (coroutine_ref != LUA_NOREF) {
      luaL_unref(lua, LUA_REGISTRYINDEX, coroutine_ref);
    }
    if (resumed_yielded_hook) {
      pooled_state->yielded.erase(hook_key);
    }
    lua_pop(lua, 1);
    runtime_state.memory_estimate_bytes = pooled_state->memory_budget.peak;
    if (active_coroutine != nullptr) {
      lua_sethook(active_coroutine, nullptr, 0, 0);
    }
    lua_sethook(lua, nullptr, 0, 0);
    const auto hook_end = std::chrono::steady_clock::now();
    runtime_state.hook_duration_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(hook_end - hook_start).count());
    RecordScriptHookTiming(hook, runtime_state.hook_duration_ns / 1000u);
    SetCurrentHost(lua, nullptr);
    return false;
  }
  if (coroutine_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, coroutine_ref);
  }
  if (active_coroutine != nullptr && result_count > 0) {
    lua_pop(active_coroutine, result_count);
  }
  if (resumed_yielded_hook) {
    pooled_state->yielded.erase(hook_key);
  }
  if (active_coroutine != nullptr) {
    lua_sethook(active_coroutine, nullptr, 0, 0);
  }
  lua_sethook(lua, nullptr, 0, 0);
  lua_rawgeti(lua, LUA_REGISTRYINDEX, hook_function_ref);
  CaptureLuaScriptVariables(lua, -2, -1, binding, hook, context, variable_snapshots);
  lua_pop(lua, 1);
  if (hook_function_ref != LUA_NOREF) {
    luaL_unref(lua, LUA_REGISTRYINDEX, hook_function_ref);
  }
  lua_pop(lua, 1);
  const auto hook_end = std::chrono::steady_clock::now();
  runtime_state.hook_duration_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(hook_end - hook_start).count());
  RecordScriptHookTiming(hook, runtime_state.hook_duration_ns / 1000u);
  runtime_state.command_count = commands.commands().size() - runtime_state.command_count;
  runtime_state.memory_estimate_bytes = pooled_state->memory_budget.peak;
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Debug,
      "scripts",
      "Lua hook executed",
      {{"entity", std::to_string(binding.entity)},
       {"entity_name", binding.entity_name},
       {"hook", std::string(to_string(hook))},
       {"source", binding.source},
       {"commands_after", std::to_string(commands.commands().size())},
       {"active_keys", std::to_string(context.input.active_keys.size())},
       {"active_mouse_buttons", std::to_string(context.input.active_mouse_buttons.size())}},
      context.frame);
  SetCurrentHost(lua, nullptr);
  return true;
}

#else

}  // namespace

struct LuaStatePool::Impl {
  std::size_t created_total = 0u;
};

LuaStatePool::LuaStatePool() : m_impl(std::make_unique<Impl>()) {}
LuaStatePool::~LuaStatePool() = default;

void LuaStatePool::clear() {}

std::size_t LuaStatePool::state_count() const {
  return 0u;
}

std::size_t LuaStatePool::created_total() const {
  return m_impl->created_total;
}

LuaStatePool::Impl& LuaStatePool::impl() {
  return *m_impl;
}

const LuaStatePool::Impl& LuaStatePool::impl() const {
  return *m_impl;
}

namespace {

#endif  // PT_ENABLE_LUA

#ifndef PT_ENABLE_LUA
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
#endif

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

EcsScriptRuntime::EcsScriptRuntime()
    : m_lua_state_pool(std::make_unique<LuaStatePool>()),
      m_script_command_queue(std::make_unique<ScriptCommandQueue>()) {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      "scripting",
      "scripting.started",
      {{"lifecycle", "uninitialized"}});
}

EcsScriptRuntime::~EcsScriptRuntime() {
  vkpt::log::Logger::instance().log(
      vkpt::log::Severity::Info,
      "scripting",
      "scripting.stopped",
      {{"lifecycle", "shutting_down"}});
}

ScriptDispatchSummary EcsScriptRuntime::dispatch_hook(const vkpt::scene::SceneWorld& world,
                                                      ScriptLifecycleHook hook,
                                                      const ScriptExecutionContext& context,
                                                      vkpt::scene::WorldCommandBuffer& commands) {
  std::unique_lock runtime_lock(m_runtime_mutex);
  vkpt::core::contracts::assert_state(
      "EcsScriptRuntime::dispatch_hook",
      m_status.lifecycle,
      {vkpt::core::contracts::ComponentLifecycle::Uninitialized,
       vkpt::core::contracts::ComponentLifecycle::Ready,
       vkpt::core::contracts::ComponentLifecycle::Degraded,
       vkpt::core::contracts::ComponentLifecycle::Failed});
  m_status.lifecycle = vkpt::core::contracts::ComponentLifecycle::Busy;
  m_status.last_frame = context.frame;
  m_status.current_flow_id = context.frame;
  commands.set_flow_id(context.frame);
  const auto current_bindings = BuildScriptBindings(world);
  if (BindingSignature(current_bindings) != BindingSignature(m_bindings)) {
    // Entity/script topology changes invalidate stable dispatch order and compiled bytecode assumptions.
    m_bindings = current_bindings;
    ApplyScriptEditorAnnotations(m_bindings);
    {
      std::scoped_lock cache_lock(m_lua_cache_mutex);
      m_lua_bytecode_cache.clear();
    }
    m_lua_state_pool->clear();
    m_script_command_queue->clear();
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
  summary.deterministic_group_b_disabled = context.deterministic;
  m_variable_snapshots.clear();
  m_runtime_states.clear();

  struct BindingDispatchResult {
    ScriptBindingRuntimeState runtime_state;
    vkpt::scene::WorldCommandBuffer commands;
    std::vector<ScriptDiagnostic> dispatch_diagnostics;
    std::vector<ScriptDiagnostic> runtime_diagnostics;
    std::vector<ScriptVariableSnapshot> variable_snapshots;
    bool hook_called = false;
    bool disabled_until_reload = false;
    std::string disabled_key;
  };

  std::vector<BindingDispatchResult> binding_results(m_bindings.size());
  std::vector<vkpt::core::JobHandle> pure_jobs;
  vkpt::jobs::IJobSystem* group_b_jobs = context.job_system;
  const bool has_parallel_pure_work =
      !context.deterministic &&
      std::any_of(m_bindings.begin(), m_bindings.end(), [](const ScriptBinding& binding) {
        return binding.enabled && binding.pure && IsSupportedLanguage(binding.language);
      });
  if (group_b_jobs == nullptr && has_parallel_pure_work) {
    if (!m_pure_job_system) {
      m_pure_job_system = std::make_unique<vkpt::jobs::JobSystem>();
    }
    group_b_jobs = m_pure_job_system.get();
  }

  auto run_lua_binding = [&](std::size_t index, const ScriptBinding& binding) {
    auto& result = binding_results[index];
#ifdef PT_ENABLE_LUA
    try {
      if (ExecuteLuaHook(binding,
                         world,
                         hook,
                         context,
                         result.commands,
                         result.dispatch_diagnostics,
                         result.runtime_diagnostics,
                         result.variable_snapshots,
                         result.runtime_state,
                         m_lua_state_pool->impl(),
                         m_lua_bytecode_cache,
                         m_lua_cache_mutex,
                         m_variable_overrides)) {
        result.hook_called = true;
      }
    } catch (const std::exception& ex) {
      result.commands.clear();
      result.runtime_state.last_error = std::string("script host exception: ") + ex.what();
      result.runtime_state.disabled_until_reload = true;
      auto diagnostic = MakeDiagnostic(ScriptDiagnosticSeverity::Error,
                                       hook,
                                       binding,
                                       context,
                                       result.runtime_state.last_error);
      LogDiagnostic(diagnostic);
      result.dispatch_diagnostics.push_back(diagnostic);
      result.runtime_diagnostics.push_back(std::move(diagnostic));
    } catch (...) {
      result.commands.clear();
      result.runtime_state.last_error = "script host exception: unknown exception";
      result.runtime_state.disabled_until_reload = true;
      auto diagnostic = MakeDiagnostic(ScriptDiagnosticSeverity::Error,
                                       hook,
                                       binding,
                                       context,
                                       result.runtime_state.last_error);
      LogDiagnostic(diagnostic);
      result.dispatch_diagnostics.push_back(diagnostic);
      result.runtime_diagnostics.push_back(std::move(diagnostic));
    }
    result.disabled_until_reload = result.runtime_state.disabled_until_reload;
#else
    result.hook_called = true;
    (void)binding;
#endif
  };

  for (std::size_t binding_index = 0; binding_index < m_bindings.size(); ++binding_index) {
    const auto& binding = m_bindings[binding_index];
    auto& result = binding_results[binding_index];
    ScriptBindingRuntimeState runtime_state;
    runtime_state.entity = binding.entity;
    runtime_state.source = binding.source;
    runtime_state.last_hook = hook;
    runtime_state.last_frame = context.frame;
    const auto disabled_key = std::to_string(binding.entity) + ":" + binding.source;
    result.disabled_key = disabled_key;
    if (!binding.enabled || !IsSupportedLanguage(binding.language)) {
      runtime_state.skip_reason = !binding.enabled ? "disabled" : "unsupported language";
      ++summary.skipped_count;
      result.runtime_state = std::move(runtime_state);
      continue;
    }

    ++summary.runnable_count;

    if (summary.scripts_disabled) {
      runtime_state.skip_reason = "scripts disabled";
      ++summary.skipped_count;
      result.runtime_state = std::move(runtime_state);
      continue;
    }
    if (summary.game_mode_blocked) {
      runtime_state.skip_reason = "game mode required";
      ++summary.skipped_count;
      result.runtime_state = std::move(runtime_state);
      continue;
    }
    if (summary.benchmark_blocked) {
      runtime_state.skip_reason = "benchmark blocked";
      ++summary.skipped_count;
      result.runtime_state = std::move(runtime_state);
      continue;
    }
    if (m_disabled_until_reload.contains(disabled_key)) {
      runtime_state.skip_reason = "disabled until reload";
      runtime_state.disabled_until_reload = true;
      ++summary.skipped_count;
      result.runtime_state = std::move(runtime_state);
      continue;
    }
    if (binding.requires_authoritative_reads && !context.allow_authoritative_reads) {
      runtime_state.skip_reason = "deferred for authoritative reads";
      ++summary.skipped_count;
      result.runtime_state = std::move(runtime_state);
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
      result.dispatch_diagnostics.push_back(diagnostic);
      result.runtime_diagnostics.push_back(std::move(diagnostic));
      result.runtime_state = std::move(runtime_state);
      continue;
    }

    result.runtime_state = std::move(runtime_state);
    if (binding.pure && group_b_jobs != nullptr && !context.deterministic) {
      ++summary.pure_job_count;
      const auto index = binding_index;
      pure_jobs.push_back(group_b_jobs->submit_job([&, index, binding]() {
        run_lua_binding(index, binding);
      }));
      continue;
    }

    run_lua_binding(binding_index, binding);
  }

  if (!pure_jobs.empty() && group_b_jobs != nullptr) {
    if (!group_b_jobs->wait_group(pure_jobs)) {
      auto diagnostic = MakeDiagnostic(ScriptDiagnosticSeverity::Error,
                                       hook,
                                       {},
                                       context,
                                       "one or more pure script jobs failed");
      LogDiagnostic(diagnostic);
      summary.diagnostics.push_back(diagnostic);
      summary.result.errors.push_back(ScriptError{
          .severity = diagnostic.severity,
          .hook = diagnostic.hook,
          .entity = diagnostic.entity,
          .frame = diagnostic.frame,
          .source = diagnostic.source,
          .message = diagnostic.message,
          .budget_exceeded = false,
          .budget = {},
          .killed = false,
      });
      m_diagnostics.push_back(std::move(diagnostic));
    }
  }

  std::size_t instructions_this_frame = 0u;
  m_status.active_scripts = summary.runnable_count;
  for (auto& result : binding_results) {
    if (result.hook_called) {
      ++summary.hook_call_count;
    }
    if (result.runtime_state.hook_fired || result.hook_called) {
      ++m_status.hooks_fired_total;
    }
    if (result.runtime_state.budget_exceeded) {
      ++m_status.budget_kills_total;
      ++summary.result.budget_exceeded_count;
      if (result.runtime_state.budget_exceeded_type == "wall_clock") {
        ++m_status.wall_clock_kills_total;
      }
    }
    const bool killed_this_dispatch = result.runtime_state.disabled_until_reload &&
                                      !result.runtime_state.last_error.empty();
    if (killed_this_dispatch) {
      ++summary.result.script_killed_count;
    }
    if (!result.runtime_state.last_error.empty()) {
      m_status.last_error_script_id = result.runtime_state.entity;
      summary.result.errors.push_back(ScriptError{
          .severity = result.runtime_state.budget_exceeded
                          ? ScriptDiagnosticSeverity::Warning
                          : ScriptDiagnosticSeverity::Error,
          .hook = result.runtime_state.last_hook,
          .entity = result.runtime_state.entity,
          .frame = result.runtime_state.last_frame,
          .source = result.runtime_state.source,
          .message = result.runtime_state.last_error,
          .budget_exceeded = result.runtime_state.budget_exceeded,
          .budget = result.runtime_state.budget_exceeded_type,
          .killed = killed_this_dispatch,
      });
    }
    instructions_this_frame += result.runtime_state.instruction_count;
    for (auto& diagnostic : result.dispatch_diagnostics) {
      summary.diagnostics.push_back(diagnostic);
    }
    for (auto& diagnostic : result.runtime_diagnostics) {
      m_diagnostics.push_back(std::move(diagnostic));
    }
    for (auto& variable : result.variable_snapshots) {
      m_variable_snapshots.push_back(std::move(variable));
    }
    for (const auto& command : result.commands.commands()) {
      if (!m_script_command_queue->push_scene_command(command)) {
        ScriptBinding diagnostic_binding;
        diagnostic_binding.entity = result.runtime_state.entity;
        diagnostic_binding.source = result.runtime_state.source;
        auto diagnostic = MakeDiagnostic(ScriptDiagnosticSeverity::Warning,
                                         hook,
                                         diagnostic_binding,
                                         context,
                                         "script command dropped because ScriptCmdRing is full");
        LogDiagnostic(diagnostic);
        summary.diagnostics.push_back(diagnostic);
        m_diagnostics.push_back(std::move(diagnostic));
      }
    }
    if (result.disabled_until_reload && !result.disabled_key.empty()) {
      m_disabled_until_reload.insert(result.disabled_key);
    }
    m_runtime_states.push_back(std::move(result.runtime_state));
  }

  m_script_command_queue->drain_to(commands);
  summary.command_count_after = commands.commands().size();
  if (!summary.result.errors.empty() || summary.result.budget_exceeded_count > 0u ||
      summary.result.script_killed_count > 0u) {
    summary.result.overall_status = summary.hook_call_count > 0u
                                        ? ScriptDispatchResult::Status::PartialFailure
                                        : ScriptDispatchResult::Status::Failure;
  } else if (summary.runnable_count > 0u && summary.hook_call_count == 0u &&
             summary.skipped_count > 0u) {
    summary.result.overall_status = ScriptDispatchResult::Status::Skipped;
  }
  if (!summary.result.errors.empty()) {
    m_status.health = vkpt::core::contracts::SubsystemHealth::Degraded;
    m_status.lifecycle = vkpt::core::contracts::ComponentLifecycle::Degraded;
    m_status.health_reason = "script_errors";
    m_status.last_error = summary.result.errors.front().message;
  } else {
    m_status.health = vkpt::core::contracts::SubsystemHealth::Ok;
    m_status.lifecycle = vkpt::core::contracts::ComponentLifecycle::Ready;
    m_status.health_reason = "ok";
    m_status.last_error.clear();
  }
  RecordScriptInstructionsPerFrame(instructions_this_frame);
  return summary;
}

}  // namespace vkpt::scripting
