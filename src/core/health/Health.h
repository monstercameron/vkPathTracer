#pragma once

// Health probes for the snapshot-bus architecture (SYSTEM.md Phase 0.5).
//
// Each subsystem registers an IHealthProbe. The HealthRegistry runs a
// heartbeat thread that polls every probe every period_ms and emits
// system.health events. On a failed/degraded transition, the probe is
// emitted immediately at the appropriate level rather than waiting for the
// next heartbeat.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vkpt::core::health {

enum class Status : std::uint8_t {
  Ok = 0,
  Degraded = 1,
  Failed = 2,
};

const char* StatusName(Status s) noexcept;

struct Report {
  Status status = Status::Ok;
  std::string reason;  // one-line human-readable
};

class IHealthProbe {
 public:
  virtual ~IHealthProbe() = default;
  virtual std::string name() const = 0;
  virtual Report check() = 0;
};

// A drop-in probe that wraps a callable so subsystems don't have to subclass.
class FunctionProbe : public IHealthProbe {
 public:
  using Fn = std::function<Report()>;
  FunctionProbe(std::string name, Fn fn) : m_name(std::move(name)), m_fn(std::move(fn)) {}
  std::string name() const override { return m_name; }
  Report check() override { return m_fn ? m_fn() : Report{Status::Ok, "no probe fn"}; }

 private:
  std::string m_name;
  Fn m_fn;
};

class HealthRegistry {
 public:
  static HealthRegistry& instance();

  // Register / unregister probes. The registry takes ownership.
  void register_probe(std::shared_ptr<IHealthProbe> probe);
  void unregister_probe(std::string_view name);

  // Manual scrape (used by REPL `health` and tests).
  std::vector<std::pair<std::string, Report>> scrape();

  // Heartbeat lifecycle.
  void start(std::chrono::milliseconds period = std::chrono::seconds{1});
  void stop();

 private:
  HealthRegistry() = default;
  ~HealthRegistry() { stop(); }
  HealthRegistry(const HealthRegistry&) = delete;
  HealthRegistry& operator=(const HealthRegistry&) = delete;

  void loop();

  struct Entry {
    std::shared_ptr<IHealthProbe> probe;
    Status last = Status::Ok;
  };

  std::mutex m_mutex;
  std::vector<Entry> m_entries;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
  std::chrono::milliseconds m_period{1000};
};

}  // namespace vkpt::core::health
