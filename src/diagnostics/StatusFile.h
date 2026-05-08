#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace vkpt::diagnostics {

inline constexpr std::string_view kStatusFileContractName = "diagnostics.status_file.v1";
inline constexpr std::string_view kStatusFileBuildOkName = "ok";
inline constexpr std::string_view kStatusFileBuildErrorName = "error";
inline constexpr std::string_view kStatusFileRunOkName = "ok";
inline constexpr std::string_view kStatusFileRenderOkName = "render_ok";
inline constexpr std::string_view kStatusFileCrashTestName = "crash_test";
inline constexpr std::string_view kStatusFileRunErrorPrefix = "error:";

// ---- StatusFile ------------------------------------------------------------
// Writes artifacts/status/latest_status.json at the end of every app run
// so agents can inspect the outcome without parsing logs.

struct StatusFileData {
  std::string build_status = std::string(kStatusFileBuildOkName);
  std::string last_run_status;        // "ok" | "render_ok" | "crash_test" | "error:<reason>"
  std::string enabled_backend;        // backend name used in this run
  std::string selected_scene;         // scene path or "none"
  std::string selected_renderer_path; // "cpu_scalar" | "vulkan_compute" | "null" etc.
  std::string last_error;             // empty if no error
  std::string last_crash_artifact;    // path to crash dir, or empty
  std::string crash_ring_dump;        // path to graceful crash-ring dump, or empty
  std::uint64_t crash_ring_events = 0;
  std::string performance_summary;    // human-readable timing summary, or empty
  std::string timestamp;              // ISO-8601 UTC
};

using StatusFileSnapshot = StatusFileData;

struct PeriodicStatusFileConfig {
  bool enabled = true;
  std::string path = "artifacts/status/latest_status.json";
  std::chrono::milliseconds period{5000};
};

class PeriodicStatusFile {
 public:
  using SnapshotFn = std::function<StatusFileData()>;

  PeriodicStatusFile(PeriodicStatusFileConfig config, SnapshotFn snapshot);
  ~PeriodicStatusFile();

  PeriodicStatusFile(const PeriodicStatusFile&) = delete;
  PeriodicStatusFile& operator=(const PeriodicStatusFile&) = delete;

  void start();
  void stop();
  bool write_now(std::string* error = nullptr);

 private:
  PeriodicStatusFileConfig m_config;
  SnapshotFn m_snapshot;
  std::mutex m_mutex;
  std::mutex m_write_mutex;
  std::condition_variable_any m_cv;
  bool m_stop = false;
  std::jthread m_thread;
};

// Write the status file.  Creates parent directories if needed.
bool WriteStatusFile(const StatusFileData& data,
                     const std::string& path = "artifacts/status/latest_status.json",
                     std::string* error = nullptr);

// Build a current ISO-8601 timestamp string.
std::string StatusTimestampNow();

}  // namespace vkpt::diagnostics
