#pragma once

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <system_error>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/Logging.h"

namespace vkpt::core {

namespace detail {

inline int CurrentProcessId() noexcept {
#if defined(_WIN32)
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

inline std::string ExecutionTraceBasePath() {
  return "artifacts/logs/execution_trace_" + std::to_string(CurrentProcessId());
}

class ExecutionPlainTextFileSink final : public vkpt::log::ILogSink {
 public:
  explicit ExecutionPlainTextFileSink(const std::string& path)
      : m_pid(CurrentProcessId()), m_out(path) {}

  void emit(const vkpt::log::LogEvent& event) override {
    if (!m_out.is_open() || event.subsystem != "execution") return;
    m_out << "[" << event.timestamp << "] "
          << "[" << vkpt::log::SeverityName(event.severity) << "] "
          << "[" << event.subsystem << "] "
          << "pid=" << m_pid << " "
          << event.message;
    for (const auto& field : event.fields) {
      m_out << " " << field.key << "=" << field.value;
    }
    m_out << '\n';
    m_out.flush();
  }

 private:
  int m_pid = 0;
  std::ofstream m_out;
};

class ExecutionJsonlFileSink final : public vkpt::log::ILogSink {
 public:
  explicit ExecutionJsonlFileSink(const std::string& path)
      : m_pid(CurrentProcessId()), m_out(path) {}

  void emit(const vkpt::log::LogEvent& event) override {
    if (!m_out.is_open() || event.subsystem != "execution") return;
    m_out << "{";
    m_out << "\"ts\":\"" << vkpt::log::EscapeJson(event.timestamp) << "\",";
    m_out << "\"severity\":\"" << vkpt::log::SeverityName(event.severity) << "\",";
    m_out << "\"subsystem\":\"" << vkpt::log::EscapeJson(event.subsystem) << "\",";
    m_out << "\"sequence\":" << event.sequence << ",";
    m_out << "\"pid\":" << m_pid << ",";
    m_out << "\"frame\":" << event.frameIndex << ",";
    m_out << "\"thread\":\"" << event.thread_id << "\",";
    m_out << "\"message\":\"" << vkpt::log::EscapeJson(event.message) << "\"";

    if (!event.fields.empty()) {
      m_out << ",\"fields\":[";
      for (std::size_t i = 0; i < event.fields.size(); ++i) {
        if (i) m_out << ',';
        m_out << "{";
        m_out << "\"k\":\"" << vkpt::log::EscapeJson(event.fields[i].key) << "\",";
        m_out << "\"v\":\"" << vkpt::log::EscapeJson(event.fields[i].value) << "\"";
        m_out << "}";
      }
      m_out << "]";
    }
    m_out << "}\n";
    m_out.flush();
  }

 private:
  int m_pid = 0;
  std::ofstream m_out;
};

}  // namespace detail

inline void EnsureExecutionTraceSinks() noexcept {
  static std::once_flag once;
  try {
    std::call_once(once, []() {
      std::error_code ec;
      std::filesystem::create_directories("artifacts/logs", ec);
      auto& logger = vkpt::log::Logger::instance();
      const std::string traceBasePath = detail::ExecutionTraceBasePath();
      logger.add_sink(std::make_unique<detail::ExecutionPlainTextFileSink>(
          traceBasePath + ".log"));
      logger.add_sink(std::make_unique<detail::ExecutionJsonlFileSink>(
          traceBasePath + ".jsonl"));
    });
  } catch (...) {
  }
}

inline void TraceExecution(std::string_view event,
                           std::initializer_list<vkpt::log::LogField> fields = {}) noexcept {
  try {
    EnsureExecutionTraceSinks();
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Info, "execution", event, fields);
  } catch (...) {
  }
}

}  // namespace vkpt::core
