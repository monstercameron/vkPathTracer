#include "scene/Scene.h"

#include "core/Logging.h"

#include <chrono>
#include <string>
#include <utility>

namespace vkpt::scene {

namespace {

uint64_t monotonic_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
}

}  // namespace

std::string_view to_string(FrameStage stage) {
  switch (stage) {
    case FrameStage::FrameBegin:
      return "FrameBegin";
    case FrameStage::Input:
      return "Input";
    case FrameStage::CommandCollection:
      return "CommandCollection";
    case FrameStage::FixedUpdate:
      return "FixedUpdate";
    case FrameStage::VariableUpdate:
      return "VariableUpdate";
    case FrameStage::TransformAssembly:
      return "TransformAssembly";
    case FrameStage::SceneMutationApply:
      return "SceneMutationApply";
    case FrameStage::RenderPreparation:
      return "RenderPreparation";
    case FrameStage::RenderSubmit:
      return "RenderSubmit";
    case FrameStage::PresentOrExport:
      return "PresentOrExport";
    case FrameStage::FrameEnd:
      return "FrameEnd";
    default:
      return "FrameBegin";
  }
}

const FrameContext& FrameLifecycleController::context() const {
  return m_context;
}

const std::vector<FrameStageTiming>& FrameLifecycleController::timings() const {
  return m_timings;
}

void FrameLifecycleController::begin_frame(vkpt::core::FrameIndex frame, double delta_seconds, bool deterministic) {
  if (m_stageOpen) {
    end_stage(m_context.stage);
  }
  m_context.frame = frame;
  m_context.delta_seconds = delta_seconds;
  m_context.deterministic = deterministic;
  begin_stage(FrameStage::FrameBegin);
}

void FrameLifecycleController::begin_stage(FrameStage stage) {
  if (m_stageOpen) {
    end_stage(m_context.stage);
  }
  m_context.stage = stage;
  m_stageStartNs = monotonic_ns();
  m_stageOpen = true;
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug, "frame", "stage begin",
                                    {{"stage", std::string(to_string(stage))}},
                                    m_context.frame);
}

void FrameLifecycleController::end_stage(FrameStage stage) {
  if (!m_stageOpen) {
    return;
  }
  const auto end_ns = monotonic_ns();
  m_timings.push_back(FrameStageTiming{m_context.frame, stage, m_stageStartNs, end_ns});
  m_stageOpen = false;
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Debug, "frame", "stage end",
                                    {{"stage", std::string(to_string(stage))},
                                     {"duration_ns", std::to_string(end_ns >= m_stageStartNs ? end_ns - m_stageStartNs : 0u)}},
                                    m_context.frame);
}

void FrameLifecycleController::end_frame() {
  if (m_stageOpen) {
    end_stage(m_context.stage);
  }
  begin_stage(FrameStage::FrameEnd);
  end_stage(FrameStage::FrameEnd);
}

void FrameLifecycleController::clear_history() {
  m_timings.clear();
}

WorldSystemScheduler::WorldSystemScheduler(std::vector<WorldSystemPhase> phaseOrder) {
  if (!phaseOrder.empty()) {
    m_phaseOrder = std::move(phaseOrder);
    return;
  }
  m_phaseOrder = {WorldSystemPhase::PreFrame,
                  WorldSystemPhase::Input,
                  WorldSystemPhase::ScriptEarly,
                  WorldSystemPhase::AnimationSample,
                  WorldSystemPhase::PhysicsFixed,
                  WorldSystemPhase::TransformAssembly,
                  WorldSystemPhase::SceneCommandApply,
                  WorldSystemPhase::RenderExtract,
                  WorldSystemPhase::PostFrame};
}

bool WorldSystemScheduler::register_system(WorldSystemSpec spec) {
  if (spec.name.empty()) {
    return false;
  }
  m_systems.push_back(std::move(spec));
  return true;
}

std::vector<std::string> WorldSystemScheduler::validate() const {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < m_systems.size(); ++i) {
    for (std::size_t j = i + 1; j < m_systems.size(); ++j) {
      const auto& lhs = m_systems[i];
      const auto& rhs = m_systems[j];
      if (lhs.phase != rhs.phase) {
        continue;
      }
      const auto rw = lhs.readMask & rhs.writeMask;
      const auto ww = lhs.writeMask & rhs.writeMask;
      const auto wr = lhs.writeMask & rhs.readMask;
      if (rw == 0 && ww == 0 && wr == 0) {
        continue;
      }
      for (std::size_t b = 0; b < static_cast<std::size_t>(ComponentKind::Count); ++b) {
        const auto mask = 1u << b;
        const bool w = (lhs.writeMask & mask) != 0 || (rhs.writeMask & mask) != 0;
        const bool r = (lhs.readMask & mask) != 0 || (rhs.readMask & mask) != 0;
        if (w && (lhs.writeMask & mask) && (rhs.writeMask & mask)) {
          out.push_back("write-write conflict between " + lhs.name + " and " + rhs.name + " on " +
                        std::string(to_string(static_cast<ComponentKind>(b))));
        } else if (rw || wr || (w && r)) {
          if ((lhs.writeMask & mask) && (rhs.readMask & mask)) {
            out.push_back("write-read conflict between " + lhs.name + " and " + rhs.name + " on " +
                          std::string(to_string(static_cast<ComponentKind>(b))));
          } else if ((rhs.writeMask & mask) && (lhs.readMask & mask)) {
            out.push_back("read-write conflict between " + lhs.name + " and " + rhs.name + " on " +
                          std::string(to_string(static_cast<ComponentKind>(b))));
          }
        }
      }
    }
  }
  return out;
}

const std::vector<WorldSystemPhase>& WorldSystemScheduler::phase_order() const {
  return m_phaseOrder;
}

const std::vector<WorldSystemSpec>& WorldSystemScheduler::systems() const {
  return m_systems;
}

std::vector<WorldSystemConflict> WorldSystemScheduler::conflicts() const {
  std::vector<WorldSystemConflict> list;
  for (std::size_t i = 0; i < m_systems.size(); ++i) {
    for (std::size_t j = i + 1; j < m_systems.size(); ++j) {
      if (m_systems[i].phase != m_systems[j].phase) {
        continue;
      }
      const auto overlap = (m_systems[i].writeMask & m_systems[j].writeMask) |
                           (m_systems[i].writeMask & m_systems[j].readMask) |
                           (m_systems[i].readMask & m_systems[j].writeMask);
      if (overlap == 0) {
        continue;
      }
      for (std::size_t b = 0; b < static_cast<std::size_t>(ComponentKind::Count); ++b) {
        if (!(overlap & (1u << b))) {
          continue;
        }
        list.push_back({m_systems[i].name, m_systems[j].name, static_cast<ComponentKind>(b), m_systems[i].phase});
      }
    }
  }
  return list;
}

}  // namespace vkpt::scene
