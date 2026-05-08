#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/contracts/Determinism.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/SubsystemStatus.h"
#include "core/health/Health.h"
#include "core/Types.h"
#include "scene/Scene.h"

namespace vkpt::jobs {
class IJobSystem;
class JobSystem;
}  // namespace vkpt::jobs

namespace vkpt::scripting {

inline constexpr std::string_view kScriptingSubsystemName = "scripting";
inline constexpr std::string_view kScriptingStatusTypeName = "ScriptingStatus";
inline constexpr std::string_view kScriptingCommandSnapshotContractName =
    "scripting.command_snapshot.v1";

struct ScriptingNamingContract {
  std::string_view subsystem_name = kScriptingSubsystemName;
  std::string_view status_type_name = kScriptingStatusTypeName;
  std::string_view command_snapshot_contract = kScriptingCommandSnapshotContractName;
  std::string_view health_probe_name = kScriptingSubsystemName;
  std::string_view lifecycle_field_name = "lifecycle";
  std::string_view last_error_field_name = "last_error";
  std::string_view flow_field_name = "current_flow_id";
};

inline constexpr ScriptingNamingContract kScriptingNamingContract{};

[[nodiscard]] inline constexpr std::string_view ScriptingSubsystemName() noexcept {
  return kScriptingSubsystemName;
}

/// Script table hooks. Names map directly through to_string(), e.g. OnUpdate -> "on_update".
enum class ScriptLifecycleHook : std::uint8_t {
  OnLoad,
  OnSpawn,
  OnEnable,
  OnDisable,
  OnUpdate,
  OnFixedUpdate,
  OnLateUpdate,
  OnDestroy,
  OnUnload,
};

enum class ScriptDiagnosticSeverity : std::uint8_t {
  Info,
  Warning,
  Error,
};

/// Enforcement policy for script CPU and allocator budgets.
///
/// KillAndDisableUntilReload is intentionally fail-closed: instruction budget
/// exhaustion raises from Lua's count hook and memory budget exhaustion fails
/// the allocating call through the Lua allocator. The current hook is killed,
/// commands from that failed hook are not replayed, and the binding is skipped
/// until reload_bindings() rebuilds the script state.
enum class BudgetPolicy : std::uint8_t {
  KillAndDisableUntilReload,
};

struct ScriptExecutionContext {
  struct RuntimeState {
    /// Empty means derive the legacy runtime mode from game_mode.
    std::string mode;
    bool scripts_running = false;
  };

  struct InputState {
    bool enabled = true;
    /// Host key codes currently active for this frame.
    std::vector<int> active_keys;
    float mouse_delta_x = 0.0f;
    float mouse_delta_y = 0.0f;
    float mouse_wheel_delta = 0.0f;
    bool viewport_focused = true;
  };

  struct EditorState {
    bool canvas_enabled = true;
    bool is_editing = false;
    vkpt::core::StableEntityId edited_entity_id = 0;
    std::string edited_component;
  };

  vkpt::core::FrameIndex frame = 0;
  double elapsed_seconds = 0.0;
  double delta_seconds = 0.0;
  double fixed_delta_seconds = 1.0 / 60.0;
  /// True when callers require deterministic behavior from script-visible state.
  bool deterministic = false;
  std::uint64_t determinism_base_seed = 0u;
  std::string determinism_scenario_id;
  /// Global runtime gate. Disabled scripts are counted but hook bodies are not executed.
  bool scripts_enabled = true;
  /// Legacy game-mode latch. New hosts should prefer runtime.mode/runtime.scripts_running.
  bool game_mode = false;
  /// Benchmark mode blocks script execution unless allow_benchmark_scripts is also true.
  bool benchmark_mode = false;
  bool allow_benchmark_scripts = false;
  BudgetPolicy budget_policy = BudgetPolicy::KillAndDisableUntilReload;
  /// Count-hook limit for Lua VM instructions. Enforcement samples every
  /// min(instruction_budget, 1000) VM instructions, so the actual count can
  /// overshoot by at most one sample interval before the hook is killed.
  std::size_t instruction_budget = 250000;
  /// Lua allocator limit. A request that would exceed the limit fails before
  /// tracked bytes are committed; runtime state records the peak/requested use.
  std::size_t memory_budget_bytes = 2 * 1024 * 1024;
  std::size_t yield_budget = 8;
  bool allow_authoritative_reads = false;
  vkpt::jobs::IJobSystem* job_system = nullptr;
  RuntimeState runtime;
  InputState input;
  EditorState editor;

  void set_determinism(const vkpt::core::DeterminismContext& context) {
    const auto previous = determinism_context();
    deterministic = context.enabled;
    frame = context.frame_index;
    determinism_base_seed = context.base_seed;
    determinism_scenario_id = context.scenario_id;
    vkpt::core::EmitDeterminismChangedIfNeeded("scripts", previous, determinism_context());
  }

  vkpt::core::DeterminismContext determinism_context() const {
    return vkpt::core::MakeDeterminismContext(deterministic,
                                              determinism_base_seed,
                                              frame,
                                              determinism_scenario_id);
  }
};

struct ScriptEditorParam {
  std::string name;
  std::string type = "text";
  std::string label;
  std::string default_value;
  double minimum = 0.0;
  double maximum = 1.0;
  double step = 0.01;
  bool has_minimum = false;
  bool has_maximum = false;
  bool has_step = false;
};

/// Runtime binding produced from an entity ScriptComponent.
struct ScriptBinding {
  vkpt::core::StableEntityId entity = 0;
  std::size_t stable_order = 0;
  std::string entity_name;
  std::string source;
  std::string language = "lua";
  std::string entry = "default";
  std::string module_id = "default";
  bool enabled = true;
  bool reload_on_save = true;
  bool pure = false;
  bool requires_authoritative_reads = false;
  std::unordered_map<std::string, std::string> params;
  std::vector<ScriptEditorParam> editor_params;
  std::vector<std::string> editor_param_diagnostics;
};

struct ScriptBindingSummary {
  std::size_t binding_count = 0;
  std::size_t runnable_count = 0;
  std::size_t disabled_count = 0;
  std::size_t unsupported_language_count = 0;
  bool lua_compiled_in = false;
  bool execution_available = false;
};

struct ScriptDiagnostic {
  ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Info;
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnLoad;
  vkpt::core::StableEntityId entity = 0;
  vkpt::core::FrameIndex frame = 0;
  std::string source;
  std::string message;
};

struct ScriptVariableSnapshot {
  vkpt::core::StableEntityId entity = 0;
  vkpt::core::FrameIndex frame = 0;
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnLoad;
  std::string source;
  std::string scope;
  std::string name;
  std::string value;
  bool editable = false;
};

struct ScriptVariableOverride {
  vkpt::core::StableEntityId entity = 0;
  std::string scope;
  std::string name;
  std::string value;
};

struct ScriptBindingRuntimeState {
  vkpt::core::StableEntityId entity = 0;
  std::string source;
  ScriptLifecycleHook last_hook = ScriptLifecycleHook::OnLoad;
  vkpt::core::FrameIndex last_frame = 0;
  std::string last_error;
  std::string skip_reason;
  std::uint64_t hook_duration_ns = 0;
  std::size_t command_count = 0;
  std::size_t memory_estimate_bytes = 0;
  std::size_t instruction_count = 0;
  std::uintptr_t state_ptr = 0;
  bool pure = false;
  bool disabled_until_reload = false;
  bool hook_fired = false;
  bool budget_exceeded = false;
  std::string budget_exceeded_type;
};

struct ScriptError {
  ScriptDiagnosticSeverity severity = ScriptDiagnosticSeverity::Error;
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnLoad;
  vkpt::core::StableEntityId entity = 0;
  vkpt::core::FrameIndex frame = 0;
  std::string source;
  std::string message;
  bool budget_exceeded = false;
  std::string budget;
  bool killed = false;
};

struct ScriptDispatchResult {
  enum class Status : std::uint8_t {
    Ok,
    Skipped,
    PartialFailure,
    Failure,
  };

  Status overall_status = Status::Ok;
  std::size_t budget_exceeded_count = 0;
  std::size_t script_killed_count = 0;
  std::vector<ScriptError> errors;
};

struct ScriptDispatchSummary {
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnLoad;
  vkpt::core::FrameIndex frame = 0;
  std::size_t binding_count = 0;
  std::size_t runnable_count = 0;
  std::size_t skipped_count = 0;
  std::size_t hook_call_count = 0;
  std::size_t command_count_before = 0;
  std::size_t command_count_after = 0;
  bool lua_compiled_in = false;
  bool execution_available = false;
  bool scripts_disabled = false;
  bool game_mode_blocked = false;
  bool benchmark_blocked = false;
  std::size_t pure_job_count = 0;
  bool deterministic_group_b_disabled = false;
  std::vector<ScriptDiagnostic> diagnostics;
  ScriptDispatchResult result;
};

struct ScriptCommandSnapshot {
  std::uint64_t generation = 0u;
  ScriptLifecycleHook hook = ScriptLifecycleHook::OnLoad;
  vkpt::scene::WorldCommandBuffer commands;
  ScriptDispatchSummary diagnostics;
};

struct ScriptingStatus {
  std::string name = std::string(kScriptingSubsystemName);
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Uninitialized;
  vkpt::core::contracts::SubsystemHealth health =
      vkpt::core::contracts::SubsystemHealth::Ok;
  std::size_t active_scripts = 0;
  std::uint64_t hooks_fired_total = 0;
  std::uint64_t budget_kills_total = 0;
  vkpt::core::StableEntityId last_error_script_id = 0;
  vkpt::core::FrameIndex last_frame = 0;
  std::uint64_t current_flow_id = 0u;
  std::string health_reason = "ok";
  std::string last_error;
};

using ScriptRuntimeStatus = ScriptingStatus;

vkpt::core::health::Report EvaluateScriptingHealth(const ScriptingStatus& status);
vkpt::core::contracts::SubsystemStatus ToSubsystemStatus(
    const ScriptingStatus& status);
std::string FormatScriptingStatus(const ScriptingStatus& status);

class IScriptRuntime {
 public:
  virtual ~IScriptRuntime() = default;

  /// Rebuild the binding list from ECS state and clear runtime script caches.
  ///
  /// reload_bindings() is a synchronous owner-thread barrier. dispatch_hook()
  /// is also synchronous, so callers must not call reload_bindings()
  /// concurrently with an in-flight dispatch on the same runtime. After reload
  /// returns, compiled Lua state, bytecode cache, queued script commands,
  /// variable snapshots, runtime state, and budget-kill skip state are cleared;
  /// scripts killed by BudgetPolicy::KillAndDisableUntilReload may run again.
  virtual ScriptBindingSummary reload_bindings(const vkpt::scene::SceneWorld& world) = 0;
  /// Dispatch one lifecycle hook. Scripts enqueue world changes into `commands`; replay is caller-owned.
  virtual ScriptDispatchSummary dispatch_hook(const vkpt::scene::SceneWorld& world,
                                             ScriptLifecycleHook hook,
                                             const ScriptExecutionContext& context,
                                             vkpt::scene::WorldCommandBuffer& commands) = 0;
  /// Snapshot-style dispatch helper. It captures hook output in an immutable
  /// value so simulation code can replay commands through its normal conflict
  /// resolution path instead of sharing a mutable command buffer.
  virtual ScriptCommandSnapshot dispatch_hook_snapshot(
      const vkpt::scene::SceneWorld& world,
      ScriptLifecycleHook hook,
      const ScriptExecutionContext& context) {
    ScriptCommandSnapshot snapshot;
    snapshot.generation = context.frame;
    snapshot.hook = hook;
    snapshot.commands.set_flow_id(context.frame);
    snapshot.diagnostics = dispatch_hook(world, hook, context, snapshot.commands);
    return snapshot;
  }
  virtual const std::vector<ScriptBinding>& bindings() const = 0;
  virtual const std::vector<ScriptDiagnostic>& diagnostics() const = 0;
  virtual const std::vector<ScriptVariableSnapshot>& variable_snapshots() const = 0;
  virtual bool set_variable_override(vkpt::core::StableEntityId entity,
                                     std::string_view scope,
                                     std::string_view name,
                                     std::string_view value) = 0;
  virtual void clear_variable_overrides(vkpt::core::StableEntityId entity = 0) = 0;
  virtual std::vector<ScriptVariableOverride> variable_overrides() const = 0;
  virtual const std::vector<ScriptBindingRuntimeState>& runtime_states() const = 0;
  virtual ScriptingStatus status() const = 0;
  virtual std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const = 0;
  virtual bool lua_compiled_in() const = 0;
  virtual bool execution_available() const = 0;
  virtual std::size_t lua_state_count() const = 0;
  virtual std::size_t lua_states_created_total() const = 0;
};

class LuaStatePool {
 public:
  struct Impl;

  LuaStatePool();
  ~LuaStatePool();

  LuaStatePool(const LuaStatePool&) = delete;
  LuaStatePool& operator=(const LuaStatePool&) = delete;

  void clear();
  std::size_t state_count() const;
  std::size_t created_total() const;
  Impl& impl();
  const Impl& impl() const;

 private:
  std::unique_ptr<Impl> m_impl;
  friend class EcsScriptRuntime;
};

class ScriptCommandQueue {
 public:
  explicit ScriptCommandQueue(std::size_t capacity = 4096u);
  ~ScriptCommandQueue();

  ScriptCommandQueue(const ScriptCommandQueue&) = delete;
  ScriptCommandQueue& operator=(const ScriptCommandQueue&) = delete;

  bool push_scene_command(const vkpt::scene::WorldCommandBuffer::Command& command);
  std::size_t drain_to(vkpt::scene::WorldCommandBuffer& commands);
  std::size_t dropped_total() const;
  void clear();

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

class EcsScriptRuntime final : public IScriptRuntime {
 public:
  EcsScriptRuntime();
  ~EcsScriptRuntime() override;

  ScriptBindingSummary reload_bindings(const vkpt::scene::SceneWorld& world) override;
  ScriptDispatchSummary dispatch_hook(const vkpt::scene::SceneWorld& world,
                                      ScriptLifecycleHook hook,
                                      const ScriptExecutionContext& context,
                                      vkpt::scene::WorldCommandBuffer& commands) override;
  const std::vector<ScriptBinding>& bindings() const override;
  const std::vector<ScriptDiagnostic>& diagnostics() const override;
  const std::vector<ScriptVariableSnapshot>& variable_snapshots() const override;
  bool set_variable_override(vkpt::core::StableEntityId entity,
                             std::string_view scope,
                             std::string_view name,
                             std::string_view value) override;
  void clear_variable_overrides(vkpt::core::StableEntityId entity = 0) override;
  std::vector<ScriptVariableOverride> variable_overrides() const override;
  const std::vector<ScriptBindingRuntimeState>& runtime_states() const override;
  ScriptingStatus status() const override;
  std::shared_ptr<vkpt::core::health::IHealthProbe> create_health_probe() const override;
  bool lua_compiled_in() const override;
  bool execution_available() const override;
  std::size_t lua_state_count() const override;
  std::size_t lua_states_created_total() const override;

 private:
  std::vector<ScriptBinding> m_bindings;
  std::vector<ScriptDiagnostic> m_diagnostics;
  std::vector<ScriptVariableSnapshot> m_variable_snapshots;
  std::vector<ScriptBindingRuntimeState> m_runtime_states;
  std::unordered_set<std::string> m_disabled_until_reload;
  std::unordered_map<std::string, std::string> m_lua_bytecode_cache;
  std::unordered_map<std::string, ScriptVariableOverride> m_variable_overrides;
  std::unique_ptr<LuaStatePool> m_lua_state_pool;
  std::unique_ptr<ScriptCommandQueue> m_script_command_queue;
  std::unique_ptr<vkpt::jobs::JobSystem> m_pure_job_system;
  ScriptingStatus m_status;
  mutable std::mutex m_runtime_mutex;
  mutable std::mutex m_lua_cache_mutex;
};

/// Scan the world in stable entity order and create runnable script bindings.
std::vector<ScriptBinding> BuildScriptBindings(const vkpt::scene::SceneWorld& world);
/// Read `-- @editor ...` annotations from binding sources and attach editor metadata.
void ApplyScriptEditorAnnotations(std::vector<ScriptBinding>& bindings);
/// Count binding states without touching runtime caches or executing scripts.
ScriptBindingSummary SummarizeScriptBindings(const std::vector<ScriptBinding>& bindings,
                                             bool lua_compiled_in,
                                             bool execution_available);
/// Create the default ECS-backed script runtime.
std::unique_ptr<IScriptRuntime> CreateScriptRuntime();
/// Format the current runtime bindings/states for a future `script list` REPL command.
std::string FormatScriptList(const IScriptRuntime& runtime);

template <typename HealthRegistryT>
void RegisterScriptingHealthProbe(IScriptRuntime& runtime, HealthRegistryT& registry) {
  registry.register_probe(runtime.create_health_probe());
}

std::string_view to_string(ScriptLifecycleHook hook);
std::string_view to_string(ScriptDiagnosticSeverity severity);

}  // namespace vkpt::scripting
