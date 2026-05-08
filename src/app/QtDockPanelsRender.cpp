#ifdef PT_ENABLE_QT

#include "app/QtDockPanelsInternal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/health/Health.h"
#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "render/backends/BackendFactory.h"

namespace vkpt::app {

namespace {

std::vector<std::string> QtRenderSettingsBackendOptions(const QtDockDeviceStats& device_stats,
                                                        std::string_view selected_backend) {
  std::vector<std::string> options;
  const auto add = [&](std::string_view option) {
    if (option.empty()) {
      return;
    }
    std::string normalized = vkpt::render::NormalizeBackendName(option);
    if (normalized == "cpu-tiled" || normalized == "cputiled") {
      normalized = "cpu";
    }
    if (std::find(options.begin(), options.end(), normalized) == options.end()) {
      options.push_back(std::move(normalized));
    }
  };
  add("auto");
  add("cpu");
  for (const auto& option : device_stats.runtime_backend_options) {
    add(option);
  }
  for (const auto& option : vkpt::render::AvailableBackendNames()) {
    add(option);
  }
  add(selected_backend);
  return options;
}

struct QtMetricHistoryPoint {
  double value = 0.0;
  std::chrono::steady_clock::time_point time{};
  std::vector<double> samples;
};

double QtMetricNumericValue(const vkpt::core::metrics::MetricSnapshot& metric) {
  switch (metric.kind) {
    case vkpt::core::metrics::Kind::CounterKind:
      return static_cast<double>(metric.counter_value);
    case vkpt::core::metrics::Kind::GaugeKind:
      return metric.gauge_value;
    case vkpt::core::metrics::Kind::HistogramKind:
      return static_cast<double>(metric.hist.p95);
  }
  return 0.0;
}

std::string QtMetricSparkline(const std::vector<double>& samples) {
  if (samples.empty()) {
    return "........";
  }
  const auto [minIt, maxIt] = std::minmax_element(samples.begin(), samples.end());
  const double minValue = *minIt;
  const double maxValue = *maxIt;
  constexpr std::string_view ramp = ".:-=+*#%@";
  std::string out;
  out.reserve(samples.size());
  for (double sample : samples) {
    std::size_t index = 0u;
    if (maxValue > minValue) {
      const double normalized = std::clamp((sample - minValue) / (maxValue - minValue), 0.0, 1.0);
      index = static_cast<std::size_t>(
          std::round(normalized * static_cast<double>(ramp.size() - 1u)));
    }
    out.push_back(ramp[index]);
  }
  return out;
}

}  // namespace

QtDockPanelContent BuildQtRenderSettingsDock(const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
                                             const vkpt::pathtracer::RenderSettings& settings,
                                             const vkpt::editor::UiRuntimeState& runtime,
                                             const vkpt::editor::UiLayoutDocument& layout,
                                             const QtDockFrameStats& frame_stats,
                                             const QtDockDeviceStats& device_stats) {
  (void)scene;
  auto panel = MakeQtDockPanel(layout, "render_settings", "Render Settings", true, 420.0f, 460.0f);
  const std::string sceneName = runtime.active_scene.empty() ? "builtin:preview" : runtime.active_scene;
  const std::string renderState = frame_stats.render_mode.empty()
      ? (frame_stats.tracer_ready ? "path tracing on" : "renderer not ready")
      : frame_stats.render_mode;
  QtDockAddProperty(panel, "Scene", sceneName);
  const std::string selectedBackend = device_stats.selected_backend.empty()
      ? (runtime.active_renderer_backend.empty() ? std::string("unknown") : runtime.active_renderer_backend)
      : device_stats.selected_backend;
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.backend",
                                   "Runtime",
                                   "Backend",
                                   selectedBackend,
                                   QtRenderSettingsBackendOptions(device_stats, selectedBackend));
  QtDockAddProperty(panel, "Render resolution",
                    std::to_string(settings.width) + "x" + std::to_string(settings.height));
  QtDockAddSliderGroupedProperty(panel,
                                 "render.resolution.width",
                                 "Resolution",
                                 "Render width",
                                 settings.width,
                                 16.0,
                                 8192.0,
                                 1.0,
                                 320.0);
  QtDockAddSliderGroupedProperty(panel,
                                 "render.resolution.height",
                                 "Resolution",
                                 "Render height",
                                 settings.height,
                                 16.0,
                                 8192.0,
                                 1.0,
                                 240.0);
  QtDockAddProperty(panel, "Published framebuffer",
                    std::to_string(frame_stats.frame_width) + "x" +
                    std::to_string(frame_stats.frame_height));
  if (frame_stats.canvas_width > 0u && frame_stats.canvas_height > 0u) {
    QtDockAddProperty(panel, "Viewport canvas",
                      std::to_string(frame_stats.canvas_width) + "x" +
                      std::to_string(frame_stats.canvas_height));
  }
  if (frame_stats.displayed_image_width > 0u && frame_stats.displayed_image_height > 0u) {
    QtDockAddProperty(panel, "Displayed image",
                      std::to_string(frame_stats.displayed_image_width) + "x" +
                      std::to_string(frame_stats.displayed_image_height));
  }
  QtDockAddProperty(panel, "Accumulation",
                    std::to_string(frame_stats.sample_count) + " spp, " + renderState);
  QtDockAddToggleGroupedProperty(panel,
                                 "render.denoiser",
                                 "",
                                 "GPU denoiser",
                                 settings.enable_denoiser);
  QtDockAddToggleGroupedProperty(panel,
                                 "render.temporal_aa",
                                 "",
                                 "Temporal AA",
                                 settings.enable_temporal_aa);
  QtDockAddSliderProperty(panel,
                          "render.max_depth",
                          "Max depth",
                          settings.max_depth,
                          1.0,
                          64.0,
                          1.0,
                          6.0);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.nee",
                                   "",
                                   "Direct lighting",
                                   QtDockBool(settings.enable_nee),
                                   QtBoolOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.mis",
                                   "",
                                   "MIS",
                                   QtDockBool(settings.enable_mis),
                                   QtBoolOptions());
  QtDockAddSliderProperty(panel,
                          "render.film.exposure",
                          "Exposure",
                          settings.film_resolve.exposure,
                          0.0,
                          8.0,
                          0.01,
                          1.0);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.tone_map",
                                   "",
                                   "Tone mapper",
                                   QtToneMapName(settings.film_resolve.tone_map),
                                   QtToneMapOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.output_transform",
                                   "",
                                   "Display transform",
                                   QtOutputTransformName(settings.film_resolve.output_transform),
                                   QtOutputTransformOptions());
  QtDockAddSliderProperty(panel,
                          "render.film.gamma",
                          "Gamma",
                          settings.film_resolve.gamma,
                          0.1,
                          4.0,
                          0.01,
                          2.2);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.clamp_output",
                                   "",
                                   "Clamp output",
                                   QtDockBool(settings.film_resolve.clamp_output),
                                   QtBoolOptions());
  return panel;
}

QtDockPanelContent BuildQtBenchmarkDock(const vkpt::editor::BenchmarkPanelModel& benchmark,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "benchmark_panel", "Benchmark", false, 560.0f, 480.0f);
  QtDockAddProperty(panel, "scene", benchmark.run_desc.scene_path);
  QtDockAddProperty(panel, "backend", benchmark.run_desc.backend);
  QtDockAddProperty(panel, "renderer", benchmark.run_desc.renderer_path);
  QtDockAddProperty(panel, "resolution", std::to_string(benchmark.run_desc.resolution.width) +
      "x" + std::to_string(benchmark.run_desc.resolution.height));
  QtDockAddProperty(panel, "spp", std::to_string(benchmark.run_desc.samples_per_pixel));
  QtDockAddProperty(panel, "max depth", std::to_string(benchmark.run_desc.max_depth));
  QtDockAddProperty(panel, "can run", QtDockBool(benchmark.can_run));
  QtDockAddProperty(panel, "summary", benchmark.result_summary);
  QtDockAddProperty(panel, "score", QtDockNumber(benchmark.score.normalized_score, 3));
  QtDockAddProperty(panel, "confidence", benchmark.score.confidence);
  for (const auto& action : benchmark.calibration_actions) {
    QtDockAddRow(panel, action.label + " [" + (action.supported ? "available" : action.unavailable_reason) + "]");
  }
  return panel;
}

QtDockPanelContent BuildQtDiagnosticsDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::SelectionState& selection,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "diagnostics", "Diagnostics", true, 720.0f, 260.0f);
  QtDockAddProperty(panel, "status", runtime.status_message);
  QtDockAddProperty(panel, "last warning/error", runtime.last_warning_or_error);
  QtDockAddProperty(panel, "last menu action", runtime.last_menu_action);
  QtDockAddProperty(panel, "last clicked entity", std::to_string(runtime.last_clicked_entity));
  QtDockAddProperty(panel, "focused panel", runtime.focused_panel);
  QtDockAddProperty(panel, "active modal", runtime.active_modal.empty() ? "none" : runtime.active_modal);
  QtDockAddProperty(panel, "tracer ready", QtDockBool(frame_stats.tracer_ready));
  QtDockAddProperty(panel, "preview status", frame_stats.preview_status);
  QtDockAddProperty(panel, "selection source", vkpt::editor::ToString(selection.selection_source));
  return panel;
}

QtDockPanelContent BuildQtPerformanceDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "performance", "Performance", true, 360.0f, 320.0f);
  QtDockAddProperty(panel, "canvas fps", QtDockNumber(runtime.fps, 1));
  QtDockAddProperty(panel, "canvas frame ms", QtDockNumber(runtime.frame_ms, 2));
  QtDockAddProperty(panel, "ui loop frame ms", QtDockNumber(frame_stats.ui_frame_ms, 2));
  QtDockAddProperty(panel, "samples", std::to_string(frame_stats.sample_count));
  QtDockAddProperty(panel, "published", std::to_string(frame_stats.render_published));
  QtDockAddProperty(panel, "render dropped", std::to_string(frame_stats.render_dropped));
  QtDockAddProperty(panel, "window received", std::to_string(frame_stats.window_received));
  QtDockAddProperty(panel, "window presented", std::to_string(frame_stats.window_presented));
  QtDockAddProperty(panel, "window painted", std::to_string(frame_stats.window_painted));
  QtDockAddProperty(panel, "window dropped", std::to_string(frame_stats.window_dropped));
  QtDockAddProperty(panel, "publish cap", frame_stats.publish_cap.empty()
      ? std::to_string(frame_stats.preview_publish_hz) + " fps"
      : frame_stats.publish_cap);
  QtDockAddProperty(panel, "gpu batches/tick", frame_stats.background_thread
      ? std::string("background")
      : std::to_string(frame_stats.gpu_batches_per_tick));
  QtDockAddProperty(panel, "gpu batch ms", QtDockNumber(frame_stats.gpu_batch_ms, 3));
  QtDockAddProperty(panel, "background jobs", std::to_string(runtime.background_job_count));
  return panel;
}

QtDockPanelContent BuildQtMetricsDock(const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "metrics", "Metrics", true, 560.0f, 360.0f);
  const auto metrics = vkpt::core::metrics::MetricsRegistry::instance().snapshot_all();
  static std::unordered_map<std::string, QtMetricHistoryPoint> history;
  const auto now = std::chrono::steady_clock::now();
  bool added = false;
  for (const auto& metric : metrics) {
    if (!std::string_view(metric.name).starts_with("vkp.")) {
      continue;
    }
    const double numericValue = QtMetricNumericValue(metric);
    auto& metricHistory = history[metric.name];
    double ratePerSec = 0.0;
    if (metric.kind == vkpt::core::metrics::Kind::CounterKind &&
        metricHistory.time != std::chrono::steady_clock::time_point{}) {
      const double dt = std::chrono::duration<double>(now - metricHistory.time).count();
      if (dt > 0.0) {
        ratePerSec = std::max(0.0, (numericValue - metricHistory.value) / dt);
      }
    }
    metricHistory.value = numericValue;
    metricHistory.time = now;
    metricHistory.samples.push_back(numericValue);
    if (metricHistory.samples.size() > 24u) {
      metricHistory.samples.erase(metricHistory.samples.begin(),
                                  metricHistory.samples.begin() +
                                      static_cast<std::ptrdiff_t>(metricHistory.samples.size() - 24u));
    }

    std::string value;
    switch (metric.kind) {
      case vkpt::core::metrics::Kind::CounterKind:
        value = "value " + std::to_string(metric.counter_value) +
                " | rate " + QtDockNumber(ratePerSec, 2) + "/s";
        break;
      case vkpt::core::metrics::Kind::GaugeKind:
        value = "value " + QtDockNumber(metric.gauge_value, 3) + " | rate --";
        break;
      case vkpt::core::metrics::Kind::HistogramKind:
        value = "count " + std::to_string(metric.hist.count) +
                " | rate --" +
                " p95 " + std::to_string(metric.hist.p95) +
                " p99 " + std::to_string(metric.hist.p99);
        break;
    }
    value += " | spark " + QtMetricSparkline(metricHistory.samples);
    QtDockAddProperty(panel, metric.name, value);
    added = true;
  }
  if (!added) {
    QtDockAddProperty(panel, "registry", "empty");
  }
  return panel;
}

QtDockPanelContent BuildQtEventsDock(const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "events", "Events", true, 520.0f, 280.0f);
  auto& logger = vkpt::core::log::Logger::instance();
  const auto emitted = logger.total_emitted();
  const auto dropped = logger.total_drop_count();
  static std::chrono::steady_clock::time_point lastSample{};
  static std::uint64_t lastEmitted = 0u;
  static double emittedRate = 0.0;
  const auto now = std::chrono::steady_clock::now();
  if (lastSample != std::chrono::steady_clock::time_point{}) {
    const double dt = std::chrono::duration<double>(now - lastSample).count();
    if (dt > 0.0) {
      emittedRate = std::max(0.0, static_cast<double>(emitted - lastEmitted) / dt);
    }
  }
  lastSample = now;
  lastEmitted = emitted;
  QtDockAddProperty(panel, "emitted", std::to_string(emitted));
  QtDockAddProperty(panel, "dropped", std::to_string(dropped));
  QtDockAddProperty(panel, "rate", QtDockNumber(emittedRate, 2) + "/s");
  QtDockAddProperty(panel, "min_level", vkpt::core::log::LevelName(logger.min_level()));
  return panel;
}

QtDockPanelContent BuildQtHealthDock(const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "health", "Health", true, 420.0f, 280.0f);
  const auto reports = vkpt::core::health::HealthRegistry::instance().scrape();
  if (reports.empty()) {
    QtDockAddProperty(panel, "registry", "no probes registered");
    return panel;
  }
  for (const auto& [name, report] : reports) {
    std::string value = vkpt::core::health::StatusName(report.status);
    if (!report.reason.empty()) {
      value += " | " + report.reason;
    }
    QtDockAddProperty(panel, name, value);
  }
  return panel;
}

QtDockPanelContent BuildQtDeviceDock(const vkpt::pathtracer::PathTracerSceneSnapshot& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     const QtDockDeviceStats& device_stats) {
  auto panel = MakeQtDockPanel(layout, "device", "Device", true, 460.0f, 360.0f);
  const auto& caps = device_stats.backend_caps;
  const auto selectedBackend = device_stats.selected_backend.empty()
      ? runtime.active_renderer_backend
      : device_stats.selected_backend;
  const auto rendererPath = device_stats.active_renderer_path.empty()
      ? runtime.active_renderer_path
      : device_stats.active_renderer_path;

  const vkpt::render::AcceleratorCapabilities* selectedAccel =
      device_stats.has_selected_accelerator ? &device_stats.selected_accelerator : nullptr;
  const auto& selectedBudget = selectedAccel != nullptr
      ? selectedAccel->backend_caps.memory_budget
      : caps.memory_budget;
  const std::uint64_t usage = selectedAccel != nullptr && selectedAccel->current_usage_bytes > 0u
      ? selectedAccel->current_usage_bytes
      : selectedBudget.current_usage_bytes;
  const std::uint64_t budget = selectedAccel != nullptr && selectedAccel->current_budget_bytes > 0u
      ? selectedAccel->current_budget_bytes
      : selectedBudget.current_budget_bytes;
  const std::uint64_t dedicatedMemory = selectedAccel != nullptr && selectedAccel->dedicated_video_memory_bytes > 0u
      ? selectedAccel->dedicated_video_memory_bytes
      : selectedBudget.dedicated_video_memory_bytes;
  const std::uint64_t sharedMemory = selectedAccel != nullptr && selectedAccel->shared_system_memory_bytes > 0u
      ? selectedAccel->shared_system_memory_bytes
      : selectedBudget.shared_system_memory_bytes;
  const std::string deviceName = selectedAccel != nullptr
      ? selectedAccel->name
      : (caps.platform.platform_name.empty() ? std::string("unknown") : caps.platform.platform_name);
  const std::string deviceKind = selectedAccel != nullptr
      ? QtDockAcceleratorKind(*selectedAccel)
      : (caps.is_simulated ? std::string("Simulated") : std::string("Unknown"));

  const std::string backendValue = !rendererPath.empty() && rendererPath != selectedBackend
      ? selectedBackend + " / " + rendererPath
      : selectedBackend;
  if (!device_stats.runtime_backend_options.empty()) {
    QtDockAddDropdownGroupedProperty(panel,
                                     "render.backend",
                                     "Runtime",
                                     "Runtime backend",
                                     selectedBackend,
                                     device_stats.runtime_backend_options);
  }
  QtDockAddProperty(panel, "Backend", backendValue);
  QtDockAddProperty(panel, "Render mode",
                    frame_stats.background_thread ? "background render thread" : "event loop renderer");

  std::ostringstream throughput;
  const double computerAverage = frame_stats.accumulated_rays_per_second > 0.0
      ? frame_stats.accumulated_rays_per_second
      : frame_stats.rolling_rays_per_second;
  throughput << "computer avg " << QtDockRate(computerAverage);
  if (frame_stats.rolling_rays_per_second > 0.0) {
    throughput << " | rolling " << QtDockRate(frame_stats.rolling_rays_per_second);
  }
  if (frame_stats.sample_count > 0u) {
    throughput << " @ " << frame_stats.sample_count << " spp";
  }
  QtDockAddProperty(panel, "Ray throughput", throughput.str());

  const auto activeMetricKey = selectedAccel != nullptr
      ? QtDockRayDeviceKeyForAccelerator(*selectedAccel)
      : QtDockActiveRayDeviceKey(device_stats);
  const auto* activeMetric = QtDockFindRayMetric(device_stats.ray_metrics, activeMetricKey);
  if (selectedAccel != nullptr) {
    QtDockAddProperty(panel, "Active ray device",
                      QtDockAcceleratorSummary(*selectedAccel, true, activeMetric));
  } else {
    std::vector<std::string> tags;
    tags.push_back(deviceKind);
    QtDockAppendRayMetricTags(tags, activeMetric);
    const auto tagText = QtDockJoin(tags);
    QtDockAddProperty(panel,
                      "Active ray device",
                      tagText.empty() ? deviceName : deviceName + "\n" + tagText);
  }

  std::vector<const vkpt::render::AcceleratorCapabilities*> gpuCandidates;
  std::vector<const vkpt::render::AcceleratorCapabilities*> cpuFallbacks;
  std::vector<const vkpt::render::AcceleratorCapabilities*> softwareFallbacks;
  for (const auto& accelerator : device_stats.accelerators) {
    if (!QtDockRayDeviceEligible(accelerator)) {
      continue;
    }
    if (selectedAccel != nullptr && QtDockSameAccelerator(accelerator, *selectedAccel)) {
      continue;
    }
    if (accelerator.cpu) {
      cpuFallbacks.push_back(&accelerator);
    } else if (accelerator.warp) {
      softwareFallbacks.push_back(&accelerator);
    } else {
      gpuCandidates.push_back(&accelerator);
    }
  }

  if (!gpuCandidates.empty()) {
    QtDockAddProperty(panel, "Other GPUs",
                      QtDockAcceleratorGroupSummary(gpuCandidates, device_stats.ray_metrics));
  }
  if (!cpuFallbacks.empty()) {
    QtDockAddProperty(panel, "CPU fallback",
                      QtDockAcceleratorGroupSummary(cpuFallbacks, device_stats.ray_metrics, 2u));
  }
  if (!softwareFallbacks.empty()) {
    QtDockAddProperty(panel, "Software fallback",
                      QtDockAcceleratorGroupSummary(softwareFallbacks, device_stats.ray_metrics, 2u));
  }

  QtDockAddProperty(panel, "Device memory",
                    QtDockMemoryUsage(usage, budget, selectedBudget.budget_unavailable_reason));
  if (dedicatedMemory > 0u || sharedMemory > 0u) {
    std::vector<std::string> memoryParts;
    if (dedicatedMemory > 0u) {
      memoryParts.push_back("dedicated " + QtDockBytes(dedicatedMemory));
    }
    if (sharedMemory > 0u &&
        (selectedAccel == nullptr || selectedAccel->unified_memory || dedicatedMemory == 0u)) {
      memoryParts.push_back("shared " + QtDockBytes(sharedMemory));
    }
    const auto memoryText = QtDockJoin(memoryParts);
    if (!memoryText.empty()) {
      QtDockAddProperty(panel, "Memory pool", memoryText);
    }
  }
  QtDockAddProperty(panel, "Scene buffers", QtDockBytes(EstimateQtSceneMemoryBytes(scene)));
  const auto featureSummary = QtDockFeatureSummary(caps);
  if (!featureSummary.empty() && featureSummary != "basic") {
    QtDockAddProperty(panel, "Capabilities", featureSummary);
  }
  return panel;
}

QtDockPanelContent BuildQtDebugViewsDock(const vkpt::editor::UiRuntimeState& runtime,
                                         const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "debug_views", "Debug Views", false, 320.0f, 300.0f);
  QtDockAddProperty(panel, "selected view", runtime.selected_debug_view.empty() ? "beauty" : runtime.selected_debug_view);
  QtDockAddProperty(panel, "active channel", runtime.active_debug_channel.empty() ? "rgb" : runtime.active_debug_channel);
  QtDockAddRow(panel, "beauty");
  QtDockAddRow(panel, "albedo");
  QtDockAddRow(panel, "normal");
  QtDockAddRow(panel, "depth");
  QtDockAddRow(panel, "sample_count");
  QtDockAddRow(panel, "selection_id");
  return panel;
}

}  // namespace vkpt::app

#endif  // PT_ENABLE_QT
