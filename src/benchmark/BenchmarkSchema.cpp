#include "benchmark/BenchmarkSchema.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <string>
#include <utility>
#include "scene/Scene.h"

namespace vkpt::benchmark {

namespace {

template <typename T>
bool read_u(std::uint64_t source, T& out) {
  if (source > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    return false;
  }
  out = static_cast<T>(source);
  return true;
}

bool as_bool(const vkpt::scene::JsonValue& object, std::string_view key, bool& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Boolean) {
    return false;
  }
  out = it->second.boolean;
  return true;
}

bool as_u64(const vkpt::scene::JsonValue& object, std::string_view key, std::uint64_t& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  out = static_cast<std::uint64_t>(it->second.number);
  return true;
}

bool as_u32(const vkpt::scene::JsonValue& object, std::string_view key, std::uint32_t& out) {
  std::uint64_t temp = 0;
  if (!as_u64(object, key, temp)) {
    return false;
  }
  return read_u(temp, out);
}

bool as_double(const vkpt::scene::JsonValue& object, std::string_view key, double& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  out = it->second.number;
  return true;
}

bool as_double_optional(const vkpt::scene::JsonValue& object, std::string_view key, double& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Number) {
    return false;
  }
  out = it->second.number;
  return true;
}

bool as_string(const vkpt::scene::JsonValue& object, std::string_view key, std::string& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::String) {
    return false;
  }
  out = it->second.string;
  return true;
}

bool as_string_optional(const vkpt::scene::JsonValue& object, std::string_view key, std::string& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::String) {
    return false;
  }
  out = it->second.string;
  return true;
}

bool as_u32_optional(const vkpt::scene::JsonValue& object, std::string_view key, std::uint32_t& out) {
  std::uint64_t temp = 0;
  if (!as_u64(object, key, temp)) {
    return false;
  }
  return read_u(temp, out);
}

bool as_resolution(const vkpt::scene::JsonValue& object,
                  std::string_view key,
                  Resolution& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_u32(it->second, "width", out.width) && as_u32(it->second, "height", out.height);
}

bool as_string_array(const vkpt::scene::JsonValue& object, std::string_view key, std::vector<std::string>& out) {
  out.clear();
  const auto it = object.object.find(std::string(key));
  if (it == object.object.end() || it->second.kind != vkpt::scene::JsonValue::Kind::Array) {
    return false;
  }
  for (const auto& value : it->second.array) {
    if (value.kind != vkpt::scene::JsonValue::Kind::String) {
      return false;
    }
    out.push_back(value.string);
  }
  return true;
}

vkpt::scene::JsonValue make_string(std::string value) {
  vkpt::scene::JsonValue out;
  out.kind = vkpt::scene::JsonValue::Kind::String;
  out.string = std::move(value);
  return out;
}

vkpt::scene::JsonValue make_number(double value) {
  vkpt::scene::JsonValue out;
  out.kind = vkpt::scene::JsonValue::Kind::Number;
  out.number = value;
  return out;
}

vkpt::scene::JsonValue make_bool(bool value) {
  vkpt::scene::JsonValue out;
  out.kind = vkpt::scene::JsonValue::Kind::Boolean;
  out.boolean = value;
  return out;
}

vkpt::scene::JsonValue make_array(std::vector<vkpt::scene::JsonValue> values) {
  vkpt::scene::JsonValue out;
  out.kind = vkpt::scene::JsonValue::Kind::Array;
  out.array = std::move(values);
  return out;
}

bool parse_build_info(const vkpt::scene::JsonValue& object, BenchmarkResultBuildInfo& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_string(object, "app_version", out.app_version) &&
         as_string(object, "git_hash", out.git_hash) &&
         as_string(object, "build_date", out.build_date) &&
         as_string(object, "compiler", out.compiler) &&
         as_string(object, "build_type", out.build_type);
}

bool parse_device_info(const vkpt::scene::JsonValue& object, BenchmarkResultDeviceInfo& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_string(object, "backend", out.backend) &&
         as_string(object, "renderer_path", out.renderer_path) &&
         as_string(object, "cpu_name", out.cpu_name) &&
         as_string(object, "gpu_name", out.gpu_name);
}

bool parse_timing(const vkpt::scene::JsonValue& object, BenchmarkTiming& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_double(object, "total_ms", out.total_ms) &&
         as_double(object, "build_ms", out.build_ms) &&
         as_double(object, "render_ms", out.render_ms) &&
         as_double(object, "cpu_ms", out.cpu_ms);
}

bool parse_timing_event(const vkpt::scene::JsonValue& object, BenchmarkTimingEvent& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_string(object, "name", out.name) &&
         as_string(object, "category", out.category) &&
         as_double(object, "ms", out.ms);
}

bool parse_timing_breakdown(const vkpt::scene::JsonValue& value, std::vector<BenchmarkTimingEvent>& out) {
  if (value.kind != vkpt::scene::JsonValue::Kind::Array) {
    return false;
  }
  out.clear();
  out.reserve(value.array.size());
  for (const auto& entry : value.array) {
    BenchmarkTimingEvent evt;
    if (!parse_timing_event(entry, evt)) {
      return false;
    }
    out.push_back(std::move(evt));
  }
  return true;
}

bool parse_throughput(const vkpt::scene::JsonValue& object, BenchmarkThroughput& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_double(object, "paths_per_sec", out.paths_per_sec) &&
         as_double(object, "samples_per_sec", out.samples_per_sec);
}

bool parse_memory(const vkpt::scene::JsonValue& object, BenchmarkMemory& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  return as_double(object, "peak_mb", out.peak_mb) &&
         as_double(object, "current_mb", out.current_mb);
}

bool parse_score(const vkpt::scene::JsonValue& object, BenchmarkScore& out) {
  if (object.kind != vkpt::scene::JsonValue::Kind::Object) {
    return false;
  }
  as_double_optional(object, "samples_per_sec_per_thread", out.samples_per_sec_per_thread);
  as_double_optional(object, "paths_per_sec_per_thread", out.paths_per_sec_per_thread);
  as_double_optional(object, "samples_per_sec_per_megapixel", out.samples_per_sec_per_megapixel);
  as_double_optional(object, "normalized_score", out.normalized_score);
  as_u32_optional(object, "hardware_threads", out.hardware_threads);
  as_string_optional(object, "normalization_basis", out.normalization_basis);
  return true;
}

std::string read_file(std::string_view path) {
  std::ifstream stream{std::string(path)};
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

std::string_view strip_utf8_bom(std::string_view text) {
  if (text.size() >= 3u &&
      static_cast<unsigned char>(text[0]) == 0xefu &&
      static_cast<unsigned char>(text[1]) == 0xbbu &&
      static_cast<unsigned char>(text[2]) == 0xbfu) {
    text.remove_prefix(3u);
  }
  return text;
}

std::filesystem::path resolve_artifact_path(const std::filesystem::path& artifactDir, const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const std::filesystem::path path(value);
  if (path.is_absolute()) {
    return path;
  }
  if (std::filesystem::exists(path)) {
    return path;
  }
  return artifactDir / path.filename();
}

void validate_artifact_file(const std::filesystem::path& artifactDir,
                            const std::string& name,
                            bool required,
                            bool requireNonEmpty,
                            BenchmarkArtifactValidation& out) {
  const auto path = artifactDir / name;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (required) {
      out.missing_files.push_back(name);
    }
    return;
  }
  if (!std::filesystem::is_regular_file(path, ec)) {
    out.invalid_files.push_back(name + ": not a regular file");
    return;
  }
  out.present_files.push_back(name);
  if (requireNonEmpty && std::filesystem::file_size(path, ec) == 0u && !ec) {
    out.empty_files.push_back(name);
  }
}

}  // namespace

vkpt::core::Result<BenchmarkResult> ParseBenchmarkResultFromText(std::string_view text) {
  const auto root = vkpt::scene::JsonParser::parse(strip_utf8_bom(text));
  if (!root || root->kind != vkpt::scene::JsonValue::Kind::Object) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  BenchmarkResult result;
  if (!as_string(*root, "run_id", result.run_id) ||
      !as_string(*root, "scene", result.scene) ||
      !as_string(*root, "backend", result.backend) ||
      !as_string(*root, "renderer_path", result.renderer_path) ||
      !as_string(*root, "cpu_simd_mode", result.cpu_simd_mode) ||
      !as_resolution(*root, "resolution", result.resolution) ||
      !as_u32(*root, "spp", result.spp) ||
      !as_u64(*root, "seed", result.seed) ||
      !as_string(*root, "scene_hash", result.scene_hash) ||
      !as_string(*root, "asset_hash", result.asset_hash) ||
      !as_string(*root, "shader_hash", result.shader_hash) ||
      !as_string(*root, "image_hash", result.image_hash) ||
      !as_double(*root, "reference_error", result.reference_error)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  if (!as_u32_optional(*root, "max_depth", result.max_depth)) {
    result.max_depth = 6;
  }
  if (result.max_depth == 0) {
    result.max_depth = 6;
  }
  if (!as_string_optional(*root, "tolerance_policy", result.tolerance_policy)) {
    result.tolerance_policy = "abs=0.001";
  }

  const auto buildIt = root->object.find("build_info");
  if (buildIt == root->object.end() || buildIt->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      !parse_build_info(buildIt->second, result.build_info)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  const auto deviceIt = root->object.find("device_info");
  if (deviceIt == root->object.end() || deviceIt->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      !parse_device_info(deviceIt->second, result.device_info)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  const auto timingIt = root->object.find("timing");
  if (timingIt == root->object.end() || timingIt->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      !parse_timing(timingIt->second, result.timing)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  const auto breakdownIt = root->object.find("timing_breakdown");
  if (breakdownIt != root->object.end()) {
    if (!parse_timing_breakdown(breakdownIt->second, result.timing_breakdown)) {
      return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
    }
  } else {
    result.timing_breakdown.clear();
  }

  const auto throughputIt = root->object.find("throughput");
  if (throughputIt == root->object.end() || throughputIt->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      !parse_throughput(throughputIt->second, result.throughput)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  const auto memoryIt = root->object.find("memory");
  if (memoryIt == root->object.end() || memoryIt->second.kind != vkpt::scene::JsonValue::Kind::Object ||
      !parse_memory(memoryIt->second, result.memory)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  const auto scoreIt = root->object.find("score");
  if (scoreIt != root->object.end() && !parse_score(scoreIt->second, result.score)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  if (!as_string_array(*root, "diagnostics", result.diagnostics)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  if (!as_string_optional(*root, "output_directory", result.output_directory)) {
    result.output_directory = "";
  }
  if (!as_string_optional(*root, "artifact_directory", result.artifact_directory)) {
    result.artifact_directory = "";
  }
  if (!as_string_optional(*root, "beauty_png", result.beauty_png)) {
    result.beauty_png = "";
  }
  if (!as_string_optional(*root, "beauty_exr", result.beauty_exr)) {
    result.beauty_exr = "";
  }
  if (!as_string_optional(*root, "diff_heatmap_png", result.diff_heatmap_png)) {
    result.diff_heatmap_png = "";
  }
  if (!as_string_optional(*root, "reference_exr", result.reference_exr)) {
    result.reference_exr = "";
  }
  if (!as_string_optional(*root, "profiler_trace_json", result.profiler_trace_json)) {
    result.profiler_trace_json = "";
  }
  if (!as_string_optional(*root, "logs_jsonl", result.logs_jsonl)) {
    result.logs_jsonl = "";
  }
  if (result.output_directory.empty()) {
    result.output_directory = "";
  }
  if (result.artifact_directory.empty()) {
    result.artifact_directory = "";
  }
  if (result.beauty_png.empty()) {
    result.beauty_png = "";
  }
  if (result.beauty_exr.empty()) {
    result.beauty_exr = "";
  }
  if (result.diff_heatmap_png.empty()) {
    result.diff_heatmap_png = "";
  }
  if (result.reference_exr.empty()) {
    result.reference_exr = "";
  }
  if (result.profiler_trace_json.empty()) {
    result.profiler_trace_json = "";
  }
  if (result.logs_jsonl.empty()) {
    result.logs_jsonl = "";
  }

  std::string issue;
  if (!ValidateBenchmarkResult(result, &issue)) {
    (void)issue;
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  return vkpt::core::Result<BenchmarkResult>::ok(std::move(result));
}

vkpt::core::Result<BenchmarkRunDesc> ParseBenchmarkRunDescFromText(std::string_view text) {
  const auto root = vkpt::scene::JsonParser::parse(strip_utf8_bom(text));
  if (!root || root->kind != vkpt::scene::JsonValue::Kind::Object) {
    return vkpt::core::Result<BenchmarkRunDesc>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  BenchmarkRunDesc desc;
  if (!as_string(*root, "scene_path", desc.scene_path) ||
      !as_string(*root, "backend", desc.backend) ||
      !as_string(*root, "renderer_path", desc.renderer_path) ||
      !as_resolution(*root, "resolution", desc.resolution) ||
      !as_u32(*root, "samples_per_pixel", desc.samples_per_pixel) ||
      !as_double(*root, "duration", desc.duration) ||
      !as_u32(*root, "warmup_frames", desc.warmup_frames) ||
      !as_u64(*root, "seed", desc.seed)) {
    return vkpt::core::Result<BenchmarkRunDesc>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  if (!as_string_optional(*root, "output_directory", desc.output_directory)) {
    desc.output_directory = "artifacts/benchmarks";
  }
  if (desc.output_directory.empty()) {
    desc.output_directory = "artifacts/benchmarks";
  }
  if (!as_string_optional(*root, "reference_image", desc.reference_image)) {
    desc.reference_image = "";
  }
  if (!as_string_optional(*root, "tolerance_policy", desc.tolerance_policy)) {
    desc.tolerance_policy = "abs=0.001";
  }
  if (!as_u32_optional(*root, "max_depth", desc.max_depth)) {
    desc.max_depth = 6;
  }
  if (desc.max_depth == 0) {
    desc.max_depth = 6;
  }
  if (!as_u32_optional(*root, "worker_count", desc.worker_count)) {
    desc.worker_count = 0;
  }
  if (!as_u32_optional(*root, "tile_height", desc.tile_height)) {
    desc.tile_height = 16;
  }
  if (desc.tile_height == 0) {
    desc.tile_height = 16;
  }
  if (!as_bool(*root, "deterministic", desc.deterministic)) {
    desc.deterministic = false;
  }

  std::string issue;
  if (!ValidateBenchmarkRunDesc(desc, &issue)) {
    (void)issue;
    return vkpt::core::Result<BenchmarkRunDesc>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  return vkpt::core::Result<BenchmarkRunDesc>::ok(std::move(desc));
}

vkpt::core::Result<BenchmarkResult> LoadBenchmarkResultFromFile(std::string_view path) {
  const auto text = read_file(path);
  if (text.empty()) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::IOError);
  }
  return ParseBenchmarkResultFromText(text);
}

vkpt::core::Result<BenchmarkRunDesc> LoadBenchmarkRunDescFromFile(std::string_view path) {
  const auto text = read_file(path);
  if (text.empty()) {
    return vkpt::core::Result<BenchmarkRunDesc>::error(vkpt::core::ErrorCode::IOError);
  }
  return ParseBenchmarkRunDescFromText(text);
}

bool ValidateBenchmarkResult(const BenchmarkResult& result, std::string* message) {
  if (result.run_id.empty()) {
    if (message) *message = "run_id is empty";
    return false;
  }
  if (result.scene.empty()) {
    if (message) *message = "scene is empty";
    return false;
  }
  if (result.backend.empty()) {
    if (message) *message = "backend is empty";
    return false;
  }
  if (result.resolution.width == 0 || result.resolution.height == 0) {
    if (message) *message = "resolution has zero dimension";
    return false;
  }
  if (result.spp == 0) {
    if (message) *message = "spp must be non-zero";
    return false;
  }
  if (result.cpu_simd_mode.empty()) {
    if (message) *message = "cpu_simd_mode is empty";
    return false;
  }
  if (result.max_depth == 0) {
    if (message) *message = "max_depth is zero";
    return false;
  }
  if (result.build_info.app_version.empty()) {
    if (message) *message = "build_info.app_version is empty";
    return false;
  }
  if (result.timing.total_ms < 0.0 || result.timing.build_ms < 0.0 ||
      result.timing.render_ms < 0.0 || result.timing.cpu_ms < 0.0) {
    if (message) *message = "timing fields must be non-negative";
    return false;
  }
  if (result.throughput.paths_per_sec < 0.0 || result.throughput.samples_per_sec < 0.0) {
    if (message) *message = "throughput fields must be non-negative";
    return false;
  }
  if (result.memory.peak_mb < 0.0 || result.memory.current_mb < 0.0) {
    if (message) *message = "memory fields must be non-negative";
    return false;
  }
  if (!std::isfinite(result.reference_error) || result.reference_error < 0.0) {
    if (message) *message = "reference_error must be finite and non-negative";
    return false;
  }
  for (const auto& event : result.timing_breakdown) {
    if (event.name.empty() || event.category.empty()) {
      if (message) *message = "timing_breakdown entries require name and category";
      return false;
    }
    if (!std::isfinite(event.ms) || event.ms < 0.0) {
      if (message) *message = "timing_breakdown entries require non-negative ms";
      return false;
    }
  }
  return true;
}

bool ValidateBenchmarkRunDesc(const BenchmarkRunDesc& desc, std::string* message) {
  if (desc.scene_path.empty()) {
    if (message) *message = "scene_path is empty";
    return false;
  }
  if (desc.backend.empty()) {
    if (message) *message = "backend is empty";
    return false;
  }
  if (desc.renderer_path.empty()) {
    if (message) *message = "renderer_path is empty";
    return false;
  }
  if (desc.resolution.width == 0 || desc.resolution.height == 0) {
    if (message) *message = "resolution has zero dimension";
    return false;
  }
  if (desc.samples_per_pixel == 0) {
    if (message) *message = "samples_per_pixel is zero";
    return false;
  }
  if (desc.warmup_frames > 100000) {
    if (message) *message = "warmup_frames is out of supported range";
    return false;
  }
  if (desc.max_depth == 0) {
    if (message) *message = "max_depth is zero";
    return false;
  }
  if (desc.output_directory.empty()) {
    if (message) *message = "output_directory is empty";
    return false;
  }
  if (desc.tile_height == 0) {
    if (message) *message = "tile_height is zero";
    return false;
  }
  return true;
}

std::string SerializeBenchmarkResult(const BenchmarkResult& result) {
  vkpt::scene::JsonValue root;
  root.kind = vkpt::scene::JsonValue::Kind::Object;

  root.object["run_id"] = make_string(result.run_id);
  root.object["scene"] = make_string(result.scene);
  root.object["backend"] = make_string(result.backend);
  root.object["renderer_path"] = make_string(result.renderer_path);
  root.object["cpu_simd_mode"] = make_string(result.cpu_simd_mode);

  vkpt::scene::JsonValue resolution;
  resolution.kind = vkpt::scene::JsonValue::Kind::Object;
  resolution.object["width"] = make_number(result.resolution.width);
  resolution.object["height"] = make_number(result.resolution.height);
  root.object["resolution"] = resolution;

  root.object["spp"] = make_number(result.spp);
  root.object["seed"] = make_number(static_cast<double>(result.seed));
  root.object["max_depth"] = make_number(result.max_depth);
  root.object["tolerance_policy"] = make_string(result.tolerance_policy);
  root.object["scene_hash"] = make_string(result.scene_hash);
  root.object["asset_hash"] = make_string(result.asset_hash);
  root.object["shader_hash"] = make_string(result.shader_hash);

  vkpt::scene::JsonValue buildInfo;
  buildInfo.kind = vkpt::scene::JsonValue::Kind::Object;
  buildInfo.object["app_version"] = make_string(result.build_info.app_version);
  buildInfo.object["git_hash"] = make_string(result.build_info.git_hash);
  buildInfo.object["build_date"] = make_string(result.build_info.build_date);
  buildInfo.object["compiler"] = make_string(result.build_info.compiler);
  buildInfo.object["build_type"] = make_string(result.build_info.build_type);
  root.object["build_info"] = buildInfo;

  vkpt::scene::JsonValue deviceInfo;
  deviceInfo.kind = vkpt::scene::JsonValue::Kind::Object;
  deviceInfo.object["backend"] = make_string(result.device_info.backend);
  deviceInfo.object["renderer_path"] = make_string(result.device_info.renderer_path);
  deviceInfo.object["cpu_name"] = make_string(result.device_info.cpu_name);
  deviceInfo.object["gpu_name"] = make_string(result.device_info.gpu_name);
  root.object["device_info"] = deviceInfo;

  vkpt::scene::JsonValue timing;
  timing.kind = vkpt::scene::JsonValue::Kind::Object;
  timing.object["total_ms"] = make_number(result.timing.total_ms);
  timing.object["build_ms"] = make_number(result.timing.build_ms);
  timing.object["render_ms"] = make_number(result.timing.render_ms);
  timing.object["cpu_ms"] = make_number(result.timing.cpu_ms);
  root.object["timing"] = timing;

  std::vector<vkpt::scene::JsonValue> breakdown;
  breakdown.reserve(result.timing_breakdown.size());
  for (const auto& evt : result.timing_breakdown) {
    vkpt::scene::JsonValue o;
    o.kind = vkpt::scene::JsonValue::Kind::Object;
    o.object["name"] = make_string(evt.name);
    o.object["category"] = make_string(evt.category);
    o.object["ms"] = make_number(evt.ms);
    breakdown.push_back(std::move(o));
  }
  root.object["timing_breakdown"] = make_array(std::move(breakdown));

  vkpt::scene::JsonValue throughput;
  throughput.kind = vkpt::scene::JsonValue::Kind::Object;
  throughput.object["paths_per_sec"] = make_number(result.throughput.paths_per_sec);
  throughput.object["samples_per_sec"] = make_number(result.throughput.samples_per_sec);
  root.object["throughput"] = throughput;

  vkpt::scene::JsonValue memory;
  memory.kind = vkpt::scene::JsonValue::Kind::Object;
  memory.object["peak_mb"] = make_number(result.memory.peak_mb);
  memory.object["current_mb"] = make_number(result.memory.current_mb);
  root.object["memory"] = memory;

  vkpt::scene::JsonValue score;
  score.kind = vkpt::scene::JsonValue::Kind::Object;
  score.object["samples_per_sec_per_thread"] = make_number(result.score.samples_per_sec_per_thread);
  score.object["paths_per_sec_per_thread"] = make_number(result.score.paths_per_sec_per_thread);
  score.object["samples_per_sec_per_megapixel"] = make_number(result.score.samples_per_sec_per_megapixel);
  score.object["normalized_score"] = make_number(result.score.normalized_score);
  score.object["hardware_threads"] = make_number(result.score.hardware_threads);
  score.object["normalization_basis"] = make_string(result.score.normalization_basis);
  root.object["score"] = score;

  root.object["image_hash"] = make_string(result.image_hash);
  root.object["reference_error"] = make_number(result.reference_error);
  root.object["output_directory"] = make_string(result.output_directory);
  root.object["artifact_directory"] = make_string(result.artifact_directory);
  root.object["beauty_png"] = make_string(result.beauty_png);
  root.object["beauty_exr"] = make_string(result.beauty_exr);
  root.object["diff_heatmap_png"] = make_string(result.diff_heatmap_png);
  root.object["reference_exr"] = make_string(result.reference_exr);
  root.object["profiler_trace_json"] = make_string(result.profiler_trace_json);
  root.object["logs_jsonl"] = make_string(result.logs_jsonl);
  std::vector<vkpt::scene::JsonValue> diagnostics;
  for (const auto& entry : result.diagnostics) {
    diagnostics.push_back(make_string(entry));
  }
  root.object["diagnostics"] = make_array(std::move(diagnostics));

  return vkpt::scene::JsonParser::stringify(root);
}

std::string SerializeBenchmarkRunDesc(const BenchmarkRunDesc& desc) {
  vkpt::scene::JsonValue root;
  root.kind = vkpt::scene::JsonValue::Kind::Object;
  root.object["scene_path"] = make_string(desc.scene_path);
  root.object["backend"] = make_string(desc.backend);
  root.object["renderer_path"] = make_string(desc.renderer_path);

  vkpt::scene::JsonValue resolution;
  resolution.kind = vkpt::scene::JsonValue::Kind::Object;
  resolution.object["width"] = make_number(desc.resolution.width);
  resolution.object["height"] = make_number(desc.resolution.height);
  root.object["resolution"] = resolution;

  root.object["samples_per_pixel"] = make_number(desc.samples_per_pixel);
  root.object["duration"] = make_number(desc.duration);
  root.object["warmup_frames"] = make_number(desc.warmup_frames);
  root.object["seed"] = make_number(static_cast<double>(desc.seed));
  root.object["output_directory"] = make_string(desc.output_directory);
  root.object["reference_image"] = make_string(desc.reference_image);
  root.object["tolerance_policy"] = make_string(desc.tolerance_policy);
  root.object["max_depth"] = make_number(desc.max_depth);
  root.object["worker_count"] = make_number(desc.worker_count);
  root.object["tile_height"] = make_number(desc.tile_height);
  root.object["deterministic"] = make_bool(desc.deterministic);

  return vkpt::scene::JsonParser::stringify(root);
}

BenchmarkArtifactContract DefaultBenchmarkArtifactContract() {
  BenchmarkArtifactContract contract;
  contract.required_files = {
      "results.json",
      "results.csv",
      "metadata.json",
      "scene_snapshot.json",
      "shader_manifest.json",
      "asset_manifest.json",
      "beauty.png",
      "beauty.exr",
      "logs.jsonl",
      "profiler_trace.json",
  };
  contract.optional_files = {
      "reference.exr",
      "diff_heatmap.png",
  };
  contract.require_non_empty = true;
  return contract;
}

BenchmarkArtifactValidation ValidateBenchmarkArtifactsOnDisk(std::string_view artifact_directory) {
  BenchmarkArtifactValidation out;
  out.artifact_directory = std::string(artifact_directory);
  const std::filesystem::path artifactDir(out.artifact_directory);
  const auto contract = DefaultBenchmarkArtifactContract();
  std::error_code ec;
  if (out.artifact_directory.empty() || !std::filesystem::exists(artifactDir, ec) ||
      !std::filesystem::is_directory(artifactDir, ec)) {
    out.missing_files.push_back("artifact_directory");
    out.ok = false;
    return out;
  }

  for (const auto& file : contract.required_files) {
    validate_artifact_file(artifactDir, file, true, contract.require_non_empty, out);
  }
  for (const auto& file : contract.optional_files) {
    validate_artifact_file(artifactDir, file, false, contract.require_non_empty, out);
  }

  const auto result = LoadBenchmarkResultFromFile((artifactDir / "results.json").string());
  if (!result) {
    out.invalid_files.push_back("results.json: failed to parse benchmark result schema");
  } else {
    const auto& r = result.value();
    const std::vector<std::pair<std::string, std::string>> resultPaths = {
        {"beauty_png", r.beauty_png},
        {"beauty_exr", r.beauty_exr},
        {"logs_jsonl", r.logs_jsonl},
        {"profiler_trace_json", r.profiler_trace_json},
    };
    for (const auto& item : resultPaths) {
      if (item.second.empty()) {
        out.invalid_files.push_back("results.json: missing " + item.first);
        continue;
      }
      const auto resolved = resolve_artifact_path(artifactDir, item.second);
      if (resolved.empty() || !std::filesystem::exists(resolved, ec)) {
        out.invalid_files.push_back("results.json: " + item.first + " does not resolve to an artifact file");
      }
    }
    if (!r.reference_exr.empty()) {
      const auto reference = resolve_artifact_path(artifactDir, r.reference_exr);
      if (reference.empty() || !std::filesystem::exists(reference, ec)) {
        out.invalid_files.push_back("results.json: reference_exr does not resolve to an artifact file");
      }
    }
    if (!r.diff_heatmap_png.empty()) {
      const auto diff = resolve_artifact_path(artifactDir, r.diff_heatmap_png);
      if (diff.empty() || !std::filesystem::exists(diff, ec)) {
        out.invalid_files.push_back("results.json: diff_heatmap_png does not resolve to an artifact file");
      }
    }
  }

  const auto metadataText = read_file((artifactDir / "metadata.json").string());
  if (metadataText.find("\"benchmark_capabilities\"") == std::string::npos) {
    out.invalid_files.push_back("metadata.json: missing benchmark_capabilities");
  }
  const auto profilerText = read_file((artifactDir / "profiler_trace.json").string());
  if (profilerText.find("\"events\"") == std::string::npos) {
    out.invalid_files.push_back("profiler_trace.json: missing events array");
  }

  out.ok = out.missing_files.empty() && out.empty_files.empty() && out.invalid_files.empty();
  return out;
}

std::string SerializeBenchmarkArtifactValidation(const BenchmarkArtifactValidation& validation) {
  auto to_array = [](const std::vector<std::string>& values) {
    std::vector<vkpt::scene::JsonValue> arr;
    arr.reserve(values.size());
    for (const auto& value : values) {
      arr.push_back(make_string(value));
    }
    return make_array(std::move(arr));
  };

  vkpt::scene::JsonValue root;
  root.kind = vkpt::scene::JsonValue::Kind::Object;
  root.object["ok"] = make_bool(validation.ok);
  root.object["artifact_directory"] = make_string(validation.artifact_directory);
  root.object["present_files"] = to_array(validation.present_files);
  root.object["missing_files"] = to_array(validation.missing_files);
  root.object["empty_files"] = to_array(validation.empty_files);
  root.object["invalid_files"] = to_array(validation.invalid_files);
  return vkpt::scene::JsonParser::stringify(root);
}

}  // namespace vkpt::benchmark
