#include "app/AppOptions.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "build_info.generated.h"
#include "core/Config.h"
#include "core/Logging.h"

#ifdef PT_ENABLE_QT
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>
#endif

namespace vkpt::app {

bool IsConsoleOptInArg(std::string_view token) {
  return token == "--console" || token == "--terminal";
}

bool ShouldEnableOptionalConsole(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] != nullptr && IsConsoleOptInArg(argv[i])) {
      return true;
    }
  }
  return false;
}

bool ParseUnsigned(std::string_view text, std::uint32_t& out) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() ||
      parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() ||
      value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool ParseFloat(std::string_view text, float& out) {
  if (text.empty()) {
    return false;
  }
  float value = 0.0f;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    return false;
  }
  out = value;
  return std::isfinite(out);
}

const char* YesNo(bool value) {
  return value ? "yes" : "no";
}

const char* JsonBool(bool value) {
  return value ? "true" : "false";
}

bool IsRawPlatformBuilt() {
  return vkpt::platform::IsPlatformBuilt(vkpt::platform::RuntimePlatformKind::Raw);
}

bool IsQtPlatformBuilt() {
  return vkpt::platform::IsPlatformBuilt(vkpt::platform::RuntimePlatformKind::Qt);
}

std::string QtSupportState() {
#if defined(PT_ENABLE_QT)
  return "enabled";
#else
  return "disabled";
#endif
}

std::string QtVersionString() {
#if defined(PT_ENABLE_QT)
  return qVersion();
#else
  return "unavailable";
#endif
}

std::string QtPlatformShellString() {
#if defined(PT_ENABLE_QT)
  if (QGuiApplication::instance() == nullptr) {
    return "not-initialized";
  }
  const std::string shell = QGuiApplication::platformName().toStdString();
  return shell.empty() ? "unknown" : shell;
#else
  return "unavailable";
#endif
}

std::string_view WindowSystemName(vkpt::platform::RuntimePlatformKind platform) {
  switch (platform) {
    case vkpt::platform::RuntimePlatformKind::Raw:
      return "raw_native";
    case vkpt::platform::RuntimePlatformKind::Qt:
      return "qt_widgets";
    case vkpt::platform::RuntimePlatformKind::Headless:
      return "headless";
    case vkpt::platform::RuntimePlatformKind::Auto:
    default:
      return "auto";
  }
}

std::string PlatformSupportSummary(vkpt::platform::RuntimePlatformKind platform) {
  const auto support = vkpt::platform::DescribeRuntimePlatform(platform);
  if (!support.built) {
    return "disabled";
  }
  if (support.available) {
    return std::string("available:") + support.implementation;
  }
  if (support.stub) {
    return std::string("stub:") + support.implementation;
  }
  return "unavailable";
}

void PrintUsage() {
  std::cout << "ptapp [options]\n";
  std::cout << "  --version             Print build metadata and exit\n";
  std::cout << "  --version --json      Print build metadata as JSON\n";
  std::cout << "  --doctor              Run full self-diagnostics (ptdoctor)\n";
  std::cout << "  --check-build         Check build metadata\n";
  std::cout << "  --check-cpu           Check CPU capabilities\n";
  std::cout << "  --check-backends      Check render backends\n";
  std::cout << "  --check-assets        Check asset directories\n";
  std::cout << "  --check-shaders       Check shader directories\n";
  std::cout << "  --check-job-system    Check job system smoke test\n";
  std::cout << "  --check-scene-schema  Check scene schema (cornell_native.json)\n";
  std::cout << "  --check-bench-write   Check benchmark artifact write\n";
  std::cout << "  --dump-config         Print resolved runtime config as JSON\n";
  std::cout << "  --config <path>       Load a config file (key=value format)\n";
  std::cout << "  --env-file <path>     Load .env variables before config/env resolution\n";
  std::cout << "  --no-env-file         Do not auto-load .env from the working directory\n";
  std::cout << "  --list-backends       Print known render backends and capabilities\n";
  std::cout << "  --list-accelerators   Print D3D12/CPU accelerator capability and ray budget plan\n";
  std::cout << "  --list-gpus           Enumerate Vulkan physical devices and select the best\n";
  std::cout << "  --headless            Initialize headless platform\n";
  std::cout << "  --window              Open desktop window and keep app running\n";
  std::cout << "  --console, --terminal Attach or create a console for GUI diagnostics\n";
  std::cout << "  --platform <name>     Select platform: auto|raw|qt|headless\n";
  std::cout << "  --window-width <px>   Window width (default 1280)\n";
  std::cout << "  --window-height <px>  Window height (default 720)\n";
  std::cout << "  --ui-present-hz <hz>  Preview present rate (1..120, default 30)\n";
  std::cout << "  --frames <n>          Exit window mode after n frames (GUI smoke)\n";
  std::cout << "  --exit                Exit window mode after one frame unless --frames is set\n";
  std::cout << "  --scene <path>        Set startup scene\n";
  std::cout << "  --backend <name>      Select backend\n";
  std::cout << "  --log-level <n>       Select log level\n";
  std::cout << "  --crash-test          Simulate a crash and write crash artifacts\n";
  std::cout << "  --ui-model-smoke      Run headless UI model smoke checks\n";
  std::cout << "  --ui-release-gate     Print UI release-gate evidence and deferred gaps\n";
  std::cout << "  --dynamic-physics-gate  Run D3D12 dynamic physics transform-update performance gate\n";
  std::cout << "  --third-person-script-gate  Emit static-mode script gate artifact\n";
  std::cout << "  --render              Render using scalar CPU path tracer\n";
  std::cout << "  --output <path>       Render output PNG path\n";
  std::cout << "  --exr-output <path>   Render output EXR path\n";
  std::cout << "  --width <px>          Render width\n";
  std::cout << "  --height <px>         Render height\n";
  std::cout << "  --spp <samples>       Samples per pixel\n";
  std::cout << "  --max-depth <depth>   Max ray depth\n";
  std::cout << "  --render-frame <n>    Select numbered render frame for still output\n";
  std::cout << "  --render-time <sec>   Attach render time metadata for still output\n";
  std::cout << "  --render-sequence <n> Write n numbered static frames from --output\n";
  std::cout << "  --render-fps <fps>    Frame rate metadata for render sequences\n";
  std::cout << "  --denoiser            Enable GPU denoiser for D3D12 renders\n";
  std::cout << "  --temporal-aa         Enable temporal reuse for D3D12 renders\n";
}

void PrintVersionText(vkpt::platform::RuntimePlatformKind platform_shell) {
  std::cout << "ptapp " << vkpt::build::kProjectVersion << '\n';
  std::cout << "git: " << vkpt::build::kGitHash << '\n';
  std::cout << "build date: " << vkpt::build::kBuildDate << '\n';
  std::cout << "compiler: " << vkpt::build::kCompilerName << ' '
            << vkpt::build::kCompilerVersion << '\n';
  std::cout << "target: " << vkpt::build::kTargetOs << '/'
            << vkpt::build::kTargetArch << '\n';
  std::cout << "host platform: "
            << vkpt::platform::HostPlatformName(vkpt::platform::HostPlatform()) << '\n';
  std::cout << "build type: " << vkpt::build::kBuildType << '\n';
  std::cout << "features: " << vkpt::build::kEnabledFeatureFlags << '\n';
  std::cout << "platform shell: "
            << vkpt::platform::RuntimePlatformKindName(platform_shell) << '\n';
  std::cout << "window system: " << WindowSystemName(platform_shell) << '\n';
  std::cout << "platforms: headless=available raw="
            << PlatformSupportSummary(vkpt::platform::RuntimePlatformKind::Raw)
            << " qt=" << PlatformSupportSummary(vkpt::platform::RuntimePlatformKind::Qt) << '\n';
  std::cout << "qt: " << QtSupportState()
            << " version=" << QtVersionString()
            << " platform_shell=" << QtPlatformShellString() << '\n';
}

void PrintVersionJson(vkpt::platform::RuntimePlatformKind platform_shell) {
  std::cout << "{\n";
  std::cout << "  \"app\": \"ptapp\",\n";
  std::cout << "  \"version\": \"" << vkpt::log::EscapeJson(vkpt::build::kProjectVersion) << "\",\n";
  std::cout << "  \"git_hash\": \"" << vkpt::log::EscapeJson(vkpt::build::kGitHash) << "\",\n";
  std::cout << "  \"build_date\": \"" << vkpt::log::EscapeJson(vkpt::build::kBuildDate) << "\",\n";
  std::cout << "  \"compiler\": \""
            << vkpt::log::EscapeJson(std::string(vkpt::build::kCompilerName) + " " +
                                      std::string(vkpt::build::kCompilerVersion))
            << "\",\n";
  std::cout << "  \"cpp_standard\": \"" << vkpt::log::EscapeJson(vkpt::build::kCxxStandard) << "\",\n";
  std::cout << "  \"target\": \""
            << vkpt::log::EscapeJson(std::string(vkpt::build::kTargetOs) + "/" +
                                      std::string(vkpt::build::kTargetArch))
            << "\",\n";
  std::cout << "  \"host_platform\": \""
            << vkpt::log::EscapeJson(vkpt::platform::HostPlatformName(vkpt::platform::HostPlatform()))
            << "\",\n";
  std::cout << "  \"build_type\": \"" << vkpt::log::EscapeJson(vkpt::build::kBuildType) << "\",\n";
  std::cout << "  \"simd_compile_options\": \""
            << vkpt::log::EscapeJson(vkpt::build::kSimdCompileOptions) << "\",\n";
  std::cout << "  \"backend_compile_options\": \""
            << vkpt::log::EscapeJson(vkpt::build::kBackendCompileOptions) << "\",\n";
  std::cout << "  \"enabled_features\": [\""
            << vkpt::log::EscapeJson(vkpt::build::kEnabledFeatureFlags) << "\"],\n";
  std::cout << "  \"disabled_features\": [\""
            << vkpt::log::EscapeJson(vkpt::build::kDisabledFeatureFlags) << "\"],\n";
  std::cout << "  \"ui\": {\n";
  std::cout << "    \"platform_shell\": \""
            << vkpt::log::EscapeJson(vkpt::platform::RuntimePlatformKindName(platform_shell))
            << "\",\n";
  std::cout << "    \"window_system\": \""
            << vkpt::log::EscapeJson(WindowSystemName(platform_shell)) << "\",\n";
  std::cout << "    \"built_platforms\": {\n";
  std::cout << "      \"headless\": true,\n";
  std::cout << "      \"raw\": " << JsonBool(IsRawPlatformBuilt()) << ",\n";
  std::cout << "      \"qt\": " << JsonBool(IsQtPlatformBuilt()) << "\n";
  std::cout << "    },\n";
  std::cout << "    \"platform_support\": {\n";
  for (const auto& support : vkpt::platform::DescribeRuntimePlatforms()) {
    std::cout << "      \"" << vkpt::log::EscapeJson(support.name) << "\": {"
              << "\"built\": " << JsonBool(support.built)
              << ", \"available\": " << JsonBool(support.available)
              << ", \"stub\": " << JsonBool(support.stub)
              << ", \"implementation\": \"" << vkpt::log::EscapeJson(support.implementation) << "\""
              << ", \"reason\": \"" << vkpt::log::EscapeJson(support.unavailable_reason) << "\"}";
    if (support.kind != vkpt::platform::RuntimePlatformKind::Qt) {
      std::cout << ",";
    }
    std::cout << "\n";
  }
  std::cout << "    },\n";
  std::cout << "    \"qt\": {\n";
  std::cout << "      \"supported\": " << JsonBool(IsQtPlatformBuilt()) << ",\n";
  std::cout << "      \"support\": \"" << QtSupportState() << "\",\n";
  std::cout << "      \"version\": \"" << vkpt::log::EscapeJson(QtVersionString()) << "\",\n";
  std::cout << "      \"platform_shell\": \""
            << vkpt::log::EscapeJson(QtPlatformShellString()) << "\"\n";
  std::cout << "    }\n";
  std::cout << "  }\n";
  std::cout << "}\n";
}

namespace {

AppOptionsParseResult ParseError(const char* message) {
  std::cerr << message << "\n";
  return AppOptionsParseResult{{}, false, true, 1};
}

bool HasValue(const std::vector<std::string_view>& args, std::size_t index) {
  return index + 1u < args.size();
}

}  // namespace

AppOptionsParseResult ParseAppOptions(int argc, char** argv) {
  AppOptionsParseResult result;
  auto& options = result.options;
  const std::vector<std::string_view> args(argv, argv + argc);

  // Keep parsing single-pass because options like --frames and --env-file
  // consume the next token and must report the specific flag that failed.
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto token = args[i];
    if (token == "--version") {
      options.show_version = true;
    } else if (token == "--json") {
      options.version_json = true;
    } else if (token == "--doctor") {
      options.doctor = true;
      options.check_build = true;
      options.check_cpu = true;
      options.check_backends = true;
      options.check_assets = true;
      options.check_shaders = true;
      options.check_job_system = true;
      options.check_scene_schema = true;
      options.check_benchmark_artifact = true;
    } else if (token == "--check-build") {
      options.check_build = true;
    } else if (token == "--check-cpu") {
      options.check_cpu = true;
    } else if (token == "--check-backends") {
      options.check_backends = true;
    } else if (token == "--check-assets") {
      options.check_assets = true;
    } else if (token == "--check-shaders") {
      options.check_shaders = true;
    } else if (token == "--check-job-system") {
      options.check_job_system = true;
    } else if (token == "--check-scene-schema") {
      options.check_scene_schema = true;
    } else if (token == "--check-bench-write") {
      options.check_benchmark_artifact = true;
    } else if (token == "--dump-config") {
      options.dump_config = true;
    } else if (token == "--list-backends") {
      options.list_backends = true;
    } else if (token == "--list-accelerators") {
      options.list_accelerators = true;
    } else if (token == "--headless") {
      options.headless = true;
    } else if (token == "--render") {
      options.do_render = true;
    } else if (token == "--window") {
      options.open_window = true;
    } else if (IsConsoleOptInArg(token)) {
      // Console attachment is handled before logging initialization.
    } else if (token == "--denoiser") {
      options.gpu_denoiser = true;
    } else if (token == "--temporal-aa") {
      options.temporal_aa = true;
    } else if (token == "--list-gpus") {
      options.list_gpus = true;
    } else if (token == "--crash-test") {
      options.crash_test = true;
    } else if (token == "--ui-model-smoke") {
      options.ui_model_smoke = true;
    } else if (token == "--ui-release-gate") {
      options.ui_release_gate = true;
    } else if (token == "--dynamic-physics-gate") {
      options.dynamic_physics_gate = true;
    } else if (token == "--third-person-script-gate") {
      options.third_person_script_gate = true;
    } else if (token == "--exit") {
      options.auto_exit_window = true;
    } else if (token == "--config") {
      if (!HasValue(args, i)) return ParseError("missing value for --config");
      options.config_file_path = std::string(args[++i]);
    } else if (token == "--env-file") {
      if (!HasValue(args, i)) return ParseError("missing value for --env-file");
      options.env_file_path = std::string(args[++i]);
      options.env_file_explicit = true;
      options.env_file_enabled = true;
    } else if (token == "--no-env-file") {
      options.env_file_enabled = false;
    } else if (token == "--scene") {
      if (!HasValue(args, i)) return ParseError("missing value for --scene");
      options.scene_path = std::string(args[++i]);
    } else if (token == "--backend") {
      if (!HasValue(args, i)) return ParseError("missing value for --backend");
      options.backend = std::string(args[++i]);
    } else if (token == "--platform") {
      if (!HasValue(args, i)) return ParseError("missing value for --platform");
      options.platform_name = std::string(args[++i]);
    } else if (token == "--log-level") {
      if (!HasValue(args, i)) return ParseError("missing value for --log-level");
      options.log_level = std::string(args[++i]);
    } else if (token == "--output") {
      if (!HasValue(args, i)) return ParseError("missing value for --output");
      options.output_path = std::string(args[++i]);
    } else if (token == "--exr-output") {
      if (!HasValue(args, i)) return ParseError("missing value for --exr-output");
      options.exr_output_path = std::string(args[++i]);
    } else if (token == "--width") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.width)) {
        return ParseError("invalid value for --width");
      }
    } else if (token == "--height") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.height)) {
        return ParseError("invalid value for --height");
      }
    } else if (token == "--window-width") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.window_width)) {
        return ParseError("invalid value for --window-width");
      }
    } else if (token == "--window-height") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.window_height)) {
        return ParseError("invalid value for --window-height");
      }
    } else if (token == "--ui-present-hz") {
      std::uint32_t parsed_ui_present_hz = 0;
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], parsed_ui_present_hz)) {
        return ParseError("invalid value for --ui-present-hz");
      }
      options.ui_present_hz = vkpt::config::ClampUiPresentHz(parsed_ui_present_hz);
    } else if (token == "--frames") {
      if (!HasValue(args, i) ||
          !ParseUnsigned(args[++i], options.window_frame_limit) ||
          options.window_frame_limit == 0u) {
        return ParseError("invalid value for --frames");
      }
    } else if (token == "--spp") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.spp)) {
        return ParseError("invalid value for --spp");
      }
    } else if (token == "--max-depth") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.max_depth)) {
        return ParseError("invalid value for --max-depth");
      }
    } else if (token == "--render-frame") {
      if (!HasValue(args, i) || !ParseUnsigned(args[++i], options.render_frame)) {
        return ParseError("invalid value for --render-frame");
      }
    } else if (token == "--render-time") {
      float parsedTime = 0.0f;
      if (!HasValue(args, i) || !ParseFloat(args[++i], parsedTime) || parsedTime < 0.0f) {
        return ParseError("invalid value for --render-time");
      }
      options.render_time_seconds = parsedTime;
    } else if (token == "--render-sequence") {
      if (!HasValue(args, i) ||
          !ParseUnsigned(args[++i], options.render_sequence_frames) ||
          options.render_sequence_frames == 0u) {
        return ParseError("invalid value for --render-sequence");
      }
    } else if (token == "--render-fps") {
      if (!HasValue(args, i) ||
          !ParseUnsigned(args[++i], options.render_fps) ||
          options.render_fps == 0u) {
        return ParseError("invalid value for --render-fps");
      }
    } else if (token == "--help" || token == "-h") {
      PrintUsage();
      result.exit_requested = true;
      result.exit_code = 0;
      return result;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      PrintUsage();
      return AppOptionsParseResult{{}, false, true, 1};
    }
  }

  // Cross-option validation lives after parsing so aliases and defaulted values
  // are resolved before deciding whether the requested mode is coherent.
  if (options.open_window && options.do_render) {
    std::cerr << "--window and --render are mutually exclusive; use --window for the interactive preview or --render for offscreen output\n";
    return AppOptionsParseResult{{}, false, true, 1};
  }
  if ((options.auto_exit_window || options.window_frame_limit != 0u) && !options.open_window) {
    std::cerr << "--exit and --frames are only valid with --window\n";
    return AppOptionsParseResult{{}, false, true, 1};
  }
  if (options.auto_exit_window && options.window_frame_limit == 0u) {
    options.window_frame_limit = 1u;
  }

  return result;
}

}  // namespace vkpt::app
