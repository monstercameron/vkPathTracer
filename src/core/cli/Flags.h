#pragma once

// Lightweight CLI flag parser for the observability surface (SYSTEM.md
// Phase 0.6 / Console section). Recognized flags:
//
//   --log-level=<trace|debug|info|warn|error|fatal>
//   --log-format=<console|kv|json>
//   --log-out=<path|stderr|stdout>
//   --verbose=<comp>:<channel>[,<comp>:<channel>...]   (raises only the named)
//   --trace=<comp>[,<comp>...]
//   --trace-out=<path>                                  (Chrome-tracing JSON)
//   --metrics-out=<path>                                (periodic JSON dump)
//   --deterministic                                      (forces serial modes)
//
// Unknown flags are passed through untouched so each subsystem can layer its
// own parser on the same argv. Flags may use --key=value or --key value.

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace vkpt::core::cli {

struct ObservabilityFlags {
  std::optional<std::string> log_level;
  std::optional<std::string> log_format;
  std::optional<std::string> log_out;
  std::vector<std::string> verbose;     // raw "comp:channel" pairs
  std::vector<std::string> trace;        // raw component names
  std::optional<std::string> trace_out;
  std::optional<std::string> metrics_out;
  bool deterministic = false;

  // Anything that didn't match above. Subsystems may consume these.
  std::vector<std::string> remaining_argv;
};

// Parse argv. argv[0] (program name) is preserved in remaining_argv.
ObservabilityFlags Parse(int argc, const char* const* argv);

// Apply parsed flags to the global observability subsystems. Idempotent. Call
// after Logger::start() and before kicking off the heartbeat threads.
//
// Returns true on success, false if a flag value failed to parse (e.g.
// --log-level=blarg). The error is also logged at warn.
bool Apply(const ObservabilityFlags& flags);

// Auto-format selection based on whether stdout is a TTY. JSON when not a TTY
// (i.e. piped to an agent harness), otherwise Console. Used as the default
// when --log-format isn't provided.
std::string AutoFormat();

}  // namespace vkpt::core::cli
