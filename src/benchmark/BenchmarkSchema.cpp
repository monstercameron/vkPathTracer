#include "benchmark/BenchmarkSchema.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <limits>
#include <string>

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

std::string read_file(std::string_view path) {
  std::ifstream stream{std::string(path)};
  std::ostringstream contents;
  contents << stream.rdbuf();
  return contents.str();
}

}  // namespace

vkpt::core::Result<BenchmarkResult> ParseBenchmarkResultFromText(std::string_view text) {
  const auto root = vkpt::scene::JsonParser::parse(text);
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
      !as_u32(*root, "max_depth", result.max_depth) ||
      !as_string(*root, "scene_hash", result.scene_hash) ||
      !as_string(*root, "asset_hash", result.asset_hash) ||
      !as_string(*root, "shader_hash", result.shader_hash) ||
      !as_string(*root, "image_hash", result.image_hash) ||
      !as_double(*root, "reference_error", result.reference_error)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
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

  if (!as_string_array(*root, "diagnostics", result.diagnostics)) {
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }

  std::string issue;
  if (!ValidateBenchmarkResult(result, &issue)) {
    (void)issue;
    return vkpt::core::Result<BenchmarkResult>::error(vkpt::core::ErrorCode::InvalidArgument);
  }
  return vkpt::core::Result<BenchmarkResult>::ok(std::move(result));
}

vkpt::core::Result<BenchmarkRunDesc> ParseBenchmarkRunDescFromText(std::string_view text) {
  const auto root = vkpt::scene::JsonParser::parse(text);
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
      !as_u64(*root, "seed", desc.seed) ||
      !as_string(*root, "output_directory", desc.output_directory) ||
      !as_string(*root, "reference_image", desc.reference_image) ||
      !as_string(*root, "tolerance_policy", desc.tolerance_policy)) {
    return vkpt::core::Result<BenchmarkRunDesc>::error(vkpt::core::ErrorCode::InvalidArgument);
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
  if (result.build_info.app_version.empty()) {
    if (message) *message = "build_info.app_version is empty";
    return false;
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
  if (desc.output_directory.empty()) {
    if (message) *message = "output_directory is empty";
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

  root.object["image_hash"] = make_string(result.image_hash);
  root.object["reference_error"] = make_number(result.reference_error);
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

  return vkpt::scene::JsonParser::stringify(root);
}

}  // namespace vkpt::benchmark
