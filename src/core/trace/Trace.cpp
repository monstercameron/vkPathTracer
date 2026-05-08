#include "core/trace/Trace.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "core/metrics/Metrics.h"

namespace vkpt::core::trace {

namespace {

std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::uint64_t HashThreadId() noexcept {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

void EscapeJson(std::string& out, std::string_view in) {
  for (char c : in) {
    switch (c) {
      case '"':  out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n");  break;
      case '\r': out.append("\\r");  break;
      case '\t': out.append("\\t");  break;
      default:   out.push_back(c);
    }
  }
}

}  // namespace

namespace flow {

namespace {
thread_local std::vector<std::uint64_t> tls_flow_stack;
}

std::uint64_t current_id() noexcept {
  return tls_flow_stack.empty() ? 0 : tls_flow_stack.back();
}

void push_id(std::uint64_t id) noexcept {
  tls_flow_stack.push_back(id);
}

void pop_id() noexcept {
  if (!tls_flow_stack.empty()) tls_flow_stack.pop_back();
}

}  // namespace flow

TraceRecorder& TraceRecorder::instance() {
  static TraceRecorder* inst = new TraceRecorder();
  return *inst;
}

void TraceRecorder::enable_component(std::string component) {
  std::scoped_lock lk(m_mutex);
  m_enabled_components.insert(std::move(component));
}

void TraceRecorder::disable_component(std::string_view component) {
  std::scoped_lock lk(m_mutex);
  m_enabled_components.erase(std::string(component));
}

void TraceRecorder::clear_components() {
  std::scoped_lock lk(m_mutex);
  m_enabled_components.clear();
}

bool TraceRecorder::is_enabled(std::string_view component) const noexcept {
  std::scoped_lock lk(m_mutex);
  if (m_enabled_components.empty()) return false;
  return m_enabled_components.find(std::string(component)) != m_enabled_components.end();
}

void TraceRecorder::set_output_path(std::string path) {
  std::scoped_lock lk(m_mutex);
  m_output_path = std::move(path);
}

void TraceRecorder::record(std::string_view component, std::string_view name,
                           std::uint64_t start_ns, std::uint64_t end_ns) noexcept {
  Event ev;
  ev.component.assign(component);
  ev.name.assign(name);
  ev.start_ns = start_ns;
  ev.dur_ns = end_ns > start_ns ? end_ns - start_ns : 0;
  ev.thread_id_hash = HashThreadId();
  std::scoped_lock lk(m_mutex);
  m_events.push_back(std::move(ev));
}

bool TraceRecorder::dump_chrome(const std::string& path) const {
  std::vector<Event> snapshot;
  {
    std::scoped_lock lk(m_mutex);
    snapshot = m_events;
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::string buf;
  buf.reserve(snapshot.size() * 128 + 16);
  buf.push_back('[');
  bool first = true;
  for (const auto& e : snapshot) {
    if (!first) buf.push_back(',');
    first = false;
    buf.append("{\"name\":\"");
    EscapeJson(buf, e.name);
    buf.append("\",\"cat\":\"");
    EscapeJson(buf, e.component);
    buf.append("\",\"ph\":\"X\",\"pid\":1,\"tid\":");
    char tid_buf[24];
    std::snprintf(tid_buf, sizeof(tid_buf), "%" PRIu64, e.thread_id_hash);
    buf.append(tid_buf);
    buf.append(",\"ts\":");
    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf), "%.3f", e.start_ns / 1000.0);
    buf.append(ts_buf);
    buf.append(",\"dur\":");
    char dur_buf[32];
    std::snprintf(dur_buf, sizeof(dur_buf), "%.3f", e.dur_ns / 1000.0);
    buf.append(dur_buf);
    buf.push_back('}');
  }
  buf.push_back(']');
  std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  return true;
}

void TraceRecorder::shutdown() {
  std::string path;
  {
    std::scoped_lock lk(m_mutex);
    path = m_output_path;
  }
  if (!path.empty()) dump_chrome(path);
  std::scoped_lock lk(m_mutex);
  m_events.clear();
}

std::size_t TraceRecorder::event_count() const noexcept {
  std::scoped_lock lk(m_mutex);
  return m_events.size();
}

void TraceRecorder::reset() {
  std::scoped_lock lk(m_mutex);
  m_events.clear();
}

ScopeTimer::ScopeTimer(const char* component, const char* name) noexcept
    : m_component(component), m_name(name), m_start_ns(NowNs()),
      m_recording(TraceRecorder::instance().is_enabled(component)) {}

ScopeTimer::~ScopeTimer() noexcept {
  const std::uint64_t end_ns = NowNs();
  const std::uint64_t dur_ns = end_ns - m_start_ns;

  // Always record into a histogram metric — cheap and useful even without
  // chrome-tracing enabled. Metric name: vkp.<comp>.<name>_us
  std::string metric_name;
  metric_name.reserve(20 + std::strlen(m_component) + std::strlen(m_name));
  metric_name.append("vkp.");
  metric_name.append(m_component);
  metric_name.push_back('.');
  metric_name.append(m_name);
  metric_name.append("_us");
  metrics::MetricsRegistry::instance().histogram(metric_name).record(dur_ns / 1000u);

  if (m_recording) {
    TraceRecorder::instance().record(m_component, m_name, m_start_ns, end_ns);
  }
}

}  // namespace vkpt::core::trace
