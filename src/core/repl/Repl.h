#pragma once

// Minimal REPL surface for runtime introspection. Commands are registered by
// subsystems and dispatched by line. Built-ins are wired by RegisterBuiltins:
//
//   metrics dump [prefix]
//   metrics reset [prefix]
//   events tail <comp>           (informational; tailing is via --verbose)
//   events dump-rings            (flushes per-thread crash rings to stderr)
//   health                       (current probe statuses)
//   tracer pause | tracer resume (left as no-ops here; Track B/D wires)
//   snapshot dump current        (no-op until Track B lands SnapshotRing)
//   script list                  (prints the registered script runtime panel)
//   help
//
// Reading a line from stdin is delegated to the host application; this module
// only handles dispatch. A blocking input thread is provided as an optional
// helper for headless runs.

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vkpt::core::repl {

using HandlerFn = std::function<std::string(const std::vector<std::string>& args)>;
using ScriptListProviderFn = std::function<std::string()>;
using StatusProviderFn = std::function<std::string()>;

class Repl {
 public:
  static Repl& instance();

  void register_command(std::string name, std::string help, HandlerFn fn);

  // Dispatch a line. Returns the textual result, or an error string.
  std::string dispatch(std::string_view line);

  // Spawn an input loop that reads stdin and dispatches each non-empty line.
  // Stops when stdin closes or stop() is called.
  void start_stdin_loop();
  void stop();

  // Built-in command set. Idempotent.
  void register_builtins();

  // Provide the backing panel text for `script list`. Hosts that own an
  // IScriptRuntime should set this to FormatScriptList(*runtime).
  void set_script_list_provider(ScriptListProviderFn fn);
  // Register a uniform `<component> status` provider. Passing an empty function
  // unregisters the component.
  void set_status_provider(std::string component, StatusProviderFn fn);

  std::string help_text() const;

 private:
  Repl() = default;
  ~Repl() { stop(); }
  Repl(const Repl&) = delete;
  Repl& operator=(const Repl&) = delete;

  void stdin_loop();
  std::string script_list_text() const;
  std::optional<std::string> component_status_text(std::string_view component) const;

  struct Command {
    std::string name;
    std::string help;
    HandlerFn fn;
  };
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, Command> m_commands;
  ScriptListProviderFn m_scriptListProvider;
  std::unordered_map<std::string, StatusProviderFn> m_statusProviders;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
};

}  // namespace vkpt::core::repl
