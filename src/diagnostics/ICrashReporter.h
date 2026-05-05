#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "core/BuildInfo.h"
#include "diagnostics/CrashRecorder.h"

namespace vkpt::diagnostics {

struct CrashReporterSessionInfo {
  std::string artifact_dir = "artifacts/crashes";
  std::string runtime_config_json = "{}";
};

class ICrashReporter {
 public:
  virtual ~ICrashReporter() = default;

  virtual void begin_session(const CrashReporterSessionInfo& session) = 0;
  virtual void record_checkpoint(std::string_view name,
                                 uint64_t frame_index = 0,
                                 std::string_view subsystem = {},
                                 std::string_view detail = {},
                                 bool successful = true) = 0;
  virtual void record_subsystem_state(std::string_view subsystem,
                                      std::string_view state_json) = 0;
  virtual std::string flush_pre_crash_state(std::string_view reason) = 0;
  virtual std::string write_crash_artifact_bundle(std::string_view reason) = 0;
};

class CrashRecorderReporter final : public ICrashReporter {
 public:
  explicit CrashRecorderReporter(CrashRecorder& recorder = CrashRecorder::instance())
      : m_recorder(recorder) {}

  void begin_session(const CrashReporterSessionInfo& session) override {
    m_artifactDir = session.artifact_dir.empty() ? "artifacts/crashes" : session.artifact_dir;

    const auto build = vkpt::build::GetBuildMetadata();
    m_recorder.set_build_info(build.project_version,
                              build.git_hash,
                              build.compiler_name + " " + build.compiler_version,
                              build.target_os,
                              build.target_arch,
                              build.build_type,
                              std::string(vkpt::build::kEnabledFeatureFlags),
                              build.build_date,
                              build.compiler_name,
                              build.compiler_version,
                              build.cxx_standard,
                              std::string(vkpt::build::kDisabledFeatureFlags),
                              build.sanitizer_mode,
                              build.sanitizer_flavor,
                              build.simd_compile_options,
                              build.backend_compile_options,
                              build.platform_shells);
    m_recorder.update_runtime_config_json(session.runtime_config_json);
    m_recorder.record_checkpoint("session_begin", 0, "diagnostics", "crash reporter session started");
  }

  void record_checkpoint(std::string_view name,
                         uint64_t frame_index = 0,
                         std::string_view subsystem = {},
                         std::string_view detail = {},
                         bool successful = true) override {
    m_recorder.record_checkpoint(name, frame_index, subsystem, detail, successful);
  }

  void record_subsystem_state(std::string_view subsystem,
                              std::string_view state_json) override {
    m_recorder.update_subsystem_state_json(subsystem, state_json);
  }

  std::string flush_pre_crash_state(std::string_view reason) override {
    m_recorder.set_last_error(reason);
    m_recorder.record_checkpoint("flush_pre_crash_state", 0, "diagnostics", reason);
    return m_recorder.flush(m_artifactDir);
  }

  std::string write_crash_artifact_bundle(std::string_view reason) override {
    m_recorder.set_last_error(reason);
    m_recorder.record_checkpoint("write_crash_artifact_bundle", 0, "diagnostics", reason);
    return m_recorder.flush(m_artifactDir);
  }

  void set_artifact_dir(std::string artifact_dir) {
    m_artifactDir = artifact_dir.empty() ? "artifacts/crashes" : std::move(artifact_dir);
  }

 private:
  CrashRecorder& m_recorder;
  std::string m_artifactDir = "artifacts/crashes";
};

inline ICrashReporter& DefaultCrashReporter() {
  static CrashRecorderReporter reporter;
  return reporter;
}

}  // namespace vkpt::diagnostics
