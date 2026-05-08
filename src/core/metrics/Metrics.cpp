#include "core/metrics/Metrics.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <string>
#include <thread>

#include "core/log/Log.h"

namespace vkpt::core::metrics {

namespace {

void AppendU64(std::string& out, std::uint64_t v) {
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "%" PRIu64, v);
  if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

void AppendDouble(std::string& out, double v) {
  char buf[32];
  int n = std::snprintf(buf, sizeof(buf), "%.6g", v);
  if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Extract the component segment from a metric name. "vkp.<comp>.<rest>" → "<comp>".
// Anything else returns "global".
std::string_view ComponentOf(std::string_view name) noexcept {
  if (name.size() < 5 || name.substr(0, 4) != "vkp.") return "global";
  auto rest = name.substr(4);
  auto dot = rest.find('.');
  if (dot == std::string_view::npos) return "global";
  return rest.substr(0, dot);
}

}  // namespace

Histogram::Snapshot Histogram::snapshot() const noexcept {
  Snapshot s;
  s.count = m_count.load(std::memory_order_relaxed);
  s.sum = m_sum.load(std::memory_order_relaxed);
  s.min_val = m_min.load(std::memory_order_relaxed);
  s.max_val = m_max.load(std::memory_order_relaxed);
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    s.buckets[i] = m_buckets[i].load(std::memory_order_relaxed);
  }
  if (s.count == 0) return s;

  // Approximate percentiles from bucket boundaries: assume value falls in the
  // middle of the bucket [2^k, 2^(k+1)). Good enough for ops dashboards.
  auto pct = [&](double p) -> std::uint64_t {
    const std::uint64_t target = static_cast<std::uint64_t>(p * static_cast<double>(s.count));
    std::uint64_t cumulative = 0;
    for (std::size_t k = 0; k < kBucketCount; ++k) {
      cumulative += s.buckets[k];
      if (cumulative >= target) {
        const std::uint64_t lo = (k == 0) ? 0u : (1ull << k);
        const std::uint64_t hi = (k + 1 < 64) ? (1ull << (k + 1)) : ~0ull;
        return lo + (hi - lo) / 2;
      }
    }
    return s.max_val;
  };
  s.p50 = pct(0.50);
  s.p95 = pct(0.95);
  s.p99 = pct(0.99);
  return s;
}

MetricsRegistry& MetricsRegistry::instance() {
  static MetricsRegistry* inst = new MetricsRegistry();
  return *inst;
}

MetricsRegistry::~MetricsRegistry() { stop_heartbeat(); }

MetricsRegistry::Entry& MetricsRegistry::get_or_create(std::string_view name, Kind kind) {
  std::scoped_lock lk(m_mutex);
  auto it = m_entries.find(std::string(name));
  if (it == m_entries.end()) {
    auto e = std::make_unique<Entry>();
    e->kind = kind;
    if (kind == Kind::CounterKind) e->counter = std::make_unique<Counter>();
    if (kind == Kind::GaugeKind)   e->gauge   = std::make_unique<Gauge>();
    if (kind == Kind::HistogramKind) e->histogram = std::make_unique<Histogram>();
    auto* raw = e.get();
    m_entries.emplace(std::string(name), std::move(e));
    return *raw;
  }
  return *it->second;
}

Counter& MetricsRegistry::counter(std::string_view name) {
  Entry& e = get_or_create(name, Kind::CounterKind);
  return *e.counter;
}

Gauge& MetricsRegistry::gauge(std::string_view name) {
  Entry& e = get_or_create(name, Kind::GaugeKind);
  return *e.gauge;
}

Histogram& MetricsRegistry::histogram(std::string_view name) {
  Entry& e = get_or_create(name, Kind::HistogramKind);
  return *e.histogram;
}

std::vector<MetricSnapshot> MetricsRegistry::snapshot_all() const {
  std::vector<MetricSnapshot> out;
  std::scoped_lock lk(m_mutex);
  out.reserve(m_entries.size());
  for (const auto& [name, e] : m_entries) {
    MetricSnapshot s;
    s.name = name;
    s.kind = e->kind;
    if (e->counter) s.counter_value = e->counter->value();
    if (e->gauge)   s.gauge_value = e->gauge->value();
    if (e->histogram) s.hist = e->histogram->snapshot();
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
  return out;
}

std::vector<MetricSnapshot> MetricsRegistry::snapshot_prefix(std::string_view prefix) const {
  auto all = snapshot_all();
  all.erase(
      std::remove_if(all.begin(), all.end(),
                     [&](const MetricSnapshot& s) {
                       return s.name.size() < prefix.size() ||
                              std::string_view(s.name).substr(0, prefix.size()) != prefix;
                     }),
      all.end());
  return all;
}

void MetricsRegistry::reset(std::string_view prefix) {
  std::scoped_lock lk(m_mutex);
  for (auto& [name, e] : m_entries) {
    if (!prefix.empty() &&
        (name.size() < prefix.size() ||
         std::string_view(name).substr(0, prefix.size()) != prefix)) {
      continue;
    }
    if (e->counter)   e->counter->reset();
    if (e->gauge)     e->gauge->reset();
    if (e->histogram) e->histogram->reset();
  }
}

std::string MetricsRegistry::dump_json() const {
  auto all = snapshot_all();
  std::string out;
  out.reserve(2048);
  out.push_back('[');
  bool first = true;
  for (const auto& s : all) {
    if (!first) out.push_back(',');
    first = false;
    out.append("{\"name\":\"");
    out.append(s.name);
    out.append("\",\"kind\":\"");
    switch (s.kind) {
      case Kind::CounterKind:   out.append("counter"); break;
      case Kind::GaugeKind:     out.append("gauge"); break;
      case Kind::HistogramKind: out.append("histogram"); break;
    }
    out.append("\"");
    if (s.kind == Kind::CounterKind) {
      out.append(",\"value\":");
      AppendU64(out, s.counter_value);
    } else if (s.kind == Kind::GaugeKind) {
      out.append(",\"value\":");
      AppendDouble(out, s.gauge_value);
    } else {
      out.append(",\"count\":");
      AppendU64(out, s.hist.count);
      out.append(",\"sum\":");
      AppendU64(out, s.hist.sum);
      out.append(",\"min\":");
      AppendU64(out, s.hist.min_val);
      out.append(",\"max\":");
      AppendU64(out, s.hist.max_val);
      out.append(",\"p50\":");
      AppendU64(out, s.hist.p50);
      out.append(",\"p95\":");
      AppendU64(out, s.hist.p95);
      out.append(",\"p99\":");
      AppendU64(out, s.hist.p99);
    }
    out.push_back('}');
  }
  out.push_back(']');
  return out;
}

void MetricsRegistry::start_heartbeat(std::chrono::milliseconds period) {
  bool expected = false;
  if (!m_heartbeat_running.compare_exchange_strong(expected, true,
                                                   std::memory_order_acq_rel)) {
    return;
  }
  m_heartbeat_period = period;
  m_heartbeat_stop.store(false, std::memory_order_release);
  m_heartbeat_thread = std::thread(&MetricsRegistry::heartbeat_loop, this);
}

void MetricsRegistry::stop_heartbeat() {
  bool was = m_heartbeat_running.exchange(false, std::memory_order_acq_rel);
  if (!was) return;
  m_heartbeat_stop.store(true, std::memory_order_release);
  if (m_heartbeat_thread.joinable()) m_heartbeat_thread.join();
}

void MetricsRegistry::heartbeat_loop() {
  using clock = std::chrono::steady_clock;
  auto next_tick = clock::now() + m_heartbeat_period;
  while (!m_heartbeat_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_until(next_tick);
    next_tick += m_heartbeat_period;

    // Snapshot under lock; keep the lock for derived rate updates so we update
    // last_counter_value atomically with the read.
    std::vector<std::pair<std::string, double>> rates;  // <name>_per_sec rates
    std::vector<MetricSnapshot> snap;
    {
      std::scoped_lock lk(m_mutex);
      const std::uint64_t now_ns = NowNs();
      snap.reserve(m_entries.size());
      for (auto& [name, e] : m_entries) {
        MetricSnapshot s;
        s.name = name;
        s.kind = e->kind;
        if (e->counter) s.counter_value = e->counter->value();
        if (e->gauge)   s.gauge_value = e->gauge->value();
        if (e->histogram) s.hist = e->histogram->snapshot();
        snap.push_back(s);

        if (e->kind == Kind::CounterKind && e->last_heartbeat_ns != 0) {
          const auto dt_ns = now_ns - e->last_heartbeat_ns;
          if (dt_ns > 0) {
            const double dv = static_cast<double>(s.counter_value - e->last_counter_value);
            const double rate = dv / (static_cast<double>(dt_ns) / 1e9);
            rates.emplace_back(name + "_per_sec", rate);
          }
        }
        if (e->kind == Kind::CounterKind) {
          e->last_counter_value = s.counter_value;
          e->last_heartbeat_ns = now_ns;
        }
      }
    }

    // Update derived rate gauges outside the per-counter loop to avoid
    // recursive lock acquisition.
    for (const auto& [name, rate] : rates) {
      gauge(name).set(rate);
    }

    // Group snap by component and emit one heartbeat event per component.
    std::unordered_map<std::string_view, std::size_t> comp_counts;
    for (const auto& s : snap) ++comp_counts[ComponentOf(s.name)];
    for (const auto& [comp, count] : comp_counts) {
      VKP_LOG(Info, "metrics", "heartbeat",
              "metric_component", std::string(comp),
              "metric_count", static_cast<std::uint64_t>(count));
    }
  }
}

}  // namespace vkpt::core::metrics
