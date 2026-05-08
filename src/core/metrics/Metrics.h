#pragma once

// Lock-free metrics registry for the snapshot-bus architecture (SYSTEM.md
// Phase 0.3). Three metric types — Counter, Gauge, Histogram — all with a
// wait-free record path. The registry indexes them by name and exposes
// snapshot/scrape APIs for the heartbeat thread, the REPL, and JSON export.
//
// Naming convention: vkp.<component>.<metric>, e.g. vkp.tracer.tile_latency_us.
//
// Recording macros cache a metric pointer in a function-local static so the
// hot path is one atomic op per record after the first call:
//
//     VKP_METRIC_INC("vkp.tracer.tiles_total");
//     VKP_METRIC_SET("vkp.tracer.spp_current", spp);
//     VKP_METRIC_OBSERVE("vkp.tracer.tile_latency_us", us);

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace vkpt::core::metrics {

class Counter {
 public:
  void inc(std::uint64_t n = 1) noexcept { m_value.fetch_add(n, std::memory_order_relaxed); }
  std::uint64_t value() const noexcept { return m_value.load(std::memory_order_relaxed); }
  void reset() noexcept { m_value.store(0, std::memory_order_relaxed); }

 private:
  std::atomic<std::uint64_t> m_value{0};
};

class Gauge {
 public:
  void set(double v) noexcept {
    std::uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    m_bits.store(bits, std::memory_order_relaxed);
  }
  double value() const noexcept {
    auto bits = m_bits.load(std::memory_order_relaxed);
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  void reset() noexcept { set(0.0); }

 private:
  std::atomic<std::uint64_t> m_bits{0};
};

// Log-spaced bucket histogram. 64 buckets cover [1, 2^63] with bucket k
// spanning [2^k, 2^(k+1)). Sample of 0 falls in bucket 0. Recording is one
// fetch_add per call. Snapshot iterates the 64 atomics and computes summary
// stats and percentiles.
class Histogram {
 public:
  static constexpr std::size_t kBucketCount = 64;

  void record(std::uint64_t v) noexcept {
    std::size_t bucket;
    if (v <= 1) {
      bucket = 0;
    } else {
      // Position of highest set bit. clzll is widely available; fall back if
      // it isn't.
#if defined(__GNUC__) || defined(__clang__)
      bucket = static_cast<std::size_t>(63 - __builtin_clzll(v));
#elif defined(_MSC_VER)
      unsigned long idx = 0;
      _BitScanReverse64(&idx, v);
      bucket = idx;
#else
      bucket = 0;
      while (v >>= 1) ++bucket;
#endif
    }
    if (bucket >= kBucketCount) bucket = kBucketCount - 1;
    m_buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    m_count.fetch_add(1, std::memory_order_relaxed);
    m_sum.fetch_add(v, std::memory_order_relaxed);
    // Atomic min/max via CAS.
    std::uint64_t cur = m_max.load(std::memory_order_relaxed);
    while (v > cur && !m_max.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
    cur = m_min.load(std::memory_order_relaxed);
    while ((cur == 0 || v < cur) &&
           !m_min.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
  }

  struct Snapshot {
    std::uint64_t count = 0;
    std::uint64_t sum = 0;
    std::uint64_t min_val = 0;
    std::uint64_t max_val = 0;
    std::uint64_t p50 = 0;
    std::uint64_t p95 = 0;
    std::uint64_t p99 = 0;
    std::array<std::uint64_t, kBucketCount> buckets{};
  };

  Snapshot snapshot() const noexcept;

  void reset() noexcept {
    for (auto& b : m_buckets) b.store(0, std::memory_order_relaxed);
    m_count.store(0, std::memory_order_relaxed);
    m_sum.store(0, std::memory_order_relaxed);
    m_min.store(0, std::memory_order_relaxed);
    m_max.store(0, std::memory_order_relaxed);
  }

 private:
  std::array<std::atomic<std::uint64_t>, kBucketCount> m_buckets{};
  std::atomic<std::uint64_t> m_count{0};
  std::atomic<std::uint64_t> m_sum{0};
  std::atomic<std::uint64_t> m_min{0};
  std::atomic<std::uint64_t> m_max{0};
};

enum class Kind : std::uint8_t { CounterKind, GaugeKind, HistogramKind };

struct MetricSnapshot {
  std::string name;
  Kind kind;
  std::uint64_t counter_value = 0;
  double gauge_value = 0.0;
  Histogram::Snapshot hist;
};

class MetricsRegistry {
 public:
  static MetricsRegistry& instance();

  Counter& counter(std::string_view name);
  Gauge& gauge(std::string_view name);
  Histogram& histogram(std::string_view name);

  std::vector<MetricSnapshot> snapshot_all() const;
  // Subset by name prefix (e.g. "vkp.tracer.").
  std::vector<MetricSnapshot> snapshot_prefix(std::string_view prefix) const;

  // Reset all metrics matching prefix. Empty prefix resets everything.
  void reset(std::string_view prefix);

  // JSON dump (one object per metric, array). Used by --metrics-out and the
  // REPL `metrics dump` command.
  std::string dump_json() const;

  // Heartbeat lifecycle. Spawns a thread that emits one
  // metrics.heartbeat per component every period_ms and updates derived
  // gauges (e.g. <metric>_per_sec). Idempotent.
  void start_heartbeat(std::chrono::milliseconds period = std::chrono::seconds{1});
  void stop_heartbeat();

 private:
  MetricsRegistry() = default;
  ~MetricsRegistry();
  MetricsRegistry(const MetricsRegistry&) = delete;
  MetricsRegistry& operator=(const MetricsRegistry&) = delete;

  struct Entry {
    Kind kind;
    std::unique_ptr<Counter> counter;
    std::unique_ptr<Gauge> gauge;
    std::unique_ptr<Histogram> histogram;
    // Heartbeat needs to compute rates; remember last counter snapshot.
    std::uint64_t last_counter_value = 0;
    std::uint64_t last_heartbeat_ns = 0;
  };

  Entry& get_or_create(std::string_view name, Kind kind);

  void heartbeat_loop();

  mutable std::mutex m_mutex;
  std::unordered_map<std::string, std::unique_ptr<Entry>> m_entries;
  std::atomic<bool> m_heartbeat_running{false};
  std::atomic<bool> m_heartbeat_stop{false};
  std::thread m_heartbeat_thread;
  std::chrono::milliseconds m_heartbeat_period{1000};
};

}  // namespace vkpt::core::metrics

// Hot-path recording macros. Each call site caches a metric pointer in a
// function-local static so subsequent records skip the registry lookup.

#define VKP_METRIC_INC(name_lit)                                              \
  do {                                                                          \
    static auto* _vkp_m_ =                                                      \
        &::vkpt::core::metrics::MetricsRegistry::instance().counter(name_lit);  \
    _vkp_m_->inc();                                                             \
  } while (false)

#define VKP_METRIC_INC_BY(name_lit, n)                                        \
  do {                                                                          \
    static auto* _vkp_m_ =                                                      \
        &::vkpt::core::metrics::MetricsRegistry::instance().counter(name_lit);  \
    _vkp_m_->inc(static_cast<std::uint64_t>(n));                                \
  } while (false)

#define VKP_METRIC_SET(name_lit, value_dbl)                                   \
  do {                                                                          \
    static auto* _vkp_m_ =                                                      \
        &::vkpt::core::metrics::MetricsRegistry::instance().gauge(name_lit);    \
    _vkp_m_->set(static_cast<double>(value_dbl));                               \
  } while (false)

#define VKP_METRIC_OBSERVE(name_lit, value_u64)                               \
  do {                                                                          \
    static auto* _vkp_m_ =                                                      \
        &::vkpt::core::metrics::MetricsRegistry::instance().histogram(name_lit);\
    _vkp_m_->record(static_cast<std::uint64_t>(value_u64));                     \
  } while (false)
