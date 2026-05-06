#include "diagnostics/StatusFile.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vkpt::diagnostics {

std::string StatusTimestampNow() {
  const auto now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

static std::string EscJson(std::string_view sv) {
  std::string out;
  out.reserve(sv.size() + 4);
  for (char c : sv) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

bool WriteStatusFile(const StatusFileData& data,
                     const std::string& path,
                     std::string* error) {
  try {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        if (error) *error = "cannot create status directory: " + ec.message();
        return false;
      }
    }

    const std::string ts = data.timestamp.empty() ? StatusTimestampNow() : data.timestamp;

    // Keep this status document small and flat; launchers and CI probes read it
    // before opening heavier crash or benchmark artifacts.
    std::ostringstream json;
    json << "{\n"
         << "  \"build_status\": \""           << EscJson(data.build_status)           << "\",\n"
         << "  \"last_run_status\": \""         << EscJson(data.last_run_status)         << "\",\n"
         << "  \"enabled_backend\": \""         << EscJson(data.enabled_backend)         << "\",\n"
         << "  \"selected_scene\": \""          << EscJson(data.selected_scene)          << "\",\n"
         << "  \"selected_renderer_path\": \""  << EscJson(data.selected_renderer_path)  << "\",\n"
         << "  \"last_error\": \""              << EscJson(data.last_error)              << "\",\n"
         << "  \"last_crash_artifact\": \""     << EscJson(data.last_crash_artifact)     << "\",\n"
         << "  \"performance_summary\": \""     << EscJson(data.performance_summary)     << "\",\n"
         << "  \"timestamp\": \""               << EscJson(ts)                           << "\"\n"
         << "}\n";

    std::ofstream out(path);
    if (!out.is_open()) {
      if (error) *error = "cannot open status file: " + path;
      return false;
    }
    out << json.str();
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace vkpt::diagnostics
