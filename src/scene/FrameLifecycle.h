#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/log/Log.h"
#include "core/metrics/Metrics.h"
#include "core/Types.h"

namespace vkpt::scene {

enum class FrameStage : std::uint8_t {
  FrameBegin,
  Input,
  CommandCollection,
  FixedUpdate,
  VariableUpdate,
  TransformAssembly,
  SceneMutationApply,
  RenderPreparation,
  RenderSubmit,
  PresentOrExport,
  FrameEnd,
  Count
};

struct FrameContext {
  vkpt::core::FrameIndex frame = 0;
  double delta_seconds = 0.0;
  FrameStage stage = FrameStage::FrameBegin;
  bool deterministic = false;
};

struct FrameStageTiming {
  vkpt::core::FrameIndex frame = 0;
  FrameStage stage = FrameStage::FrameBegin;
  uint64_t start_ns = 0u;
  uint64_t end_ns = 0u;
  uint64_t duration_ns() const { return end_ns >= start_ns ? end_ns - start_ns : 0u; }
  uint64_t duration_us() const {
    const auto ns = duration_ns();
    return ns == 0u ? 0u : (ns + 999u) / 1000u;
  }
};

inline constexpr std::size_t kFrameStageCount =
    static_cast<std::size_t>(FrameStage::Count);

struct FrameStageTelemetryConfig {
  std::array<std::uint64_t, kFrameStageCount> overrun_threshold_us{};
};

inline std::size_t FrameStageIndex(FrameStage stage) noexcept {
  const auto index = static_cast<std::size_t>(stage);
  return index < kFrameStageCount ? index : 0u;
}

inline const char* FrameStageLabel(FrameStage stage) noexcept {
  switch (stage) {
    case FrameStage::FrameBegin:
      return "frame_begin";
    case FrameStage::Input:
      return "input";
    case FrameStage::CommandCollection:
      return "command_collection";
    case FrameStage::FixedUpdate:
      return "fixed_update";
    case FrameStage::VariableUpdate:
      return "variable_update";
    case FrameStage::TransformAssembly:
      return "transform_assembly";
    case FrameStage::SceneMutationApply:
      return "scene_mutation_apply";
    case FrameStage::RenderPreparation:
      return "render_preparation";
    case FrameStage::RenderSubmit:
      return "render_submit";
    case FrameStage::PresentOrExport:
      return "present_or_export";
    case FrameStage::FrameEnd:
      return "frame_end";
    default:
      return "frame_begin";
  }
}

inline const char* FrameStageMetricName(FrameStage stage) noexcept {
  switch (stage) {
    case FrameStage::FrameBegin:
      return "vkp.scene.stage_frame_begin_us";
    case FrameStage::Input:
      return "vkp.scene.stage_input_us";
    case FrameStage::CommandCollection:
      return "vkp.scene.stage_command_collection_us";
    case FrameStage::FixedUpdate:
      return "vkp.scene.stage_fixed_update_us";
    case FrameStage::VariableUpdate:
      return "vkp.scene.stage_variable_update_us";
    case FrameStage::TransformAssembly:
      return "vkp.scene.stage_transform_assembly_us";
    case FrameStage::SceneMutationApply:
      return "vkp.scene.stage_scene_mutation_apply_us";
    case FrameStage::RenderPreparation:
      return "vkp.scene.stage_render_preparation_us";
    case FrameStage::RenderSubmit:
      return "vkp.scene.stage_render_submit_us";
    case FrameStage::PresentOrExport:
      return "vkp.scene.stage_present_or_export_us";
    case FrameStage::FrameEnd:
      return "vkp.scene.stage_frame_end_us";
    default:
      return "vkp.scene.stage_frame_begin_us";
  }
}

class FrameStageTelemetryState final {
 public:
  void configure(FrameStageTelemetryConfig config) {
    std::scoped_lock lock(m_mutex);
    m_config = config;
  }

  FrameStageTelemetryConfig config() const {
    std::scoped_lock lock(m_mutex);
    return m_config;
  }

  void set_threshold(FrameStage stage, std::uint64_t threshold_us) {
    std::scoped_lock lock(m_mutex);
    m_config.overrun_threshold_us[FrameStageIndex(stage)] = threshold_us;
  }

  std::uint64_t threshold(FrameStage stage) const {
    std::scoped_lock lock(m_mutex);
    return m_config.overrun_threshold_us[FrameStageIndex(stage)];
  }

  void record_latest(FrameStageTiming timing) {
    std::scoped_lock lock(m_mutex);
    if (!m_hasLatestFrame || timing.frame != m_latestFrame) {
      m_latestFrame = timing.frame;
      m_hasLatestFrame = true;
      m_latestTimings.clear();
    }
    m_latestTimings.push_back(timing);
  }

  std::vector<FrameStageTiming> latest_timings() const {
    std::scoped_lock lock(m_mutex);
    return m_latestTimings;
  }

  void clear_latest() {
    std::scoped_lock lock(m_mutex);
    m_latestTimings.clear();
    m_latestFrame = 0u;
    m_hasLatestFrame = false;
  }

 private:
  mutable std::mutex m_mutex;
  FrameStageTelemetryConfig m_config{};
  std::vector<FrameStageTiming> m_latestTimings;
  vkpt::core::FrameIndex m_latestFrame = 0u;
  bool m_hasLatestFrame = false;
};

inline FrameStageTelemetryState& FrameStageTelemetry() {
  static FrameStageTelemetryState state;
  return state;
}

inline void ConfigureFrameStageTelemetry(FrameStageTelemetryConfig config) {
  FrameStageTelemetry().configure(config);
}

inline FrameStageTelemetryConfig GetFrameStageTelemetryConfig() {
  return FrameStageTelemetry().config();
}

inline void SetFrameStageOverrunThresholdUs(FrameStage stage,
                                            std::uint64_t threshold_us) {
  FrameStageTelemetry().set_threshold(stage, threshold_us);
}

inline std::vector<FrameStageTiming> LatestFrameStageTimings() {
  return FrameStageTelemetry().latest_timings();
}

inline void ClearLatestFrameStageTimings() {
  FrameStageTelemetry().clear_latest();
}

inline void RecordFrameStageTimingTelemetry(const FrameStageTiming& timing) {
  const auto duration_us = timing.duration_us();
  vkpt::core::metrics::MetricsRegistry::instance()
      .histogram(FrameStageMetricName(timing.stage))
      .record(duration_us);

  FrameStageTelemetry().record_latest(timing);
  const auto threshold_us = FrameStageTelemetry().threshold(timing.stage);
  if (threshold_us > 0u && duration_us > threshold_us) {
    VKP_LOG(Warn,
            "scene",
            "stage_overrun",
            "frame",
            timing.frame,
            "stage",
            FrameStageLabel(timing.stage),
            "duration_us",
            duration_us,
            "threshold_us",
            threshold_us);
  }
}

inline std::string FormatLatestFrameStageTimingsForRepl() {
  const auto timings = LatestFrameStageTimings();
  if (timings.empty()) {
    return "scene stages: no timings recorded\n";
  }

  std::ostringstream out;
  out << "frame stage duration_us start_ns end_ns\n";
  for (const auto& timing : timings) {
    out << timing.frame << ' '
        << FrameStageLabel(timing.stage) << ' '
        << timing.duration_us() << ' '
        << timing.start_ns << ' '
        << timing.end_ns << '\n';
  }
  return out.str();
}

class FrameLifecycleController {
 public:
  const FrameContext& context() const;
  const std::vector<FrameStageTiming>& timings() const;

  void begin_frame(vkpt::core::FrameIndex frame, double delta_seconds, bool deterministic = false);
  void begin_stage(FrameStage stage);
  void end_stage(FrameStage stage);
  void end_frame();
  void clear_history();

 private:
  FrameContext m_context{};
  std::vector<FrameStageTiming> m_timings;
  uint64_t m_stageStartNs = 0u;
  bool m_stageOpen = false;
};

std::string_view to_string(FrameStage stage);

}  // namespace vkpt::scene
