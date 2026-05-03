#pragma once

#include <string>

namespace vkpt::diagnostics {

// ---- StatusFile ------------------------------------------------------------
// Writes artifacts/status/latest_status.json at the end of every app run
// so agents can inspect the outcome without parsing logs.

struct StatusFileData {
  std::string build_status = "ok";    // "ok" | "error"
  std::string last_run_status;        // "ok" | "render_ok" | "crash_test" | "error:<reason>"
  std::string enabled_backend;        // backend name used in this run
  std::string selected_scene;         // scene path or "none"
  std::string selected_renderer_path; // "cpu_scalar" | "vulkan_compute" | "null" etc.
  std::string last_error;             // empty if no error
  std::string last_crash_artifact;    // path to crash dir, or empty
  std::string performance_summary;    // human-readable timing summary, or empty
  std::string timestamp;              // ISO-8601 UTC
};

// Write the status file.  Creates parent directories if needed.
bool WriteStatusFile(const StatusFileData& data,
                     const std::string& path = "artifacts/status/latest_status.json",
                     std::string* error = nullptr);

// Build a current ISO-8601 timestamp string.
std::string StatusTimestampNow();

}  // namespace vkpt::diagnostics
