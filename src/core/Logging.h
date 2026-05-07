#pragma once

#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/Types.h"

namespace vkpt::log {

enum class Severity {
  Trace,
  Debug,
  Info,
  Warning,
  Error,
  Fatal
};

inline const char* SeverityName(Severity severity) {
  switch (severity) {
    case Severity::Trace:
      return "trace";
    case Severity::Debug:
      return "debug";
    case Severity::Info:
      return "info";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
    case Severity::Fatal:
      return "fatal";
    default:
      return "info";
  }
}

inline std::string EscapeJson(std::string_view text) {
  std::string escaped;
  escaped.reserve(text.size() + 16);
  for (const char ch : text) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

struct LogField {
  std::string key;
  std::string value;
};

struct LogEvent {
  Severity severity = Severity::Info;
  vkpt::core::FrameIndex sequence = 0;
  vkpt::core::FrameIndex frameIndex = 0;
  std::thread::id thread_id;
  std::string subsystem;
  std::string message;
  std::vector<LogField> fields;
  std::string timestamp;
};

class ILogSink {
 public:
  virtual ~ILogSink() = default;
  virtual void emit(const LogEvent& event) = 0;
};

class RingBufferSink final : public ILogSink {
 public:
  explicit RingBufferSink(std::size_t capacity = 1024) : m_capacity(capacity) {}

  void emit(const LogEvent& event) override {
    std::scoped_lock lock(m_mutex);
    if (m_events.size() >= m_capacity) {
      m_events.pop_front();
    }
    m_events.push_back(event);
  }

  std::vector<LogEvent> snapshot() const {
    std::scoped_lock lock(m_mutex);
    return {m_events.begin(), m_events.end()};
  }

 private:
  std::size_t m_capacity;
  mutable std::mutex m_mutex;
  std::deque<LogEvent> m_events;
};

class ConsoleSink final : public ILogSink {
 public:
  explicit ConsoleSink(std::ostream& out = std::cout) : m_out(out) {}

  void emit(const LogEvent& event) override {
    m_out << "[" << event.timestamp << "] "
          << "[" << SeverityName(event.severity) << "] "
          << "[" << event.subsystem << "] "
          << "frame=" << event.frameIndex << " "
          << "thread=" << event.thread_id << " "
          << event.message;
    for (const auto& field : event.fields) {
      m_out << " " << field.key << "=" << field.value;
    }
    m_out << '\n';
    m_out.flush();
  }

 private:
  std::ostream& m_out;
};

class PlainTextFileSink final : public ILogSink {
 public:
  explicit PlainTextFileSink(const std::string& path) : m_out(path) {}

  void emit(const LogEvent& event) override {
    if (!m_out.is_open()) return;
    m_out << "[" << event.timestamp << "] "
          << "[" << SeverityName(event.severity) << "] "
          << "[" << event.subsystem << "] "
          << event.message << '\n';
    m_out.flush();
  }

 private:
  std::ofstream m_out;
};

class JsonlFileSink final : public ILogSink {
 public:
  explicit JsonlFileSink(const std::string& path) : m_out(path) {}

  void emit(const LogEvent& event) override {
    if (!m_out.is_open()) return;
    m_out << "{";
    m_out << "\"ts\":\"" << EscapeJson(event.timestamp) << "\",";
    m_out << "\"severity\":\"" << SeverityName(event.severity) << "\",";
    m_out << "\"subsystem\":\"" << EscapeJson(event.subsystem) << "\",";
    m_out << "\"sequence\":" << event.sequence << ",";
    m_out << "\"frame\":" << event.frameIndex << ",";
    m_out << "\"thread\":\"" << event.thread_id << "\",";
    m_out << "\"message\":\"" << EscapeJson(event.message) << "\"";

    if (!event.fields.empty()) {
      m_out << ",\"fields\":[";
      for (std::size_t i = 0; i < event.fields.size(); ++i) {
        if (i) m_out << ',';
        m_out << "{";
        m_out << "\"k\":\"" << EscapeJson(event.fields[i].key) << "\",";
        m_out << "\"v\":\"" << EscapeJson(event.fields[i].value) << "\"";
        m_out << "}";
      }
      m_out << "]";
    }
    m_out << "}\n";
    m_out.flush();
  }

 private:
  std::ofstream m_out;
};

class Logger final {
 public:
  static Logger& instance() {
    static Logger singleton;
    return singleton;
  }

  void set_min_severity(Severity severity) { m_minSeverity = severity; }
  bool enabled(Severity severity) const { return severity >= m_minSeverity; }
  void add_sink(std::unique_ptr<ILogSink> sink) {
    std::scoped_lock lock(m_mutex);
    m_sinks.push_back(std::move(sink));
  }

  void log(Severity severity,
           std::string_view subsystem,
           std::string_view message,
           std::initializer_list<LogField> fields = {},
           vkpt::core::FrameIndex frameIndex = 0) {
    if (!enabled(severity)) {
      return;
    }
    LogEvent event;
    event.severity = severity;
    event.sequence = m_sequence.fetch_add(1, std::memory_order_relaxed);
    event.frameIndex = frameIndex;
    event.thread_id = std::this_thread::get_id();
    event.subsystem = std::string(subsystem);
    event.message = std::string(message);
    event.timestamp = NowString();
    event.fields.assign(fields.begin(), fields.end());

    std::scoped_lock lock(m_mutex);
    for (auto& sink : m_sinks) {
      if (sink) sink->emit(event);
    }
  }

 private:
  Logger() = default;

  static std::string NowString() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count() % 1000;

    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
    out << '.' << std::setfill('0') << std::setw(3) << ms << "Z";
    return out.str();
  }

  Severity m_minSeverity = Severity::Info;
  std::atomic<vkpt::core::FrameIndex> m_sequence{0};
  std::mutex m_mutex;
  std::vector<std::unique_ptr<ILogSink>> m_sinks;
};

}  // namespace vkpt::log

