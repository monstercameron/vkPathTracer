#include "diagnostics/CrashRecorder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#include "core/BuildInfo.h"

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

static std::string JsonDocumentOr(std::string_view json, std::string_view fallback) {
  if (json.empty()) {
    return std::string(fallback);
  }
  return std::string(json);
}

static void WriteTextFile(const std::string& path, std::string_view text) {
  std::ofstream out(path);
  out << text;
  if (text.empty() || text.back() != '\n') {
    out << '\n';
  }
}

static std::string CreateCrashArtifactDir(const std::string& base_dir) {
  std::filesystem::create_directories(base_dir);
  const std::string timestamp = TimestampNow();
  // Crash artifacts are append-only: retry with a numeric suffix instead of
  // overwriting an earlier crash from the same UTC second.
  for (uint32_t attempt = 0; attempt < 1000; ++attempt) {
    std::string name = "crash_" + timestamp;
    if (attempt != 0) {
      name += "_" + std::to_string(attempt);
    }
    const std::filesystem::path dir = std::filesystem::path(base_dir) / name;
    if (std::filesystem::create_directories(dir)) {
      return dir.string();
    }
  }
  throw std::runtime_error("failed to allocate unique crash artifact directory");
}

static std::string StringOr(std::string value, std::string_view fallback) {
  if (value.empty()) {
    return std::string(fallback);
  }
  return value;
}

static void AppendLiveResourcesJson(std::ostringstream& out,
                                    const std::vector<LiveResourceInfo>& resources,
                                    std::string_view indent) {
  out << "[\n";
  for (std::size_t i = 0; i < resources.size(); ++i) {
    const auto& r = resources[i];
    out << indent << "  { \"label\": \"" << EscapeJson(r.label) << "\","
        << " \"kind\": \"" << EscapeJson(r.kind) << "\","
        << " \"size_bytes\": " << r.size_bytes << ","
        << " \"version\": " << r.version << " }";
    if (i + 1 < resources.size()) {
      out << ',';
    }
    out << '\n';
  }
  out << indent << ']';
}

static void AppendCheckpointsJson(std::ostringstream& out,
                                  const std::vector<CrashCheckpoint>& checkpoints,
                                  std::string_view indent) {
  out << "[\n";
  for (std::size_t i = 0; i < checkpoints.size(); ++i) {
    const auto& c = checkpoints[i];
    out << indent << "  { \"name\": \"" << EscapeJson(c.name) << "\","
        << " \"subsystem\": \"" << EscapeJson(c.subsystem) << "\","
        << " \"detail\": \"" << EscapeJson(c.detail) << "\","
        << " \"frame_index\": " << c.frame_index << ","
        << " \"successful\": " << (c.successful ? "true" : "false") << ","
        << " \"timestamp_utc\": \"" << EscapeJson(c.timestamp_utc) << "\" }";
    if (i + 1 < checkpoints.size()) {
      out << ',';
    }
    out << '\n';
  }
  out << indent << ']';
}

static std::string SerializeFrameStateFile(const CrashStateSnapshot& s) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"last_frame_stage\": \"" << EscapeJson(s.last_frame_stage) << "\",\n";
  out << "  \"last_frame_index\": " << s.last_frame_index << ",\n";
  out << "  \"last_pass_name\": \"" << EscapeJson(s.last_pass_name) << "\",\n";
  out << "  \"last_shader_variant\": \"" << EscapeJson(s.last_shader_variant) << "\",\n";
  out << "  \"last_successful_checkpoint\": \"" << EscapeJson(s.last_successful_checkpoint) << "\",\n";
  out << "  \"checkpoints\": ";
  AppendCheckpointsJson(out, s.checkpoints, "  ");
  out << ",\n";
  out << "  \"reported_state\": " << JsonDocumentOr(s.frame_state_json, "{}") << '\n';
  out << '}';
  return out.str();
}

static std::string SerializeResourceStateFile(const CrashStateSnapshot& s) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"live_resource_count\": " << s.live_resources.size() << ",\n";
  out << "  \"live_resources\": ";
  AppendLiveResourcesJson(out, s.live_resources, "  ");
  out << ",\n";
  out << "  \"reported_state\": " << JsonDocumentOr(s.resource_state_json, "{}") << '\n';
  out << '}';
  return out.str();
}

static std::string SerializeBackendStateFile(const CrashStateSnapshot& s) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"selected_backend\": \"" << EscapeJson(s.selected_backend) << "\",\n";
  out << "  \"last_pass_name\": \"" << EscapeJson(s.last_pass_name) << "\",\n";
  out << "  \"last_shader_variant\": \"" << EscapeJson(s.last_shader_variant) << "\",\n";
  out << "  \"renderer_state\": " << JsonDocumentOr(s.renderer_state_json, "{}") << ",\n";
  out << "  \"reported_state\": " << JsonDocumentOr(s.backend_state_json, "{}") << '\n';
  out << '}';
  return out.str();
}

static std::string SerializeSceneStateFile(const CrashStateSnapshot& s) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"active_scene\": \"" << EscapeJson(s.active_scene) << "\",\n";
  out << "  \"reported_state\": " << JsonDocumentOr(s.scene_state_json, "{}") << '\n';
  out << '}';
  return out.str();
}

static std::string SerializeSubsystemStatesFile(const CrashStateSnapshot& s) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"subsystems\": [\n";
  for (std::size_t i = 0; i < s.subsystem_states.size(); ++i) {
    const auto& entry = s.subsystem_states[i];
    out << "    { \"subsystem\": \"" << EscapeJson(entry.subsystem) << "\","
        << " \"timestamp_utc\": \"" << EscapeJson(entry.timestamp_utc) << "\","
        << " \"state\": " << JsonDocumentOr(entry.state_json, "{}") << " }";
    if (i + 1 < s.subsystem_states.size()) {
      out << ',';
    }
    out << '\n';
  }
  out << "  ]\n";
  out << '}';
  return out.str();
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
                                   std::string enabled_features,
                                   std::string build_date,
                                   std::string compiler_name,
                                   std::string compiler_version,
                                   std::string cxx_standard,
                                   std::string disabled_features,
                                   std::string sanitizer_mode,
                                   std::string sanitizer_flavor,
                                   std::string simd_compile_options,
                                   std::string backend_compile_options,
                                   std::string platform_shells) {
  const auto build = vkpt::build::GetBuildMetadata();
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.build_version = detail::StringOr(std::move(version), build.project_version);
  m_snapshot.git_hash = detail::StringOr(std::move(git_hash), build.git_hash);
  m_snapshot.build_date = detail::StringOr(std::move(build_date), build.build_date);
  m_snapshot.compiler_name = detail::StringOr(std::move(compiler_name), build.compiler_name);
  m_snapshot.compiler_version = detail::StringOr(std::move(compiler_version), build.compiler_version);
  m_snapshot.compiler = detail::StringOr(
      std::move(compiler), m_snapshot.compiler_name + " " + m_snapshot.compiler_version);
  m_snapshot.cxx_standard = detail::StringOr(std::move(cxx_standard), build.cxx_standard);
  m_snapshot.target_os = detail::StringOr(std::move(target_os), build.target_os);
  m_snapshot.target_arch = detail::StringOr(std::move(target_arch), build.target_arch);
  m_snapshot.build_type = detail::StringOr(std::move(build_type), build.build_type);
  m_snapshot.enabled_features =
      detail::StringOr(std::move(enabled_features), std::string(vkpt::build::kEnabledFeatureFlags));
  m_snapshot.disabled_features =
      detail::StringOr(std::move(disabled_features), std::string(vkpt::build::kDisabledFeatureFlags));
  m_snapshot.sanitizer_mode = detail::StringOr(std::move(sanitizer_mode), build.sanitizer_mode);
  m_snapshot.sanitizer_flavor =
      detail::StringOr(std::move(sanitizer_flavor), build.sanitizer_flavor);
  m_snapshot.simd_compile_options =
      detail::StringOr(std::move(simd_compile_options), build.simd_compile_options);
  m_snapshot.backend_compile_options =
      detail::StringOr(std::move(backend_compile_options), build.backend_compile_options);
  m_snapshot.platform_shells = detail::StringOr(std::move(platform_shells), build.platform_shells);
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

void CrashRecorder::update_ui_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.ui_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_selection_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.selection_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_layout_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.layout_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_ui_events_jsonl(std::string_view jsonl) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.ui_events_jsonl = std::string(jsonl);
}

void CrashRecorder::update_editor_commands_jsonl(std::string_view jsonl) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.editor_commands_jsonl = std::string(jsonl);
}

void CrashRecorder::update_renderer_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.renderer_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_runtime_config_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.runtime_config_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_frame_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.frame_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_resource_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.resource_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_backend_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.backend_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_scene_state_json(std::string_view json) {
  std::scoped_lock lock(g_crashMutex);
  m_snapshot.scene_state_json = json.empty() ? "{}" : std::string(json);
}

void CrashRecorder::update_subsystem_state_json(std::string_view subsystem, std::string_view json) {
  const std::string subsystem_name(subsystem);
  const std::string state = json.empty() ? "{}" : std::string(json);

  std::scoped_lock lock(g_crashMutex);
  if (subsystem_name == "runtime_config") {
    m_snapshot.runtime_config_json = state;
  } else if (subsystem_name == "frame") {
    m_snapshot.frame_state_json = state;
  } else if (subsystem_name == "resource") {
    m_snapshot.resource_state_json = state;
  } else if (subsystem_name == "backend") {
    m_snapshot.backend_state_json = state;
  } else if (subsystem_name == "scene") {
    m_snapshot.scene_state_json = state;
  } else if (subsystem_name == "renderer") {
    m_snapshot.renderer_state_json = state;
  } else if (subsystem_name == "ui") {
    m_snapshot.ui_state_json = state;
  } else if (subsystem_name == "selection") {
    m_snapshot.selection_state_json = state;
  } else if (subsystem_name == "layout") {
    m_snapshot.layout_state_json = state;
  }

  auto it = std::find_if(m_snapshot.subsystem_states.begin(),
                         m_snapshot.subsystem_states.end(),
                         [&](const SubsystemStateInfo& entry) {
                           return entry.subsystem == subsystem_name;
                         });
  if (it == m_snapshot.subsystem_states.end()) {
    SubsystemStateInfo info;
    info.subsystem = subsystem_name;
    info.state_json = state;
    info.timestamp_utc = detail::TimestampNow();
    m_snapshot.subsystem_states.push_back(std::move(info));
  } else {
    it->state_json = state;
    it->timestamp_utc = detail::TimestampNow();
  }
}

void CrashRecorder::record_checkpoint(std::string_view name,
                                      uint64_t frame_index,
                                      std::string_view subsystem,
                                      std::string_view checkpoint_detail,
                                      bool successful) {
  CrashCheckpoint checkpoint;
  checkpoint.name = std::string(name);
  checkpoint.subsystem = std::string(subsystem);
  checkpoint.detail = std::string(checkpoint_detail);
  checkpoint.frame_index = frame_index;
  checkpoint.successful = successful;
  checkpoint.timestamp_utc = detail::TimestampNow();

  std::scoped_lock lock(g_crashMutex);
  if (successful && !checkpoint.name.empty()) {
    m_snapshot.last_successful_checkpoint = checkpoint.name;
  }
  m_snapshot.checkpoints.push_back(std::move(checkpoint));
  constexpr std::size_t kMaxCheckpoints = 256;
  if (m_snapshot.checkpoints.size() > kMaxCheckpoints) {
    m_snapshot.checkpoints.erase(m_snapshot.checkpoints.begin(),
                                 m_snapshot.checkpoints.begin() +
                                     (m_snapshot.checkpoints.size() - kMaxCheckpoints));
  }
}

void CrashRecorder::track_resource(std::string_view label,
                                    std::string_view kind,
                                    uint64_t size_bytes) {
  std::scoped_lock lock(g_crashMutex);
  auto it = std::find_if(m_snapshot.live_resources.begin(),
                         m_snapshot.live_resources.end(),
                         [&](const LiveResourceInfo& r) { return r.label == label; });
  if (it != m_snapshot.live_resources.end()) {
    it->kind = std::string(kind);
    it->size_bytes = size_bytes;
    ++it->version;
    return;
  }

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
    const std::string dir = detail::CreateCrashArtifactDir(base_dir);

    detail::WriteTextFile(dir + "/crash_state.json", SerializeCrashState(snap));

    const std::string recent_logs = SerializeRecentLogs(1024);
    detail::WriteTextFile(dir + "/last_1024_log_events.jsonl", recent_logs);
    detail::WriteTextFile(dir + "/last_log_events.jsonl", recent_logs);

    // build_info.json  (from compile-time constants)
    {
      std::ofstream out(dir + "/build_info.json");
      out << "{\n"
          << "  \"version\": \"" << detail::EscapeJson(snap.build_version) << "\",\n"
          << "  \"git_hash\": \"" << detail::EscapeJson(snap.git_hash) << "\",\n"
          << "  \"build_date\": \"" << detail::EscapeJson(snap.build_date) << "\",\n"
          << "  \"compiler\": \"" << detail::EscapeJson(snap.compiler) << "\",\n"
          << "  \"compiler_name\": \"" << detail::EscapeJson(snap.compiler_name) << "\",\n"
          << "  \"compiler_version\": \"" << detail::EscapeJson(snap.compiler_version) << "\",\n"
          << "  \"cxx_standard\": \"" << detail::EscapeJson(snap.cxx_standard) << "\",\n"
          << "  \"target_os\": \"" << detail::EscapeJson(snap.target_os) << "\",\n"
          << "  \"target_arch\": \"" << detail::EscapeJson(snap.target_arch) << "\",\n"
          << "  \"build_type\": \"" << detail::EscapeJson(snap.build_type) << "\",\n"
          << "  \"platform_shells\": \"" << detail::EscapeJson(snap.platform_shells) << "\",\n"
          << "  \"enabled_features_csv\": \"" << detail::EscapeJson(snap.enabled_features) << "\",\n"
          << "  \"disabled_features_csv\": \"" << detail::EscapeJson(snap.disabled_features) << "\",\n"
          << "  \"sanitizer_mode\": \"" << detail::EscapeJson(snap.sanitizer_mode) << "\",\n"
          << "  \"sanitizer_flavor\": \"" << detail::EscapeJson(snap.sanitizer_flavor) << "\",\n"
          << "  \"simd_compile_options\": \"" << detail::EscapeJson(snap.simd_compile_options) << "\",\n"
          << "  \"backend_compile_options\": \"" << detail::EscapeJson(snap.backend_compile_options) << "\"\n"
          << "}\n";
    }

    detail::WriteTextFile(dir + "/runtime_config.json",
                          detail::JsonDocumentOr(snap.runtime_config_json, "{}"));
    detail::WriteTextFile(dir + "/last_frame_state.json", detail::SerializeFrameStateFile(snap));
    detail::WriteTextFile(dir + "/resource_state.json", detail::SerializeResourceStateFile(snap));
    detail::WriteTextFile(dir + "/active_backend_state.json",
                          detail::SerializeBackendStateFile(snap));
    detail::WriteTextFile(dir + "/active_scene_state.json", detail::SerializeSceneStateFile(snap));
    detail::WriteTextFile(dir + "/subsystem_states.json",
                          detail::SerializeSubsystemStatesFile(snap));

    detail::WriteTextFile(dir + "/ui_state.json", detail::JsonDocumentOr(snap.ui_state_json, "{}"));
    detail::WriteTextFile(dir + "/selection_state.json",
                          detail::JsonDocumentOr(snap.selection_state_json, "{}"));
    detail::WriteTextFile(dir + "/layout_state.json",
                          detail::JsonDocumentOr(snap.layout_state_json, "{}"));

    // ui_events.jsonl
    {
      std::ofstream out(dir + "/ui_events.jsonl");
      out << snap.ui_events_jsonl;
    }

    // editor_commands.jsonl
    {
      std::ofstream out(dir + "/editor_commands.jsonl");
      out << snap.editor_commands_jsonl;
    }

    detail::WriteTextFile(dir + "/renderer_state.json",
                          detail::JsonDocumentOr(snap.renderer_state_json, "{}"));

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
  out << "  \"build_date\": \""       << esc(s.build_date)         << "\",\n";
  out << "  \"compiler\": \""         << esc(s.compiler)          << "\",\n";
  out << "  \"compiler_name\": \""    << esc(s.compiler_name)     << "\",\n";
  out << "  \"compiler_version\": \"" << esc(s.compiler_version)  << "\",\n";
  out << "  \"cxx_standard\": \""     << esc(s.cxx_standard)      << "\",\n";
  out << "  \"target_os\": \""        << esc(s.target_os)         << "\",\n";
  out << "  \"target_arch\": \""      << esc(s.target_arch)       << "\",\n";
  out << "  \"build_type\": \""       << esc(s.build_type)        << "\",\n";
  out << "  \"enabled_features\": \"" << esc(s.enabled_features)  << "\",\n";
  out << "  \"disabled_features\": \"" << esc(s.disabled_features) << "\",\n";
  out << "  \"sanitizer_mode\": \"" << esc(s.sanitizer_mode) << "\",\n";
  out << "  \"sanitizer_flavor\": \"" << esc(s.sanitizer_flavor) << "\",\n";
  out << "  \"simd_compile_options\": \"" << esc(s.simd_compile_options) << "\",\n";
  out << "  \"backend_compile_options\": \"" << esc(s.backend_compile_options) << "\",\n";
  out << "  \"platform_shells\": \"" << esc(s.platform_shells) << "\",\n";
  out << "  \"selected_backend\": \"" << esc(s.selected_backend)  << "\",\n";
  out << "  \"last_frame_stage\": \"" << esc(s.last_frame_stage)  << "\",\n";
  out << "  \"last_frame_index\": "   << s.last_frame_index       << ",\n";
  out << "  \"last_pass_name\": \""   << esc(s.last_pass_name)    << "\",\n";
  out << "  \"last_shader_variant\": \"" << esc(s.last_shader_variant) << "\",\n";
  out << "  \"last_error\": \""       << esc(s.last_error)        << "\",\n";
  out << "  \"active_scene\": \""     << esc(s.active_scene)      << "\",\n";
  out << "  \"last_successful_checkpoint\": \"" << esc(s.last_successful_checkpoint) << "\",\n";
  out << "  \"runtime_config\": " << detail::JsonDocumentOr(s.runtime_config_json, "{}") << ",\n";
  out << "  \"frame_state\": " << detail::JsonDocumentOr(s.frame_state_json, "{}") << ",\n";
  out << "  \"resource_state\": " << detail::JsonDocumentOr(s.resource_state_json, "{}") << ",\n";
  out << "  \"backend_state\": " << detail::JsonDocumentOr(s.backend_state_json, "{}") << ",\n";
  out << "  \"scene_state\": " << detail::JsonDocumentOr(s.scene_state_json, "{}") << ",\n";

  out << "  \"checkpoints\": ";
  detail::AppendCheckpointsJson(out, s.checkpoints, "  ");
  out << ",\n";

  out << "  \"live_resources\": [\n";
  for (std::size_t i = 0; i < s.live_resources.size(); ++i) {
    const auto& r = s.live_resources[i];
    out << "    { \"label\": \"" << esc(r.label) << "\","
        << " \"kind\": \"" << esc(r.kind) << "\","
        << " \"size_bytes\": " << r.size_bytes << ","
        << " \"version\": " << r.version << " }";
    if (i + 1 < s.live_resources.size()) out << ',';
    out << '\n';
  }
  out << "  ],\n";

  out << "  \"subsystem_states\": [\n";
  for (std::size_t i = 0; i < s.subsystem_states.size(); ++i) {
    const auto& entry = s.subsystem_states[i];
    out << "    { \"subsystem\": \"" << esc(entry.subsystem) << "\","
        << " \"timestamp_utc\": \"" << esc(entry.timestamp_utc) << "\","
        << " \"state\": " << detail::JsonDocumentOr(entry.state_json, "{}") << " }";
    if (i + 1 < s.subsystem_states.size()) out << ',';
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
