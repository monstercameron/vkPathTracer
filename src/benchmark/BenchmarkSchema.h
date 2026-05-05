#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Types.h"

namespace vkpt::benchmark {

struct Resolution {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct BenchmarkTiming {
  double total_ms = 0.0;
  double build_ms = 0.0;
  double render_ms = 0.0;
  double cpu_ms = 0.0;
};

struct BenchmarkTimingEvent {
  std::string name;
  std::string category;   // e.g. "startup", "scene", "render", "io"
  double ms = 0.0;
};

enum class ProfilerEventKind : uint8_t {
  CpuZone,
  GpuZone,
  JobTiming,
  FrameStage,
  AssetImport,
  BvhBuild,
  ShaderCompile,
  RenderPass,
};

struct ProfilerEvent {
  ProfilerEventKind kind = ProfilerEventKind::CpuZone;
  std::string name;
  std::string category;
  uint32_t thread_id = 0;
  double start_ms = 0.0;
  double duration_ms = 0.0;
};

std::string_view ProfilerEventKindName(ProfilerEventKind kind);

struct BenchmarkThroughput {
  double paths_per_sec = 0.0;
  double samples_per_sec = 0.0;
};

struct BenchmarkMemory {
  double peak_mb = 0.0;
  double current_mb = 0.0;
};

struct BenchmarkScore {
  double samples_per_sec_per_thread = 0.0;
  double paths_per_sec_per_thread = 0.0;
  double samples_per_sec_per_megapixel = 0.0;
  double normalized_score = 0.0;
  uint32_t hardware_threads = 0;
  std::string normalization_basis;
};

struct BenchmarkResultBuildInfo {
  std::string app_version;
  std::string git_hash;
  std::string build_date;
  std::string compiler;
  std::string build_type;
};

struct BenchmarkResultDeviceInfo {
  std::string backend;
  std::string renderer_path;
  std::string cpu_name;
  std::string gpu_name;
};

struct BenchmarkResult {
  std::string run_id;
  std::string scene;
  std::string backend;
  std::string renderer_path;
  std::string cpu_simd_mode;
  std::string tolerance_policy;
  Resolution resolution;
  uint32_t spp = 0;
  uint64_t seed = 0;
  uint32_t max_depth = 0;
  BenchmarkResultBuildInfo build_info;
  BenchmarkResultDeviceInfo device_info;
  std::string scene_hash;
  std::string asset_hash;
  std::string shader_hash;
  BenchmarkTiming timing;
  std::vector<BenchmarkTimingEvent> timing_breakdown;
  BenchmarkThroughput throughput;
  BenchmarkMemory memory;
  BenchmarkScore score;
  std::string image_hash;
  double reference_error = 0.0;
  std::vector<std::string> diagnostics;

  std::string output_directory;
  std::string artifact_directory;
  std::string beauty_png;
  std::string beauty_exr;
  std::string diff_heatmap_png;
  std::string reference_exr;
  std::string profiler_trace_json;
  std::string logs_jsonl;
};

struct BenchmarkRunDesc {
  std::string scene_path;
  std::string backend;
  std::string renderer_path;
  Resolution resolution;
  uint32_t samples_per_pixel = 1;
  double duration = 0.0;
  uint32_t warmup_frames = 0;
  uint64_t seed = 0;
  std::string output_directory;
  std::string reference_image;
  std::string tolerance_policy;
  uint32_t max_depth = 6;
  uint32_t worker_count = 0;
  uint32_t tile_height = 16;
  bool deterministic = false;
};

struct BenchmarkArtifactContract {
  std::vector<std::string> required_files;
  std::vector<std::string> optional_files;
  bool require_non_empty = true;
};

struct BenchmarkArtifactValidation {
  bool ok = false;
  std::string artifact_directory;
  std::vector<std::string> present_files;
  std::vector<std::string> missing_files;
  std::vector<std::string> empty_files;
  std::vector<std::string> invalid_files;
};

struct ProfilerCapabilities {
  std::string schema_version = "1.0";
  bool cpu_zones = true;
  bool gpu_zones = true;
  bool job_timings = true;
  bool frame_stage_timings = true;
  bool asset_import_timings = true;
  bool bvh_build_timings = true;
  bool shader_compile_timings = true;
  bool render_pass_timings = true;
  bool json_trace_export = true;
};

struct BenchmarkCapabilities {
  std::string schema_version = "1.0";
  std::vector<std::string> commands;
  std::vector<std::string> artifact_exports;
  std::vector<std::string> experiment_support;
  std::vector<std::string> backend_experiment_targets;
  bool descriptor_files = true;
  bool artifact_validation = true;
  bool normalized_scores = true;
  bool benchmark_runner_interface = true;
  bool profiler_service_contract = true;
  bool profiler_trace_export = true;
};

using ProfilerEventHandle = uint64_t;

class IProfiler {
 public:
  virtual ~IProfiler() = default;
  virtual ProfilerEventHandle begin_event(ProfilerEventKind kind,
                                          std::string_view name,
                                          std::string_view category,
                                          uint32_t thread_id) = 0;
  virtual void end_event(ProfilerEventHandle handle) = 0;
  virtual std::string emit_trace() const = 0;
  virtual void reset_frame() = 0;
  virtual ProfilerCapabilities describe_capabilities() const = 0;
};

class IBenchmarkRunner {
 public:
  virtual ~IBenchmarkRunner() = default;
  virtual vkpt::core::Result<BenchmarkResult> run_once(const BenchmarkRunDesc& desc) = 0;
  virtual vkpt::core::Result<std::vector<BenchmarkResult>> run_suite(const std::vector<BenchmarkRunDesc>& descs) = 0;
  virtual BenchmarkArtifactValidation validate_artifacts(std::string_view artifact_directory) const = 0;
  virtual std::string summarize_results(const std::vector<BenchmarkResult>& results) const = 0;
};

vkpt::core::Result<BenchmarkResult> ParseBenchmarkResultFromText(std::string_view text);
vkpt::core::Result<BenchmarkRunDesc> ParseBenchmarkRunDescFromText(std::string_view text);
vkpt::core::Result<BenchmarkResult> LoadBenchmarkResultFromFile(std::string_view path);
vkpt::core::Result<BenchmarkRunDesc> LoadBenchmarkRunDescFromFile(std::string_view path);

bool ValidateBenchmarkResult(const BenchmarkResult& result, std::string* message = nullptr);
bool ValidateBenchmarkRunDesc(const BenchmarkRunDesc& desc, std::string* message = nullptr);

std::string SerializeBenchmarkResult(const BenchmarkResult& result);
std::string SerializeBenchmarkRunDesc(const BenchmarkRunDesc& desc);
std::string SerializeBenchmarkArtifactValidation(const BenchmarkArtifactValidation& validation);
std::string SerializeBenchmarkCapabilities(const BenchmarkCapabilities& capabilities);
std::string SerializeProfilerCapabilities(const ProfilerCapabilities& capabilities);

BenchmarkArtifactContract DefaultBenchmarkArtifactContract();
BenchmarkArtifactValidation ValidateBenchmarkArtifactsOnDisk(std::string_view artifact_directory);
BenchmarkCapabilities DefaultBenchmarkCapabilities();
ProfilerCapabilities DefaultProfilerCapabilities();
BenchmarkScore ComputeBenchmarkScore(const BenchmarkThroughput& throughput,
                                     const Resolution& resolution,
                                     uint32_t hardware_threads);
std::string SummarizeBenchmarkResults(const std::vector<BenchmarkResult>& results);

std::string SerializeProfilerEvent(const ProfilerEvent& event);
std::string SerializeProfilerTrace(const std::vector<ProfilerEvent>& trace);

}  // namespace vkpt::benchmark
