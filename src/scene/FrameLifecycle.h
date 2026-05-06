#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

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
};

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
