#include "core/repl/Repl.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "core/trace/Trace.h"
#include "scene/FrameLifecycle.h"

namespace vkpt::core::repl {

namespace {

std::vector<std::string> Tokenize(std::string_view line) {
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    if (i >= line.size()) {
      break;
    }
    const std::size_t start = i;
    while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
      ++i;
    }
    out.emplace_back(line.substr(start, i - start));
  }
  return out;
}

}  // namespace

Repl& Repl::instance() {
  static Repl* inst = new Repl();
  return *inst;
}

void Repl::register_command(std::string name, std::string help, HandlerFn fn) {
  std::scoped_lock lk(m_mutex);
  Command c;
  c.name = name;
  c.help = std::move(help);
  c.fn = std::move(fn);
  m_commands[std::move(name)] = std::move(c);
}

std::string Repl::dispatch(std::string_view line) {
  auto tokens = Tokenize(line);
  if (tokens.empty()) {
    return "";
  }
  std::string head = tokens.front();
  std::vector<std::string> args(tokens.begin() + 1, tokens.end());
  if (args.size() == 1u && args[0] == "status") {
    if (auto status = component_status_text(head)) {
      return *status;
    }
  }
  HandlerFn fn;
  {
    std::scoped_lock lk(m_mutex);
    auto it = m_commands.find(head);
    if (it == m_commands.end()) {
      return "unknown command: " + head + " (try 'help')";
    }
    fn = it->second.fn;
  }
  try {
    return fn(args);
  } catch (const std::exception& e) {
    return std::string("command threw: ") + e.what();
  } catch (...) {
    return "command threw unknown exception";
  }
}

std::string Repl::help_text() const {
  std::scoped_lock lk(m_mutex);
  std::vector<const Command*> sorted;
  sorted.reserve(m_commands.size());
  for (const auto& [_, c] : m_commands) {
    sorted.push_back(&c);
  }
  std::sort(sorted.begin(),
            sorted.end(),
            [](const Command* a, const Command* b) { return a->name < b->name; });
  std::ostringstream os;
  for (const auto* c : sorted) {
    os << "  " << c->name << " - " << c->help << '\n';
  }
  return os.str();
}

void Repl::set_script_list_provider(ScriptListProviderFn fn) {
  std::scoped_lock lk(m_mutex);
  m_scriptListProvider = std::move(fn);
}

void Repl::set_status_provider(std::string component, StatusProviderFn fn) {
  if (component.empty()) {
    return;
  }
  std::scoped_lock lk(m_mutex);
  if (fn) {
    m_statusProviders[std::move(component)] = std::move(fn);
  } else {
    m_statusProviders.erase(component);
  }
}

std::optional<std::string> Repl::component_status_text(std::string_view component) const {
  StatusProviderFn provider;
  {
    std::scoped_lock lk(m_mutex);
    const auto it = m_statusProviders.find(std::string(component));
    if (it == m_statusProviders.end()) {
      return std::nullopt;
    }
    provider = it->second;
  }
  if (!provider) {
    return std::nullopt;
  }
  auto out = provider();
  if (out.empty()) {
    out = std::string(component) + " status unavailable: provider returned no data\n";
  }
  if (out.back() != '\n') {
    out.push_back('\n');
  }
  return out;
}

std::string Repl::script_list_text() const {
  ScriptListProviderFn provider;
  {
    std::scoped_lock lk(m_mutex);
    provider = m_scriptListProvider;
  }
  if (!provider) {
    return "script list unavailable: no script runtime registered\n";
  }
  auto out = provider();
  if (out.empty()) {
    out = "script list unavailable: script runtime returned no data\n";
  }
  if (out.back() != '\n') {
    out.push_back('\n');
  }
  return out;
}

void Repl::register_builtins() {
  register_command("help", "show available commands", [this](const std::vector<std::string>&) {
    return help_text();
  });

  register_command(
      "metrics",
      "metrics dump|reset [prefix] - dump or reset registry",
      [](const std::vector<std::string>& args) -> std::string {
        if (args.empty()) {
          return "usage: metrics dump|reset [prefix]";
        }
        auto& reg = metrics::MetricsRegistry::instance();
        const std::string& sub = args[0];
        const std::string prefix = args.size() >= 2 ? args[1] : std::string{};
        if (sub == "dump") {
          auto snap = prefix.empty() ? reg.snapshot_all() : reg.snapshot_prefix(prefix);
          std::ostringstream os;
          for (const auto& s : snap) {
            os << s.name << " ";
            switch (s.kind) {
              case metrics::Kind::CounterKind:
                os << "counter=" << s.counter_value;
                break;
              case metrics::Kind::GaugeKind:
                os << "gauge=" << s.gauge_value;
                break;
              case metrics::Kind::HistogramKind:
                os << "hist count=" << s.hist.count
                   << " p50=" << s.hist.p50
                   << " p95=" << s.hist.p95
                   << " p99=" << s.hist.p99;
                break;
            }
            os << '\n';
          }
          return os.str();
        }
        if (sub == "reset") {
          reg.reset(prefix);
          return "ok\n";
        }
        if (sub == "json") {
          return reg.dump_json() + "\n";
        }
        return "usage: metrics dump|reset|json [prefix]";
      });

  register_command(
      "events",
      "events dump-rings - flush per-thread crash rings to stderr",
      [](const std::vector<std::string>& args) -> std::string {
        if (!args.empty() && args[0] == "dump-rings") {
          log::Logger::instance().emergency_dump();
          return "dumped\n";
        }
        return "usage: events dump-rings\n";
      });

  register_command("health", "show current probe statuses", [](const std::vector<std::string>&) {
    auto reports = health::HealthRegistry::instance().scrape();
    std::ostringstream os;
    for (const auto& [name, r] : reports) {
      os << "  " << name << " : " << health::StatusName(r.status)
         << " - " << r.reason << '\n';
    }
    if (reports.empty()) {
      os << "  (no probes registered)\n";
    }
    return os.str();
  });

  register_command("tracer",
                   "tracer pause|resume - Track B will wire this",
                   [](const std::vector<std::string>&) -> std::string {
                     return "tracer command stub - Track B not yet wired\n";
                   });

  register_command("snapshot",
                   "snapshot dump current - Track B will wire this",
                   [](const std::vector<std::string>&) -> std::string {
                     return "snapshot command stub - Track B not yet wired\n";
                   });

  register_command("scene",
                   "scene stages - print latest frame stage timings",
                   [](const std::vector<std::string>& args) -> std::string {
                     if (args.size() == 1u && args[0] == "stages") {
                       return vkpt::scene::FormatLatestFrameStageTimingsForRepl();
                     }
                     return "usage: scene stages\n";
                   });

  register_command("script",
                   "script list - show loaded scripts and runtime stats",
                   [this](const std::vector<std::string>& args) -> std::string {
                     if (args.size() == 1u && args[0] == "list") {
                       return script_list_text();
                     }
                     return "usage: script list\n";
                   });
}

void Repl::start_stdin_loop() {
  bool expected = false;
  if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  m_stop.store(false, std::memory_order_release);
  m_thread = std::thread(&Repl::stdin_loop, this);
}

void Repl::stop() {
  const bool was = m_running.exchange(false, std::memory_order_acq_rel);
  if (!was) {
    return;
  }
  m_stop.store(true, std::memory_order_release);
  // No clean way to interrupt a blocking std::cin; rely on stdin closing.
  if (m_thread.joinable()) {
    m_thread.detach();
  }
}

void Repl::stdin_loop() {
  std::string line;
  while (!m_stop.load(std::memory_order_acquire)) {
    if (!std::getline(std::cin, line)) {
      break;
    }
    if (line.empty()) {
      continue;
    }
    std::string result = dispatch(line);
    if (!result.empty()) {
      std::cout << result;
    }
  }
}

}  // namespace vkpt::core::repl
