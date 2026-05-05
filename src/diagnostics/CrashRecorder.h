#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <string>
#include <vector>

#include "core/Logging.h"

namespace vkpt::diagnostics {

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

  // Write all crash artifacts to a timestamped subdirectory under base_dir.
  // Returns the directory path on success.
  std::string flush(const std::string& base_dir = "artifacts/crashes");

  // Expose the current snapshot for embedding in other outputs.
  const CrashStateSnapshot& snapshot() const { return m_snapshot; }

 private:
  CrashRecorder() = default;

  CrashStateSnapshot m_snapshot;
};

// ---- Helpers ----------------------------------------------------------------

// Serialize a CrashStateSnapshot to a JSON string.
std::string SerializeCrashState(const CrashStateSnapshot& state);

// Serialize the last N log events from the global logger ring buffer to JSONL.
std::string SerializeRecentLogs(std::size_t max_events = 1024);

}  // namespace vkpt::diagnostics
