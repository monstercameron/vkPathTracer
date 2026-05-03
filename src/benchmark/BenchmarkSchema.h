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

struct BenchmarkThroughput {
  double paths_per_sec = 0.0;
  double samples_per_sec = 0.0;
};

struct BenchmarkMemory {
  double peak_mb = 0.0;
  double current_mb = 0.0;
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
  BenchmarkThroughput throughput;
  BenchmarkMemory memory;
  std::string image_hash;
  double reference_error = 0.0;
  std::vector<std::string> diagnostics;
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
};

vkpt::core::Result<BenchmarkResult> ParseBenchmarkResultFromText(std::string_view text);
vkpt::core::Result<BenchmarkRunDesc> ParseBenchmarkRunDescFromText(std::string_view text);
vkpt::core::Result<BenchmarkResult> LoadBenchmarkResultFromFile(std::string_view path);
vkpt::core::Result<BenchmarkRunDesc> LoadBenchmarkRunDescFromFile(std::string_view path);

bool ValidateBenchmarkResult(const BenchmarkResult& result, std::string* message = nullptr);
bool ValidateBenchmarkRunDesc(const BenchmarkRunDesc& desc, std::string* message = nullptr);

std::string SerializeBenchmarkResult(const BenchmarkResult& result);
std::string SerializeBenchmarkRunDesc(const BenchmarkRunDesc& desc);

}  // namespace vkpt::benchmark
