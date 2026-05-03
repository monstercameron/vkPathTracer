#include "core/Config.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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

static uint32_t ParseU32(const std::string& value) {
  try { return static_cast<uint32_t>(std::stoul(value)); } catch (...) { return 0; }
}

void ApplyConfigEntries(const std::vector<ParsedConfigEntry>& entries,
                        RuntimeConfig& config) {
  for (const auto& e : entries) {
    if      (e.key == "backend")              { config.backend            = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "scene_path")           { config.scene_path         = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "log_level")            { config.log_level          = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "headless")             { config.headless           = {ParseBool(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "benchmark_mode")       { config.benchmark_mode     = {ParseBool(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "render_width")         { config.render_width       = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "render_height")        { config.render_height      = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "spp")                  { config.spp                = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "max_depth")            { config.max_depth          = {ParseU32(e.value), ConfigSource::ConfigFile}; }
    else if (e.key == "output_path")          { config.output_path        = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "exr_output_path")      { config.exr_output_path    = {e.value, ConfigSource::ConfigFile}; }
    else if (e.key == "benchmark_warmup")     { config.benchmark_warmup_frames = {ParseU32(e.value), ConfigSource::ConfigFile}; }
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
    const char* v = std::getenv(name);
    if (v && *v) return std::string(v);
    return std::nullopt;
  };

  if (auto v = env("PTAPP_BACKEND"))    { config.backend   = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_LOG_LEVEL"))  { config.log_level = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_SCENE"))      { config.scene_path = {*v, ConfigSource::EnvVar}; }
  if (auto v = env("PTAPP_HEADLESS"))   { config.headless  = {*v == "1" || *v == "true", ConfigSource::EnvVar}; }
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
  fields.push_back(detail::FieldJson("headless",             c.headless.value,             c.headless.source));
  fields.push_back(detail::FieldJson("benchmark_mode",       c.benchmark_mode.value,       c.benchmark_mode.source));
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
  // Overlay env vars first (lower priority than CLI but higher than file).
  ApplyEnvVars(config);
  // Overlay config file if provided.
  if (!config_file_path.empty()) {
    std::vector<ParsedConfigEntry> entries;
    std::string error;
    if (ParseConfigFile(config_file_path, entries, &error)) {
      ApplyConfigEntries(entries, config);
    }
  }
  return config;
}

}  // namespace vkpt::config
