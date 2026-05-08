#pragma once

// Tracing primitives for the snapshot-bus architecture (SYSTEM.md Phase 0.4).
//
//   * VKP_TRACE_SCOPE("comp", "name") — RAII scope timer. Records duration
//     into the histogram metric vkp.<comp>.<name>_us and, when tracing is
//     enabled, also pushes a Chrome-tracing-format event into the trace
//     recorder.
//   * VKP_TRACE_FLOW("flow_id_ull") — attaches a snapshot/generation flow ID
//     to every VKP_LOG event emitted within the scope. Lets agents stitch
//     events from different threads into a single causal flow.
//   * The trace recorder is opt-in (CLI: --trace=<comp>[,...]) and writes to
//     a Chrome-tracing JSON file on shutdown (--trace-out=<path>) so flame
//     graphs can be viewed in chrome://tracing or Perfetto.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace vkpt::core::trace {

class TraceRecorder {
 public:
  static TraceRecorder& instance();

  // Enable trace capture for a specific component name. May be called any
  // time. Disable with disable_component() or clear_components().
  void enable_component(std::string component);
  void disable_component(std::string_view component);
  void clear_components();
  bool is_enabled(std::string_view component) const noexcept;

  // Set the output path. If non-empty, dump_chrome() is called on shutdown.
  void set_output_path(std::string path);

  // Recording API used by VKP_TRACE_SCOPE.
  void record(std::string_view component, std::string_view name,
              std::uint64_t start_ns, std::uint64_t end_ns) noexcept;

  // Manual export (idempotent — caller may write/discard freely).
  bool dump_chrome(const std::string& path) const;

  void shutdown();  // dumps to set_output_path() if set, then clears events.

  // Test helper.
  std::size_t event_count() const noexcept;
  void reset();

 private:
  TraceRecorder() = default;
  ~TraceRecorder() { shutdown(); }
  TraceRecorder(const TraceRecorder&) = delete;
  TraceRecorder& operator=(const TraceRecorder&) = delete;

  struct Event {
    std::string component;
    std::string name;
    std::uint64_t start_ns;
    std::uint64_t dur_ns;
    std::uint64_t thread_id_hash;
  };

  mutable std::mutex m_mutex;
  std::unordered_set<std::string> m_enabled_components;
  std::vector<Event> m_events;
  std::string m_output_path;
};

// Per-thread current flow ID. VKP_LOG can read this to tag events.
namespace flow {
std::uint64_t current_id() noexcept;
void push_id(std::uint64_t id) noexcept;
void pop_id() noexcept;

class ScopedId {
 public:
  explicit ScopedId(std::uint64_t id) noexcept { push_id(id); }
  ~ScopedId() noexcept { pop_id(); }
  ScopedId(const ScopedId&) = delete;
  ScopedId& operator=(const ScopedId&) = delete;
};
}  // namespace flow

// RAII scope timer.
class ScopeTimer {
 public:
  ScopeTimer(const char* component, const char* name) noexcept;
  ~ScopeTimer() noexcept;

  ScopeTimer(const ScopeTimer&) = delete;
  ScopeTimer& operator=(const ScopeTimer&) = delete;

 private:
  const char* m_component;
  const char* m_name;
  std::uint64_t m_start_ns;
  bool m_recording;  // captured at start to avoid mid-scope toggling
};

}  // namespace vkpt::core::trace

// Tokens are concatenated to make the histogram metric name vkp.<comp>.<name>_us.
#define VKP_TRACE_SCOPE(comp_lit, name_lit) \
  ::vkpt::core::trace::ScopeTimer _vkp_scope_##__LINE__((comp_lit), (name_lit))

#define VKP_TRACE_FLOW(flow_id_u64) \
  ::vkpt::core::trace::flow::ScopedId _vkp_flow_##__LINE__(static_cast<std::uint64_t>(flow_id_u64))
