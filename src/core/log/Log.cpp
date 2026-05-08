#include "core/log/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cinttypes>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace vkpt::core::log {

namespace {

// Per-thread state lives until thread exit; the Logger keeps a list of weak
// references so the writer can drain them. The thread that created this state
// also owns the responsibility for de-registering it (on dtor).
struct alignas(64) ThreadStateImpl {
  vkpt::core::sync::SpscRing<LogEvent> ring;
  vkpt::core::sync::SpscRing<LogEvent> crash_ring;
  std::atomic<std::uint64_t> drops{0};
  std::string thread_name;
  std::uint64_t thread_id_hash = 0;
  std::unordered_map<const void*, std::uint64_t> sample_last_ns;
  bool registered = false;

  ThreadStateImpl(std::size_t ring_cap, std::size_t crash_cap)
      : ring(ring_cap), crash_ring(crash_cap) {}
};

std::uint64_t HashThreadId(std::thread::id id) noexcept {
  return std::hash<std::thread::id>{}(id);
}

constexpr std::string_view kAnsiReset = "\x1b[0m";
constexpr std::string_view kAnsiGray = "\x1b[90m";
constexpr std::string_view kAnsiRed = "\x1b[31m";
constexpr std::string_view kAnsiGreen = "\x1b[32m";
constexpr std::string_view kAnsiYellow = "\x1b[33m";
constexpr std::string_view kAnsiBlue = "\x1b[34m";
constexpr std::string_view kAnsiMagenta = "\x1b[35m";
constexpr std::string_view kAnsiCyan = "\x1b[36m";

std::string_view AnsiForLevel(Level lvl) noexcept {
  switch (lvl) {
    case Level::Trace: return kAnsiGray;
    case Level::Debug: return kAnsiCyan;
    case Level::Info:  return kAnsiGreen;
    case Level::Warn:  return kAnsiYellow;
    case Level::Error: return kAnsiRed;
    case Level::Fatal: return kAnsiMagenta;
  }
  return kAnsiBlue;
}

void AppendUInt(std::string& out, std::uint64_t v) {
  char buf[24];
  int n = std::snprintf(buf, sizeof(buf), "%" PRIu64, v);
  if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

void AppendInt(std::string& out, std::int64_t v) {
  char buf[24];
  int n = std::snprintf(buf, sizeof(buf), "%" PRId64, v);
  if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

void AppendDouble(std::string& out, double v) {
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "%.6g", v);
  if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

void AppendField(std::string& out, const Field& f, bool quote_strings) {
  switch (f.value.kind) {
    case FieldValue::Kind::Empty:
      out.append("null");
      break;
    case FieldValue::Kind::I64:
      AppendInt(out, f.value.storage.i64);
      break;
    case FieldValue::Kind::U64:
      AppendUInt(out, f.value.storage.u64);
      break;
    case FieldValue::Kind::F64:
      AppendDouble(out, f.value.storage.f64);
      break;
    case FieldValue::Kind::Bool:
      out.append(f.value.storage.b ? "true" : "false");
      break;
    case FieldValue::Kind::StrInline:
      if (quote_strings) out.push_back('"');
      out.append(f.value.storage.str_inline);
      if (quote_strings) out.push_back('"');
      break;
    case FieldValue::Kind::StrHeap:
      if (quote_strings) out.push_back('"');
      out.append(f.value.str_heap);
      if (quote_strings) out.push_back('"');
      break;
  }
}

void EscapeJsonInto(std::string& out, std::string_view in) {
  for (char c : in) {
    switch (c) {
      case '"':  out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n");  break;
      case '\r': out.append("\\r");  break;
      case '\t': out.append("\\t");  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out.append(buf);
        } else {
          out.push_back(c);
        }
    }
  }
}

void AppendJsonField(std::string& out, const Field& f) {
  switch (f.value.kind) {
    case FieldValue::Kind::Empty:
      out.append("null");
      break;
    case FieldValue::Kind::I64:
      AppendInt(out, f.value.storage.i64);
      break;
    case FieldValue::Kind::U64:
      AppendUInt(out, f.value.storage.u64);
      break;
    case FieldValue::Kind::F64:
      AppendDouble(out, f.value.storage.f64);
      break;
    case FieldValue::Kind::Bool:
      out.append(f.value.storage.b ? "true" : "false");
      break;
    case FieldValue::Kind::StrInline:
      out.push_back('"');
      EscapeJsonInto(out, f.value.storage.str_inline);
      out.push_back('"');
      break;
    case FieldValue::Kind::StrHeap:
      out.push_back('"');
      EscapeJsonInto(out, f.value.str_heap);
      out.push_back('"');
      break;
  }
}

}  // namespace

const char* LevelName(Level lvl) noexcept {
  switch (lvl) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info:  return "info";
    case Level::Warn:  return "warn";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
  }
  return "info";
}

std::optional<Level> ParseLevel(std::string_view name) {
  if (name == "trace") return Level::Trace;
  if (name == "debug") return Level::Debug;
  if (name == "info")  return Level::Info;
  if (name == "warn" || name == "warning") return Level::Warn;
  if (name == "error") return Level::Error;
  if (name == "fatal") return Level::Fatal;
  return std::nullopt;
}

std::optional<Format> ParseFormat(std::string_view name) {
  if (name == "console") return Format::Console;
  if (name == "kv")      return Format::Kv;
  if (name == "json")    return Format::Json;
  return std::nullopt;
}

// ----- Sinks -----------------------------------------------------------------

StreamSink::StreamSink(Stream s) : m_stream(s) {}

void StreamSink::write(std::string_view formatted) {
  std::FILE* f = (m_stream == Stream::Stderr) ? stderr : stdout;
  std::fwrite(formatted.data(), 1, formatted.size(), f);
}

void StreamSink::flush() {
  std::FILE* f = (m_stream == Stream::Stderr) ? stderr : stdout;
  std::fflush(f);
}

FileSink::FileSink(std::string path) : m_path(std::move(path)) {
  std::FILE* f = std::fopen(m_path.c_str(), "ab");
  m_handle = f;
}

FileSink::~FileSink() {
  if (m_handle) {
    std::fflush(static_cast<std::FILE*>(m_handle));
    std::fclose(static_cast<std::FILE*>(m_handle));
  }
}

void FileSink::write(std::string_view formatted) {
  std::FILE* f = static_cast<std::FILE*>(m_handle);
  if (!f) {
    // Fallback so we never silently swallow events.
    std::fwrite(formatted.data(), 1, formatted.size(), stderr);
    return;
  }
  std::fwrite(formatted.data(), 1, formatted.size(), f);
}

void FileSink::flush() {
  if (m_handle) std::fflush(static_cast<std::FILE*>(m_handle));
}

// ----- Logger ----------------------------------------------------------------

struct Logger::ThreadState {
  ThreadStateImpl impl;

  ThreadState(std::size_t ring_cap, std::size_t crash_cap)
      : impl(ring_cap, crash_cap) {}
};

namespace {

struct ThreadRegistration {
  Logger::ThreadState* ts = nullptr;
  Logger* owner = nullptr;
  ~ThreadRegistration();
};

thread_local std::unique_ptr<ThreadRegistration> tls_reg;

}  // namespace

Logger& Logger::instance() {
  // Intentionally process-lifetime: thread-local registrations keep a back
  // pointer to the logger and may be destroyed after function-local statics on
  // Windows. Explicit obs::Shutdown()/Logger::shutdown() still drains the sink.
  static Logger* inst = new Logger();
  return *inst;
}

Logger::~Logger() { shutdown(); }

void Logger::start(Config cfg) {
  bool expected = false;
  if (!m_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  m_cfg = cfg;
  m_min_level.store(cfg.min_level, std::memory_order_release);
  m_format.store(cfg.format, std::memory_order_release);
  m_stop.store(false, std::memory_order_release);
  if (!m_sink) {
    m_sink = std::make_unique<StreamSink>(StreamSink::Stream::Stdout);
  }
  m_writer = std::thread(&Logger::writer_loop, this);
}

void Logger::shutdown() {
  bool was_started = m_started.exchange(false, std::memory_order_acq_rel);
  if (!was_started) return;
  m_stop.store(true, std::memory_order_release);
  if (m_writer.joinable()) m_writer.join();
  flush_for_test();  // best-effort drain on shutdown
  std::scoped_lock lk(m_sink_mutex);
  if (m_sink) m_sink->flush();
}

void Logger::set_sink(std::unique_ptr<ISink> sink) {
  std::scoped_lock lk(m_sink_mutex);
  m_sink = std::move(sink);
}

void Logger::add_verbosity_override(VerbosityOverride ov) {
  std::scoped_lock lk(m_overrides_mutex);
  m_overrides.push_back(std::move(ov));
}

void Logger::clear_verbosity_overrides() {
  std::scoped_lock lk(m_overrides_mutex);
  m_overrides.clear();
}

Level Logger::effective_level(const char* component, const char* event) const noexcept {
  Level base = m_min_level.load(std::memory_order_acquire);
  std::scoped_lock lk(m_overrides_mutex);
  for (const auto& ov : m_overrides) {
    if (ov.component != component) continue;
    if (!ov.event_prefix.empty()) {
      const std::string_view ev_sv(event);
      if (ev_sv.substr(0, ov.event_prefix.size()) != ov.event_prefix) continue;
    }
    if (ov.level < base) base = ov.level;
  }
  return base;
}

bool Logger::enabled(Level lvl, const char* component, const char* event) const noexcept {
  return lvl >= effective_level(component, event);
}

Logger::ThreadState& Logger::thread_state() {
  if (!tls_reg) {
    auto reg = std::make_unique<ThreadRegistration>();
    auto ts = new ThreadState(m_cfg.per_thread_ring_capacity > 0
                                  ? m_cfg.per_thread_ring_capacity
                                  : 4096,
                              m_cfg.per_thread_crash_ring_capacity > 0
                                  ? m_cfg.per_thread_crash_ring_capacity
                                  : 1024);
    ts->impl.thread_id_hash = HashThreadId(std::this_thread::get_id());
    reg->ts = ts;
    reg->owner = this;
    {
      std::scoped_lock lk(m_threads_mutex);
      m_threads.push_back(ts);
    }
    ts->impl.registered = true;
    tls_reg = std::move(reg);
  }
  return *tls_reg->ts;
}

ThreadRegistration::~ThreadRegistration() {
  // Drain remaining events via the writer? Cheaper: leave them; the writer is
  // about to be torn down too. Just remove our entry from the owner list.
  if (!owner || !ts) return;
  owner->unregister_thread_state(ts);
  delete ts;
}

void Logger::unregister_thread_state(ThreadState* ts) {
  if (ts == nullptr) return;
  if (m_started.load(std::memory_order_acquire)) {
    for (int i = 0; i < 1000 && ts->impl.ring.depth() > 0; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  // Drain the thread's crash ring into the global graveyard so dump survives
  // thread death. Cap at crash ring capacity to avoid unbounded growth.
  {
    std::scoped_lock lk(m_threads_mutex);
    auto it = std::find(m_threads.begin(), m_threads.end(), ts);
    if (it != m_threads.end()) m_threads.erase(it);
    LogEvent ev;
    while (ts->impl.crash_ring.try_pop(ev)) {
      if (m_graveyard.size() >= 16384) m_graveyard.erase(m_graveyard.begin());
      m_graveyard.push_back(std::move(ev));
    }
  }
}

bool Logger::push(LogEvent ev) noexcept {
  ThreadState& ts = thread_state();
  ev.thread_id_hash = ts.impl.thread_id_hash;
  if (!ts.impl.thread_name.empty()) ev.thread_name = ts.impl.thread_name;
  // Keep a copy on the crash ring (last-N). We must clone before push moves.
  LogEvent crash_copy = ev;
  ts.impl.crash_ring.try_push(std::move(crash_copy));
  if (!ts.impl.ring.try_push(std::move(ev))) {
    ts.impl.drops.fetch_add(1, std::memory_order_relaxed);
    m_total_drops.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  m_total_emitted.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool Logger::sample_ok(const void* site_token, std::uint64_t sample_period_ns) noexcept {
  ThreadState& ts = thread_state();
  const std::uint64_t now = detail::NowNs();
  auto it = ts.impl.sample_last_ns.find(site_token);
  if (it == ts.impl.sample_last_ns.end()) {
    ts.impl.sample_last_ns.emplace(site_token, now);
    return true;
  }
  if (now - it->second >= sample_period_ns) {
    it->second = now;
    return true;
  }
  return false;
}

void Logger::set_thread_name(std::string name) noexcept {
  ThreadState& ts = thread_state();
  ts.impl.thread_name = std::move(name);
}

std::uint64_t Logger::thread_drop_count() const noexcept {
  if (!tls_reg || !tls_reg->ts) return 0;
  return tls_reg->ts->impl.drops.load(std::memory_order_relaxed);
}

void Logger::format_event(const LogEvent& ev, std::string& out) const {
  switch (m_format.load(std::memory_order_acquire)) {
    case Format::Console: format_event_console(ev, out); break;
    case Format::Kv:      format_event_kv(ev, out); break;
    case Format::Json:    format_event_json(ev, out); break;
  }
}

void Logger::format_event_console(const LogEvent& ev, std::string& out) const {
  AppendUInt(out, ev.ts_ns);
  out.push_back(' ');
  out.append(AnsiForLevel(ev.level));
  out.push_back('[');
  out.append(LevelName(ev.level));
  out.push_back(']');
  out.append(kAnsiReset);
  out.push_back(' ');
  out.append(ev.thread_name.empty() ? "?" : ev.thread_name);
  out.push_back('/');
  out.append(ev.component);
  out.push_back('.');
  out.append(ev.event);
  for (std::uint8_t i = 0; i < ev.field_count; ++i) {
    out.push_back(' ');
    out.append(ev.fields[i].key);
    out.push_back('=');
    AppendField(out, ev.fields[i], false);
  }
  if (ev.coalesced > 1) {
    out.append(" coalesced=");
    AppendUInt(out, ev.coalesced);
  }
  out.push_back('\n');
}

void Logger::format_event_kv(const LogEvent& ev, std::string& out) const {
  out.append("ts=");
  AppendUInt(out, ev.ts_ns);
  out.append(" lvl=");
  out.append(LevelName(ev.level));
  out.append(" thr=");
  if (!ev.thread_name.empty()) {
    out.append(ev.thread_name);
  } else {
    AppendUInt(out, ev.thread_id_hash);
  }
  out.append(" comp=");
  out.append(ev.component);
  out.append(" ev=");
  out.append(ev.event);
  for (std::uint8_t i = 0; i < ev.field_count; ++i) {
    out.push_back(' ');
    out.append(ev.fields[i].key);
    out.push_back('=');
    AppendField(out, ev.fields[i], false);
  }
  if (ev.coalesced > 1) {
    out.append(" coalesced=");
    AppendUInt(out, ev.coalesced);
  }
  out.push_back('\n');
}

void Logger::format_event_json(const LogEvent& ev, std::string& out) const {
  out.push_back('{');
  out.append("\"ts\":");
  AppendUInt(out, ev.ts_ns);
  out.append(",\"lvl\":\"");
  out.append(LevelName(ev.level));
  out.append("\",\"thr\":\"");
  if (!ev.thread_name.empty()) {
    EscapeJsonInto(out, ev.thread_name);
  } else {
    AppendUInt(out, ev.thread_id_hash);
  }
  out.append("\",\"comp\":\"");
  EscapeJsonInto(out, ev.component);
  out.append("\",\"ev\":\"");
  EscapeJsonInto(out, ev.event);
  out.push_back('"');
  for (std::uint8_t i = 0; i < ev.field_count; ++i) {
    out.append(",\"");
    EscapeJsonInto(out, ev.fields[i].key);
    out.append("\":");
    AppendJsonField(out, ev.fields[i]);
  }
  if (ev.coalesced > 1) {
    out.append(",\"coalesced\":");
    AppendUInt(out, ev.coalesced);
  }
  out.append("}\n");
}

void Logger::writer_loop() {
  std::string buf;
  buf.reserve(1024);

  // Burst-collapse state: lookup keyed by (component, event) literal pointers.
  struct CollapseKey {
    const char* component;
    const char* event;
    bool operator==(const CollapseKey& o) const {
      return component == o.component && event == o.event;
    }
  };
  struct CollapseKeyHash {
    std::size_t operator()(const CollapseKey& k) const noexcept {
      auto h = std::hash<const void*>{}(k.component);
      return h ^ (std::hash<const void*>{}(k.event) << 1);
    }
  };
  struct CollapseEntry {
    LogEvent pending;
    std::uint32_t count = 0;
    std::uint64_t window_start_ns = 0;
    bool pending_valid = false;
  };
  std::unordered_map<CollapseKey, CollapseEntry, CollapseKeyHash> collapse;

  const std::uint32_t threshold = m_cfg.burst_collapse_threshold_per_sec > 0
                                      ? m_cfg.burst_collapse_threshold_per_sec
                                      : 64;

  auto flush_pending = [&](CollapseEntry& e) {
    if (e.count == 0) return;
    if (!e.pending_valid) {
      e.count = 0;
      return;
    }
    e.pending.coalesced = e.count;
    buf.clear();
    format_event(e.pending, buf);
    {
      std::scoped_lock lk(m_sink_mutex);
      if (m_sink) m_sink->write(buf);
    }
    e.count = 0;
    e.pending_valid = false;
  };

  auto handle = [&](LogEvent& ev) {
    CollapseKey key{ev.component, ev.event};
    auto it = collapse.find(key);
    const std::uint64_t window_ns = 1'000'000'000ull;
    if (it == collapse.end()) {
      // First sighting in window — emit immediately.
      buf.clear();
      format_event(ev, buf);
      {
        std::scoped_lock lk(m_sink_mutex);
        if (m_sink) m_sink->write(buf);
      }
      collapse.emplace(key, CollapseEntry{LogEvent{}, 0u, ev.ts_ns});
      return;
    }
    auto& e = it->second;
    if (ev.ts_ns - e.window_start_ns >= window_ns) {
      flush_pending(e);
      buf.clear();
      format_event(ev, buf);
      {
        std::scoped_lock lk(m_sink_mutex);
        if (m_sink) m_sink->write(buf);
      }
      e.window_start_ns = ev.ts_ns;
      e.count = 0;
      e.pending_valid = false;
      return;
    }
    // Within the window: count, and if above threshold start coalescing.
    if (e.count == 0) {
      // First repeat: emit immediately too unless we're past threshold for the
      // window. Track count to detect threshold.
      buf.clear();
      format_event(ev, buf);
      {
        std::scoped_lock lk(m_sink_mutex);
        if (m_sink) m_sink->write(buf);
      }
      e.count = 0;  // not coalescing yet
    }
    // Approximate threshold tracking: coalesce after N occurrences within
    // the window. We track the running count via collapse; reset on flush.
    e.count = std::min<std::uint32_t>(e.count + 1u, 1'000'000u);
    if (e.count >= threshold) {
      e.pending = ev;  // sample; the timestamp moves forward with the bursts
      e.pending_valid = true;
    }
  };

  while (true) {
    bool any = false;
    std::vector<ThreadState*> threads_snapshot;
    {
      std::scoped_lock lk(m_threads_mutex);
      threads_snapshot = m_threads;
    }
    for (auto* ts : threads_snapshot) {
      LogEvent ev;
      while (ts->impl.ring.try_pop(ev)) {
        handle(ev);
        any = true;
      }
    }
    // Periodic flush of coalesced entries even if quiet.
    const std::uint64_t now = detail::NowNs();
    for (auto& [_, e] : collapse) {
      if (e.count >= threshold && now - e.window_start_ns >= 1'000'000'000ull) {
        flush_pending(e);
        e.window_start_ns = now;
      }
    }
    if (!any) {
      if (m_stop.load(std::memory_order_acquire)) {
        // Drain one more time then exit.
        bool drained_any = false;
        {
          std::scoped_lock lk(m_threads_mutex);
          threads_snapshot = m_threads;
        }
        for (auto* ts : threads_snapshot) {
          LogEvent ev;
          while (ts->impl.ring.try_pop(ev)) {
            handle(ev);
            drained_any = true;
          }
        }
        if (!drained_any) break;
      } else {
        std::this_thread::sleep_for(m_cfg.writer_idle_sleep);
      }
    }
  }

  // Final flush of any pending coalesced bursts.
  for (auto& [_, e] : collapse) flush_pending(e);
  std::scoped_lock lk(m_sink_mutex);
  if (m_sink) m_sink->flush();
}

void Logger::flush_for_test() {
  // Best-effort: yield repeatedly until both rings appear empty across all
  // threads. Caller must not be inside a log call site.
  for (int i = 0; i < 1000; ++i) {
    bool any = false;
    {
      std::scoped_lock lk(m_threads_mutex);
      for (auto* ts : m_threads) {
        if (ts->impl.ring.depth() > 0) { any = true; break; }
      }
    }
    if (!any) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::scoped_lock lk(m_sink_mutex);
  if (m_sink) m_sink->flush();
}

std::vector<LogEvent> Logger::dump_crash_rings() const {
  std::vector<LogEvent> out;
  std::scoped_lock lk(m_threads_mutex);
  for (const auto& ev : m_graveyard) out.push_back(ev);
  for (auto* ts : m_threads) {
    LogEvent ev;
    while (ts->impl.crash_ring.try_pop(ev)) {
      out.push_back(std::move(ev));
    }
  }
  std::sort(out.begin(), out.end(),
            [](const LogEvent& a, const LogEvent& b) { return a.ts_ns < b.ts_ns; });
  return out;
}

void Logger::emergency_dump() noexcept {
  // Async-signal-safe-ish: avoid mutex acquisition when possible. We do hold
  // the threads mutex briefly because the alternative is unsafe iteration.
  // This is acceptable because the process is about to exit and the writer is
  // either dead or quiescent.
  std::vector<ThreadState*> snapshot;
  std::vector<LogEvent> graveyard_copy;
  {
    std::scoped_lock lk(m_threads_mutex);
    snapshot = m_threads;
    graveyard_copy = m_graveyard;
  }
  std::string buf;
  buf.reserve(8192);
  buf.append("==== vkpt crash ring dump ====\n");
  std::vector<LogEvent> all;
  for (auto& ev : graveyard_copy) all.push_back(std::move(ev));
  for (auto* ts : snapshot) {
    LogEvent ev;
    while (ts->impl.crash_ring.try_pop(ev)) {
      all.push_back(std::move(ev));
    }
  }
  std::sort(all.begin(), all.end(),
            [](const LogEvent& a, const LogEvent& b) { return a.ts_ns < b.ts_ns; });
  for (const auto& ev : all) {
    format_event_json(ev, buf);
  }
  buf.append("==== end crash ring dump ====\n");
  std::fwrite(buf.data(), 1, buf.size(), stderr);
  std::fflush(stderr);
}

namespace detail {
std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
}  // namespace detail

}  // namespace vkpt::core::log
