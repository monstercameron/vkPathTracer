#pragma once

// Async, structured, lock-free observability logger for the snapshot-bus
// architecture (see SYSTEM.md, Track A — Phase 0.2).
//
// Producer side (any thread): VKP_LOG / VKP_LOG_RT / VKP_LOG_SAMPLED move a
// LogEvent into a per-thread SpscRing. No mutex, no I/O, no string formatting,
// no allocation in the steady state — events carry small inline typed fields
// and string-literal pointers for component/event/keys.
//
// Each thread also keeps a per-thread crash ring (last N events) so a signal
// handler can dump the final seconds before death.
//
// A single writer thread drains every per-thread ring and writes formatted
// records to the active sink. Burst collapse: identical (component, event)
// emitted >threshold/sec gets coalesced into one record carrying coalesced=N.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/sync/SpscRing.h"

namespace vkpt::core::log {

enum class Level : std::uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warn = 3,
  Error = 4,
  Fatal = 5,
};

enum class Format : std::uint8_t {
  Console,
  Kv,
  Json,
};

const char* LevelName(Level lvl) noexcept;
std::optional<Level> ParseLevel(std::string_view name);
std::optional<Format> ParseFormat(std::string_view name);

// Tagged value union for event fields. Strings up to 31 bytes are inlined to
// avoid heap allocation on the hot path.
struct FieldValue {
  enum class Kind : std::uint8_t { Empty, I64, U64, F64, Bool, StrInline, StrHeap };

  Kind kind = Kind::Empty;
  union Storage {
    Storage() : i64(0) {}
    ~Storage() {}
    std::int64_t i64;
    std::uint64_t u64;
    double f64;
    bool b;
    char str_inline[32];
  } storage;
  std::string str_heap;

  FieldValue() = default;
  FieldValue(const FieldValue& other) { copy_from(other); }
  FieldValue(FieldValue&& other) noexcept { move_from(std::move(other)); }
  FieldValue& operator=(const FieldValue& other) {
    if (this != &other) {
      reset();
      copy_from(other);
    }
    return *this;
  }
  FieldValue& operator=(FieldValue&& other) noexcept {
    if (this != &other) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }
  ~FieldValue() { reset(); }

  static FieldValue make_i64(std::int64_t v) {
    FieldValue f;
    f.kind = Kind::I64;
    f.storage.i64 = v;
    return f;
  }
  static FieldValue make_u64(std::uint64_t v) {
    FieldValue f;
    f.kind = Kind::U64;
    f.storage.u64 = v;
    return f;
  }
  static FieldValue make_f64(double v) {
    FieldValue f;
    f.kind = Kind::F64;
    f.storage.f64 = v;
    return f;
  }
  static FieldValue make_bool(bool v) {
    FieldValue f;
    f.kind = Kind::Bool;
    f.storage.b = v;
    return f;
  }
  static FieldValue make_str(std::string_view sv) {
    FieldValue f;
    if (sv.size() < sizeof(Storage::str_inline)) {
      f.kind = Kind::StrInline;
      std::memcpy(f.storage.str_inline, sv.data(), sv.size());
      f.storage.str_inline[sv.size()] = '\0';
    } else {
      f.kind = Kind::StrHeap;
      f.str_heap = std::string(sv);
    }
    return f;
  }

 private:
  void reset() {
    if (kind == Kind::StrHeap) {
      str_heap.clear();
      str_heap.shrink_to_fit();
    }
    kind = Kind::Empty;
  }
  void copy_from(const FieldValue& other) {
    kind = other.kind;
    storage = other.storage;
    if (kind == Kind::StrHeap) str_heap = other.str_heap;
  }
  void move_from(FieldValue&& other) noexcept {
    kind = other.kind;
    storage = other.storage;
    if (kind == Kind::StrHeap) str_heap = std::move(other.str_heap);
    other.kind = Kind::Empty;
  }
};

namespace detail {
template <typename T>
FieldValue MakeFieldValue(T&& v) {
  using D = std::decay_t<T>;
  if constexpr (std::is_same_v<D, bool>) {
    return FieldValue::make_bool(v);
  } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
    return FieldValue::make_i64(static_cast<std::int64_t>(v));
  } else if constexpr (std::is_integral_v<D> && std::is_unsigned_v<D>) {
    return FieldValue::make_u64(static_cast<std::uint64_t>(v));
  } else if constexpr (std::is_floating_point_v<D>) {
    return FieldValue::make_f64(static_cast<double>(v));
  } else if constexpr (std::is_same_v<D, FieldValue>) {
    return std::forward<T>(v);
  } else {
    return FieldValue::make_str(std::string_view(v));
  }
}
}  // namespace detail

constexpr std::size_t kMaxInlineFields = 8;

struct Field {
  const char* key = "";  // string literal lifetime expected
  FieldValue value;
};

struct LogEvent {
  std::uint64_t ts_ns = 0;
  Level level = Level::Info;
  const char* component = "";  // string literal lifetime
  const char* event = "";       // string literal lifetime
  std::array<Field, kMaxInlineFields> fields{};
  std::uint8_t field_count = 0;
  std::uint64_t thread_id_hash = 0;
  std::string thread_name;
  std::uint32_t coalesced = 1;
};

class ISink {
 public:
  virtual ~ISink() = default;
  virtual void write(std::string_view formatted) = 0;
  virtual void flush() {}
};

class StreamSink : public ISink {
 public:
  enum class Stream { Stdout, Stderr };
  explicit StreamSink(Stream s = Stream::Stdout);
  void write(std::string_view formatted) override;
  void flush() override;

 private:
  Stream m_stream;
};

class FileSink : public ISink {
 public:
  explicit FileSink(std::string path);
  ~FileSink() override;
  void write(std::string_view formatted) override;
  void flush() override;

 private:
  std::string m_path;
  void* m_handle = nullptr;  // opaque FILE*
};

struct Config {
  Level min_level = Level::Info;
  Format format = Format::Console;
  std::size_t per_thread_ring_capacity = 4096;
  std::size_t per_thread_crash_ring_capacity = 1024;
  std::uint32_t burst_collapse_threshold_per_sec = 64;
  std::chrono::milliseconds writer_idle_sleep{1};
};

struct VerbosityOverride {
  std::string component;
  std::string event_prefix;
  Level level;
};

class Logger {
 public:
  static Logger& instance();

  void start(Config cfg);
  void shutdown();
  bool started() const noexcept { return m_started.load(std::memory_order_acquire); }

  void set_sink(std::unique_ptr<ISink> sink);

  void set_min_level(Level lvl) noexcept { m_min_level.store(lvl, std::memory_order_release); }
  Level min_level() const noexcept { return m_min_level.load(std::memory_order_acquire); }

  void set_format(Format fmt) noexcept { m_format.store(fmt, std::memory_order_release); }
  Format format() const noexcept { return m_format.load(std::memory_order_acquire); }

  void add_verbosity_override(VerbosityOverride ov);
  void clear_verbosity_overrides();

  bool enabled(Level lvl, const char* component, const char* event) const noexcept;
  bool push(LogEvent ev) noexcept;
  bool sample_ok(const void* site_token, std::uint64_t sample_period_ns) noexcept;

  std::vector<LogEvent> dump_crash_rings() const;
  void emergency_dump() noexcept;

  void set_thread_name(std::string name) noexcept;

  std::uint64_t thread_drop_count() const noexcept;
  std::uint64_t total_drop_count() const noexcept {
    return m_total_drops.load(std::memory_order_relaxed);
  }
  std::uint64_t total_emitted() const noexcept {
    return m_total_emitted.load(std::memory_order_relaxed);
  }

  void flush_for_test();

  // Used by the per-thread registration RAII type. Public so it can be called
  // from outside the class without a friend declaration.
  struct ThreadState;
  void unregister_thread_state(ThreadState* ts);

 private:
  Logger() = default;
  ~Logger();

  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  ThreadState& thread_state();

  void writer_loop();
  void format_event(const LogEvent& ev, std::string& out) const;
  void format_event_console(const LogEvent& ev, std::string& out) const;
  void format_event_kv(const LogEvent& ev, std::string& out) const;
  void format_event_json(const LogEvent& ev, std::string& out) const;
  Level effective_level(const char* component, const char* event) const noexcept;

  std::atomic<bool> m_started{false};
  std::atomic<bool> m_stop{false};
  std::thread m_writer;
  std::mutex m_sink_mutex;
  std::unique_ptr<ISink> m_sink;
  std::atomic<Level> m_min_level{Level::Info};
  std::atomic<Format> m_format{Format::Console};
  Config m_cfg{};
  mutable std::mutex m_overrides_mutex;
  std::vector<VerbosityOverride> m_overrides;
  mutable std::mutex m_threads_mutex;
  std::vector<ThreadState*> m_threads;
  // Crash ring contents from threads that have already exited. Drained by
  // unregister_thread_state, read by dump_crash_rings / emergency_dump.
  std::vector<LogEvent> m_graveyard;
  std::atomic<std::uint64_t> m_total_drops{0};
  std::atomic<std::uint64_t> m_total_emitted{0};
};

namespace detail {

std::uint64_t NowNs() noexcept;

template <typename K, typename V, typename... Rest>
void fill_fields(LogEvent& ev, K&& key, V&& value, Rest&&... rest) {
  ev.fields[ev.field_count].key = std::forward<K>(key);
  ev.fields[ev.field_count].value = MakeFieldValue(std::forward<V>(value));
  ++ev.field_count;
  if constexpr (sizeof...(Rest) > 0) {
    fill_fields(ev, std::forward<Rest>(rest)...);
  }
}
inline void fill_fields(LogEvent&) {}

template <typename... KV>
LogEvent MakeEvent(Level lvl, const char* component, const char* event, KV&&... kv) {
  LogEvent ev{};
  ev.ts_ns = NowNs();
  ev.level = lvl;
  ev.component = component;
  ev.event = event;
  if constexpr (sizeof...(KV) > 0) {
    static_assert(sizeof...(KV) % 2 == 0,
                  "VKP_LOG fields must come in key,value pairs");
    static_assert(sizeof...(KV) / 2 <= kMaxInlineFields,
                  "Too many fields — increase kMaxInlineFields or split events");
    fill_fields(ev, std::forward<KV>(kv)...);
  }
  return ev;
}

}  // namespace detail

}  // namespace vkpt::core::log

#define VKP_LOG(level_enum, comp_lit, event_lit, ...)                          \
  do {                                                                          \
    auto& _vkp_logger_ = ::vkpt::core::log::Logger::instance();                 \
    if (_vkp_logger_.enabled(::vkpt::core::log::Level::level_enum,              \
                             (comp_lit), (event_lit))) {                        \
      _vkp_logger_.push(::vkpt::core::log::detail::MakeEvent(                   \
          ::vkpt::core::log::Level::level_enum,                                 \
          (comp_lit), (event_lit) __VA_OPT__(,) __VA_ARGS__));                  \
    }                                                                           \
  } while (false)

#define VKP_LOG_RT(level_enum, comp_lit, event_lit, ...) \
  VKP_LOG(level_enum, comp_lit, event_lit, ##__VA_ARGS__)

#define VKP_LOG_SAMPLED(period_ns, level_enum, comp_lit, event_lit, ...)        \
  do {                                                                           \
    auto& _vkp_logger_ = ::vkpt::core::log::Logger::instance();                  \
    if (_vkp_logger_.enabled(::vkpt::core::log::Level::level_enum,               \
                             (comp_lit), (event_lit))) {                         \
      static const char _vkp_site_ = 0;                                          \
      if (_vkp_logger_.sample_ok(&_vkp_site_, (period_ns))) {                    \
        _vkp_logger_.push(::vkpt::core::log::detail::MakeEvent(                  \
            ::vkpt::core::log::Level::level_enum,                                \
            (comp_lit), (event_lit), ##__VA_ARGS__));                            \
      }                                                                          \
    }                                                                            \
  } while (false)
