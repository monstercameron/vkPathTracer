#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "platform/PlatformFactory.h"

namespace vkpt::app {

struct AppOptions {
  bool show_version = false;
  bool version_json = false;
  bool headless = false;
  bool crash_test = false;
  bool doctor = false;
  bool check_build = false;
  bool check_cpu = false;
  bool check_backends = false;
  bool check_assets = false;
  bool check_shaders = false;
  bool check_job_system = false;
  bool check_scene_schema = false;
  bool check_benchmark_artifact = false;
  bool dump_config = false;
  bool list_backends = false;
  bool list_accelerators = false;
  bool do_render = false;
  bool ui_model_smoke = false;
  bool ui_release_gate = false;
  bool dynamic_physics_gate = false;
  bool third_person_script_gate = false;
  bool qt_stress_gate = false;
  bool open_window = false;
  bool list_gpus = false;
  bool auto_exit_window = false;

  std::string config_file_path;
  std::string env_file_path = ".env";
  bool env_file_explicit = false;
  bool env_file_enabled = true;
  std::string scene_path;
  std::string backend;
  std::string audio_backend = "auto";
  std::string platform_name;
  std::string output_path = "artifacts/renders/cornell.png";
  std::string exr_output_path;
  std::string qt_stress_output = "artifacts/perf-qt-stress-gate";
  std::string qt_stress_scene;
  std::uint32_t qt_stress_phase_seconds = 5;
  std::string log_level = "info";
  std::uint32_t width = 320;
  std::uint32_t height = 240;
  std::uint32_t window_width = 1280;
  std::uint32_t window_height = 720;
  std::uint32_t window_frame_limit = 0;
  std::uint32_t spp = 16;
  std::uint32_t max_depth = 6;
  std::uint32_t render_frame = 0;
  std::uint32_t render_sequence_frames = 1;
  std::uint32_t render_fps = 24;
  std::optional<float> render_time_seconds;
  bool gpu_denoiser = false;
  bool temporal_aa = false;
  bool audio_mute = false;
  bool deterministic = false;
  bool snapshot_bus = true;
  std::optional<std::uint32_t> ui_present_hz;
  // Multi-GPU dispatch:
  //   --gpus N           => gpu_count = N (must be >= 1, 0 means "auto-detect")
  //   --include-integrated => include integrated/low-VRAM adapters in auto-pick
  std::optional<std::uint32_t> gpu_count;
  bool include_integrated_gpu = false;
  // Auto-enter Play runtime mode on Qt startup. Equivalent to pressing F1
  // once the editor window is visible. Used to drive ragdoll / animation /
  // scripted gameplay without manual UI interaction (CI smokes, screen
  // capture, demo launches). No effect on non-Qt platforms.
  bool start_in_play_mode = false;
};

struct AppOptionsParseResult {
  AppOptions options;
  bool ok = true;
  bool exit_requested = false;
  int exit_code = 0;
};

bool IsConsoleOptInArg(std::string_view token);
bool ShouldEnableOptionalConsole(int argc, char** argv);
AppOptionsParseResult ParseAppOptions(int argc, char** argv);

bool ParseUnsigned(std::string_view text, std::uint32_t& out);
const char* YesNo(bool value);
const char* JsonBool(bool value);
bool IsRawPlatformBuilt();
bool IsQtPlatformBuilt();
std::string QtSupportState();
std::string QtVersionString();
std::string QtPlatformShellString();
std::string_view WindowSystemName(vkpt::platform::RuntimePlatformKind platform);

void PrintUsage();
void PrintVersionText(vkpt::platform::RuntimePlatformKind platform_shell);
void PrintVersionJson(vkpt::platform::RuntimePlatformKind platform_shell);

}  // namespace vkpt::app
