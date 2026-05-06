#include "editor/UiModels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::editor {

vkpt::benchmark::BenchmarkRunDesc MakeDefaultBenchmarkRunDesc(std::string_view scene_path,
                                                             std::string_view backend,
                                                             std::string_view renderer_path,
                                                             std::uint32_t spp,
                                                             std::uint32_t max_depth,
                                                             std::uint64_t seed,
                                                             std::uint32_t width,
                                                             std::uint32_t height) {
  vkpt::benchmark::BenchmarkRunDesc desc;
  desc.scene_path = std::string(scene_path);
  desc.backend = std::string(backend);
  desc.renderer_path = std::string(renderer_path);
  desc.resolution.width = width;
  desc.resolution.height = height;
  desc.samples_per_pixel = spp;
  desc.max_depth = max_depth;
  desc.seed = seed;
  desc.duration = 0.0;
  desc.warmup_frames = 0;
  desc.tolerance_policy = "default";
  return desc;
}

BenchmarkScoreModel ComputeBenchmarkScore(double measured_units_per_second,
                                          double expected_units_per_second,
                                          double raw_samples_per_second,
                                          double workload_units,
                                          bool calibration_valid) {
  BenchmarkScoreModel score;
  score.raw_samples_per_second = raw_samples_per_second;
  score.workload_units = workload_units;
  score.expected_units_per_second = expected_units_per_second;
  score.measured_units_per_second = measured_units_per_second;
  score.calibration_valid = calibration_valid;
  if (expected_units_per_second > 0.0) {
    score.normalized_score = measured_units_per_second / expected_units_per_second;
  }
  if (!calibration_valid) {
    score.confidence = "uncalibrated";
    score.warnings.push_back("hardware calibration profile is missing or invalid");
  } else if (score.normalized_score <= 0.0) {
    score.confidence = "invalid";
    score.warnings.push_back("measured throughput is zero");
  } else if (score.normalized_score < 0.75 || score.normalized_score > 1.25) {
    score.confidence = "low";
  } else {
    score.confidence = "high";
  }
  return score;
}

WorkloadComplexityModel EstimateWorkloadComplexity(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                                   std::uint32_t light_count,
                                                   std::uint64_t triangle_count,
                                                   std::uint64_t bvh_node_count,
                                                   std::uint64_t texture_bytes,
                                                   bool denoiser_enabled) {
  WorkloadComplexityModel model;
  model.width = desc.resolution.width;
  model.height = desc.resolution.height;
  model.samples_per_pixel = desc.samples_per_pixel;
  model.max_depth = desc.max_depth;
  model.light_count = light_count;
  model.triangle_count = triangle_count;
  model.bvh_node_count = bvh_node_count;
  model.texture_bytes = texture_bytes;

  const double pixel_count = static_cast<double>(std::max<std::uint32_t>(1u, model.width)) *
                             static_cast<double>(std::max<std::uint32_t>(1u, model.height));
  const double spp_cost = static_cast<double>(std::max<std::uint32_t>(1u, model.samples_per_pixel));
  const double depth_cost = static_cast<double>(std::max<std::uint32_t>(1u, model.max_depth));
  const double light_cost = 1.0 + static_cast<double>(light_count) * 0.08;
  const double triangle_cost = 1.0 + std::log2(1.0 + static_cast<double>(triangle_count)) * 0.02;
  const double bvh_cost = 1.0 + std::log2(1.0 + static_cast<double>(bvh_node_count)) * 0.015;
  const double texture_cost = 1.0 + (static_cast<double>(texture_bytes) / (1024.0 * 1024.0 * 1024.0)) * 0.05;
  const double denoiser_cost = denoiser_enabled ? 1.12 : 1.0;

  model.normalized_cost_units =
      (pixel_count * spp_cost * depth_cost * light_cost * triangle_cost * bvh_cost * texture_cost * denoiser_cost) /
      (1280.0 * 720.0);

  model.cost_drivers.push_back("resolution");
  model.cost_drivers.push_back("SPP");
  model.cost_drivers.push_back("max_depth");
  if (light_count > 0) {
    model.cost_drivers.push_back("light_count");
  }
  if (triangle_count > 0) {
    model.cost_drivers.push_back("triangle_count");
  }
  if (bvh_node_count > 0) {
    model.cost_drivers.push_back("BVH_node_count");
  }
  if (texture_bytes > 0) {
    model.cost_drivers.push_back("texture_memory");
  }
  if (denoiser_enabled) {
    model.cost_drivers.push_back("denoiser");
  }
  if (!desc.renderer_path.empty()) {
    model.cost_drivers.push_back("renderer_path:" + desc.renderer_path);
  }
  if (!desc.backend.empty()) {
    model.cost_drivers.push_back("backend:" + desc.backend);
  }
  return model;
}

std::vector<BenchmarkCalibrationActionModel> BuildDefaultBenchmarkCalibrationActions(
    bool gpu_compute_available,
    bool hardware_rt_available) {
  return {
    {"calibration.cpu_scalar", "Run CPU Scalar Calibration", "cpu", "cpu_scalar", true, {}},
    {"calibration.cpu_threaded", "Run CPU Threaded Calibration", "cpu", "cpu_threaded", true, {}},
    {"calibration.cpu_simd", "Run CPU SIMD Calibration", "cpu", "cpu_simd", true, {}},
    {"calibration.gpu_compute", "Run GPU Compute Calibration", "gpu", "gpu_compute", gpu_compute_available,
     gpu_compute_available ? std::string{} : std::string{"no GPU compute backend is available in this build"}},
    {"calibration.hardware_rt", "Run Hardware RT Calibration", "gpu", "hardware_rt", hardware_rt_available,
     hardware_rt_available ? std::string{} : std::string{"hardware ray tracing is unavailable or not selected"}},
    {"calibration.backend_compare", "Run Backend Comparison", "mixed", "backend_comparison",
     gpu_compute_available || hardware_rt_available,
     (gpu_compute_available || hardware_rt_available) ? std::string{} : std::string{"backend comparison needs at least one GPU backend"}}
  };
}

BenchmarkPanelModel BuildBenchmarkPanelModel(const vkpt::benchmark::BenchmarkRunDesc& desc,
                                            const BenchmarkRawMetricsModel& raw_metrics,
                                            const BenchmarkScoreModel& score,
                                            const WorkloadComplexityModel& workload,
                                            std::string_view artifact_location,
                                            std::string_view result_summary,
                                            bool can_run,
                                            std::string_view unavailable_reason) {
  BenchmarkPanelModel model;
  model.run_desc = desc;
  model.can_run = can_run;
  model.can_cancel = false;
  model.unavailable_reason = std::string(unavailable_reason);
  model.artifact_location = std::string(artifact_location);
  model.result_summary = std::string(result_summary);
  model.raw_metrics = raw_metrics;
  model.score = score;
  model.workload = workload;
  model.calibration_actions = BuildDefaultBenchmarkCalibrationActions(true, false);
  return model;
}

}  // namespace vkpt::editor
