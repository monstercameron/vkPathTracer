#pragma once

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include "core/log/Log.h"

namespace vkpt::core::contracts {

enum class ComponentLifecycle : std::uint8_t {
  Uninitialized = 0,
  Initializing,
  Ready,
  Busy,
  Degraded,
  Failed,
  ShuttingDown,
};

inline const char* ComponentLifecycleName(ComponentLifecycle state) noexcept {
  switch (state) {
    case ComponentLifecycle::Uninitialized:
      return "uninitialized";
    case ComponentLifecycle::Initializing:
      return "initializing";
    case ComponentLifecycle::Ready:
      return "ready";
    case ComponentLifecycle::Busy:
      return "busy";
    case ComponentLifecycle::Degraded:
      return "degraded";
    case ComponentLifecycle::Failed:
      return "failed";
    case ComponentLifecycle::ShuttingDown:
      return "shutting_down";
  }
  return "unknown";
}

inline bool state_allowed(ComponentLifecycle current,
                          std::initializer_list<ComponentLifecycle> allowed) noexcept {
  for (const ComponentLifecycle candidate : allowed) {
    if (candidate == current) {
      return true;
    }
  }
  return false;
}

inline bool assert_state(std::string_view method,
                         ComponentLifecycle current,
                         std::initializer_list<ComponentLifecycle> allowed) {
  const bool ok = state_allowed(current, allowed);
  (void)method;
#ifndef NDEBUG
  assert(ok && "component lifecycle state contract violated");
#endif
  return ok;
}

}  // namespace vkpt::core::contracts

// Standard lifecycle events for agent-readable subsystem logs. The component
// remains in the log component field and the event is one of started/stopped/config.
#define VKP_LIFECYCLE_STARTED(comp_lit, ...) \
  VKP_LOG(Info, (comp_lit), "started" __VA_OPT__(,) __VA_ARGS__)

#define VKP_LIFECYCLE_STOPPED(comp_lit, ...) \
  VKP_LOG(Info, (comp_lit), "stopped" __VA_OPT__(,) __VA_ARGS__)

#define VKP_LIFECYCLE_CONFIG(comp_lit, ...) \
  VKP_LOG(Info, (comp_lit), "config" __VA_OPT__(,) __VA_ARGS__)
