#include "core/Config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#include "core/EngineConfig.h"

namespace vkpt::config {

namespace detail {

static std::string Trim(std::string_view sv) {
  const auto start = sv.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return {};
  const auto end = sv.find_last_not_of(" \t\r\n");
  return std::string(sv.substr(start, end - start + 1));
}

static std::string EscapeJsonString(std::string_view sv) {
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

static std::optional<std::string> ReadEnvVar(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t value_size = 0;
  if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out(value, value_size > 0 ? value_size - 1 : 0);
  std::free(value);
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
#else
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

}  // namespace detail

// ---- ParseConfigFile --------------------------------------------------------

bool ParseConfigFile(const std::string& path,
                     std::vector<ParsedConfigEntry>& out_entries,
                     std::string* error) {
  std::ifstream in(path);
  if (!in.is_open()) {
    if (error) *error = "cannot open config file: " + path;
    return false;
  }
  std::string line;
  int line_num = 0;
  while (std::getline(in, line)) {
    ++line_num;
    const auto comment = line.find('#');
    if (comment != std::string::npos) {
      line = line.substr(0, comment);
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    ParsedConfigEntry entry;
    entry.key = detail::Trim(line.substr(0, eq));
    entry.value = detail::Trim(line.substr(eq + 1));
    entry.line = line_num;
    if (!entry.key.empty()) {
      out_entries.push_back(std::move(entry));
    }
  }
  return true;
}

// ---- ApplyConfigEntries -----------------------------------------------------

static bool ParseBool(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower == "true" || lower == "1" || lower == "yes";
}

static std::optional<uint32_t> TryParseU32(const std::string& value) {
  try {
    std::size_t parsed = 0;
    const auto parsed_value = std::stoul(value, &parsed);
    if (parsed != value.size() || parsed_value > std::numeric_limits<uint32_t>::max()) {
      return std::nullopt;
    }
    return static_cast<uint32_t>(parsed_value);
  } catch (...) {
    return std::nullopt;
  }
}

static uint32_t ParseU32(const std::string& value) {
  return TryParseU32(value).value_or(0);
}

static uint32_t ParseUiPresentHz(const std::string& value) {
  return ClampUiPresentHz(ParseU32(value));
}

void ApplyConfigEntries(const std::vector<ParsedConfigEntry>& entries,
                        RuntimeConfig& config) {
  for (const auto& e : entries) {
    if      (e.key == "backend")              { config.backend            = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "scene_path")           { config.scene_path         = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "log_level")            { config.log_level          = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "platform")             { config.platform           = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "headless")             { config.headless           = {ParseBool(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "benchmark_mode")       { config.benchmark_mode     = {ParseBool(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "ui_present_hz")        { config.ui_present_hz      = {ParseUiPresentHz(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "render_width")         { config.render_width       = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "render_height")        { config.render_height      = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "spp")                  { config.spp                = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "max_depth")            { config.max_depth          = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "output_path")          { config.output_path        = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "exr_output_path")      { config.exr_output_path    = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "benchmark_warmup" ||
             e.key == "benchmark_warmup_frames") { config.benchmark_warmup_frames = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "benchmark_tile_size")  { config.benchmark_tile_size     = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "write_status_file")    { config.write_status_file  = {ParseBool(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "status_file_path")     { config.status_file_path   = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "crash_artifact_dir")   { config.crash_artifact_dir = {e.value, ConfigSource::ConfigFile}; }
    // Unknown keys are silently ignored to allow forward-compat config files.
  }
}

// ---- ApplyEnvVars -----------------------------------------------------------

void ApplyEnvVars(RuntimeConfig& config) {
  auto env = [](const char* name) -> std::optional<std::string> {
    return detail::ReadEnvVar(name);
  };

  auto apply_u32 = [&](const char* name, ConfigValue<uint32_t>& field) {
    if (auto v = env(name)) {
      if (auto parsed = TryParseU32(*v)) {
        field = {*parsed, ConfigSource::EnvVar};
      }
    }
  };
  auto apply_ui_present_hz = [&](const char* name, ConfigValue<uint32_t>& field) {
    if (auto v = env(name)) {
      if (auto parsed = TryParseU32(*v)) {
        field = {ClampUiPresentHz(*parsed), ConfigSource::EnvVar};
      }
    }
  };

  if (auto v = env("PTAPP_BACKEND")) { config.backend = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_LOG_LEVEL")) { config.log_level = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_SCENE")) { config.scene_path = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_PLATFORM")) { config.platform = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_HEADLESS")) { config.headless = {ParseBool(*v), ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_BENCHMARK_MODE")) {
    config.benchmark_mode = {ParseBool(*v), ConfigSource::EnvVar};
  }
  apply_ui_present_hz("PTAPP_UI_PRESENT_HZ", config.ui_present_hz);
  apply_u32("PTAPP_RENDER_WIDTH", config.render_width);
  apply_u32("PTAPP_RENDER_HEIGHT", config.render_height);
  apply_u32("PTAPP_SPP", config.spp);
  apply_u32("PTAPP_MAX_DEPTH", config.max_depth);
  if (auto v = env("PTAPP_OUTPUT_PATH")) { config.output_path = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_EXR_OUTPUT_PATH")) {
    config.exr_output_path = {*v, ConfigSource::EnvVar};
  }
  apply_u32("PTAPP_BENCHMARK_WARMUP_FRAMES", config.benchmark_warmup_frames);
  apply_u32("PTAPP_BENCHMARK_TILE_SIZE", config.benchmark_tile_size);
  if (auto v = env("PTAPP_WRITE_STATUS_FILE")) {
    config.write_status_file = {ParseBool(*v), ConfigSource::EnvVar};
  }
  if (auto v = env("PTAPP_STATUS_FILE_PATH")) {
    config.status_file_path = {*v, ConfigSource::EnvVar};
  }
  if (auto v = env("PTAPP_CRASH_ARTIFACT_DIR")) {
    config.crash_artifact_dir = {*v, ConfigSource::EnvVar};
  }
}

// ---- SerializeRuntimeConfig -------------------------------------------------

namespace detail {
static std::string FieldJson(std::string_view key, bool value, ConfigSource src) {
  std::ostringstream out;
  out << "    \"" << key << "\": { \"value\": " << (value ? "true" : "false")
      << ", \"source\": \"" << ConfigSourceName(src) << "\" }";
  return out.str();
}
static std::string FieldJson(std::string_view key, uint32_t value, ConfigSource src) {
  std::ostringstream out;
  out << "    \"" << key << "\": { \"value\": " << value
      << ", \"source\": \"" << ConfigSourceName(src) << "\" }";
  return out.str();
}
static std::string FieldJson(std::string_view key, const std::string& value, ConfigSource src) {
  std::ostringstream out;
  out << "    \"" << key << "\": { \"value\": \"" << EscapeJsonString(value)
      << "\", \"source\": \"" << ConfigSourceName(src) << "\" }";
  return out.str();
}
}  // namespace detail

std::string SerializeRuntimeConfig(const RuntimeConfig& c) {
  std::vector<std::string> fields;
  fields.push_back(detail::FieldJson("backend",              c.backend.value,              c.backend.source));
  fields.push_back(detail::FieldJson("scene_path",           c.scene_path.value,           c.scene_path.source));
  fields.push_back(detail::FieldJson("log_level",            c.log_level.value,            c.log_level.source));
  fields.push_back(detail::FieldJson("platform",             c.platform.value,             c.platform.source));
  fields.push_back(detail::FieldJson("headless",             c.headless.value,             c.headless.source));
  fields.push_back(detail::FieldJson("benchmark_mode",       c.benchmark_mode.value,       c.benchmark_mode.source));
  fields.push_back(detail::FieldJson("ui_present_hz",        c.ui_present_hz.value,        c.ui_present_hz.source));
  fields.push_back(detail::FieldJson("render_width",         c.render_width.value,         c.render_width.source));
  fields.push_back(detail::FieldJson("render_height",        c.render_height.value,        c.render_height.source));
  fields.push_back(detail::FieldJson("spp",                  c.spp.value,                  c.spp.source));
  fields.push_back(detail::FieldJson("max_depth",            c.max_depth.value,            c.max_depth.source));
  fields.push_back(detail::FieldJson("output_path",          c.output_path.value,          c.output_path.source));
  fields.push_back(detail::FieldJson("exr_output_path",      c.exr_output_path.value,      c.exr_output_path.source));
  fields.push_back(detail::FieldJson("benchmark_warmup_frames", c.benchmark_warmup_frames.value, c.benchmark_warmup_frames.source));
  fields.push_back(detail::FieldJson("benchmark_tile_size",  c.benchmark_tile_size.value,  c.benchmark_tile_size.source));
  fields.push_back(detail::FieldJson("write_status_file",    c.write_status_file.value,    c.write_status_file.source));
  fields.push_back(detail::FieldJson("status_file_path",     c.status_file_path.value,     c.status_file_path.source));
  fields.push_back(detail::FieldJson("crash_artifact_dir",   c.crash_artifact_dir.value,   c.crash_artifact_dir.source));
  fields.push_back("    \"engine_config\": " + SerializeEngineConfig(c));
  fields.push_back("    \"feature_flags\": " + SerializeFeatureFlags());
  fields.push_back("    \"build_info\": " + vkpt::build::SerializeBuildMetadata());

  std::ostringstream out;
  out << "{\n";
  for (size_t i = 0; i < fields.size(); ++i) {
    out << fields[i];
    if (i + 1 < fields.size()) out << ',';
    out << '\n';
  }
  out << "}";
  return out.str();
}

// ---- BuildDefaultConfig -----------------------------------------------------

RuntimeConfig BuildDefaultConfig(const std::string& config_file_path) {
  RuntimeConfig config;
  // Overlay config file if provided.
  if (!config_file_path.empty()) {
    std::vector<ParsedConfigEntry> entries;
    std::string error;
    if (ParseConfigFile(config_file_path, entries, &error)) {
      ApplyConfigEntries(entries, config);
    }
  }
  // Environment variables override config files and remain below CLI flags.
  ApplyEnvVars(config);
  return config;
}

}  // namespace vkpt::config
