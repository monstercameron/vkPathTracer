#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vkpt::config {

inline constexpr uint32_t kMinUiPresentHz = 1;
inline constexpr uint32_t kDefaultUiPresentHz = 60;
inline constexpr uint32_t kMaxUiPresentHz = 120;

inline uint32_t ClampUiPresentHz(uint32_t value) {
  return value < kMinUiPresentHz ? kMinUiPresentHz
       : value > kMaxUiPresentHz ? kMaxUiPresentHz
                                  : value;
}

// ---- RuntimeConfig ----------------------------------------------------------
// Resolved configuration that controls the entire run.  Every field has a
// clear source (default / config-file / CLI flag / env-var), so the system
// can explain exactly which value was chosen and why.

enum class ConfigSource : uint8_t {
  Default,
  ConfigFile,
  EnvVar,
  CliFlag
};

inline const char* ConfigSourceName(ConfigSource src) {
  switch (src) {
    case ConfigSource::Default:    return "default";
    case ConfigSource::ConfigFile: return "config_file";
    case ConfigSource::EnvVar:     return "env_var";
    case ConfigSource::CliFlag:    return "cli_flag";
  }
  return "unknown";
}

template <typename T>
struct ConfigValue {
  T value{};
  ConfigSource source = ConfigSource::Default;
};

struct RuntimeConfig {
  // --- Application mode ---
  ConfigValue<std::string>  backend{"auto"};
  ConfigValue<std::string>  scene_path{""};
  ConfigValue<std::string>  log_level{"info"};
  ConfigValue<std::string>  platform{"auto"};
  ConfigValue<bool>         headless{false};
  ConfigValue<bool>         benchmark_mode{false};
  ConfigValue<uint32_t>     ui_present_hz{kDefaultUiPresentHz};

  // --- Render settings ---
  ConfigValue<uint32_t>     render_width{320};
  ConfigValue<uint32_t>     render_height{240};
  ConfigValue<uint32_t>     spp{16};
  ConfigValue<uint32_t>     max_depth{6};
  ConfigValue<std::string>  output_path{"artifacts/renders/output.png"};
  ConfigValue<std::string>  exr_output_path{""};
  // Number of GPU path tracers to spin up. 1 = legacy single-GPU fast path.
  // 0 means "all viable adapters" (auto-detect). N >= 2 enables multi-GPU
  // dispatch via TileScheduler row striping.
  ConfigValue<uint32_t>     gpu_count{1u};
  // When auto-detecting (--gpus 0 or N greater than 1), allow integrated /
  // low-VRAM adapters that would normally be skipped.
  ConfigValue<bool>         include_integrated_gpu{false};

  // --- Benchmark ---
  ConfigValue<uint32_t>     benchmark_warmup_frames{2};
  ConfigValue<uint32_t>     benchmark_tile_size{32};

  // --- Diagnostics ---
  ConfigValue<bool>         write_status_file{true};
  ConfigValue<std::string>  status_file_path{"artifacts/status/latest_status.json"};
  ConfigValue<std::string>  crash_artifact_dir{"artifacts/crashes"};
};

// ---- Config file format (simple key=value, # comments) ---------------------

struct ParsedConfigEntry {
  std::string key;
  std::string value;
  int line = 0;
};

// Parse a simple "key = value" config file.  Returns false on IO error.
bool ParseConfigFile(const std::string& path,
                     std::vector<ParsedConfigEntry>& out_entries,
                     std::string* error = nullptr);

// Load a .env-style key=value file into the process environment. Existing
// environment variables are preserved unless override_existing is true.
bool LoadDotEnvFile(const std::string& path,
                    bool override_existing = false,
                    std::string* error = nullptr);

// Apply parsed entries to a RuntimeConfig; warns on unknown keys.
void ApplyConfigEntries(const std::vector<ParsedConfigEntry>& entries,
                        RuntimeConfig& config);

// Probe PTAPP_* environment variables. These override config-file values.
void ApplyEnvVars(RuntimeConfig& config);

// Serialise the resolved config to a JSON string for --dump-config.
std::string SerializeRuntimeConfig(const RuntimeConfig& config);

// Build a RuntimeConfig with precedence: defaults < config file < env.
// Callers apply CLI flags afterward as the final override layer.
RuntimeConfig BuildDefaultConfig(const std::string& config_file_path = "");

}  // namespace vkpt::config
