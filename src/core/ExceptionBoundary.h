#pragma once

#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/Logging.h"
#include "diagnostics/CrashRecorder.h"

namespace vkpt::core {

namespace exception_boundary_detail {

inline void EnsureUnhandledExceptionLogSink() noexcept {
  static std::once_flag once;
  try {
    std::call_once(once, []() {
      std::error_code ec;
      std::filesystem::create_directories("artifacts/logs", ec);
      auto& logger = vkpt::log::Logger::instance();
      logger.add_sink(std::make_unique<vkpt::log::PlainTextFileSink>(
          "artifacts/logs/unhandled_exceptions.log"));
      logger.add_sink(std::make_unique<vkpt::log::JsonlFileSink>(
          "artifacts/logs/unhandled_exceptions.jsonl"));
    });
  } catch (...) {
  }
}

inline std::string ExceptionJson(std::string_view subsystem,
                                 std::string_view context,
                                 std::string_view error) {
  std::ostringstream out;
  out << "{"
      << "\"subsystem\":\"" << vkpt::log::EscapeJson(subsystem) << "\","
      << "\"context\":\"" << vkpt::log::EscapeJson(context) << "\","
      << "\"error\":\"" << vkpt::log::EscapeJson(error) << "\""
      << "}";
  return out.str();
}

inline int ReportUnhandledException(std::string_view subsystem,
                                    std::string_view context,
                                    std::string_view error,
                                    int exit_code) noexcept {
  constexpr std::string_view kUnknownError = "unknown exception";
  if (error.empty()) {
    error = kUnknownError;
  }

  std::cerr << subsystem << ": unhandled exception in " << context << ": "
            << error << "\n";

  EnsureUnhandledExceptionLogSink();

  try {
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Error,
        subsystem,
        "unhandled exception",
        {{"context", std::string(context)}, {"error", std::string(error)}});
  } catch (...) {
  }

  try {
    auto& recorder = vkpt::diagnostics::CrashRecorder::instance();
    // Record enough crash context before returning the caller's exit code; all
    // failures here are swallowed so exception reporting never throws again.
    recorder.set_last_error(error);
    recorder.record_checkpoint(
        "unhandled_exception", 0, subsystem, context, false);
    recorder.update_subsystem_state_json(
        "exception_boundary", ExceptionJson(subsystem, context, error));
    const auto dir = recorder.flush("artifacts/crashes");
    if (!dir.empty()) {
      std::cerr << subsystem << ": diagnostic artifact written to " << dir
                << "\n";
    }
  } catch (...) {
  }

  return exit_code;
}

}  // namespace exception_boundary_detail

template <typename Fn>
int RunWithExceptionBoundary(std::string_view subsystem,
                             std::string_view context,
                             Fn&& fn,
                             int exit_code = 1) noexcept {
  try {
    if constexpr (std::is_void_v<std::invoke_result_t<Fn&>>) {
      std::forward<Fn>(fn)();
      return 0;
    } else {
      return static_cast<int>(std::forward<Fn>(fn)());
    }
  } catch (const std::bad_alloc& ex) {
    return exception_boundary_detail::ReportUnhandledException(
        subsystem, context, ex.what(), exit_code);
  } catch (const std::filesystem::filesystem_error& ex) {
    return exception_boundary_detail::ReportUnhandledException(
        subsystem, context, ex.what(), exit_code);
  } catch (const std::exception& ex) {
    return exception_boundary_detail::ReportUnhandledException(
        subsystem, context, ex.what(), exit_code);
  } catch (...) {
    return exception_boundary_detail::ReportUnhandledException(
        subsystem, context, "non-standard exception", exit_code);
  }
}

}  // namespace vkpt::core
