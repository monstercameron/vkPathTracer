#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "core/Logging.h"

namespace vkpt::diagnostics {

enum class LifecycleEvent : uint8_t {
  Construct,
  Configure,
  InitializeBegin,
  InitializeSuccess,
  InitializeFailure,
  ShutdownBegin,
  ShutdownSuccess,
  ShutdownFailure,
};

inline const char* LifecycleEventName(LifecycleEvent event) {
  switch (event) {
    case LifecycleEvent::Construct:
      return "construct";
    case LifecycleEvent::Configure:
      return "configure";
    case LifecycleEvent::InitializeBegin:
      return "initialize_begin";
    case LifecycleEvent::InitializeSuccess:
      return "initialize_success";
    case LifecycleEvent::InitializeFailure:
      return "initialize_failure";
    case LifecycleEvent::ShutdownBegin:
      return "shutdown_begin";
    case LifecycleEvent::ShutdownSuccess:
      return "shutdown_success";
    case LifecycleEvent::ShutdownFailure:
      return "shutdown_failure";
  }
  return "unknown";
}

inline vkpt::log::Severity LifecycleSeverity(LifecycleEvent event) {
  switch (event) {
    case LifecycleEvent::InitializeFailure:
    case LifecycleEvent::ShutdownFailure:
      return vkpt::log::Severity::Error;
    default:
      return vkpt::log::Severity::Info;
  }
}

inline void LogLifecycleEvent(std::string_view subsystem,
                              LifecycleEvent event,
                              std::string_view config_summary = {},
                              std::string_view failure_reason = {},
                              uint64_t duration_ms = 0,
                              uint64_t frame_index = 0) {
  std::initializer_list<vkpt::log::LogField> fields = {
      {"event", LifecycleEventName(event)},
      {"config", std::string(config_summary)},
      {"reason", std::string(failure_reason)},
      {"duration_ms", std::to_string(duration_ms)},
  };

  vkpt::log::Logger::instance().log(
      LifecycleSeverity(event),
      subsystem,
      LifecycleEventName(event),
      fields,
      frame_index);
}

class LifecycleOperation {
 public:
  LifecycleOperation(std::string subsystem,
                     LifecycleEvent begin_event,
                     LifecycleEvent success_event,
                     LifecycleEvent failure_event,
                     std::string config_summary = {},
                     uint64_t frame_index = 0)
      : m_subsystem(std::move(subsystem)),
        m_successEvent(success_event),
        m_failureEvent(failure_event),
        m_configSummary(std::move(config_summary)),
        m_frameIndex(frame_index),
        m_start(std::chrono::steady_clock::now()) {
    LogLifecycleEvent(m_subsystem, begin_event, m_configSummary, {}, 0, m_frameIndex);
  }

  LifecycleOperation(const LifecycleOperation&) = delete;
  LifecycleOperation& operator=(const LifecycleOperation&) = delete;

  LifecycleOperation(LifecycleOperation&& other) noexcept
      : m_subsystem(std::move(other.m_subsystem)),
        m_successEvent(other.m_successEvent),
        m_failureEvent(other.m_failureEvent),
        m_configSummary(std::move(other.m_configSummary)),
        m_frameIndex(other.m_frameIndex),
        m_start(other.m_start),
        m_finished(other.m_finished) {
    other.m_finished = true;
  }

  ~LifecycleOperation() noexcept {
    if (!m_finished) {
      try {
        fail("scope exited before explicit success");
      } catch (...) {
      }
    }
  }

  void success() {
    if (m_finished) {
      return;
    }
    m_finished = true;
    LogLifecycleEvent(m_subsystem, m_successEvent, m_configSummary, {}, elapsed_ms(), m_frameIndex);
  }

  void fail(std::string_view reason) {
    if (m_finished) {
      return;
    }
    m_finished = true;
    LogLifecycleEvent(m_subsystem, m_failureEvent, m_configSummary, reason, elapsed_ms(), m_frameIndex);
  }

 private:
  uint64_t elapsed_ms() const {
    const auto elapsed = std::chrono::steady_clock::now() - m_start;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
  }

  std::string m_subsystem;
  LifecycleEvent m_successEvent;
  LifecycleEvent m_failureEvent;
  std::string m_configSummary;
  uint64_t m_frameIndex = 0;
  std::chrono::steady_clock::time_point m_start;
  bool m_finished = false;
};

inline LifecycleOperation BeginInitializeLifecycle(std::string subsystem,
                                                   std::string config_summary = {},
                                                   uint64_t frame_index = 0) {
  return LifecycleOperation(std::move(subsystem),
                            LifecycleEvent::InitializeBegin,
                            LifecycleEvent::InitializeSuccess,
                            LifecycleEvent::InitializeFailure,
                            std::move(config_summary),
                            frame_index);
}

inline LifecycleOperation BeginShutdownLifecycle(std::string subsystem,
                                                 std::string config_summary = {},
                                                 uint64_t frame_index = 0) {
  return LifecycleOperation(std::move(subsystem),
                            LifecycleEvent::ShutdownBegin,
                            LifecycleEvent::ShutdownSuccess,
                            LifecycleEvent::ShutdownFailure,
                            std::move(config_summary),
                            frame_index);
}

}  // namespace vkpt::diagnostics

#define VKPT_LIFECYCLE_CONSTRUCT(subsystem) \
  ::vkpt::diagnostics::LogLifecycleEvent((subsystem), ::vkpt::diagnostics::LifecycleEvent::Construct)

#define VKPT_LIFECYCLE_CONFIGURE(subsystem, config_summary) \
  ::vkpt::diagnostics::LogLifecycleEvent((subsystem), ::vkpt::diagnostics::LifecycleEvent::Configure, (config_summary))
