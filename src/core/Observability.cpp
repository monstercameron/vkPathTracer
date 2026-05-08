#include "core/Observability.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "core/repl/Repl.h"
#include "core/trace/Trace.h"

namespace vkpt::core::obs {

namespace {

std::atomic<bool> g_initialized{false};
std::mutex g_mu;
std::string g_metrics_out_path;
std::string g_trace_out_path;

extern "C" void SigHandler(int sig) {
  log::Logger::instance().emergency_dump();
  // Re-raise with default handler so the OS produces its usual diagnostic.
  std::signal(sig, SIG_DFL);
  std::raise(sig);
}

void InstallSignalHandlers() {
#if !defined(_WIN32)
  std::signal(SIGSEGV, SigHandler);
  std::signal(SIGABRT, SigHandler);
  std::signal(SIGFPE, SigHandler);
  std::signal(SIGILL, SigHandler);
  std::signal(SIGBUS, SigHandler);
#else
  std::signal(SIGSEGV, SigHandler);
  std::signal(SIGABRT, SigHandler);
  std::signal(SIGFPE, SigHandler);
  std::signal(SIGILL, SigHandler);
#endif
  std::signal(SIGINT, SigHandler);
  std::signal(SIGTERM, SigHandler);
}

}  // namespace

void Init() {
  bool expected = false;
  if (!g_initialized.compare_exchange_strong(expected, true,
                                             std::memory_order_acq_rel)) {
    return;
  }
  log::Config cfg{};
  log::Logger::instance().start(cfg);
  log::Logger::instance().set_thread_name("main");
  metrics::MetricsRegistry::instance().start_heartbeat();
  health::HealthRegistry::instance().start();
  repl::Repl::instance().register_builtins();
  InstallSignalHandlers();
  VKP_LOG(Info, "obs", "started");
}

void InitFromFlags(const cli::ObservabilityFlags& flags) {
  Init();
  cli::Apply(flags);
  std::scoped_lock lk(g_mu);
  if (flags.metrics_out) g_metrics_out_path = *flags.metrics_out;
  if (flags.trace_out) g_trace_out_path = *flags.trace_out;
}

void Shutdown() {
  bool was = g_initialized.exchange(false, std::memory_order_acq_rel);
  if (!was) return;
  VKP_LOG(Info, "obs", "stopping");

  // Persist metrics if requested.
  std::string metrics_out;
  std::string trace_out;
  {
    std::scoped_lock lk(g_mu);
    metrics_out = g_metrics_out_path;
    trace_out = g_trace_out_path;
  }
  if (!metrics_out.empty()) {
    std::ofstream f(metrics_out);
    if (f.is_open()) {
      f << metrics::MetricsRegistry::instance().dump_json();
    }
  }
  if (!trace_out.empty()) {
    trace::TraceRecorder::instance().dump_chrome(trace_out);
  }

  health::HealthRegistry::instance().stop();
  metrics::MetricsRegistry::instance().stop_heartbeat();
  repl::Repl::instance().stop();
  log::Logger::instance().shutdown();
}

std::string MetricsOutputPath() {
  std::scoped_lock lk(g_mu);
  return g_metrics_out_path;
}

}  // namespace vkpt::core::obs
