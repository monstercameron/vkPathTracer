#include "core/Assert.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "core/Logging.h"

namespace vkpt::assert_detail {

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
      auto esc = [](std::string_view sv) -> std::string {
        std::string s;
        s.reserve(sv.size() + 4);
        for (char c : sv) {
          if (c == '"')  { s += "\\\""; }
          else if (c == '\\') { s += "\\\\"; }
          else if (c == '\n') { s += "\\n"; }
          else { s += c; }
        }
        return s;
      };
      out << "{\n"
          << "  \"type\": \"assert_failure\",\n"
          << "  \"condition\": \"" << esc(condition) << "\",\n"
          << "  \"message\": \"" << esc(message) << "\",\n"
          << "  \"file\": \"" << esc(file) << "\",\n"
          << "  \"line\": " << line << ",\n"
          << "  \"function\": \"" << esc(function) << "\"\n"
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

  // Write a minimal crash artifact so agents can inspect the failure.
  write_minimal_crash_artifact(condition, message, file, line, function);
}

}  // namespace vkpt::assert_detail
