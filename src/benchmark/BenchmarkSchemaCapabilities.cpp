#include "benchmark/BenchmarkSchema.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include "scene/Json.h"

namespace vkpt::benchmark {
namespace {

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

}  // namespace

BenchmarkCapabilities DefaultBenchmarkCapabilities() {
  BenchmarkCapabilities caps;
  caps.commands = {
      "run",
      "echo-desc",
      "list-scenes",
      "list-backends",
      "list-renderer-paths",
      "validate-scene",
      "validate-artifacts",
      "compare",
      "dump-capabilities",
      "run-experiments",
      "backend-experiments",
      "thread-sweep",
      "simd-sweep",
      "tile-sweep",
      "gpu-mem-pressure",
      "shader-matrix",
      "release-check",
  };
  caps.artifact_exports = DefaultBenchmarkArtifactContract().required_files;
  caps.experiment_support = {
      "cpu-scalar",
      "cpu-tiled",
      "thread-scaling",
      "simd-sweep",
      "tile-height-sweep",
      "gpu-memory-pressure-simulated",
      "shader-matrix-with-skips",
      "backend-experiment-availability-matrix",
  };
  caps.backend_experiment_targets = {
      "vulkan-compute",
      "vulkan-rt",
      "d3d12-compute",
      "d3d12-dxr",
      "metal-compute",
      "metal-rt",
      "webgpu-compute",
  };
  return caps;
}

ProfilerCapabilities DefaultProfilerCapabilities() {
  return {};
}

std::string SerializeProfilerCapabilities(const ProfilerCapabilities& capabilities) {
  vkpt::scene::JsonValue root;
  root.kind = vkpt::scene::JsonValue::Kind::Object;
  root.object["schema_version"] = make_string(capabilities.schema_version);
  root.object["cpu_zones"] = make_bool(capabilities.cpu_zones);
  root.object["gpu_zones"] = make_bool(capabilities.gpu_zones);
  root.object["job_timings"] = make_bool(capabilities.job_timings);
  root.object["frame_stage_timings"] = make_bool(capabilities.frame_stage_timings);
  root.object["asset_import_timings"] = make_bool(capabilities.asset_import_timings);
  root.object["bvh_build_timings"] = make_bool(capabilities.bvh_build_timings);
  root.object["shader_compile_timings"] = make_bool(capabilities.shader_compile_timings);
  root.object["render_pass_timings"] = make_bool(capabilities.render_pass_timings);
  root.object["json_trace_export"] = make_bool(capabilities.json_trace_export);
  return vkpt::scene::JsonParser::stringify(root);
}

std::string SerializeBenchmarkCapabilities(const BenchmarkCapabilities& capabilities) {
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
  root.object["schema_version"] = make_string(capabilities.schema_version);
  root.object["commands"] = to_array(capabilities.commands);
  root.object["artifact_exports"] = to_array(capabilities.artifact_exports);
  root.object["experiment_support"] = to_array(capabilities.experiment_support);
  root.object["backend_experiment_targets"] = to_array(capabilities.backend_experiment_targets);
  root.object["descriptor_files"] = make_bool(capabilities.descriptor_files);
  root.object["artifact_validation"] = make_bool(capabilities.artifact_validation);
  root.object["normalized_scores"] = make_bool(capabilities.normalized_scores);
  root.object["benchmark_runner_interface"] = make_bool(capabilities.benchmark_runner_interface);
  root.object["profiler_service_contract"] = make_bool(capabilities.profiler_service_contract);
  root.object["profiler_trace_export"] = make_bool(capabilities.profiler_trace_export);
  return vkpt::scene::JsonParser::stringify(root);
}

BenchmarkScore ComputeBenchmarkScore(const BenchmarkThroughput& throughput,
                                     const Resolution& resolution,
                                     uint32_t hardware_threads) {
  BenchmarkScore score;
  score.hardware_threads = std::max(1u, hardware_threads);
  const double threadCount = static_cast<double>(score.hardware_threads);
  const double megapixels =
      std::max(1.0e-6, static_cast<double>(resolution.width) * static_cast<double>(resolution.height) / 1.0e6);
  score.samples_per_sec_per_thread = throughput.samples_per_sec / threadCount;
  score.paths_per_sec_per_thread = throughput.paths_per_sec / threadCount;
  score.samples_per_sec_per_megapixel = throughput.samples_per_sec / megapixels;
  score.normalized_score = score.samples_per_sec_per_thread / megapixels;
  score.normalization_basis = "samples_per_sec / hardware_thread / megapixel";
  return score;
}

std::string SummarizeBenchmarkResults(const std::vector<BenchmarkResult>& results) {
  std::ostringstream out;
  out << "runs=" << results.size();
  if (results.empty()) {
    return out.str();
  }
  double totalSamples = 0.0;
  double bestScore = 0.0;
  std::string bestRun;
  for (const auto& result : results) {
    totalSamples += result.throughput.samples_per_sec;
    if (result.score.normalized_score > bestScore) {
      bestScore = result.score.normalized_score;
      bestRun = result.run_id;
    }
  }
  out << " avg_samples_per_sec=" << std::fixed << std::setprecision(2)
      << (totalSamples / static_cast<double>(results.size()));
  out << " best_run=" << bestRun;
  out << " best_normalized_score=" << std::fixed << std::setprecision(2) << bestScore;
  return out.str();
}

std::string_view ProfilerEventKindName(ProfilerEventKind kind) {
  switch (kind) {
    case ProfilerEventKind::CpuZone:      return "cpu_zone";
    case ProfilerEventKind::GpuZone:      return "gpu_zone";
    case ProfilerEventKind::JobTiming:    return "job_timing";
    case ProfilerEventKind::FrameStage:   return "frame_stage";
    case ProfilerEventKind::AssetImport:  return "asset_import";
    case ProfilerEventKind::BvhBuild:     return "bvh_build";
    case ProfilerEventKind::ShaderCompile: return "shader_compile";
    case ProfilerEventKind::RenderPass:   return "render_pass";
    default:                              return "unknown";
  }
}

std::string SerializeProfilerEvent(const ProfilerEvent& event) {
  vkpt::scene::JsonValue root;
  root.kind = vkpt::scene::JsonValue::Kind::Object;
  root.object["kind"]        = make_string(std::string(ProfilerEventKindName(event.kind)));
  root.object["name"]        = make_string(event.name);
  root.object["category"]    = make_string(event.category);
  root.object["thread_id"]   = make_number(static_cast<double>(event.thread_id));
  root.object["start_ms"]    = make_number(event.start_ms);
  root.object["duration_ms"] = make_number(event.duration_ms);
  return vkpt::scene::JsonParser::stringify(root);
}

std::string SerializeProfilerTrace(const std::vector<ProfilerEvent>& trace) {
  vkpt::scene::JsonValue arr;
  arr.kind = vkpt::scene::JsonValue::Kind::Array;
  arr.array.reserve(trace.size());
  for (const auto& ev : trace) {
    vkpt::scene::JsonValue obj;
    obj.kind = vkpt::scene::JsonValue::Kind::Object;
    obj.object["kind"]        = make_string(std::string(ProfilerEventKindName(ev.kind)));
    obj.object["name"]        = make_string(ev.name);
    obj.object["category"]    = make_string(ev.category);
    obj.object["thread_id"]   = make_number(static_cast<double>(ev.thread_id));
    obj.object["start_ms"]    = make_number(ev.start_ms);
    obj.object["duration_ms"] = make_number(ev.duration_ms);
    arr.array.push_back(std::move(obj));
  }
  vkpt::scene::JsonValue root;
  root.kind = vkpt::scene::JsonValue::Kind::Object;
  root.object["events"] = std::move(arr);
  return vkpt::scene::JsonParser::stringify(root);
}


}  // namespace vkpt::benchmark
