#include "benchmark/BenchmarkTraceProfiler.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace vkpt::benchmark::ptbench {

vkpt::benchmark::ProfilerEventHandle TraceProfiler::begin_event(
    vkpt::benchmark::ProfilerEventKind kind,
    std::string_view name,
    std::string_view category,
    uint32_t thread_id) {
  ActiveEvent active;
  active.handle = m_nextHandle++;
  active.event.kind = kind;
  active.event.name = std::string(name);
  active.event.category = std::string(category);
  active.event.thread_id = thread_id;
  active.event.start_ms = elapsed_ms();
  m_active.push_back(std::move(active));
  return m_active.back().handle;
}

void TraceProfiler::end_event(vkpt::benchmark::ProfilerEventHandle handle) {
  const auto now = elapsed_ms();
  const auto it = std::find_if(m_active.begin(), m_active.end(), [&](const ActiveEvent& active) {
    return active.handle == handle;
  });
  if (it == m_active.end()) {
    return;
  }
  auto event = it->event;
  event.duration_ms = std::max(0.0, now - event.start_ms);
  m_events.push_back(std::move(event));
  m_active.erase(it);
}

std::string TraceProfiler::emit_trace() const {
  return vkpt::benchmark::SerializeProfilerTrace(m_events);
}

void TraceProfiler::reset_frame() {
  m_active.clear();
  m_events.clear();
  m_origin = std::chrono::steady_clock::now();
}

vkpt::benchmark::ProfilerCapabilities TraceProfiler::describe_capabilities() const {
  return vkpt::benchmark::DefaultProfilerCapabilities();
}

double TraceProfiler::elapsed_ms() const {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_origin).count();
}

}  // namespace vkpt::benchmark::ptbench
