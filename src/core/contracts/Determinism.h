#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/Logging.h"

namespace vkpt::core {

struct DeterminismContext {
  bool enabled = false;
  std::uint64_t base_seed = 0u;
  std::uint64_t frame_index = 0u;
  std::string scenario_id;
};

inline bool operator==(const DeterminismContext& lhs,
                       const DeterminismContext& rhs) noexcept {
  return lhs.enabled == rhs.enabled &&
         lhs.base_seed == rhs.base_seed &&
         lhs.frame_index == rhs.frame_index &&
         lhs.scenario_id == rhs.scenario_id;
}

inline bool operator!=(const DeterminismContext& lhs,
                       const DeterminismContext& rhs) noexcept {
  return !(lhs == rhs);
}

inline DeterminismContext MakeDeterminismContext(bool enabled,
                                                 std::uint64_t base_seed,
                                                 std::uint64_t frame_index = 0u,
                                                 std::string_view scenario_id = {}) {
  DeterminismContext out;
  out.enabled = enabled;
  out.base_seed = base_seed;
  out.frame_index = frame_index;
  out.scenario_id = std::string(scenario_id);
  return out;
}

inline void EmitDeterminismChanged(std::string_view component,
                                   const DeterminismContext& context) {
  try {
    const std::string event = std::string(component) + ".determinism_changed";
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info,
        component,
        event,
        {{"enabled", context.enabled ? "true" : "false"},
         {"base_seed", std::to_string(context.base_seed)},
         {"frame_index", std::to_string(context.frame_index)},
         {"scenario_id", context.scenario_id}},
        context.frame_index);
  } catch (...) {
  }
}

inline void EmitDeterminismChangedIfNeeded(std::string_view component,
                                           const DeterminismContext& previous,
                                           const DeterminismContext& current) {
  if (previous != current) {
    EmitDeterminismChanged(component, current);
  }
}

}  // namespace vkpt::core
