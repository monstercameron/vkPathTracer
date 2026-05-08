#include "core/health/Health.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/log/Log.h"

namespace vkpt::core::health {

const char* StatusName(Status s) noexcept {
  switch (s) {
    case Status::Ok:       return "ok";
    case Status::Degraded: return "degraded";
    case Status::Failed:   return "failed";
  }
  return "ok";
}

HealthRegistry& HealthRegistry::instance() {
  static HealthRegistry* inst = new HealthRegistry();
  return *inst;
}

void HealthRegistry::register_probe(std::shared_ptr<IHealthProbe> probe) {
  if (!probe) return;
  std::scoped_lock lk(m_mutex);
  // Replace if same name exists.
  const std::string name = probe->name();
  for (auto& e : m_entries) {
    if (e.probe && e.probe->name() == name) {
      e.probe = std::move(probe);
      e.last = Status::Ok;
      return;
    }
  }
  m_entries.push_back({std::move(probe), Status::Ok});
}

void HealthRegistry::unregister_probe(std::string_view name) {
  std::scoped_lock lk(m_mutex);
  m_entries.erase(
      std::remove_if(m_entries.begin(), m_entries.end(),
                     [&](const Entry& e) { return e.probe && e.probe->name() == name; }),
      m_entries.end());
}

std::vector<std::pair<std::string, Report>> HealthRegistry::scrape() {
  std::vector<std::pair<std::string, Report>> out;
  std::scoped_lock lk(m_mutex);
  out.reserve(m_entries.size());
  for (auto& e : m_entries) {
    if (!e.probe) continue;
    Report r = e.probe->check();
    out.emplace_back(e.probe->name(), std::move(r));
  }
  return out;
}

void HealthRegistry::start(std::chrono::milliseconds period) {
  bool expected = false;
  if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  m_period = period;
  m_stop.store(false, std::memory_order_release);
  m_thread = std::thread(&HealthRegistry::loop, this);
}

void HealthRegistry::stop() {
  bool was = m_running.exchange(false, std::memory_order_acq_rel);
  if (!was) return;
  m_stop.store(true, std::memory_order_release);
  if (m_thread.joinable()) m_thread.join();
}

void HealthRegistry::loop() {
  using clock = std::chrono::steady_clock;
  auto next_tick = clock::now() + m_period;
  while (!m_stop.load(std::memory_order_acquire)) {
    std::this_thread::sleep_until(next_tick);
    next_tick += m_period;

    std::vector<Entry> snapshot;
    {
      std::scoped_lock lk(m_mutex);
      snapshot = m_entries;
    }

    int ok = 0, degraded = 0, failed = 0;
    for (auto& e : snapshot) {
      if (!e.probe) continue;
      Report r = e.probe->check();
      if (r.status == Status::Ok) ++ok;
      if (r.status == Status::Degraded) ++degraded;
      if (r.status == Status::Failed) ++failed;

      // Detect transitions to emit immediate event.
      Status prev = Status::Ok;
      {
        std::scoped_lock lk(m_mutex);
        for (auto& live : m_entries) {
          if (live.probe && live.probe->name() == e.probe->name()) {
            prev = live.last;
            live.last = r.status;
            break;
          }
        }
      }
      if (r.status != prev) {
        if (r.status == Status::Failed) {
          VKP_LOG(Error, "health", "transition",
                  "probe", e.probe->name(),
                  "from", std::string(StatusName(prev)),
                  "to", std::string(StatusName(r.status)),
                  "reason", r.reason);
        } else if (r.status == Status::Degraded) {
          VKP_LOG(Warn, "health", "transition",
                  "probe", e.probe->name(),
                  "from", std::string(StatusName(prev)),
                  "to", std::string(StatusName(r.status)),
                  "reason", r.reason);
        } else {
          VKP_LOG(Info, "health", "transition",
                  "probe", e.probe->name(),
                  "from", std::string(StatusName(prev)),
                  "to", std::string(StatusName(r.status)),
                  "reason", r.reason);
        }
      }
    }

    VKP_LOG(Info, "health", "heartbeat",
            "ok", static_cast<std::uint64_t>(ok),
            "degraded", static_cast<std::uint64_t>(degraded),
            "failed", static_cast<std::uint64_t>(failed));
  }
}

}  // namespace vkpt::core::health
