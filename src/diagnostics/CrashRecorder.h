#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

#include "core/Logging.h"
#include "core/contracts/Lifecycle.h"
#include "core/contracts/Result.h"
#include "core/contracts/SubsystemStatus.h"

namespace vkpt::diagnostics {

inline constexpr std::string_view kDiagnosticsSubsystemName = "diagnostics";
inline constexpr std::string_view kDiagnosticsStatusTypeName = "DiagnosticsStatus";
inline constexpr std::string_view kCrashRecorderContractName =
    "diagnostics.crash_recorder.v1";

struct DiagnosticsNamingContract {
  std::string_view subsystem_name = kDiagnosticsSubsystemName;
  std::string_view status_type_name = kDiagnosticsStatusTypeName;
  std::string_view crash_recorder_contract = kCrashRecorderContractName;
  std::string_view health_probe_name = kDiagnosticsSubsystemName;
  std::string_view lifecycle_field_name = "lifecycle";
  std::string_view last_error_field_name = "last_error";
  std::string_view flow_field_name = "current_flow_id";
};

inline constexpr DiagnosticsNamingContract kDiagnosticsNamingContract{};

[[nodiscard]] inline constexpr std::string_view DiagnosticsSubsystemName() noexcept {
  return kDiagnosticsSubsystemName;
}

// ---- CrashStateSnapshot ----------------------------------------------------
// Snapshot of the entire application state at crash time.  Every subsystem
// should call UpdateLastFrameStage / UpdateBackendState when its state changes
// so the snapshot is populated before a crash occurs.

struct LiveResourceInfo {
  std::string label;
  std::string kind;        // "buffer", "texture", "pipeline", etc.
  uint64_t    size_bytes = 0;
  uint64_t    version    = 0;
};

struct CrashCheckpoint {
  std::string name;
  std::string subsystem;
  std::string detail;
  uint64_t frame_index = 0;
  bool successful = true;
  std::string timestamp_utc;
};

struct SubsystemStateInfo {
  std::string subsystem;
  std::string state_json = "{}";
  std::string timestamp_utc;
};

struct CrashStateSnapshot {
  // --- Build ---
  std::string build_version;
  std::string git_hash;
  std::string build_date;
  std::string compiler;
  std::string compiler_name;
  std::string compiler_version;
  std::string cxx_standard;
  std::string target_os;
  std::string target_arch;
  std::string build_type;
  std::string enabled_features;
  std::string disabled_features;
  std::string sanitizer_mode = "disabled";
  std::string sanitizer_flavor = "disabled";
  std::string simd_compile_options;
  std::string backend_compile_options;
  std::string platform_shells;

  // --- Runtime ---
  std::string selected_backend   = "none";
  std::string last_frame_stage   = "unknown";
  uint64_t    last_frame_index   = 0;
  std::string last_pass_name;
  std::string last_shader_variant;
  std::string last_error;
  std::string active_scene       = "none";
  std::string last_successful_checkpoint = "none";
  std::string runtime_config_json = "{}";
  std::string frame_state_json = "{}";
  std::string resource_state_json = "{}";
  std::string backend_state_json = "{}";
  std::string scene_state_json = "{}";
  std::vector<CrashCheckpoint> checkpoints;
  std::vector<SubsystemStateInfo> subsystem_states;

  // --- Resources ---
  std::vector<LiveResourceInfo> live_resources;

  // --- UI snapshots ---
  std::string ui_state_json = "{}";
  std::string selection_state_json = "{}";
  std::string layout_state_json = "{}";
  std::string ui_events_jsonl;
  std::string editor_commands_jsonl;

  // --- Renderer crash state ---
  // Serialized vkpt::render::RenderCrashState JSON (or "{}" if unavailable).
  std::string renderer_state_json = "{}";
};

struct DiagnosticsStatus {
  std::string name = std::string(kDiagnosticsSubsystemName);
  vkpt::core::contracts::ComponentLifecycle lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Ready;
  vkpt::core::contracts::SubsystemHealth health =
      vkpt::core::contracts::SubsystemHealth::Ok;
  bool has_unflushed_record = false;
  std::uint64_t checkpoints_total = 0;
  std::uint64_t live_resources = 0;
  std::uint64_t subsystem_states = 0;
  std::uint64_t flushes_total = 0;
  std::uint64_t flush_errors_total = 0;
  std::uint64_t current_flow_id = 0;
  std::string last_error;
  std::string last_flush_artifact;

  vkpt::core::contracts::SubsystemStatus to_subsystem_status() const;
};

using CrashRecorderStatus = DiagnosticsStatus;

// ---- CrashRecorder ---------------------------------------------------------
// Singleton that keeps the latest crash snapshot in memory and writes it
// to the artifact directory on demand (crash or orderly shutdown).

class CrashRecorder {
 public:
  static CrashRecorder& instance();

  // Populate build information once at startup.
  void set_build_info(std::string version,
                      std::string git_hash,
                      std::string compiler,
                      std::string target_os,
                      std::string target_arch,
                      std::string build_type,
                      std::string enabled_features,
                      std::string build_date = {},
                      std::string compiler_name = {},
                      std::string compiler_version = {},
                      std::string cxx_standard = {},
                      std::string disabled_features = {},
                      std::string sanitizer_mode = {},
                      std::string sanitizer_flavor = {},
                      std::string simd_compile_options = {},
                      std::string backend_compile_options = {},
                      std::string platform_shells = {});

  // Call these when important state changes. Prefer stage/checkpoint updates in
  // hot paths and full JSON snapshots at subsystem boundaries.
  void update_backend(std::string_view backend_name);
  void update_frame_stage(std::string_view stage, uint64_t frame_index);
  void update_pass(std::string_view pass_name);
  void update_shader(std::string_view shader_variant);
  void update_scene(std::string_view scene_name);
  void set_last_error(std::string_view error);
  void update_ui_state_json(std::string_view json);
  void update_selection_state_json(std::string_view json);
  void update_layout_state_json(std::string_view json);
  void update_ui_events_jsonl(std::string_view jsonl);
  void update_editor_commands_jsonl(std::string_view jsonl);
  void update_renderer_state_json(std::string_view json);
  void update_runtime_config_json(std::string_view json);
  void update_frame_state_json(std::string_view json);
  void update_resource_state_json(std::string_view json);
  void update_backend_state_json(std::string_view json);
  void update_scene_state_json(std::string_view json);
  void update_subsystem_state_json(std::string_view subsystem, std::string_view json);
  void record_checkpoint(std::string_view name,
                         uint64_t frame_index = 0,
                         std::string_view subsystem = {},
                         std::string_view checkpoint_detail = {},
                         bool successful = true);

  // Track a resource (call on create; reverse with release_resource).
  void track_resource(std::string_view label, std::string_view kind,
                      uint64_t size_bytes);
  void release_resource(std::string_view label);

  // State contract:
  // state\method      update_*  status  flush_result
  // Ready             ok        ok      ->Busy -> Ready/Failed
  // Busy              ok        ok      Busy
  // Degraded          ok        ok      ->Busy -> Ready/Failed
  // Failed            ok        ok      retry allowed
  //
  // Write all crash artifacts to a timestamped subdirectory under base_dir.
  // Returns the directory path on success. Prefer flush_result() for new code
  // that needs a typed failure signal; flush() is kept for existing callers.
  std::string flush(const std::string& base_dir = "artifacts/crashes");
  vkpt::core::Result<std::string> flush_result(
      const std::string& base_dir = "artifacts/crashes");

  bool has_unflushed_record() const;
  DiagnosticsStatus status() const;

  // Expose the current snapshot for embedding in other outputs.
  const CrashStateSnapshot& snapshot() const { return m_snapshot; }

 private:
  CrashRecorder() = default;

  CrashStateSnapshot m_snapshot;
  bool m_unflushed_record = false;
  vkpt::core::contracts::ComponentLifecycle m_lifecycle =
      vkpt::core::contracts::ComponentLifecycle::Ready;
  std::uint64_t m_flushes_total = 0;
  std::uint64_t m_flush_errors_total = 0;
  std::string m_last_flush_artifact;
};

// ---- Helpers ----------------------------------------------------------------

// Serialize a CrashStateSnapshot to a JSON string.
std::string SerializeCrashState(const CrashStateSnapshot& state);

// Serialize the last N log events from the global logger ring buffer to JSONL.
std::string SerializeRecentLogs(std::size_t max_events = 1024);

}  // namespace vkpt::diagnostics
