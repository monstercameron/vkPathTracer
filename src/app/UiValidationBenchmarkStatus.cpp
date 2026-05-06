#include "app/UiValidationInternal.h"

#include "benchmark/BenchmarkSchema.h"

#include <string>

namespace vkpt::app {

void RunUiBenchmarkStatusSmokeChecks(const UiSmokeCheckFn& check_true,
                                     const vkpt::editor::SelectionState& selection) {
  using namespace vkpt::editor;
  const auto benchmark_desc = MakeDefaultBenchmarkRunDesc("scenes/test.json", "vulkan", "hybrid", 128, 10, 42, 1024, 576);
  check_true("benchmark desc scene path", benchmark_desc.scene_path == "scenes/test.json");
  check_true("benchmark desc backend", benchmark_desc.backend == "vulkan");
  check_true("benchmark desc renderer path", benchmark_desc.renderer_path == "hybrid");
  check_true("benchmark desc spp", benchmark_desc.samples_per_pixel == 128);
  check_true("benchmark desc width", benchmark_desc.resolution.width == 1024);
  check_true("benchmark desc height", benchmark_desc.resolution.height == 576);
  check_true("benchmark desc max depth", benchmark_desc.max_depth == 10);
  check_true("benchmark desc seed", benchmark_desc.seed == 42);
  check_true("benchmark desc tolerance default", benchmark_desc.tolerance_policy == "default");
  const auto workload = EstimateWorkloadComplexity(benchmark_desc, 3, 1024, 512, 16 * 1024 * 1024, true);
  check_true("workload model has cost", workload.normalized_cost_units > 0.0);
  check_true("workload model explains drivers", !workload.cost_drivers.empty());
  auto normalized_score = ComputeBenchmarkScore(2048.0, 2048.0, 1024.0, workload.normalized_cost_units, true);
  normalized_score.raw_paths_per_second = 4096.0;
  normalized_score.raw_gpu_ms = 4.0;
  normalized_score.raw_cpu_ms = 2.0;
  const BenchmarkRawMetricsModel raw_metrics{
    120.0, 8.33, 4.0, 2.0, 1024.0, 4096.0, 8192.0, 128, 32 * 1024 * 1024, 1.5, 0.25
  };
  const auto benchmark_panel = BuildBenchmarkPanelModel(
      benchmark_desc, raw_metrics, normalized_score, workload,
      "artifacts/benchmarks/ui-smoke", "ok", true);
  const std::string benchmark_panel_json = SerializeBenchmarkPanelModel(benchmark_panel);
  check_true("benchmark panel serializes raw metrics",
             benchmark_panel_json.find("\"raw_metrics\"") != std::string::npos &&
             benchmark_panel_json.find("\"path_vertices_per_second\":8192") != std::string::npos);
  check_true("benchmark panel serializes normalized score",
             benchmark_panel_json.find("\"normalized_score\":1") != std::string::npos);
  check_true("benchmark panel calibration actions",
             !benchmark_panel.calibration_actions.empty() &&
             !BuildDefaultBenchmarkCalibrationActions(false, false).back().supported);
  UiRuntimeState runtime_for_status = CreateDefaultRuntimeState();
  runtime_for_status.active_scene = "assets/scenes/cornell_native.json";
  runtime_for_status.active_renderer_backend = "cpu";
  runtime_for_status.active_renderer_path = "cpu_scalar";
  runtime_for_status.spp_accumulated = 128;
  runtime_for_status.fps = 120.0;
  runtime_for_status.frame_ms = 8.33;
  runtime_for_status.background_job_count = 2;
  runtime_for_status.last_warning_or_error = "none";
  const auto status_bar = BuildStatusBarModel(runtime_for_status, selection, &normalized_score);
  check_true("status bar renderer path", status_bar.renderer_path == "cpu_scalar");
  check_true("status bar spp/fps/jobs", status_bar.spp == 128 && status_bar.fps == 120.0 && status_bar.background_job_count == 2);
  check_true("status bar selected count", status_bar.selected_entity_count == selection.selected_entity_ids.size());

}

}  // namespace vkpt::app