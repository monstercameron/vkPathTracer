#include "core/Assert.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Logging.h"
#include "diagnostics/CrashRecorder.h"

namespace vkpt::assert_detail {

static std::string escape_json(std::string_view sv) {
  std::string s;
  s.reserve(sv.size() + 4);
  for (char c : sv) {
    if (c == '"') { s += "\\\""; }
    else if (c == '\\') { s += "\\\\"; }
    else if (c == '\n') { s += "\\n"; }
    else if (c == '\r') { s += "\\r"; }
    else if (c == '\t') { s += "\\t"; }
    else { s += c; }
  }
  return s;
}

// Write a minimal crash artifact without depending on the full crash recorder
// (which may not be initialised when an early assertion fires).
static void write_minimal_crash_artifact(std::string_view condition,
                                         std::string_view message,
                                         std::string_view file,
                                         int line,
                                         std::string_view function) noexcept {
  try {
    std::filesystem::create_directories("artifacts/crashes");
    // Build a timestamp string.
    const auto now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tm);

    const std::string dir = std::string("artifacts/crashes/assert_") + ts;
    std::filesystem::create_directories(dir);

    std::ofstream out(dir + "/crash_state.json");
    if (out.is_open()) {
      out << "{\n"
          << "  \"type\": \"assert_failure\",\n"
          << "  \"condition\": \"" << escape_json(condition) << "\",\n"
          << "  \"message\": \"" << escape_json(message) << "\",\n"
          << "  \"file\": \"" << escape_json(file) << "\",\n"
          << "  \"line\": " << line << ",\n"
          << "  \"function\": \"" << escape_json(function) << "\"\n"
          << "}\n";
    }
  } catch (...) {
    // Best-effort; do not throw from noexcept context.
  }
}

void handle_failure(std::string_view condition,
                    std::string_view message,
                    std::string_view file,
                    int line,
                    std::string_view function) noexcept {
  // Print to stderr immediately (safe path, no allocation required).
  std::fprintf(stderr,
               "\n[FATAL] %.*s\n  condition: %.*s\n  file: %.*s:%d\n  function: %.*s\n",
               static_cast<int>(message.size()), message.data(),
               static_cast<int>(condition.size()), condition.data(),
               static_cast<int>(file.size()), file.data(),
               line,
               static_cast<int>(function.size()), function.data());
  std::fflush(stderr);

  // Try to log via the structured logger (may already be shut down).
  try {
    std::ostringstream oss;
    oss << message << " [" << condition << "] at " << file << ":" << line
        << " in " << function;
    vkpt::log::Logger::instance().log(
        vkpt::log::Severity::Fatal, "assert", oss.str());
  } catch (...) {}

  bool wrote_full_bundle = false;
  try {
    std::ostringstream state;
    state << "{\n"
          << "  \"type\": \"assert_failure\",\n"
          << "  \"condition\": \"" << escape_json(condition) << "\",\n"
          << "  \"message\": \"" << escape_json(message) << "\",\n"
          << "  \"file\": \"" << escape_json(file) << "\",\n"
          << "  \"line\": " << line << ",\n"
          << "  \"function\": \"" << escape_json(function) << "\"\n"
          << "}";

    auto& recorder = vkpt::diagnostics::CrashRecorder::instance();
    recorder.set_last_error(message);
    recorder.record_checkpoint("assert_failure", 0, "assert", condition, false);
    recorder.update_subsystem_state_json("assert", state.str());
    wrote_full_bundle = !recorder.flush("artifacts/crashes").empty();
  } catch (...) {
    wrote_full_bundle = false;
  }

  if (!wrote_full_bundle) {
    write_minimal_crash_artifact(condition, message, file, line, function);
  }
}

}  // namespace vkpt::assert_detail
