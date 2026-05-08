#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "core/Types.h"
#include "scene/Scene.h"

namespace vkpt::scripting {

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
  /// Global runtime gate. Disabled scripts are counted but hook bodies are not executed.
  bool scripts_enabled = true;
  /// Legacy game-mode latch. New hosts should prefer runtime.mode/runtime.scripts_running.
  bool game_mode = false;
  /// Benchmark mode blocks script execution unless allow_benchmark_scripts is also true.
  bool benchmark_mode = false;
  bool allow_benchmark_scripts = false;
  std::size_t instruction_budget = 250000;
  std::size_t memory_budget_bytes = 2 * 1024 * 1024;
  RuntimeState runtime;
  InputState input;
  EditorState editor;
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
  bool disabled_until_reload = false;
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
  std::vector<ScriptDiagnostic> diagnostics;
};

class IScriptRuntime {
 public:
  virtual ~IScriptRuntime() = default;

  /// Rebuild the binding list from ECS state and clear runtime script caches.
  virtual ScriptBindingSummary reload_bindings(const vkpt::scene::SceneWorld& world) = 0;
  /// Dispatch one lifecycle hook. Scripts enqueue world changes into `commands`; replay is caller-owned.
  virtual ScriptDispatchSummary dispatch_hook(const vkpt::scene::SceneWorld& world,
                                             ScriptLifecycleHook hook,
                                             const ScriptExecutionContext& context,
                                             vkpt::scene::WorldCommandBuffer& commands) = 0;
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
  virtual bool lua_compiled_in() const = 0;
  virtual bool execution_available() const = 0;
};

class EcsScriptRuntime final : public IScriptRuntime {
 public:
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
  bool lua_compiled_in() const override;
  bool execution_available() const override;

 private:
  std::vector<ScriptBinding> m_bindings;
  std::vector<ScriptDiagnostic> m_diagnostics;
  std::vector<ScriptVariableSnapshot> m_variable_snapshots;
  std::vector<ScriptBindingRuntimeState> m_runtime_states;
  std::unordered_set<std::string> m_disabled_until_reload;
  std::unordered_map<std::string, std::string> m_lua_bytecode_cache;
  std::unordered_map<std::string, ScriptVariableOverride> m_variable_overrides;
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

std::string_view to_string(ScriptLifecycleHook hook);
std::string_view to_string(ScriptDiagnosticSeverity severity);

}  // namespace vkpt::scripting
