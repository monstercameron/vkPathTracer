#include "diagnostics/CrashRecorder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "build_info.generated.h"

namespace vkpt::diagnostics {

// ---- helpers ----------------------------------------------------------------

namespace detail {
static std::string EscapeJson(std::string_view sv) {
  std::string out;
  out.reserve(sv.size() + 4);
  for (char c : sv) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

static std::string TimestampNow() {
  const auto now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &now);
#else
  gmtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
  return buf;
}
}  // namespace detail

// ---- CrashRecorder ----------------------------------------------------------

static std::mutex g_crashMutex;

CrashRecorder& CrashRecorder::instance() {
  static CrashRecorder singleton;
  return singleton;
}

void CrashRecorder::set_build_info(std::string version,
                                   std::string git_hash,
                                   std::string compiler,
                                   std::string target_os,
                                   std::string target_arch,
                                   std::string build_type,
                                   std::string enabled_features) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.build_version    = std::move(version);
  m_snapshot.git_hash         = std::move(git_hash);
  m_snapshot.compiler         = std::move(compiler);
  m_snapshot.target_os        = std::move(target_os);
  m_snapshot.target_arch      = std::move(target_arch);
  m_snapshot.build_type       = std::move(build_type);
  m_snapshot.enabled_features = std::move(enabled_features);
}

void CrashRecorder::update_backend(std::string_view backend_name) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.selected_backend = std::string(backend_name);
}

void CrashRecorder::update_frame_stage(std::string_view stage, uint64_t frame_index) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.last_frame_stage = std::string(stage);
  m_snapshot.last_frame_index = frame_index;
}

void CrashRecorder::update_pass(std::string_view pass_name) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.last_pass_name = std::string(pass_name);
}

void CrashRecorder::update_shader(std::string_view shader_variant) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.last_shader_variant = std::string(shader_variant);
}

void CrashRecorder::update_scene(std::string_view scene_name) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.active_scene = std::string(scene_name);
}

void CrashRecorder::set_last_error(std::string_view error) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.last_error = std::string(error);
}

void CrashRecorder::track_resource(std::string_view label,
                                    std::string_view kind,
                                    uint64_t size_bytes) {
  std::scoped_lock lock(g_crashMutex);
  LiveResourceInfo info;
  info.label      = std::string(label);
  info.kind       = std::string(kind);
  info.size_bytes = size_bytes;
  info.version    = 0;
  m_snapshot.live_resources.push_back(std::move(info));
}

void CrashRecorder::release_resource(std::string_view label) {
  std::scoped_lock lock(g_crashMutex);
  auto it = std::find_if(m_snapshot.live_resources.begin(),
                          m_snapshot.live_resources.end(),
                          [&](const LiveResourceInfo& r) { return r.label == label; });
  if (it != m_snapshot.live_resources.end()) {
    m_snapshot.live_resources.erase(it);
  }
}

std::string CrashRecorder::flush(const std::string& base_dir) {
  CrashStateSnapshot snap;
  {
    std::scoped_lock lock(g_crashMutex);
    snap = m_snapshot;
  }

  try {
    std::filesystem::create_directories(base_dir);
    const std::string dir = base_dir + "/crash_" + detail::TimestampNow();
    std::filesystem::create_directories(dir);

    // crash_state.json
    {
      std::ofstream out(dir + "/crash_state.json");
      out << SerializeCrashState(snap) << '\n';
    }

    // last_log_events.jsonl
    {
      std::ofstream out(dir + "/last_log_events.jsonl");
      out << SerializeRecentLogs(1024);
    }

    // build_info.json  (from compile-time constants)
    {
      std::ofstream out(dir + "/build_info.json");
      out << "{\n"
          << "  \"version\": \"" << detail::EscapeJson(snap.build_version) << "\",\n"
          << "  \"git_hash\": \"" << detail::EscapeJson(snap.git_hash) << "\",\n"
          << "  \"compiler\": \"" << detail::EscapeJson(snap.compiler) << "\",\n"
          << "  \"target_os\": \"" << detail::EscapeJson(snap.target_os) << "\",\n"
          << "  \"target_arch\": \"" << detail::EscapeJson(snap.target_arch) << "\",\n"
          << "  \"build_type\": \"" << detail::EscapeJson(snap.build_type) << "\",\n"
          << "  \"enabled_features\": \"" << detail::EscapeJson(snap.enabled_features) << "\"\n"
          << "}\n";
    }

    return dir;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "[crash-recorder] flush failed: %s\n", ex.what());
    return {};
  }
}

// ---- SerializeCrashState ----------------------------------------------------

std::string SerializeCrashState(const CrashStateSnapshot& s) {
  auto esc = detail::EscapeJson;
  std::ostringstream out;
  out << "{\n";
  out << "  \"build_version\": \""    << esc(s.build_version)    << "\",\n";
  out << "  \"git_hash\": \""         << esc(s.git_hash)          << "\",\n";
  out << "  \"compiler\": \""         << esc(s.compiler)          << "\",\n";
  out << "  \"target_os\": \""        << esc(s.target_os)         << "\",\n";
  out << "  \"target_arch\": \""      << esc(s.target_arch)       << "\",\n";
  out << "  \"build_type\": \""       << esc(s.build_type)        << "\",\n";
  out << "  \"enabled_features\": \"" << esc(s.enabled_features)  << "\",\n";
  out << "  \"selected_backend\": \"" << esc(s.selected_backend)  << "\",\n";
  out << "  \"last_frame_stage\": \"" << esc(s.last_frame_stage)  << "\",\n";
  out << "  \"last_frame_index\": "   << s.last_frame_index       << ",\n";
  out << "  \"last_pass_name\": \""   << esc(s.last_pass_name)    << "\",\n";
  out << "  \"last_shader_variant\": \"" << esc(s.last_shader_variant) << "\",\n";
  out << "  \"last_error\": \""       << esc(s.last_error)        << "\",\n";
  out << "  \"active_scene\": \""     << esc(s.active_scene)      << "\",\n";

  out << "  \"live_resources\": [\n";
  for (std::size_t i = 0; i < s.live_resources.size(); ++i) {
    const auto& r = s.live_resources[i];
    out << "    { \"label\": \"" << esc(r.label) << "\","
        << " \"kind\": \"" << esc(r.kind) << "\","
        << " \"size_bytes\": " << r.size_bytes << " }";
    if (i + 1 < s.live_resources.size()) out << ',';
    out << '\n';
  }
  out << "  ]\n";
  out << "}";
  return out.str();
}

// ---- SerializeRecentLogs ----------------------------------------------------

std::string SerializeRecentLogs(std::size_t max_events) {
  // Pull the ring buffer snapshot from the global logger.
  // We look for a RingBufferSink via the public snapshot() method.
  // Since the logger doesn't expose sinks directly, we store a pointer
  // to the ring buffer sink set during InitializeLogging and access it via
  // a file-static registration path.
  //
  // For simplicity, reuse the JSONL file that is always written by
  // JsonlFileSink. This function is only called during a crash, so we
  // read the last N lines from artifacts/logs/ptapp.jsonl.

  std::ostringstream out;
  try {
    std::ifstream in("artifacts/logs/ptapp.jsonl");
    if (!in.is_open()) return out.str();

    std::deque<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
      lines.push_back(line);
      if (lines.size() > max_events) {
        lines.pop_front();
      }
    }
    for (const auto& l : lines) {
      out << l << '\n';
    }
  } catch (...) {}
  return out.str();
}

}  // namespace vkpt::diagnostics
