#include "core/cli/Flags.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define VKP_ISATTY ::_isatty
#define VKP_FILENO ::_fileno
#else
#include <unistd.h>
#define VKP_ISATTY ::isatty
#define VKP_FILENO ::fileno
#endif

#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "core/trace/Trace.h"

namespace vkpt::core::cli {

namespace {

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

void SplitCsv(std::string_view in, std::vector<std::string>& out) {
  std::size_t start = 0;
  while (start < in.size()) {
    auto comma = in.find(',', start);
    auto piece = in.substr(start, comma == std::string_view::npos ? std::string_view::npos
                                                                  : comma - start);
    if (!piece.empty()) out.emplace_back(piece);
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
}

bool TryEqArg(std::string_view arg, std::string_view key, std::string& out) {
  if (!StartsWith(arg, key)) return false;
  if (arg.size() == key.size()) return false;
  if (arg[key.size()] != '=') return false;
  out.assign(arg.substr(key.size() + 1));
  return true;
}

}  // namespace

ObservabilityFlags Parse(int argc, const char* const* argv) {
  ObservabilityFlags out;
  if (argc <= 0) return out;
  out.remaining_argv.emplace_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    std::string val;
    if (TryEqArg(arg, "--log-level", val)) {
      out.log_level = val;
    } else if (TryEqArg(arg, "--log-format", val)) {
      out.log_format = val;
    } else if (TryEqArg(arg, "--log-out", val)) {
      out.log_out = val;
    } else if (TryEqArg(arg, "--verbose", val)) {
      SplitCsv(val, out.verbose);
    } else if (TryEqArg(arg, "--trace-out", val)) {
      out.trace_out = val;
    } else if (TryEqArg(arg, "--trace", val)) {
      SplitCsv(val, out.trace);
    } else if (TryEqArg(arg, "--metrics-out", val)) {
      out.metrics_out = val;
    } else if (arg == "--deterministic") {
      out.deterministic = true;
    } else {
      out.remaining_argv.emplace_back(arg);
    }
  }
  return out;
}

std::string AutoFormat() {
  return VKP_ISATTY(VKP_FILENO(stdout)) ? "console" : "json";
}

bool Apply(const ObservabilityFlags& flags) {
  bool ok = true;
  auto& logger = log::Logger::instance();

  // Format first so subsequent log messages render with the chosen format.
  std::string fmt = flags.log_format.value_or(AutoFormat());
  if (auto f = log::ParseFormat(fmt)) {
    logger.set_format(*f);
  } else {
    VKP_LOG(Warn, "cli", "bad_format", "value", fmt);
    ok = false;
  }

  if (flags.log_level) {
    if (auto lvl = log::ParseLevel(*flags.log_level)) {
      logger.set_min_level(*lvl);
    } else {
      VKP_LOG(Warn, "cli", "bad_level", "value", *flags.log_level);
      ok = false;
    }
  }

  if (flags.log_out) {
    if (*flags.log_out == "stderr") {
      logger.set_sink(std::make_unique<log::StreamSink>(log::StreamSink::Stream::Stderr));
    } else if (*flags.log_out == "stdout") {
      logger.set_sink(std::make_unique<log::StreamSink>(log::StreamSink::Stream::Stdout));
    } else {
      logger.set_sink(std::make_unique<log::FileSink>(*flags.log_out));
    }
  }

  for (const auto& v : flags.verbose) {
    auto colon = v.find(':');
    if (colon == std::string::npos) {
      VKP_LOG(Warn, "cli", "bad_verbose", "value", v);
      ok = false;
      continue;
    }
    log::VerbosityOverride ov;
    ov.component = v.substr(0, colon);
    ov.event_prefix = v.substr(colon + 1);
    ov.level = log::Level::Debug;  // default lift to debug
    logger.add_verbosity_override(std::move(ov));
  }

  for (const auto& comp : flags.trace) {
    trace::TraceRecorder::instance().enable_component(comp);
  }
  if (flags.trace_out) {
    trace::TraceRecorder::instance().set_output_path(*flags.trace_out);
  }

  // metrics_out is honored at shutdown by the integrating app — we don't
  // spawn a periodic file writer here to avoid a second background thread
  // per process. Instead the apply pass logs the path so the shutdown hook
  // can pick it up.
  if (flags.metrics_out) {
    VKP_LOG(Info, "cli", "metrics_out", "path", *flags.metrics_out);
  }

  if (flags.deterministic) {
    VKP_LOG(Info, "cli", "deterministic", "enabled", true);
  }

  VKP_LOG(Info, "cli", "config",
          "format", fmt,
          "level", std::string(log::LevelName(logger.min_level())),
          "verbose_overrides", static_cast<std::uint64_t>(flags.verbose.size()),
          "trace_components", static_cast<std::uint64_t>(flags.trace.size()),
          "deterministic", flags.deterministic);

  return ok;
}

}  // namespace vkpt::core::cli
