#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/BuildInfo.h"
#include "core/Config.h"

namespace vkpt::config {

struct FeatureFlags {
  bool vulkan = false;
  bool d3d12 = false;
  bool metal = false;
  bool webgpu = false;
  bool opengl_experimental = false;
  bool cpu_raytracer = false;
  bool cpu_simd = false;
  bool avx = false;
  bool avx2 = false;
  bool avx512 = false;
  bool neon = false;
  bool sve = false;
  bool jolt = false;
  bool lua = false;
  bool audio = false;
  bool editor = false;
  bool benchmark = false;
  bool profiling = false;
  bool sanitizers = false;
  bool strict_determinism = false;
  bool qt = false;
  bool qt_editor = false;
  bool qt_rhi = false;
  bool raw_desktop = false;
  std::string sanitizer_flavor;
  std::string sanitizer_mode;
  std::string platform_shells;
  std::string qt_version;
  std::vector<std::string> enabled;
  std::vector<std::string> disabled;
};

struct EngineConfig {
  ConfigValue<std::string> backend;
  ConfigValue<std::string> scene_path;
  ConfigValue<std::string> log_level;
  ConfigValue<std::string> platform;
  ConfigValue<bool> headless;
  ConfigValue<bool> benchmark_mode;
  ConfigValue<uint32_t> render_width;
  ConfigValue<uint32_t> render_height;
  ConfigValue<uint32_t> spp;
  ConfigValue<uint32_t> max_depth;
  ConfigValue<std::string> output_path;
  ConfigValue<std::string> exr_output_path;
  ConfigValue<uint32_t> benchmark_warmup_frames;
  ConfigValue<uint32_t> benchmark_tile_size;
  ConfigValue<bool> write_status_file;
  ConfigValue<std::string> status_file_path;
  ConfigValue<std::string> crash_artifact_dir;
};

inline FeatureFlags FeatureFlagsFromBuildInfo(const vkpt::build::BuildMetadata& metadata =
                                                  vkpt::build::GetBuildMetadata()) {
  const auto enabled = [&](std::string_view feature) {
    return vkpt::build::FeatureListContains(metadata.enabled_features, feature);
  };

  FeatureFlags out;
  out.vulkan = enabled("PT_ENABLE_VULKAN");
  out.d3d12 = enabled("PT_ENABLE_D3D12");
  out.metal = enabled("PT_ENABLE_METAL");
  out.webgpu = enabled("PT_ENABLE_WEBGPU");
  out.opengl_experimental = enabled("PT_ENABLE_OPENGL_EXPERIMENTAL");
  out.cpu_raytracer = enabled("PT_ENABLE_CPU_RAYTRACER");
  out.cpu_simd = enabled("PT_ENABLE_CPU_SIMD");
  out.avx = enabled("PT_ENABLE_AVX");
  out.avx2 = enabled("PT_ENABLE_AVX2");
  out.avx512 = enabled("PT_ENABLE_AVX512");
  out.neon = enabled("PT_ENABLE_NEON");
  out.sve = enabled("PT_ENABLE_SVE");
  out.jolt = enabled("PT_ENABLE_JOLT");
  out.lua = enabled("PT_ENABLE_LUA");
  out.audio = enabled("PT_ENABLE_AUDIO");
  out.editor = enabled("PT_ENABLE_EDITOR");
  out.benchmark = enabled("PT_ENABLE_BENCHMARK");
  out.profiling = enabled("PT_ENABLE_PROFILING");
  out.sanitizers = metadata.sanitizers_enabled;
  out.strict_determinism = metadata.strict_determinism;
  out.qt = metadata.qt_enabled;
  out.qt_editor = metadata.qt_editor_enabled;
  out.qt_rhi = metadata.qt_rhi_enabled;
  out.raw_desktop = enabled("PT_ENABLE_RAW_DESKTOP");
  out.sanitizer_flavor = metadata.sanitizer_flavor;
  out.sanitizer_mode = metadata.sanitizer_mode;
  out.platform_shells = metadata.platform_shells;
  out.qt_version = metadata.qt_version;
  out.enabled = metadata.enabled_features;
  out.disabled = metadata.disabled_features;
  return out;
}

inline EngineConfig EngineConfigFromRuntimeConfig(const RuntimeConfig& runtime) {
  EngineConfig out;
  out.backend = runtime.backend;
  out.scene_path = runtime.scene_path;
  out.log_level = runtime.log_level;
  out.platform = runtime.platform;
  out.headless = runtime.headless;
  out.benchmark_mode = runtime.benchmark_mode;
  out.render_width = runtime.render_width;
  out.render_height = runtime.render_height;
  out.spp = runtime.spp;
  out.max_depth = runtime.max_depth;
  out.output_path = runtime.output_path;
  out.exr_output_path = runtime.exr_output_path;
  out.benchmark_warmup_frames = runtime.benchmark_warmup_frames;
  out.benchmark_tile_size = runtime.benchmark_tile_size;
  out.write_status_file = runtime.write_status_file;
  out.status_file_path = runtime.status_file_path;
  out.crash_artifact_dir = runtime.crash_artifact_dir;
  return out;
}

namespace detail {

inline std::string ContractFieldJson(std::string_view key, bool value, ConfigSource src) {
  std::ostringstream out;
  out << "      \"" << key << "\": { \"value\": " << (value ? "true" : "false")
      << ", \"source\": \"" << ConfigSourceName(src) << "\" }";
  return out.str();
}

inline std::string ContractFieldJson(std::string_view key, uint32_t value, ConfigSource src) {
  std::ostringstream out;
  out << "      \"" << key << "\": { \"value\": " << value
      << ", \"source\": \"" << ConfigSourceName(src) << "\" }";
  return out.str();
}

inline std::string ContractFieldJson(std::string_view key,
                                     const std::string& value,
                                     ConfigSource src) {
  std::ostringstream out;
  out << "      \"" << key << "\": { \"value\": \"" << vkpt::build::EscapeJson(value)
      << "\", \"source\": \"" << ConfigSourceName(src) << "\" }";
  return out.str();
}

inline void AppendFields(std::ostringstream& out, const std::vector<std::string>& fields) {
  for (std::size_t i = 0; i < fields.size(); ++i) {
    out << fields[i];
    if (i + 1 < fields.size()) {
      out << ',';
    }
    out << '\n';
  }
}

inline void AppendFlag(std::ostringstream& out, std::string_view key, bool value, bool comma) {
  out << "    \"" << key << "\": " << (value ? "true" : "false");
  if (comma) {
    out << ',';
  }
  out << '\n';
}

}  // namespace detail

inline std::string SerializeFeatureFlags(const FeatureFlags& flags) {
  std::ostringstream out;
  out << "{\n";
  detail::AppendFlag(out, "vulkan", flags.vulkan, true);
  detail::AppendFlag(out, "d3d12", flags.d3d12, true);
  detail::AppendFlag(out, "metal", flags.metal, true);
  detail::AppendFlag(out, "webgpu", flags.webgpu, true);
  detail::AppendFlag(out, "opengl_experimental", flags.opengl_experimental, true);
  detail::AppendFlag(out, "cpu_raytracer", flags.cpu_raytracer, true);
  detail::AppendFlag(out, "cpu_simd", flags.cpu_simd, true);
  detail::AppendFlag(out, "avx", flags.avx, true);
  detail::AppendFlag(out, "avx2", flags.avx2, true);
  detail::AppendFlag(out, "avx512", flags.avx512, true);
  detail::AppendFlag(out, "neon", flags.neon, true);
  detail::AppendFlag(out, "sve", flags.sve, true);
  detail::AppendFlag(out, "jolt", flags.jolt, true);
  detail::AppendFlag(out, "lua", flags.lua, true);
  detail::AppendFlag(out, "audio", flags.audio, true);
  detail::AppendFlag(out, "editor", flags.editor, true);
  detail::AppendFlag(out, "benchmark", flags.benchmark, true);
  detail::AppendFlag(out, "profiling", flags.profiling, true);
  detail::AppendFlag(out, "sanitizers", flags.sanitizers, true);
  detail::AppendFlag(out, "strict_determinism", flags.strict_determinism, true);
  detail::AppendFlag(out, "qt", flags.qt, true);
  detail::AppendFlag(out, "qt_editor", flags.qt_editor, true);
  detail::AppendFlag(out, "qt_rhi", flags.qt_rhi, true);
  detail::AppendFlag(out, "raw_desktop", flags.raw_desktop, true);
  out << "    \"sanitizer_flavor\": \"" << vkpt::build::EscapeJson(flags.sanitizer_flavor) << "\",\n";
  out << "    \"sanitizer_mode\": \"" << vkpt::build::EscapeJson(flags.sanitizer_mode) << "\",\n";
  out << "    \"platform_shells\": \"" << vkpt::build::EscapeJson(flags.platform_shells) << "\",\n";
  out << "    \"qt_version\": \"" << vkpt::build::EscapeJson(flags.qt_version) << "\",\n";
  out << "    \"enabled\": ";
  vkpt::build::AppendStringArrayJson(out, flags.enabled);
  out << ",\n";
  out << "    \"disabled\": ";
  vkpt::build::AppendStringArrayJson(out, flags.disabled);
  out << '\n';
  out << "  }";
  return out.str();
}

inline std::string SerializeFeatureFlags() {
  return SerializeFeatureFlags(FeatureFlagsFromBuildInfo());
}

inline std::string SerializeEngineConfig(const EngineConfig& config) {
  std::ostringstream out;
  out << "{\n";

  out << "    \"application\": {\n";
  detail::AppendFields(out, {
      detail::ContractFieldJson("backend", config.backend.value, config.backend.source),
      detail::ContractFieldJson("scene_path", config.scene_path.value, config.scene_path.source),
      detail::ContractFieldJson("log_level", config.log_level.value, config.log_level.source),
      detail::ContractFieldJson("platform", config.platform.value, config.platform.source),
      detail::ContractFieldJson("headless", config.headless.value, config.headless.source),
      detail::ContractFieldJson("benchmark_mode", config.benchmark_mode.value, config.benchmark_mode.source),
  });
  out << "    },\n";

  out << "    \"render\": {\n";
  detail::AppendFields(out, {
      detail::ContractFieldJson("render_width", config.render_width.value, config.render_width.source),
      detail::ContractFieldJson("render_height", config.render_height.value, config.render_height.source),
      detail::ContractFieldJson("spp", config.spp.value, config.spp.source),
      detail::ContractFieldJson("max_depth", config.max_depth.value, config.max_depth.source),
      detail::ContractFieldJson("output_path", config.output_path.value, config.output_path.source),
      detail::ContractFieldJson("exr_output_path", config.exr_output_path.value, config.exr_output_path.source),
  });
  out << "    },\n";

  out << "    \"benchmark\": {\n";
  detail::AppendFields(out, {
      detail::ContractFieldJson("benchmark_warmup_frames",
                                config.benchmark_warmup_frames.value,
                                config.benchmark_warmup_frames.source),
      detail::ContractFieldJson("benchmark_tile_size",
                                config.benchmark_tile_size.value,
                                config.benchmark_tile_size.source),
  });
  out << "    },\n";

  out << "    \"diagnostics\": {\n";
  detail::AppendFields(out, {
      detail::ContractFieldJson("write_status_file",
                                config.write_status_file.value,
                                config.write_status_file.source),
      detail::ContractFieldJson("status_file_path",
                                config.status_file_path.value,
                                config.status_file_path.source),
      detail::ContractFieldJson("crash_artifact_dir",
                                config.crash_artifact_dir.value,
                                config.crash_artifact_dir.source),
  });
  out << "    }\n";
  out << "  }";
  return out.str();
}

inline std::string SerializeEngineConfig(const RuntimeConfig& runtime) {
  return SerializeEngineConfig(EngineConfigFromRuntimeConfig(runtime));
}

}  // namespace vkpt::config
