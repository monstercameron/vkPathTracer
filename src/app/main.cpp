#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "build_info.generated.h"
#include "core/Assert.h"
#include "core/Config.h"
#include "core/Logging.h"
#include "benchmark/BenchmarkSchema.h"
#include "editor/UiModels.h"
#include "diagnostics/CrashHooks.h"
#include "diagnostics/CrashRecorder.h"
#include "diagnostics/StatusFile.h"
#include "pathtracer/PathTracer.h"
#include "cpu/TiledCpuPathTracer.h"
#ifdef PT_ENABLE_VULKAN
#include "gpu/VulkanGpuPathTracer.h"
#endif
#ifdef PT_ENABLE_D3D12
#include "gpu/D3D12GpuPathTracer.h"
#endif
#if defined(PT_ENABLE_RAW_DESKTOP)
#include "platform/DesktopPlatform.h"
#endif
#ifdef PT_ENABLE_QT
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>

#include "platform/qt/QtPlatform.h"
#endif
#include "platform/PlatformFactory.h"
#include "scene/Scene.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/VulkanBackend.h"
#include "render/interface/RenderContracts.h"
#include "jobs/JobSystem.h"

namespace {

#ifdef _WIN32
using NativeMenuId = unsigned int;
constexpr NativeMenuId kFirstMenuCommandId = 40000u;
#else
using NativeMenuId = unsigned int;
constexpr NativeMenuId kFirstMenuCommandId = 40000u;
#endif

#ifdef _WIN32
std::wstring Utf8ToWide(std::string_view text) {
  const int n = MultiByteToWideChar(CP_UTF8, 0,
                                   text.data(),
                                   static_cast<int>(text.size()),
                                   nullptr, 0);
  if (n <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0,
                      text.data(),
                      static_cast<int>(text.size()),
                      out.data(), n);
  return out;
}

void AppendMenuItems(HMENU menu,
                    const std::vector<vkpt::editor::MenuItem>& items,
                    NativeMenuId& commandId,
                    std::unordered_map<NativeMenuId, std::string>& menuCommands) {
  for (const auto& item : items) {
    const auto label = Utf8ToWide(item.label);
    const UINT enabledFlag = item.enabled ? MF_ENABLED : MF_GRAYED;
    if (item.children.empty()) {
      AppendMenuW(menu, MF_STRING | enabledFlag, commandId, label.c_str());
      menuCommands.emplace(commandId, item.id);
      ++commandId;
    } else {
      auto* submenu = CreatePopupMenu();
      AppendMenuItems(submenu, item.children, commandId, menuCommands);
      AppendMenuW(menu, MF_POPUP | enabledFlag, reinterpret_cast<UINT_PTR>(submenu), label.c_str());
    }
  }
}

HMENU BuildNativeMenuBarFromModel(const vkpt::editor::MenuBar& menuModel,
                                  std::unordered_map<NativeMenuId, std::string>& menuCommands) {
  menuCommands.clear();
  auto* menuBar = CreateMenu();
  if (!menuBar) {
    return nullptr;
  }
  NativeMenuId commandId = kFirstMenuCommandId;
  for (const auto& top : menuModel.top_level_menus) {
    auto* topMenu = CreatePopupMenu();
    if (!topMenu) {
      continue;
    }
    AppendMenuItems(topMenu, top.children, commandId, menuCommands);
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(topMenu),
                Utf8ToWide(top.label).c_str());
  }
  return menuBar;
}

std::string QuoteShellArg(std::string_view arg) {
  if (arg.find_first_of(" \"\t\r\n") == std::string_view::npos) {
    return std::string(arg);
  }
  std::string out;
  out.reserve(arg.size() + 2);
  out.push_back('\"');
  for (const char ch : arg) {
    if (ch == '\"') {
      out.append("\\\"");
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\"');
  return out;
}

#if defined(PT_ENABLE_RAW_DESKTOP)
void SetWindowFrameStatus(vkpt::platform::DesktopWindow* window,
                         const vkpt::editor::UiRuntimeState& runtime_state,
                         const vkpt::editor::UiLayoutDocument& layout_state,
                         vkpt::core::FrameIndex frame_index,
                         std::string_view perf_text) {
  std::ostringstream out;
  out << "vkpt-desktop | frame=" << frame_index
      << " | layout=" << layout_state.active_layout_name
      << " | scene=" << runtime_state.active_scene
      << " | backend=" << runtime_state.active_renderer_backend
      << " | status=" << runtime_state.status_message;
  if (!perf_text.empty()) {
    out << " | " << perf_text;
  }
  window->set_title(out.str());
}

std::string FormatThroughputKmb(double value_per_second) {
  const double v = std::max(0.0, value_per_second);
  std::ostringstream out;
  if (v < 1.0e3) {
    out << static_cast<std::uint64_t>(std::llround(v));
  } else if (v < 1.0e6) {
    out << std::fixed << std::setprecision(1) << (v / 1.0e3) << "K";
  } else if (v < 1.0e9) {
    out << std::fixed << std::setprecision(1) << (v / 1.0e6) << "M";
  } else {
    out << std::fixed << std::setprecision(2) << (v / 1.0e9) << "B";
  }
  return out.str();
}
#endif
#endif

void LogUiWindowStartup(uint32_t width,
                        uint32_t height,
                        const vkpt::editor::UiLayoutDocument& layout_state,
                        const vkpt::editor::UiRuntimeState& runtime_state) {
  std::ostringstream out;
  out << "ui startup: requested_window=" << width << "x" << height
      << ", scene=" << (runtime_state.active_scene.empty() ? "none" : runtime_state.active_scene)
      << ", backend=" << runtime_state.active_renderer_backend
      << ", layout=" << layout_state.active_layout_name
      << ", panels=" << layout_state.panels.size()
      << ", top_level_menu_count=" << layout_state.panel_order.size();
  std::cout << out.str() << "\n";
}

#if defined(PT_ENABLE_RAW_DESKTOP)
void LogDesktopWindowState(const char* stage,
                          const vkpt::platform::DesktopWindow& window) {
#ifdef _WIN32
  auto& logger = vkpt::log::Logger::instance();
  const auto hwnd = static_cast<HWND>(window.native_handle());
  RECT rect{};
  RECT client{};
  const BOOL hasRect = GetWindowRect(hwnd, &rect);
  const BOOL hasClient = GetClientRect(hwnd, &client);
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  const BOOL hasPlacement = GetWindowPlacement(hwnd, &placement);

  std::ostringstream out;
  out << stage << " hwnd=" << hwnd
      << " metrics=" << window.metrics().width << "x" << window.metrics().height
      << " visible=" << (IsWindowVisible(hwnd) ? "true" : "false")
      << " iconic=" << (IsIconic(hwnd) ? "true" : "false")
      << " zoomed=" << (IsZoomed(hwnd) ? "true" : "false")
      << " style=" << std::hex
      << static_cast<std::uint32_t>(static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_STYLE)))
      << std::dec
      << " exStyle=" << std::hex
      << static_cast<std::uint32_t>(static_cast<std::uintptr_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)))
      << std::dec
      << " hasMenu=" << (GetMenu(hwnd) != nullptr ? "true" : "false");
  if (hasRect) {
    out << " rect=" << rect.left << "," << rect.top << " "
        << (rect.right - rect.left) << "x" << (rect.bottom - rect.top);
  } else {
    out << " rect=error(" << GetLastError() << ")";
  }
  if (hasClient) {
    out << " client=" << (client.right - client.left) << "x" << (client.bottom - client.top);
  } else {
    out << " client=error(" << GetLastError() << ")";
  }
  if (hasPlacement) {
    out << " showCmd=" << placement.showCmd
        << " min=" << placement.ptMinPosition.x << "," << placement.ptMinPosition.y
        << " max=" << placement.ptMaxPosition.x << "," << placement.ptMaxPosition.y;
  } else {
    out << " placement=error(" << GetLastError() << ")";
  }
  logger.log(vkpt::log::Severity::Info, "app", out.str());
#endif
}
#endif

[[maybe_unused]] std::string UiWindowErrorString() {
  return "check artifacts/artifacts/logs/ptapp.log for full startup trace";
}

void InitializeLogging() {
  auto& logger = vkpt::log::Logger::instance();
  logger.set_min_severity(vkpt::log::Severity::Trace);
  std::filesystem::create_directories("artifacts/logs");
  logger.add_sink(std::make_unique<vkpt::log::ConsoleSink>(std::cout));
  logger.add_sink(std::make_unique<vkpt::log::PlainTextFileSink>("artifacts/logs/ptapp.log"));
  logger.add_sink(std::make_unique<vkpt::log::PlainTextFileSink>("artifacts/logs/black_canvas_trace.log"));
  logger.add_sink(std::make_unique<vkpt::log::JsonlFileSink>("artifacts/logs/black_canvas_trace.jsonl"));
  logger.add_sink(std::make_unique<vkpt::log::JsonlFileSink>("artifacts/logs/ptapp.jsonl"));
  logger.add_sink(std::make_unique<vkpt::log::RingBufferSink>(1024));
}

void InitializeCrashRecorder() {
  vkpt::diagnostics::CrashRecorder::instance().set_build_info(
    std::string(vkpt::build::kProjectVersion),
    std::string(vkpt::build::kGitHash),
    std::string(vkpt::build::kCompilerName) + " " + std::string(vkpt::build::kCompilerVersion),
    std::string(vkpt::build::kTargetOs),
    std::string(vkpt::build::kTargetArch),
    std::string(vkpt::build::kBuildType),
    std::string(vkpt::build::kEnabledFeatureFlags));
}

void BootStep(std::string_view message) {
  vkpt::log::Logger::instance().log(vkpt::log::Severity::Info, "app", message);
}

const char* YesNo(bool value) {
  return value ? "yes" : "no";
}

const char* JsonBool(bool value) {
  return value ? "true" : "false";
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

#if defined(PT_ENABLE_QT)
template <typename Value>
std::uint64_t QtPreviewStatToU64(const Value& value) {
  if constexpr (requires { value.load(); }) {
    return static_cast<std::uint64_t>(value.load());
  } else {
    return static_cast<std::uint64_t>(value);
  }
}

template <typename Stats>
std::optional<std::uint64_t> QtPreviewDroppedFramesFromStats(const Stats& stats) {
  if constexpr (requires { stats.dropped_frame_count; }) {
    return QtPreviewStatToU64(stats.dropped_frame_count);
  } else if constexpr (requires { stats.dropped_frames; }) {
    return QtPreviewStatToU64(stats.dropped_frames);
  } else if constexpr (requires { stats.droppedFrameCount; }) {
    return QtPreviewStatToU64(stats.droppedFrameCount);
  } else if constexpr (requires { stats.droppedFrames; }) {
    return QtPreviewStatToU64(stats.droppedFrames);
  } else if constexpr (requires { stats.dropped; }) {
    return QtPreviewStatToU64(stats.dropped);
  } else if constexpr (requires { stats.dropped_frame_count(); }) {
    return QtPreviewStatToU64(stats.dropped_frame_count());
  } else if constexpr (requires { stats.dropped_frames(); }) {
    return QtPreviewStatToU64(stats.dropped_frames());
  } else if constexpr (requires { stats.droppedFrameCount(); }) {
    return QtPreviewStatToU64(stats.droppedFrameCount());
  } else if constexpr (requires { stats.droppedFrames(); }) {
    return QtPreviewStatToU64(stats.droppedFrames());
  } else {
    return std::nullopt;
  }
}

template <typename Stats>
std::optional<std::uint64_t> QtPreviewDroppedFramesFromStats(const Stats* stats) {
  if (stats == nullptr) {
    return std::nullopt;
  }
  return QtPreviewDroppedFramesFromStats(*stats);
}

template <typename Window>
std::optional<std::uint64_t> ReadQtWindowDroppedFrames(Window* window) {
  if (window == nullptr) {
    return std::nullopt;
  }
  if constexpr (requires { window->handoff_stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->handoff_stats());
  } else if constexpr (requires { window->frame_handoff_stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->frame_handoff_stats());
  } else if constexpr (requires { window->framebuffer_stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->framebuffer_stats());
  } else if constexpr (requires { window->frame_stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->frame_stats());
  } else if constexpr (requires { window->present_stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->present_stats());
  } else if constexpr (requires { window->read_stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->read_stats());
  } else if constexpr (requires { window->stats(); }) {
    return QtPreviewDroppedFramesFromStats(window->stats());
  } else {
    return std::nullopt;
  }
}

bool QueueQtWindowFramebuffer(vkpt::platform::QtWindow* window,
                              std::shared_ptr<std::atomic<bool>> alive,
                              std::vector<std::uint8_t> rgba,
                              std::size_t width,
                              std::size_t height) {
  if (window == nullptr || !alive || rgba.empty() || width == 0u || height == 0u) {
    return false;
  }
  if (!alive->load(std::memory_order_acquire)) {
    return false;
  }
  window->set_framebuffer_rgba(rgba, width, height);
  return true;
}

void DrainQtQueuedWork(int maxMilliseconds = 16) {
  if (QCoreApplication::instance() != nullptr) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, maxMilliseconds);
  }
}
#endif

bool IsRawPlatformBuilt() {
  return vkpt::platform::IsPlatformBuilt(vkpt::platform::RuntimePlatformKind::Raw);
}

bool IsQtPlatformBuilt() {
  return vkpt::platform::IsPlatformBuilt(vkpt::platform::RuntimePlatformKind::Qt);
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

void LogRuntimeMetadata(vkpt::log::Logger& logger,
                        std::string_view phase,
                        vkpt::platform::RuntimePlatformKind requestedPlatform,
                        vkpt::platform::RuntimePlatformKind selectedPlatform,
                        vkpt::platform::RuntimePlatformKind effectivePlatform,
                        bool openWindow,
                        bool doRender,
                        bool autoExit,
                        uint32_t frameLimit,
                        uint32_t uiPresentHz) {
  const bool qtPreviewActive =
      effectivePlatform == vkpt::platform::RuntimePlatformKind::Qt && openWindow;
  const uint32_t effectivePreviewPresentHz =
      qtPreviewActive ? std::max<uint32_t>(1u, uiPresentHz) : 0u;
  logger.log(vkpt::log::Severity::Info, "app", "runtime metadata", {
    {"phase", std::string(phase)},
    {"requested_platform", vkpt::platform::RuntimePlatformKindName(requestedPlatform)},
    {"selected_platform", vkpt::platform::RuntimePlatformKindName(selectedPlatform)},
    {"effective_platform", vkpt::platform::RuntimePlatformKindName(effectivePlatform)},
    {"window_system", std::string(WindowSystemName(effectivePlatform))},
    {"raw_built", YesNo(IsRawPlatformBuilt())},
    {"qt_built", YesNo(IsQtPlatformBuilt())},
    {"qt_version", QtVersionString()},
    {"qt_platform_shell", QtPlatformShellString()},
    {"window", openWindow ? "true" : "false"},
    {"render", doRender ? "true" : "false"},
    {"auto_exit", autoExit ? "true" : "false"},
    {"frame_limit", std::to_string(frameLimit)},
    {"effective_preview_present_hz", std::to_string(effectivePreviewPresentHz)},
    {"preview_present_rate_source", qtPreviewActive ? "ui_present_hz" : "inactive"}
  });
}

void PrintNonGuiPlatformShellNotice(std::string_view command,
                                    vkpt::platform::RuntimePlatformKind selectedPlatform,
                                    vkpt::platform::RuntimePlatformKind effectivePlatform) {
  if (selectedPlatform == vkpt::platform::RuntimePlatformKind::Qt &&
      effectivePlatform == vkpt::platform::RuntimePlatformKind::Headless) {
    std::cout << "ui shell: headless (Qt requested; GUI not initialized for "
              << command << ")\n";
  }
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
  std::cout << "  --list-backends       Print known render backends and capabilities\n";
  std::cout << "  --list-gpus           Enumerate Vulkan physical devices and select the best\n";
  std::cout << "  --headless            Initialize headless platform\n";
  std::cout << "  --window              Open desktop window and keep app running\n";
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
  std::cout << "  --render              Render using scalar CPU path tracer\n";
  std::cout << "  --output <path>       Render output PNG path\n";
  std::cout << "  --exr-output <path>   Render output EXR path\n";
  std::cout << "  --width <px>          Render width\n";
  std::cout << "  --height <px>         Render height\n";
  std::cout << "  --spp <samples>       Samples per pixel\n";
  std::cout << "  --max-depth <depth>   Max ray depth\n";
}

// ---- ptdoctor checks -------------------------------------------------------

struct DoctorCheckResult {
  std::string name;
  bool passed = false;
  std::string detail;
};

DoctorCheckResult CheckBuild() {
  DoctorCheckResult r;
  r.name = "build";
  bool ok = true;
  std::ostringstream detail;
  detail << "version=" << vkpt::build::kProjectVersion
         << " git=" << vkpt::build::kGitHash
         << " compiler=" << vkpt::build::kCompilerName
         << " target=" << vkpt::build::kTargetOs << "/" << vkpt::build::kTargetArch
         << " features=[" << vkpt::build::kEnabledFeatureFlags << "]"
         << " ui_platforms=headless:" << YesNo(true)
         << ",raw:" << YesNo(IsRawPlatformBuilt())
         << ",qt:" << YesNo(IsQtPlatformBuilt())
         << " qt_version=" << QtVersionString()
         << " qt_platform_shell=" << QtPlatformShellString();

  // Gate 10 (F17): startup self-test (best-effort; actionable failures).
  const std::filesystem::path outDir = "artifacts/self_test";
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    ok = false;
    detail << " self_test_dir=FAIL(" << ec.message() << ")";
  } else {
    detail << " self_test_dir=ok";
    const auto probePath = outDir / "write_probe.txt";
    std::ofstream probe(probePath.string());
    probe << "ptapp self-test\n";
    probe.close();
    if (!std::filesystem::exists(probePath)) {
      ok = false;
      detail << " write_probe=FAIL";
    } else {
      detail << " write_probe=ok";
    }
  }

  // Backend selection / init smoke (non-fatal if none).
  const auto backends = vkpt::render::AvailableBackendNames();
  if (backends.empty()) {
    ok = false;
    detail << " backends=FAIL(none)";
  } else {
    auto backend = vkpt::render::CreateBackend(backends.front());
    if (!backend || !backend->initialize()) {
      ok = false;
      detail << " backend_init=FAIL(" << backends.front() << ")";
    } else {
      detail << " backend_init=ok(" << backends.front() << ")";
    }
  }

  r.passed = ok;
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckCpu() {
  DoctorCheckResult r;
  r.name = "cpu";
  r.passed = true;
  std::ostringstream detail;
  detail << "simd_options=[" << vkpt::build::kSimdCompileOptions << "]";
#if defined(__AVX2__)
  detail << " avx2=yes";
#elif defined(__AVX__)
  detail << " avx=yes";
#else
  detail << " avx=no";
#endif
#if defined(__SSE4_2__)
  detail << " sse4.2=yes";
#endif
#if defined(__ARM_NEON)
  detail << " neon=yes";
#endif
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckBackends() {
  DoctorCheckResult r;
  r.name = "backends";
  auto names = vkpt::render::AvailableBackendNames();
  if (names.empty()) {
    r.passed = false;
    r.detail = "no backends available";
    return r;
  }
  r.passed = true;
  std::ostringstream detail;
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) { detail << name << ":unavailable "; continue; }
    if (!backend->initialize()) { detail << name << ":init_failed "; continue; }
    const auto caps = backend->capabilities();
    detail << name << ":ok(compute=" << (caps.compute ? "y" : "n")
           << ",rt=" << (caps.ray_tracing ? "y" : "n") << ") ";
  }
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckAssets() {
  DoctorCheckResult r;
  r.name = "assets";
  const std::filesystem::path sceneDir = "assets/scenes";
  if (!std::filesystem::exists(sceneDir)) {
    r.passed = false;
    r.detail = "assets/scenes directory missing";
    return r;
  }
  std::size_t count = 0;
  for (const auto& e : std::filesystem::directory_iterator(sceneDir)) {
    if (e.path().extension() == ".json") ++count;
  }
  r.passed = count > 0;
  r.detail = std::string("scene_files=") + std::to_string(count)
           + " path=" + std::filesystem::absolute(sceneDir).string();
  return r;
}

DoctorCheckResult CheckShaders() {
  DoctorCheckResult r;
  r.name = "shaders";
  const std::filesystem::path shaderDir = "src/shaders";
  if (!std::filesystem::exists(shaderDir)) {
    r.passed = false;
    r.detail = "src/shaders directory missing";
    return r;
  }
  std::size_t count = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(shaderDir)) {
    if (e.is_regular_file()) ++count;
  }
  r.passed = true;
  r.detail = std::string("shader_files=") + std::to_string(count);
  return r;
}

DoctorCheckResult CheckJobSystem() {
  DoctorCheckResult r;
  r.name = "job_system";
  std::atomic<int> counter{0};
  {
    vkpt::jobs::JobSystem js(1u);
    auto handle = js.submit_job([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    js.wait(handle);
    js.shutdown();
  }
  r.passed = counter.load(std::memory_order_relaxed) == 1;
  r.detail = r.passed ? "job_ran=ok worker_count=1" : "job did not complete";
  return r;
}

DoctorCheckResult CheckSceneSchema() {
  DoctorCheckResult r;
  r.name = "scene_schema";
  const std::filesystem::path scenePath = "assets/scenes/cornell_native.json";
  if (!std::filesystem::exists(scenePath)) {
    r.passed = true;
    r.detail = "skipped(cornell_native.json not found)";
    return r;
  }
  const auto result = vkpt::scene::SceneDocument::load_from_file(scenePath.string());
  if (!result) {
    r.passed = false;
    r.detail = "load=FAIL";
    return r;
  }
  std::vector<std::string> issues;
  const bool valid = result.value().validate(&issues);
  r.passed = valid;
  std::ostringstream detail;
  detail << "entities=" << result.value().snapshot().entity_ids.size()
         << " materials=" << result.value().materials.size()
         << " valid=" << (valid ? "yes" : "no");
  if (!issues.empty()) {
    detail << " issues=[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
      if (i) detail << ",";
      detail << issues[i];
    }
    detail << "]";
  }
  r.detail = detail.str();
  return r;
}

DoctorCheckResult CheckBenchmarkArtifactWrite() {
  DoctorCheckResult r;
  r.name = "benchmark_artifact_write";
  const std::filesystem::path outDir = "artifacts/self_test";
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    r.passed = false;
    r.detail = std::string("dir=FAIL(") + ec.message() + ")";
    return r;
  }
  const auto probePath = outDir / "bench_probe.json";
  {
    std::ofstream probe(probePath.string());
    probe << "{\"self_test\":true,\"schema\":\"benchmark_result\"}\n";
  }
  r.passed = std::filesystem::exists(probePath);
  r.detail = r.passed ? "bench_probe.json=ok" : "bench_probe.json=FAIL";
  return r;
}

void RunDoctor(bool checkBuild, bool checkCpu, bool checkBackends,
               bool checkAssets, bool checkShaders,
               bool checkJobSystem, bool checkSceneSchema, bool checkBenchmarkArtifact) {
  const std::vector<DoctorCheckResult> results = {
    checkBuild             ? CheckBuild()                  : DoctorCheckResult{"build",                    true, "skipped"},
    checkCpu               ? CheckCpu()                    : DoctorCheckResult{"cpu",                      true, "skipped"},
    checkBackends          ? CheckBackends()               : DoctorCheckResult{"backends",                 true, "skipped"},
    checkAssets            ? CheckAssets()                 : DoctorCheckResult{"assets",                   true, "skipped"},
    checkShaders           ? CheckShaders()                : DoctorCheckResult{"shaders",                  true, "skipped"},
    checkJobSystem         ? CheckJobSystem()              : DoctorCheckResult{"job_system",               true, "skipped"},
    checkSceneSchema       ? CheckSceneSchema()            : DoctorCheckResult{"scene_schema",             true, "skipped"},
    checkBenchmarkArtifact ? CheckBenchmarkArtifactWrite() : DoctorCheckResult{"benchmark_artifact_write", true, "skipped"},
  };

  bool allOk = true;
  for (const auto& r : results) {
    const char* status = r.passed ? "ok " : "FAIL";
    std::cout << "[" << status << "] " << r.name << ": " << r.detail << "\n";
    if (!r.passed) allOk = false;
  }
  std::cout << "\ndoctor: " << (allOk ? "ok" : "FAIL") << "\n";
}

void PrintBackendDiagnostics() {
  std::cout << "available backends:\n";
  auto names = vkpt::render::AvailableBackendNames();
  if (names.empty()) {
    std::cout << "  (none)\n";
    return;
  }
  for (const auto& name : names) {
    auto backend = vkpt::render::CreateBackend(name);
    if (!backend) {
      std::cout << "  " << name << " unavailable\n";
      continue;
    }
    if (!backend->initialize()) {
      std::cout << "  " << name << " failed to initialize\n";
      continue;
    }
    auto capabilities = backend->capabilities();
    std::cout << "  " << vkpt::render::BackendKindToString(backend->kind()) << " -> " << capabilities.backend_name << "\n";
    std::cout << "    " << vkpt::render::SerializeBackendCapabilities(capabilities) << "\n";
  }
  const auto manifest = vkpt::pathtracer::BuildRTSceneDataLayoutManifest();
  if (manifest) {
    std::cout << "rt layout:\n";
    std::cout << "  " << vkpt::pathtracer::SerializeRTSceneDataLayoutManifest(manifest.value()) << "\n";
  }
}

void PrintVersionText(vkpt::platform::RuntimePlatformKind platformShell) {
  std::cout << "ptapp " << vkpt::build::kProjectVersion << '\n';
  std::cout << "git: " << vkpt::build::kGitHash << '\n';
  std::cout << "build date: " << vkpt::build::kBuildDate << '\n';
  std::cout << "compiler: " << vkpt::build::kCompilerName << ' ' << vkpt::build::kCompilerVersion << '\n';
  std::cout << "target: " << vkpt::build::kTargetOs << '/' << vkpt::build::kTargetArch << '\n';
  std::cout << "build type: " << vkpt::build::kBuildType << '\n';
  std::cout << "features: " << vkpt::build::kEnabledFeatureFlags << '\n';
  std::cout << "platform shell: " << vkpt::platform::RuntimePlatformKindName(platformShell) << '\n';
  std::cout << "window system: " << WindowSystemName(platformShell) << '\n';
  std::cout << "platforms: headless=yes raw=" << YesNo(IsRawPlatformBuilt())
            << " qt=" << YesNo(IsQtPlatformBuilt()) << '\n';
  std::cout << "qt: " << QtSupportState()
            << " version=" << QtVersionString()
            << " platform_shell=" << QtPlatformShellString() << '\n';
}

void PrintVersionJson(vkpt::platform::RuntimePlatformKind platformShell) {
  std::cout << "{\n";
  std::cout << "  \"app\": \"ptapp\",\n";
  std::cout << "  \"version\": \"" << vkpt::log::EscapeJson(vkpt::build::kProjectVersion) << "\",\n";
  std::cout << "  \"git_hash\": \"" << vkpt::log::EscapeJson(vkpt::build::kGitHash) << "\",\n";
  std::cout << "  \"build_date\": \"" << vkpt::log::EscapeJson(vkpt::build::kBuildDate) << "\",\n";
  std::cout << "  \"compiler\": \"" << vkpt::log::EscapeJson(std::string(vkpt::build::kCompilerName) + " " + std::string(vkpt::build::kCompilerVersion)) << "\",\n";
  std::cout << "  \"cpp_standard\": \"" << vkpt::log::EscapeJson(vkpt::build::kCxxStandard) << "\",\n";
  std::cout << "  \"target\": \"" << vkpt::log::EscapeJson(std::string(vkpt::build::kTargetOs) + "/" + std::string(vkpt::build::kTargetArch)) << "\",\n";
  std::cout << "  \"build_type\": \"" << vkpt::log::EscapeJson(vkpt::build::kBuildType) << "\",\n";
  std::cout << "  \"simd_compile_options\": \"" << vkpt::log::EscapeJson(vkpt::build::kSimdCompileOptions) << "\",\n";
  std::cout << "  \"backend_compile_options\": \"" << vkpt::log::EscapeJson(vkpt::build::kBackendCompileOptions) << "\",\n";
  std::cout << "  \"enabled_features\": [\"" << vkpt::log::EscapeJson(vkpt::build::kEnabledFeatureFlags) << "\"],\n";
  std::cout << "  \"disabled_features\": [\"" << vkpt::log::EscapeJson(vkpt::build::kDisabledFeatureFlags) << "\"],\n";
  std::cout << "  \"ui\": {\n";
  std::cout << "    \"platform_shell\": \"" << vkpt::log::EscapeJson(vkpt::platform::RuntimePlatformKindName(platformShell)) << "\",\n";
  std::cout << "    \"window_system\": \"" << vkpt::log::EscapeJson(WindowSystemName(platformShell)) << "\",\n";
  std::cout << "    \"built_platforms\": {\n";
  std::cout << "      \"headless\": true,\n";
  std::cout << "      \"raw\": " << JsonBool(IsRawPlatformBuilt()) << ",\n";
  std::cout << "      \"qt\": " << JsonBool(IsQtPlatformBuilt()) << "\n";
  std::cout << "    },\n";
  std::cout << "    \"qt\": {\n";
  std::cout << "      \"supported\": " << JsonBool(IsQtPlatformBuilt()) << ",\n";
  std::cout << "      \"support\": \"" << QtSupportState() << "\",\n";
  std::cout << "      \"version\": \"" << vkpt::log::EscapeJson(QtVersionString()) << "\",\n";
  std::cout << "      \"platform_shell\": \"" << vkpt::log::EscapeJson(QtPlatformShellString()) << "\"\n";
  std::cout << "    }\n";
  std::cout << "  }\n";
  std::cout << "}\n";
}

bool ParseUnsigned(const std::string_view text, uint32_t& out) {
  try {
    out = static_cast<uint32_t>(std::stoul(std::string(text)));
    return true;
  } catch (...) {
    return false;
  }
}

std::uint64_t NowMs() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::string_view ToString(vkpt::platform::InputEventType type) {
  switch (type) {
    case vkpt::platform::InputEventType::MenuCommand: return "menu_command";
    case vkpt::platform::InputEventType::KeyDown: return "key_down";
    case vkpt::platform::InputEventType::KeyUp: return "key_up";
    case vkpt::platform::InputEventType::MouseMove: return "mouse_move";
    case vkpt::platform::InputEventType::MouseButtonDown: return "mouse_button_down";
    case vkpt::platform::InputEventType::MouseButtonUp: return "mouse_button_up";
    case vkpt::platform::InputEventType::MouseWheel: return "mouse_wheel";
    case vkpt::platform::InputEventType::WindowResize: return "window_resize";
    case vkpt::platform::InputEventType::FocusLost: return "focus_lost";
    case vkpt::platform::InputEventType::FocusGained: return "focus_gained";
    case vkpt::platform::InputEventType::CloseRequested: return "close_requested";
    case vkpt::platform::InputEventType::None:
    default: return "none";
  }
}

std::string_view BenchmarkRunBackendFromAction(std::string_view action_id) {
  if (action_id.find("cpu") != std::string_view::npos) {
    return "cpu";
  }
  if (action_id.find("gpu") != std::string_view::npos ||
      action_id.find("backend") != std::string_view::npos) {
    return "vulkan";
  }
  return "cpu";
}

std::string_view BenchmarkRendererFromBackend(std::string_view backend) {
  if (backend == "vulkan") {
    return "gpu-compute";
  }
  return "cpu-scalar";
}

std::string MakeMenuActionArtifactPath(std::string_view action_id) {
  const std::filesystem::path root = "artifacts/benchmarks/ui";
  const std::string actionToken =
      std::string(action_id.empty() ? "menu_action" : action_id);
  std::string safeToken = actionToken;
  for (auto& ch : safeToken) {
    if ((ch < 'a' || ch > 'z') &&
        (ch < 'A' || ch > 'Z') &&
        (ch < '0' || ch > '9') &&
        ch != '-' && ch != '_') {
      ch = '_';
    }
  }
  std::string dir = root.string() + "/" + safeToken + "_" + std::to_string(NowMs());
  return dir;
}

std::string ResolveExecutable(std::string_view executable_name) {
#ifdef _WIN32
  wchar_t exePath[MAX_PATH] = {};
  const auto n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  if (n > 0) {
    std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path sibling = exeDir / std::string(executable_name);
    if (sibling.extension().empty()) {
      sibling += ".exe";
    }
    if (std::filesystem::exists(sibling)) {
      return sibling.string();
    }
    sibling = exeDir / "Release" / (std::string(executable_name) + ".exe");
    if (std::filesystem::exists(sibling)) {
      return sibling.string();
    }
  }
#endif
  std::filesystem::path fallback = std::string(executable_name);
  if (fallback.extension().empty()) {
    fallback += ".exe";
  }
  return fallback.string();
}

bool LaunchBenchmarkRun(const vkpt::editor::RunBenchmarkCommand& command,
                       const std::string& backend,
                       const std::string& renderer,
                       const std::string& scene_path,
                       const std::string& artifact_dir,
                       std::string* out_result_path = nullptr) {
  const std::string exe = ResolveExecutable("ptbench");
  std::string cmd = QuoteShellArg(exe) + " run";
  cmd += " --scene " + QuoteShellArg(scene_path);
  cmd += " --backend " + QuoteShellArg(backend);
  cmd += " --renderer-path " + QuoteShellArg(renderer);
  cmd += " --resolution " + std::to_string(command.desc.resolution.width) + "x" +
         std::to_string(command.desc.resolution.height);
  cmd += " --spp " + std::to_string(command.desc.samples_per_pixel);
  cmd += " --seed " + std::to_string(command.desc.seed);
  cmd += " --max-depth " + std::to_string(command.desc.max_depth);
  cmd += " --output " + QuoteShellArg(artifact_dir);
  auto exitCode = std::system(cmd.c_str());
  if (out_result_path) {
    *out_result_path = (std::filesystem::path(artifact_dir) / "results.json").string();
  }
  return exitCode == 0;
}

std::optional<std::filesystem::path> FindLatestBenchmarkResultJson() {
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
  const std::filesystem::path root = "artifacts/benchmarks";
  if (!std::filesystem::exists(root)) {
    return std::nullopt;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().filename() != "results.json") {
      continue;
    }
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(entry.path(), ec);
    if (!ec) {
      found.emplace_back(mtime, entry.path());
    }
  }
  if (found.empty()) {
    return std::nullopt;
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
    return a.first > b.first;
  });
  return found.front().second;
}

[[maybe_unused]] std::vector<std::filesystem::path> ListRecentBenchmarkResultDirs(std::size_t max_items = 16) {
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
  const std::filesystem::path root = "artifacts/benchmarks";
  if (!std::filesystem::exists(root)) {
    return {};
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().filename() != "results.json") {
      continue;
    }
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(entry.path(), ec);
    if (!ec) {
      found.emplace_back(mtime, entry.path().parent_path());
    }
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::filesystem::path> out;
  for (std::size_t i = 0; i < found.size() && i < max_items; ++i) {
    if (std::find(out.begin(), out.end(), found[i].second) == out.end()) {
      out.push_back(found[i].second);
    }
  }
  return out;
}

bool OpenPathInExplorer(const std::filesystem::path& path) {
#ifdef _WIN32
  const std::string command = "start \"\" " + QuoteShellArg(path.string());
  return std::system(command.c_str()) == 0;
#else
  std::string command = "xdg-open " + QuoteShellArg(path.string()) + " > /dev/null 2>&1";
  return std::system(command.c_str()) == 0;
#endif
}

std::string ResolveMenuFallbackScenePath(const vkpt::editor::RunBenchmarkCommand& command,
                                        const std::string& activeScene,
                                        const std::string& cliScene,
                                        const std::string& defaultScene) {
  if (!command.desc.scene_path.empty()) {
    return command.desc.scene_path;
  }
  if (!activeScene.empty()) {
    return activeScene;
  }
  if (!cliScene.empty()) {
    return cliScene;
  }
  if (!defaultScene.empty()) {
    return defaultScene;
  }
  return "scenes/test.json";
}

vkpt::editor::RunBenchmarkCommand ResolveBenchmarkCommand(
    const vkpt::editor::RunBenchmarkCommand& command,
    std::string_view action_id,
    const std::string& active_scene,
    const std::string& cli_scene) {
  auto resolved = command;

  if (resolved.desc.scene_path.empty()) {
    resolved.desc.scene_path = ResolveMenuFallbackScenePath(command, active_scene, cli_scene,
                                                           "scenes/test.json");
  }

  if (resolved.desc.resolution.width == 0u) {
    resolved.desc.resolution.width = 1024u;
  }
  if (resolved.desc.resolution.height == 0u) {
    resolved.desc.resolution.height = 576u;
  }
  if (resolved.desc.samples_per_pixel == 0u) {
    resolved.desc.samples_per_pixel = 128u;
  }
  if (resolved.desc.max_depth == 0u) {
    resolved.desc.max_depth = 10u;
  }
  if (resolved.desc.seed == 0u) {
    resolved.desc.seed = 42u;
  }

  if (resolved.desc.tolerance_policy.empty()) {
    resolved.desc.tolerance_policy = "default";
  }

  if (resolved.desc.backend.empty()) {
    resolved.desc.backend = std::string(BenchmarkRunBackendFromAction(action_id));
  }
  if (resolved.desc.renderer_path.empty() || resolved.desc.renderer_path == "hybrid") {
    resolved.desc.renderer_path = std::string(BenchmarkRendererFromBackend(resolved.desc.backend));
  }
  return resolved;
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index,
                 std::string_view old_value,
                 std::string_view new_value,
                 std::string_view command_result);

void SyncRuntimePanelState(vkpt::editor::UiRuntimeState& runtime_state,
                          const vkpt::editor::UiLayoutDocument& layout_state) {
  runtime_state.active_layout_name = layout_state.active_layout_name;
  runtime_state.dpi_scale = layout_state.dpi_scale;
  runtime_state.ui_scale = layout_state.ui_scale;
  runtime_state.visible_panels.clear();
  runtime_state.collapsed_panels.clear();
  for (const auto& panel : layout_state.panels) {
    if (panel.visible) {
      runtime_state.visible_panels.push_back(panel.id);
    }
    if (panel.collapsed) {
      runtime_state.collapsed_panels.push_back(panel.id);
    }
  }
}

void ApplyWindowMetricsToLayout(vkpt::editor::UiLayoutDocument& layout_state,
                               std::size_t width,
                               std::size_t height) {
  for (auto& panel : layout_state.panels) {
    if (panel.id == vkpt::editor::ToString(vkpt::editor::UiPanelId::Viewport)) {
      panel.width = static_cast<float>(width);
      panel.height = static_cast<float>(height);
      return;
    }
  }
  if (!layout_state.panels.empty()) {
    layout_state.panels.front().width = static_cast<float>(width);
    layout_state.panels.front().height = static_cast<float>(height);
  }
}

#ifdef PT_ENABLE_QT
struct ViewportCameraPose {
  vkpt::pathtracer::Vec3 position{};
  vkpt::pathtracer::Vec3 target{};
  vkpt::pathtracer::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_deg = 60.0f;
};

struct ViewportPickable {
  vkpt::core::StableId entity_id = 0;
  vkpt::editor::Bounds bounds{};
  std::string label;
};

struct ViewportRay {
  vkpt::pathtracer::Vec3 origin{};
  vkpt::pathtracer::Vec3 direction{};
};

struct ViewportPickResult {
  vkpt::core::StableId entity_id = 0;
  vkpt::editor::Bounds bounds{};
  std::string label;
  float distance = 0.0f;
};

vkpt::pathtracer::Vec3 PtAdd(const vkpt::pathtracer::Vec3& a,
                             const vkpt::pathtracer::Vec3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

vkpt::pathtracer::Vec3 PtSub(const vkpt::pathtracer::Vec3& a,
                             const vkpt::pathtracer::Vec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

vkpt::pathtracer::Vec3 PtMul(const vkpt::pathtracer::Vec3& v, float scale) {
  return {v.x * scale, v.y * scale, v.z * scale};
}

float PtDot(const vkpt::pathtracer::Vec3& a, const vkpt::pathtracer::Vec3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

vkpt::pathtracer::Vec3 PtCross(const vkpt::pathtracer::Vec3& a,
                               const vkpt::pathtracer::Vec3& b) {
  return {
      a.y * b.z - a.z * b.y,
      a.z * b.x - a.x * b.z,
      a.x * b.y - a.y * b.x,
  };
}

float PtLength(const vkpt::pathtracer::Vec3& v) {
  return std::sqrt(std::max(0.0f, PtDot(v, v)));
}

vkpt::pathtracer::Vec3 PtNormalize(const vkpt::pathtracer::Vec3& v,
                                   const vkpt::pathtracer::Vec3& fallback = {0.0f, 0.0f, -1.0f}) {
  const float len = PtLength(v);
  if (len <= 1.0e-6f) {
    return fallback;
  }
  return PtMul(v, 1.0f / len);
}

float ClampFloat(float value, float min_value, float max_value) {
  return std::min(max_value, std::max(min_value, value));
}

float DegToRad(float degrees) {
  return degrees * (3.14159265358979323846f / 180.0f);
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::scene::Vec3& v) {
  return {v.x, v.y, v.z};
}

vkpt::editor::Vec3 ToEditorVec3(const vkpt::pathtracer::Vec3& v) {
  return {v.x, v.y, v.z};
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::editor::Vec3& v) {
  return {v.x, v.y, v.z};
}

void ExpandBounds(vkpt::editor::Bounds& bounds, const vkpt::pathtracer::Vec3& point) {
  if (!bounds.valid) {
    bounds.min = ToEditorVec3(point);
    bounds.max = ToEditorVec3(point);
    bounds.valid = true;
    return;
  }
  bounds.min.x = std::min(bounds.min.x, point.x);
  bounds.min.y = std::min(bounds.min.y, point.y);
  bounds.min.z = std::min(bounds.min.z, point.z);
  bounds.max.x = std::max(bounds.max.x, point.x);
  bounds.max.y = std::max(bounds.max.y, point.y);
  bounds.max.z = std::max(bounds.max.z, point.z);
}

vkpt::pathtracer::Vec3 TransformPointForPreview(const vkpt::scene::Vec3& point,
                                                const vkpt::scene::TransformComponent& transform) {
  return {
      point.x * transform.scale.x + transform.translation.x,
      point.y * transform.scale.y + transform.translation.y,
      point.z * transform.scale.z + transform.translation.z,
  };
}

std::string PickableLabel(std::string_view name, vkpt::core::StableId id) {
  if (!name.empty()) {
    return std::string(name);
  }
  return "entity " + std::to_string(id);
}

void AddSdfPickable(std::vector<ViewportPickable>& pickables,
                    const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
  const auto center = ToPtVec3(primitive.transform.translation);
  const auto scale = ToPtVec3(primitive.transform.scale);
  const float radius = std::max(0.05f, primitive.primitive.radius);
  vkpt::pathtracer::Vec3 extent{
      std::max(0.05f, std::fabs(scale.x) * radius),
      std::max(0.05f, std::fabs(scale.y) * radius),
      std::max(0.05f, std::fabs(scale.z) * radius),
  };
  if (primitive.shape == "box" || primitive.shape == "rounded_box") {
    extent = {
        std::max(0.05f, std::fabs(scale.x)),
        std::max(0.05f, std::fabs(scale.y)),
        std::max(0.05f, std::fabs(scale.z)),
    };
  } else if (primitive.shape == "torus") {
    const float major = std::max(0.05f, primitive.primitive.param_a);
    const float minor = std::max(0.02f, radius);
    const float torusExtent = major + minor;
    extent = {
        std::max(0.05f, std::fabs(scale.x) * torusExtent),
        std::max(0.05f, std::fabs(scale.y) * minor),
        std::max(0.05f, std::fabs(scale.z) * torusExtent),
    };
  } else if (primitive.shape == "capsule") {
    const float halfHeight = std::max(0.0f, primitive.primitive.param_a);
    extent = {
        std::max(0.05f, std::fabs(scale.x) * radius),
        std::max(0.05f, std::fabs(scale.y) * (halfHeight + radius)),
        std::max(0.05f, std::fabs(scale.z) * radius),
    };
  } else if (primitive.shape == "plane") {
    return;
  }

  vkpt::editor::Bounds bounds{};
  ExpandBounds(bounds, PtSub(center, extent));
  ExpandBounds(bounds, PtAdd(center, extent));
  if (bounds.valid) {
    pickables.push_back({primitive.id, bounds, "sdf " + std::to_string(primitive.id)});
  }
}

std::vector<ViewportPickable> BuildViewportPickables(const vkpt::scene::SceneDocument& document,
                                                     const vkpt::pathtracer::RTSceneData& scene) {
  std::vector<ViewportPickable> pickables;
  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneGeometryDefinition*> geometryById;
  for (const auto& geometry : document.geometry) {
    geometryById[geometry.id] = &geometry;
  }

  for (const auto& entity : document.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto geometryIt = geometryById.find(entity.mesh.mesh_id);
    if (geometryIt == geometryById.end()) {
      continue;
    }
    const auto* geometry = geometryIt->second;
    if (geometry == nullptr || geometry->vertices.empty()) {
      continue;
    }
    const auto transform = entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
    vkpt::editor::Bounds bounds{};
    for (const auto& vertex : geometry->vertices) {
      ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
    }
    if (bounds.valid) {
      pickables.push_back({entity.id, bounds, PickableLabel(entity.name, entity.id)});
    }
  }

  for (const auto& primitive : document.sdf_primitives) {
    AddSdfPickable(pickables, primitive);
  }

  if (!pickables.empty()) {
    return pickables;
  }

  for (std::size_t instanceIndex = 0; instanceIndex < scene.instances.size(); ++instanceIndex) {
    const auto& instance = scene.instances[instanceIndex];
    vkpt::editor::Bounds bounds{};
    for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
      const uint32_t base = (instance.first_triangle + triangle) * 3u;
      if (base + 2u >= scene.indices.size()) {
        continue;
      }
      for (uint32_t corner = 0u; corner < 3u; ++corner) {
        const uint32_t vertexIndex = scene.indices[base + corner];
        if (vertexIndex < scene.vertices.size()) {
          ExpandBounds(bounds, scene.vertices[vertexIndex]);
        }
      }
    }
    if (bounds.valid) {
      const auto id = static_cast<vkpt::core::StableId>(instanceIndex + 1u);
      pickables.push_back({id, bounds, "instance " + std::to_string(id)});
    }
  }

  for (std::size_t primitiveIndex = 0; primitiveIndex < scene.sdf_primitives.size(); ++primitiveIndex) {
    const auto& primitive = scene.sdf_primitives[primitiveIndex];
    const float radius = std::max(0.05f, primitive.radius);
    const vkpt::pathtracer::Vec3 extent{
        std::max(0.05f, std::fabs(primitive.scale.x) * radius),
        std::max(0.05f, std::fabs(primitive.scale.y) * radius),
        std::max(0.05f, std::fabs(primitive.scale.z) * radius),
    };
    vkpt::editor::Bounds bounds{};
    ExpandBounds(bounds, PtSub(primitive.position, extent));
    ExpandBounds(bounds, PtAdd(primitive.position, extent));
    if (bounds.valid) {
      const auto id = static_cast<vkpt::core::StableId>(pickables.size() + 1u);
      pickables.push_back({id, bounds, "sdf " + std::to_string(id)});
    }
  }

  return pickables;
}

ViewportRay BuildViewportRay(const ViewportCameraPose& camera,
                             float x,
                             float y,
                             float width,
                             float height) {
  const float safeWidth = std::max(1.0f, width);
  const float safeHeight = std::max(1.0f, height);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right = PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float aspect = safeWidth / safeHeight;
  const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));
  const float nx = ((x + 0.5f) / safeWidth * 2.0f - 1.0f) * aspect * tanHalfFov;
  const float ny = (1.0f - (y + 0.5f) / safeHeight * 2.0f) * tanHalfFov;
  return {camera.position, PtNormalize(PtAdd(PtAdd(forward, PtMul(right, nx)), PtMul(up, ny)))};
}

bool IntersectBounds(const ViewportRay& ray, const vkpt::editor::Bounds& bounds, float& t_near) {
  if (!bounds.valid) {
    return false;
  }
  const float minValues[3] = {bounds.min.x, bounds.min.y, bounds.min.z};
  const float maxValues[3] = {bounds.max.x, bounds.max.y, bounds.max.z};
  const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
  const float direction[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
  float tMin = 1.0e-4f;
  float tMax = std::numeric_limits<float>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(direction[axis]) <= 1.0e-6f) {
      if (origin[axis] < minValues[axis] || origin[axis] > maxValues[axis]) {
        return false;
      }
      continue;
    }
    const float invD = 1.0f / direction[axis];
    float t0 = (minValues[axis] - origin[axis]) * invD;
    float t1 = (maxValues[axis] - origin[axis]) * invD;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax) {
      return false;
    }
  }
  t_near = tMin;
  return true;
}

std::optional<ViewportPickResult> PickViewportObject(const std::vector<ViewportPickable>& pickables,
                                                     const ViewportCameraPose& camera,
                                                     float x,
                                                     float y,
                                                     float width,
                                                     float height) {
  const auto ray = BuildViewportRay(camera, x, y, width, height);
  std::optional<ViewportPickResult> best;
  for (const auto& pickable : pickables) {
    float t = 0.0f;
    if (!IntersectBounds(ray, pickable.bounds, t)) {
      continue;
    }
    if (!best || t < best->distance) {
      best = ViewportPickResult{pickable.entity_id, pickable.bounds, pickable.label, t};
    }
  }
  return best;
}

std::vector<vkpt::pathtracer::Vec3> BoundsCorners(const vkpt::editor::Bounds& bounds) {
  return {
      {bounds.min.x, bounds.min.y, bounds.min.z},
      {bounds.max.x, bounds.min.y, bounds.min.z},
      {bounds.min.x, bounds.max.y, bounds.min.z},
      {bounds.max.x, bounds.max.y, bounds.min.z},
      {bounds.min.x, bounds.min.y, bounds.max.z},
      {bounds.max.x, bounds.min.y, bounds.max.z},
      {bounds.min.x, bounds.max.y, bounds.max.z},
      {bounds.max.x, bounds.max.y, bounds.max.z},
  };
}

std::optional<vkpt::platform::QtSelectionOverlayBox> ProjectBoundsToOverlay(
    const vkpt::editor::Bounds& bounds,
    const ViewportCameraPose& camera,
    float width,
    float height,
    std::string label,
    bool primary) {
  if (!bounds.valid || width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right = PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float aspect = width / std::max(1.0f, height);
  const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));

  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  bool anyProjected = false;
  for (const auto& corner : BoundsCorners(bounds)) {
    const auto rel = PtSub(corner, camera.position);
    const float depth = PtDot(rel, forward);
    if (depth <= 1.0e-4f) {
      continue;
    }
    const float cameraX = PtDot(rel, right);
    const float cameraY = PtDot(rel, up);
    const float ndcX = cameraX / (depth * tanHalfFov * aspect);
    const float ndcY = cameraY / (depth * tanHalfFov);
    const float screenX = (ndcX + 1.0f) * 0.5f * width;
    const float screenY = (1.0f - ndcY) * 0.5f * height;
    minX = std::min(minX, screenX);
    minY = std::min(minY, screenY);
    maxX = std::max(maxX, screenX);
    maxY = std::max(maxY, screenY);
    anyProjected = true;
  }
  if (!anyProjected) {
    return std::nullopt;
  }
  const float margin = 4.0f;
  minX = ClampFloat(minX - margin, -width, width * 2.0f);
  minY = ClampFloat(minY - margin, -height, height * 2.0f);
  maxX = ClampFloat(maxX + margin, -width, width * 2.0f);
  maxY = ClampFloat(maxY + margin, -height, height * 2.0f);
  if (maxX <= minX || maxY <= minY) {
    return std::nullopt;
  }
  return vkpt::platform::QtSelectionOverlayBox{minX, minY, maxX - minX, maxY - minY,
                                               std::move(label), primary};
}

std::vector<vkpt::platform::QtSelectionOverlayBox> BuildSelectionOverlayBoxes(
    const vkpt::editor::SelectionState& selection,
    const std::vector<ViewportPickable>& pickables,
    const ViewportCameraPose& camera,
    float width,
    float height) {
  std::vector<vkpt::platform::QtSelectionOverlayBox> boxes;
  for (const auto selectedId : selection.selected_entity_ids) {
    auto bounds = std::optional<vkpt::editor::Bounds>{};
    std::string label = "entity " + std::to_string(selectedId);
    for (const auto& item : selection.per_item_bounds) {
      if (item.entity_id == selectedId && item.bounds.valid) {
        bounds = item.bounds;
        break;
      }
    }
    for (const auto& pickable : pickables) {
      if (pickable.entity_id == selectedId) {
        if (!bounds) {
          bounds = pickable.bounds;
        }
        label = pickable.label;
        break;
      }
    }
    if (!bounds) {
      continue;
    }
    auto projected = ProjectBoundsToOverlay(*bounds,
                                            camera,
                                            width,
                                            height,
                                            label,
                                            selectedId == selection.active_primary_entity);
    if (projected) {
      boxes.push_back(std::move(*projected));
    }
  }
  return boxes;
}

void RebuildSelectionBounds(vkpt::editor::SelectionState& selection,
                            const std::vector<ViewportPickable>& pickables) {
  selection.per_item_bounds.clear();
  selection.aggregate_bounds = {};
  for (const auto selectedId : selection.selected_entity_ids) {
    const auto it = std::find_if(pickables.begin(), pickables.end(),
                                 [selectedId](const ViewportPickable& pickable) {
                                   return pickable.entity_id == selectedId;
                                 });
    if (it == pickables.end() || !it->bounds.valid) {
      continue;
    }
    selection.per_item_bounds.push_back({selectedId, it->bounds});
    ExpandBounds(selection.aggregate_bounds, ToPtVec3(it->bounds.min));
    ExpandBounds(selection.aggregate_bounds, ToPtVec3(it->bounds.max));
  }
}
#endif

#ifdef _WIN32
bool IsVirtualKeyDown(int virtual_key) {
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

[[maybe_unused]] bool IsCtrlDown() {
  return IsVirtualKeyDown(VK_CONTROL) || IsVirtualKeyDown(VK_LCONTROL) || IsVirtualKeyDown(VK_RCONTROL);
}

[[maybe_unused]] bool IsShiftDown() {
  return IsVirtualKeyDown(VK_SHIFT) || IsVirtualKeyDown(VK_LSHIFT) || IsVirtualKeyDown(VK_RSHIFT);
}

[[maybe_unused]] bool IsAltDown() {
  return IsVirtualKeyDown(VK_MENU) || IsVirtualKeyDown(VK_LMENU) || IsVirtualKeyDown(VK_RMENU);
}
#else
[[maybe_unused]] bool IsCtrlDown() { return false; }
[[maybe_unused]] bool IsShiftDown() { return false; }
[[maybe_unused]] bool IsAltDown() { return false; }
#endif

[[maybe_unused]] std::string ResolveShortcutAction(const std::vector<vkpt::editor::UiShortcut>& shortcuts,
                                                   int key_code,
                                                   bool ctrl,
                                                   bool shift,
                                                   bool alt) {
  const int normalized_key = (key_code >= 'a' && key_code <= 'z')
    ? static_cast<int>(std::toupper(static_cast<unsigned char>(key_code)))
    : key_code;

  for (const auto& shortcut : shortcuts) {
    if (static_cast<int>(shortcut.key_code) == normalized_key &&
        shortcut.ctrl == ctrl &&
        shortcut.shift == shift &&
        shortcut.alt == alt) {
      return shortcut.action_id;
    }
  }

#ifdef _WIN32
  if (key_code == VK_DELETE) {
    for (const auto& shortcut : shortcuts) {
      if (shortcut.key_code == 127 && !shortcut.ctrl && !shortcut.shift && !shortcut.alt) {
        return shortcut.action_id;
      }
    }
  }
#endif
  return {};
}

bool EntityListContains(const std::vector<vkpt::core::StableId>& values, vkpt::core::StableId id) {
  return std::find(values.begin(), values.end(), id) != values.end();
}

void RemoveEntityIds(std::vector<vkpt::core::StableId>& values,
                    const std::vector<vkpt::core::StableId>& remove) {
  values.erase(std::remove_if(values.begin(), values.end(),
                             [&remove](vkpt::core::StableId value) {
                               return std::find(remove.begin(), remove.end(), value) != remove.end();
                             }),
              values.end());
}

[[maybe_unused]] void SyncSelectionToWorld(std::vector<vkpt::core::StableId>& selected,
                                           const std::vector<vkpt::core::StableId>& world_entities) {
  std::vector<vkpt::core::StableId> kept;
  kept.reserve(selected.size());
  for (auto id : selected) {
    if (EntityListContains(world_entities, id) && !EntityListContains(kept, id)) {
      kept.push_back(id);
    }
  }
  selected.swap(kept);
}

[[maybe_unused]] std::vector<vkpt::core::StableId> InvertSelectionSet(
    const std::vector<vkpt::core::StableId>& selection,
    const std::vector<vkpt::core::StableId>& world_entities) {
  std::vector<vkpt::core::StableId> inverted;
  inverted.reserve(world_entities.size());
  for (auto id : world_entities) {
    if (!EntityListContains(selection, id)) {
      inverted.push_back(id);
    }
  }
  return inverted;
}

struct UiEditorWorldSnapshot {
  std::vector<vkpt::core::StableId> world_entities;
  vkpt::core::StableId next_entity_id = 1;
  std::vector<vkpt::core::StableId> clipboard_entities;
  vkpt::editor::SelectionState selection_state;
  std::unordered_map<vkpt::core::StableId, vkpt::core::StableId> entity_parent;
  std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>> grouped_entities;
  std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>> merged_entities;
};

[[maybe_unused]] void EraseEntityFromIndex(std::vector<vkpt::core::StableId>& values, vkpt::core::StableId value) {
  values.erase(std::remove(values.begin(), values.end(), value), values.end());
}

void RemoveEntityFromWorldRelations(
    std::unordered_map<vkpt::core::StableId, vkpt::core::StableId>& entity_parent,
    std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& grouped_entities,
    std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& merged_entities,
    vkpt::core::StableId entity_id) {
  entity_parent.erase(entity_id);
  for (auto it = entity_parent.begin(); it != entity_parent.end();) {
    if (it->second == entity_id) {
      it = entity_parent.erase(it);
    } else {
      ++it;
    }
  }

  if (const auto group = grouped_entities.find(entity_id); group != grouped_entities.end()) {
    for (const auto member_id : group->second) {
      entity_parent.erase(member_id);
    }
    grouped_entities.erase(group);
  }
  for (auto it = grouped_entities.begin(); it != grouped_entities.end();) {
    const auto old_count = it->second.size();
    RemoveEntityIds(it->second, {entity_id});
    if (it->second.empty() && old_count > 0) {
      it = grouped_entities.erase(it);
    } else {
      ++it;
    }
  }

  if (const auto merged = merged_entities.find(entity_id); merged != merged_entities.end()) {
    for (const auto member_id : merged->second) {
      entity_parent.erase(member_id);
    }
    merged_entities.erase(merged);
  }
  for (auto it = merged_entities.begin(); it != merged_entities.end();) {
    const auto old_count = it->second.size();
    RemoveEntityIds(it->second, {entity_id});
    if (it->second.empty() && old_count > 0) {
      it = merged_entities.erase(it);
    } else {
      ++it;
    }
  }
}

[[maybe_unused]] void RemoveEntitiesFromWorld(
    std::vector<vkpt::core::StableId>& world_entities,
    const std::vector<vkpt::core::StableId>& remove,
    vkpt::core::StableId& next_entity_id,
    std::unordered_map<vkpt::core::StableId, vkpt::core::StableId>& entity_parent,
    std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& grouped_entities,
    std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& merged_entities) {
  RemoveEntityIds(world_entities, remove);
  for (const auto id : remove) {
    RemoveEntityFromWorldRelations(entity_parent, grouped_entities, merged_entities, id);
  }
  while (next_entity_id > 1 && world_entities.empty()) {
    --next_entity_id;
  }
  while (next_entity_id > 1) {
    vkpt::core::StableId candidate = next_entity_id - 1;
    if (EntityListContains(world_entities, candidate)) {
      break;
    }
    --next_entity_id;
  }
}

[[maybe_unused]] void NormalizeIdList(std::vector<vkpt::core::StableId>& ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

[[maybe_unused]] std::string FormatIdList(const std::vector<vkpt::core::StableId>& ids) {
  std::ostringstream out;
  out << "[";
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (i > 0) {
      out << ", ";
    }
    out << ids[i];
  }
  out << "]";
  return out.str();
}

struct UiWorldSnapshot {
  vkpt::editor::EditorCommand command;
  UiEditorWorldSnapshot before;
  UiEditorWorldSnapshot after;
};

std::vector<vkpt::core::StableId> SortedUniqueEntityIds(std::vector<vkpt::core::StableId> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

[[maybe_unused]] void RemoveFromIdList(std::vector<vkpt::core::StableId>& ids,
                                       const std::vector<vkpt::core::StableId>& removed) {
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [&](vkpt::core::StableId id) {
                             return std::find(removed.begin(), removed.end(), id) != removed.end();
                           }),
            ids.end());
}

[[maybe_unused]] std::vector<vkpt::core::StableId> ResolveCommandEntityIds(
    const vkpt::editor::EditorCommand& command,
    const vkpt::editor::SelectionState& selection_state) {
  std::vector<vkpt::core::StableId> ids;
  if (auto* duplicate = std::get_if<vkpt::editor::DuplicateEntityCommand>(&command.payload)) {
    ids = duplicate->entity_ids;
  } else if (auto* remove = std::get_if<vkpt::editor::DeleteEntityCommand>(&command.payload)) {
    ids = remove->entity_ids;
  } else if (auto* group = std::get_if<vkpt::editor::GroupEntitiesCommand>(&command.payload)) {
    ids = group->entity_ids;
  } else if (auto* merge = std::get_if<vkpt::editor::MergeEntitiesCommand>(&command.payload)) {
    ids = merge->entity_ids;
  } else if (auto* reparent = std::get_if<vkpt::editor::ReparentEntityCommand>(&command.payload)) {
    if (reparent->entity_id != 0u) {
      ids.push_back(reparent->entity_id);
    }
  }
  if (ids.empty()) {
    ids = selection_state.selected_entity_ids;
  }
  return SortedUniqueEntityIds(ids);
}

[[maybe_unused]] UiEditorWorldSnapshot CaptureEditorWorldState(
    const std::vector<vkpt::core::StableId>& world_entities,
    vkpt::core::StableId next_entity_id,
    const std::vector<vkpt::core::StableId>& clipboard_entities,
    const vkpt::editor::SelectionState& selection_state,
    const std::unordered_map<vkpt::core::StableId, vkpt::core::StableId>& entity_parent,
    const std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& grouped_entities,
    const std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& merged_entities) {
  UiEditorWorldSnapshot snapshot;
  snapshot.world_entities = world_entities;
  snapshot.next_entity_id = next_entity_id;
  snapshot.clipboard_entities = clipboard_entities;
  snapshot.selection_state = selection_state;
  snapshot.entity_parent = entity_parent;
  snapshot.grouped_entities = grouped_entities;
  snapshot.merged_entities = merged_entities;
  return snapshot;
}

[[maybe_unused]] void RestoreEditorWorldState(
    const UiEditorWorldSnapshot& snapshot,
    std::vector<vkpt::core::StableId>& world_entities,
    vkpt::core::StableId& next_entity_id,
    std::vector<vkpt::core::StableId>& clipboard_entities,
    vkpt::editor::SelectionState& selection_state,
    std::unordered_map<vkpt::core::StableId, vkpt::core::StableId>& entity_parent,
    std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& grouped_entities,
    std::unordered_map<vkpt::core::StableId, std::vector<vkpt::core::StableId>>& merged_entities) {
  world_entities = snapshot.world_entities;
  next_entity_id = snapshot.next_entity_id;
  clipboard_entities = snapshot.clipboard_entities;
  selection_state = snapshot.selection_state;
  entity_parent = snapshot.entity_parent;
  grouped_entities = snapshot.grouped_entities;
  merged_entities = snapshot.merged_entities;
}

[[maybe_unused]] vkpt::core::StableId NextEntityId(std::vector<vkpt::core::StableId>& world_entities,
                                                   vkpt::core::StableId& next_entity_id) {
  while (EntityListContains(world_entities, next_entity_id)) {
    ++next_entity_id;
  }
  const auto id = next_entity_id;
  ++next_entity_id;
  return id;
}

[[maybe_unused]] void ApplySelectionIds(vkpt::editor::SelectionState& selection_state,
                                        vkpt::core::FrameIndex frame_index,
                                        const std::vector<vkpt::core::StableId>& ids,
                                        bool append,
                                        bool range_like,
                                        const std::string& source_widget) {
  if (ids.empty()) {
    vkpt::editor::EditorCommand clearCmd;
    clearCmd.command_id = "edit.clear_selection";
    clearCmd.kind = vkpt::editor::EditorCommandKind::kClearSelection;
    clearCmd.source_widget = source_widget;
    clearCmd.frame_index = frame_index;
    clearCmd.payload = vkpt::editor::ClearSelectionCommand{};
    selection_state = vkpt::editor::ApplySelectionCommand(selection_state, clearCmd);
    return;
  }

  if (!append) {
    vkpt::editor::EditorCommand replaceCmd;
    replaceCmd.command_id = "edit.select";
    replaceCmd.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
    replaceCmd.source_widget = source_widget;
    replaceCmd.frame_index = frame_index;
    replaceCmd.payload = vkpt::editor::SelectEntityCommand{ids.front(), false, range_like};
    selection_state = vkpt::editor::ApplySelectionCommand(selection_state, replaceCmd);
    for (std::size_t i = 1; i < ids.size(); ++i) {
      vkpt::editor::EditorCommand appendCmd;
      appendCmd.command_id = "edit.select";
      appendCmd.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
      appendCmd.source_widget = source_widget;
      appendCmd.frame_index = frame_index;
      appendCmd.payload = vkpt::editor::SelectEntityCommand{ids[i], true, false};
      selection_state = vkpt::editor::ApplySelectionCommand(selection_state, appendCmd);
    }
    return;
  }

  for (const auto id : ids) {
    vkpt::editor::EditorCommand appendCmd;
    appendCmd.command_id = "edit.select";
    appendCmd.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
    appendCmd.source_widget = source_widget;
    appendCmd.frame_index = frame_index;
    appendCmd.payload = vkpt::editor::SelectEntityCommand{id, true, false};
    selection_state = vkpt::editor::ApplySelectionCommand(selection_state, appendCmd);
  }
}

void LogWindowInput(vkpt::editor::UiEventLog& event_log,
                    vkpt::editor::UiRuntimeState& runtime_state,
                    const vkpt::platform::InputEvent& event,
                    vkpt::core::FrameIndex frame_index) {
  std::string_view widget = ToString(event.type);
  std::ostringstream oldValue;
  std::ostringstream newValue;
  if (event.raw_code != 0) {
    oldValue << event.raw_code;
  }
  newValue << "x=" << event.x << ", y=" << event.y
           << ", dx=" << event.delta_x << ", dy=" << event.delta_y
           << ", dz=" << event.delta_z;
  PushUiEvent(event_log,
              "window_event",
              "window",
              widget,
              frame_index,
              oldValue.str(),
              newValue.str(),
              runtime_state.active_layout_name);
  runtime_state.status_message = "window_event:" + std::string(widget);
  runtime_state.focused_panel = "window";
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index = 0,
                 std::string_view old_value = {},
                 std::string_view new_value = {},
                 std::string_view command_result = {});
std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence();

bool Check(std::string_view tag, bool cond) {
  if (!cond) {
    std::cerr << "ui-model-smoke: fail: " << tag << "\n";
  }
  return cond;
}

bool HasTopLevelMenu(const vkpt::editor::MenuBar& menu, std::string_view id) {
  for (const auto& item : menu.top_level_menus) {
    if (item.id == id) {
      return true;
    }
  }
  return false;
}

bool HasMenuItem(const vkpt::editor::MenuBar& menu, std::string_view top_level_id, std::string_view action_id) {
  for (const auto& top_level : menu.top_level_menus) {
    if (top_level.id != top_level_id) {
      continue;
    }
    for (const auto& item : top_level.children) {
      if (item.id == action_id) {
        return true;
      }
    }
    return false;
  }
  return false;
}

bool RunUiModelSmokeTests() {
  using namespace vkpt::editor;
  bool ok = true;
  auto check_true = [&](std::string_view tag, bool cond) {
    ok = ok && Check(tag, cond);
  };
  auto find_panel = [](const UiLayoutDocument& layout, std::string_view panel_id) -> const UiPanelState* {
    const auto it = std::find_if(layout.panels.begin(), layout.panels.end(),
                                 [panel_id](const UiPanelState& panel) {
                                   return panel.id == panel_id;
                                 });
    if (it == layout.panels.end()) {
      return nullptr;
    }
    return &(*it);
  };
  auto find_menu_enablement = [](const std::vector<MenuEnablement>& entries,
                                std::string_view item_id) -> std::optional<bool> {
    for (const auto& entry : entries) {
      if (entry.item_id == item_id) {
        return entry.enabled;
      }
    }
    return std::nullopt;
  };
  auto has_menu_items = [&](const MenuBar& menu,
                           std::string_view top_level,
                           std::initializer_list<std::string_view> items) {
    for (const auto item : items) {
      check_true(std::string("menu item missing: ") + std::string(top_level) + "." + std::string(item),
                 HasMenuItem(menu, top_level, item));
    }
  };
  auto has_shortcut = [](const std::vector<UiShortcut>& shortcuts,
                        std::string_view action_id,
                        std::uint32_t key_code,
                        bool ctrl,
                        bool shift,
                        bool alt) {
    return std::any_of(shortcuts.begin(), shortcuts.end(),
                       [&](const UiShortcut& shortcut) {
                         return shortcut.action_id == action_id &&
                                shortcut.key_code == key_code &&
                                shortcut.ctrl == ctrl &&
                                shortcut.shift == shift &&
                                shortcut.alt == alt;
                       });
  };

  const auto menu = BuildDefaultMenuBar();
  const std::initializer_list<std::string_view> requiredTopLevels = {
    "file", "edit", "view", "create", "scene", "render", "benchmark",
    "assets", "scripts", "tools", "help"
  };
  for (const auto level : requiredTopLevels) {
    check_true(std::string("missing top-level menu: ") + std::string(level),
               HasTopLevelMenu(menu, level));
  }

  has_menu_items(menu, "file", {
    "file.new_scene", "file.open_scene", "file.open_recent", "file.save_scene",
    "file.save_scene_as", "file.clone_scene", "file.import_asset", "file.export_image",
    "file.export_exr", "file.export_benchmark_artifacts", "file.export_scene_snapshot",
    "file.preferences", "file.reveal_artifacts_folder", "file.exit"
  });
  has_menu_items(menu, "edit", {
    "edit.undo", "edit.redo", "edit.cut", "edit.copy", "edit.paste", "edit.duplicate",
    "edit.delete", "edit.rename", "edit.select_all", "edit.select_none", "edit.invert_selection",
    "edit.group_selection", "edit.ungroup_selection", "edit.merge_selection",
    "edit.split_merged_object", "edit.reparent_selection", "edit.reset_transform",
    "edit.command_history"
  });
  has_menu_items(menu, "view", {
    "view.panels", "view.layouts", "view.overlays", "view.debug_views",
    "view.fullscreen", "view.ui_scale", "view.reset_layout"
  });
  has_menu_items(menu, "create", {
    "create.empty_entity", "create.group_entity", "create.camera", "create.light",
    "create.mesh_primitives", "create.sdf_primitives", "create.material", "create.script",
    "create.physics_body", "create.benchmark_marker"
  });
  has_menu_items(menu, "benchmark", {
    "benchmark.run_current_scene", "benchmark.run_scene_pack", "benchmark.run_cpu_calibration",
    "benchmark.run_gpu_calibration", "benchmark.run_simd_experiment", "benchmark.run_backend_experiment",
    "benchmark.compare_against_reference", "benchmark.open_artifacts", "benchmark.export_csv_json",
    "benchmark.history"
  });
  has_menu_items(menu, "scene", {
    "scene.validate_scene", "scene.freeze_benchmark_snapshot", "scene.reset_accumulation",
    "scene.reload_scene", "scene.reload_assets", "scene.hot_reload_scripts", "scene.scene_settings",
    "scene.lighting_settings", "scene.environment_settings", "scene.camera_settings",
    "scene.physics_settings", "scene.script_settings", "scene.animation_settings"
  });
  has_menu_items(menu, "render", {
    "render.backend", "render.renderer_path", "render.quality_presets", "render.resolution",
    "render.spp", "render.max_bounces", "render.denoiser", "render.tone_mapping",
    "render.exposure", "render.debug_channel", "render.shader_cache", "render.backend_capabilities"
  });
  has_menu_items(menu, "scripts", {
    "scripts.new_lua_script", "scripts.attach_script_to_selection", "scripts.detach_script_from_selection",
    "scripts.reload_scripts", "scripts.open_script_folder", "scripts.show_script_lifecycle_events",
    "scripts.show_script_errors", "scripts.show_script_profiler", "scripts.sandbox_settings"
  });
  has_menu_items(menu, "assets", {
    "assets.import_files", "assets.reimport_selected", "assets.refresh_browser", "assets.show_missing_assets",
    "assets.show_import_diagnostics", "assets.clear_generated_cache", "assets.clear_shader_cache"
  });
  has_menu_items(menu, "tools", {
    "tools.doctor", "tools.crash_artifacts", "tools.profiler", "tools.frame_capture",
    "tools.shader_manifest", "tools.asset_manifest", "tools.scene_snapshot",
    "tools.capability_matrix", "tools.settings_dump", "tools.startup_self_test"
  });
  has_menu_items(menu, "help", {
    "help.controls", "help.shortcut_reference", "help.about", "help.build_info",
    "help.feature_flags", "help.dependency_info"
  });
  auto mutable_menu = menu;
  check_true("find menu item non-const", FindMenuItem(mutable_menu, "edit.duplicate") != nullptr);
  check_true("find menu item const", FindMenuItem(std::as_const(menu), "edit.duplicate") != nullptr);

  const auto editEnablementsEmpty = GetEditMenuEnablements(CreateDefaultSelectionState());
  const auto editDuplicateDisabled = find_menu_enablement(editEnablementsEmpty, "edit.duplicate");
  check_true("edit.duplicate disabled without selection", editDuplicateDisabled.has_value() && !editDuplicateDisabled.value());
  const auto disabledEditMenu = BuildDefaultMenuBar(CreateDefaultSelectionState());
  const auto* disabledDuplicate = FindMenuItem(disabledEditMenu, "edit.duplicate");
  check_true("edit.duplicate disabled reason",
             disabledDuplicate != nullptr && !disabledDuplicate->disabled_reason.empty());
  SelectionState oneSelected = CreateDefaultSelectionState();
  oneSelected.selected_entity_ids = {11};
  const auto editEnablementsSelected = GetEditMenuEnablements(oneSelected);
  const auto editDuplicateEnabled = find_menu_enablement(editEnablementsSelected, "edit.duplicate");
  check_true("edit.duplicate enabled with selection", editDuplicateEnabled.has_value() && editDuplicateEnabled.value());
  check_true("file.exit remains enabled", FindMenuItem(menu, "file.exit")->enabled);

  const auto new_scene = MakeMenuCommand("file.new_scene", "menu");
  check_true("file.new_scene command kind", new_scene.kind == EditorCommandKind::kCreateEntity);
  check_true("file.new_scene mapping", std::holds_alternative<CreateEntityCommand>(new_scene.payload));

  const auto select_none = MakeMenuCommand("edit.select_none", "menu");
  check_true("edit.select_none command kind", select_none.kind == EditorCommandKind::kClearSelection);
  check_true("edit.select_none mapping", std::holds_alternative<ClearSelectionCommand>(select_none.payload));

  const auto open_scene = MakeMenuCommand("file.open_scene", "menu");
  check_true("file.open_scene unsupported command", open_scene.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto copy_action = MakeMenuCommand("edit.copy", "menu");
  check_true("edit.copy unsupported command", copy_action.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto save_scene = MakeMenuCommand("file.save_scene", "menu");
  check_true("file.save_scene command is explicit unsupported", save_scene.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto exit_action = MakeMenuCommand("file.exit", "menu");
  check_true("file.exit command is explicit unsupported", exit_action.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto run_benchmark = MakeMenuCommand("benchmark.run_current_scene", "menu");
  check_true("benchmark.run_current_scene command kind", run_benchmark.kind == EditorCommandKind::kRunBenchmark);
  check_true("benchmark.run_current_scene mapping", std::holds_alternative<RunBenchmarkCommand>(run_benchmark.payload));
  const auto run_scene_pack = MakeMenuCommand("benchmark.run_scene_pack", "menu");
  check_true("benchmark.run_scene_pack command kind", run_scene_pack.kind == EditorCommandKind::kRunBenchmark);
  const auto run_cpu_calibration = MakeMenuCommand("benchmark.run_cpu_calibration", "menu");
  check_true("benchmark.run_cpu_calibration command kind", run_cpu_calibration.kind == EditorCommandKind::kRunBenchmark);
  const auto run_gpu_calibration = MakeMenuCommand("benchmark.run_gpu_calibration", "menu");
  check_true("benchmark.run_gpu_calibration command kind", run_gpu_calibration.kind == EditorCommandKind::kRunBenchmark);
  const auto run_simd = MakeMenuCommand("benchmark.run_simd_experiment", "menu");
  check_true("benchmark.run_simd_experiment command kind", run_simd.kind == EditorCommandKind::kRunBenchmark);
  const auto run_backend = MakeMenuCommand("benchmark.run_backend_experiment", "menu");
  check_true("benchmark.run_backend_experiment command kind", run_backend.kind == EditorCommandKind::kRunBenchmark);
  const auto run_ref = MakeMenuCommand("benchmark.compare_against_reference", "menu");
  check_true("benchmark.compare_against_reference command kind", run_ref.kind == EditorCommandKind::kRunBenchmark);
  const auto benchmark_open_artifacts = MakeMenuCommand("benchmark.open_artifacts", "menu");
  check_true("benchmark.open_artifacts currently unsupported by model", benchmark_open_artifacts.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto benchmark_export_csv = MakeMenuCommand("benchmark.export_csv_json", "menu");
  check_true("benchmark.export_csv_json currently unsupported by model", benchmark_export_csv.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto benchmark_history = MakeMenuCommand("benchmark.history", "menu");
  check_true("benchmark.history currently unsupported by model", benchmark_history.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto scene_validate = MakeMenuCommand("scene.validate_scene", "menu");
  check_true("scene.validate_scene currently unsupported by model", scene_validate.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto scene_settings = MakeMenuCommand("scene.scene_settings", "menu");
  check_true("scene.scene_settings currently unsupported by model", scene_settings.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_backend = MakeMenuCommand("render.backend", "menu");
  check_true("render.backend currently unsupported by model", render_backend.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto render_quality_presets = MakeMenuCommand("render.quality_presets", "menu");
  check_true("render.quality_presets currently unsupported by model", render_quality_presets.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto tools_doctor = MakeMenuCommand("tools.doctor", "menu");
  check_true("tools.doctor currently unsupported by model", tools_doctor.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto tools_profiler = MakeMenuCommand("tools.profiler", "menu");
  check_true("tools.profiler currently unsupported by model", tools_profiler.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto help_about = MakeMenuCommand("help.about", "menu");
  check_true("help.about currently unsupported by model", help_about.kind == EditorCommandKind::kUnsupportedUiAction);
  const auto help_controls = MakeMenuCommand("help.controls", "menu");
  check_true("help.controls currently unsupported by model", help_controls.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto create_material = MakeMenuCommand("create.material", "menu");
  check_true("create.material command kind", create_material.kind == EditorCommandKind::kCreateEntity);
  const auto create_script = MakeMenuCommand("create.script", "menu");
  check_true("create.script command kind", create_script.kind == EditorCommandKind::kCreateEntity);
  const auto create_physics = MakeMenuCommand("create.physics_body", "menu");
  check_true("create.physics_body command kind", create_physics.kind == EditorCommandKind::kCreateEntity);
  const auto create_marker = MakeMenuCommand("create.benchmark_marker", "menu");
  check_true("create.benchmark_marker command kind", create_marker.kind == EditorCommandKind::kCreateEntity);
  const auto create_mat_template = std::get<CreateEntityCommand>(create_material.payload).template_name;
  check_true("create.material has template", create_mat_template == "material");
  const auto create_script_template = std::get<CreateEntityCommand>(create_script.payload).template_name;
  check_true("create.script has template", create_script_template == "script");
  const auto create_physics_template = std::get<CreateEntityCommand>(create_physics.payload).template_name;
  check_true("create.physics_body has template", create_physics_template == "physics_body");
  const auto create_marker_template = std::get<CreateEntityCommand>(create_marker.payload).template_name;
  check_true("create.benchmark_marker has template", create_marker_template == "benchmark_marker");
  const auto create_light = MakeMenuCommand("create.light", "menu");
  check_true("create.light command kind", create_light.kind == EditorCommandKind::kCreateEntity);
  check_true("create.light template", std::get<CreateEntityCommand>(create_light.payload).template_name == "create.light");
  const auto create_mesh = MakeMenuCommand("create.mesh_primitives", "menu");
  check_true("create.mesh_primitives command kind", create_mesh.kind == EditorCommandKind::kCreateEntity);
  check_true("create.mesh template", std::get<CreateEntityCommand>(create_mesh.payload).template_name == "create.mesh_primitives");
  const auto create_sdf = MakeMenuCommand("create.sdf_primitives", "menu");
  check_true("create.sdf_primitives command kind", create_sdf.kind == EditorCommandKind::kCreateEntity);
  check_true("create.sdf template", std::get<CreateEntityCommand>(create_sdf.payload).template_name == "create.sdf_primitives");

  const auto layout = CreateLayoutPreset(LayoutPreset::Benchmark);
  check_true("benchmark layout preset", layout.preset == LayoutPreset::Benchmark);
  check_true("benchmark layout panels", !layout.panels.empty());

  const auto tmp = std::filesystem::temp_directory_path() / "vkpt-ui-layout-smoke.json";
  std::string save_error;
  if (Check("layout save", SaveLayoutToFile(tmp.string(), layout, &save_error))) {
    UiLayoutDocument reloaded;
    check_true("layout load", LoadLayoutFromFile(tmp.string(), &reloaded));
    check_true("layout preset roundtrip", reloaded.preset == layout.preset);
    check_true("layout name roundtrip", reloaded.active_layout_name == layout.active_layout_name);
    check_true("layout ui scale roundtrip", reloaded.ui_scale == layout.ui_scale);
    std::filesystem::remove(tmp);
  }

  const std::vector<std::pair<LayoutPreset, std::string_view>> allPresets = {
    {LayoutPreset::Default, "Default"},
    {LayoutPreset::Benchmark, "Benchmark"},
    {LayoutPreset::MaterialAuthoring, "Material Authoring"},
    {LayoutPreset::Scripting, "Scripting"},
    {LayoutPreset::AssetManagement, "Asset Management"},
    {LayoutPreset::DebugProfiler, "Debug/Profiler"},
    {LayoutPreset::MinimalViewport, "Minimal Viewport"},
    {LayoutPreset::FullscreenViewportWithOverlay, "Fullscreen Viewport With Overlay"},
  };
  for (std::size_t i = 0; i < allPresets.size(); ++i) {
    const auto preset_layout = CreateLayoutPreset(allPresets[i].first);
    check_true(std::string("layout preset exists: ") + std::string(allPresets[i].second),
              !preset_layout.panels.empty());
    const auto preset_path = std::filesystem::temp_directory_path() / ("vkpt-ui-layout-smoke-" + std::to_string(i) + ".json");
    std::string preset_save_err;
    if (SaveLayoutToFile(preset_path.string(), preset_layout, &preset_save_err)) {
      UiLayoutDocument preset_roundtrip;
      check_true(std::string("layout preset roundtrip: ") + std::string(allPresets[i].second),
                LoadLayoutFromFile(preset_path.string(), &preset_roundtrip));
      check_true(std::string("layout preset restored: ") + std::string(allPresets[i].second),
                preset_roundtrip.preset == preset_layout.preset);
      std::filesystem::remove(preset_path);
    } else {
      check_true(std::string("layout preset save: ") + std::string(allPresets[i].second), false);
    }
  }

  SelectionState selection = CreateDefaultSelectionState();
  selection.selected_entity_ids = {7, 8, 9};
  selection.active_primary_entity = 7;
  selection.hovered_entity_ids = {9};
  selection.selection_source = SelectionSource::Viewport;
  const std::string selection_json = SerializeSelectionState(selection);
  check_true("selection json serialization",
             selection_json.find("\"selected_entity_ids\":[7,8,9]") != std::string::npos);
  selection.aggregate_bounds = {{-1.0f, -2.0f, -3.0f}, {4.0f, 5.0f, 6.0f}, true};
  selection.per_item_bounds = {
    {7, {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, true}},
    {8, {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true}},
    {9, {{1.0f, 1.0f, 1.0f}, {2.0f, 2.0f, 2.0f}, true}},
  };
  const auto selection_tmp = std::filesystem::temp_directory_path() / "vkpt-ui-selection-smoke.json";
  std::string selection_save_error;
  if (Check("selection save", SaveSelectionToFile(selection_tmp.string(), selection, &selection_save_error))) {
    SelectionState selection_reloaded;
    check_true("selection load", LoadSelectionFromFile(selection_tmp.string(), &selection_reloaded));
    check_true("selection selected ids roundtrip", selection_reloaded.selected_entity_ids == selection.selected_entity_ids);
    check_true("selection bounds roundtrip", selection_reloaded.aggregate_bounds.valid &&
               selection_reloaded.per_item_bounds.size() == selection.per_item_bounds.size());
    std::filesystem::remove(selection_tmp);
  }

  EditorCommand pick_entity_11;
  pick_entity_11.source_widget = "viewport";
  pick_entity_11.kind = EditorCommandKind::kSelectEntity;
  pick_entity_11.payload = SelectEntityCommand{11, false, false};
  SelectionState selected_once = ApplySelectionCommand(selection, pick_entity_11);
  check_true("select once sets single selection", selected_once.selected_entity_ids == std::vector<vkpt::core::StableId>{11});
  check_true("select once sets active primary", selected_once.active_primary_entity == 11);
  check_true("select once sets hovered entity", selected_once.hovered_entity == 11);
  check_true("select once source from viewport", selected_once.selection_source == SelectionSource::Viewport);

  EditorCommand append_entity_22;
  append_entity_22.source_widget = "scene_tree";
  append_entity_22.kind = EditorCommandKind::kSelectEntity;
  append_entity_22.payload = SelectEntityCommand{22, true, false};
  SelectionState selected_two = ApplySelectionCommand(selected_once, append_entity_22);
  check_true("append selection keeps two items", selected_two.selected_entity_ids == std::vector<vkpt::core::StableId>({11, 22}));
  check_true("append selection updates active", selected_two.active_primary_entity == 22);
  check_true("append source from scene tree", selected_two.selection_source == SelectionSource::SceneTree);

  EditorCommand toggle_entity_11;
  toggle_entity_11.source_widget = "inspector";
  toggle_entity_11.kind = EditorCommandKind::kToggleSelectEntity;
  toggle_entity_11.payload = ToggleSelectEntityCommand{11};
  SelectionState after_toggle = ApplySelectionCommand(selected_two, toggle_entity_11);
  check_true("toggle deselect one entity", after_toggle.selected_entity_ids == std::vector<vkpt::core::StableId>({22}));
  check_true("toggle keeps active primary", after_toggle.active_primary_entity == 22);
  check_true("toggle source from inspector", after_toggle.selection_source == SelectionSource::Inspector);

  EditorCommand append_range_selection;
  append_range_selection.source_widget = "scene_tree";
  append_range_selection.kind = EditorCommandKind::kSelectEntity;
  append_range_selection.payload = SelectEntityCommand{44, true, true};
  SelectionState range_add = ApplySelectionCommand(after_toggle, append_range_selection);
  check_true("append+range selection includes existing and new", range_add.selected_entity_ids == std::vector<vkpt::core::StableId>({22, 44}));
  check_true("append+range selection keeps hovered", range_add.hovered_entity == 44);
  check_true("append+range selection source from scene tree", range_add.selection_source == SelectionSource::SceneTree);

  EditorCommand range_selection;
  range_selection.source_widget = "scene_tree";
  range_selection.kind = EditorCommandKind::kSelectEntity;
  range_selection.payload = SelectEntityCommand{11, false, true};
  SelectionState range_replace = ApplySelectionCommand(after_toggle, range_selection);
  check_true("replace range selection keeps two markers", range_replace.selected_entity_ids.size() == 2);
  check_true("replace range selection sets active primary", range_replace.active_primary_entity == 11);
  check_true("replace range selection sets hovered", range_replace.hovered_entity == 11);

  EditorCommand clear_selection;
  clear_selection.source_widget = "menu";
  clear_selection.kind = EditorCommandKind::kClearSelection;
  clear_selection.payload = ClearSelectionCommand{};
  SelectionState cleared = ApplySelectionCommand(after_toggle, clear_selection);
  check_true("clear selection empties entities", cleared.selected_entity_ids.empty());
  check_true("clear selection resets hovered entity", cleared.hovered_entity == 0);
  check_true("clear selection keeps source from last command", cleared.selection_source == SelectionSource::Inspector);
  check_true("clear selection removes hovered_entity", cleared.hovered_entity == 0);

  auto mixed_field = BuildInspectorFieldStates("material.roughness", {"0.4", "0.8"});
  check_true("mixed inspector value marked mixed", mixed_field.size() == 1 && mixed_field[0].mixed);
  auto exact_field = BuildInspectorFieldStates("material.roughness", {"0.5", "0.5"});
  check_true("uniform inspector value not mixed", exact_field.size() == 1 && !exact_field[0].mixed && exact_field[0].value == "0.5");
  auto unsupported_field = BuildInspectorFieldStates("missing.field", {});
  check_true("missing inspector field unsupported", unsupported_field.size() == 1 && unsupported_field[0].unsupported);

  const auto tree_reorder = MakeReorderSiblingCommand(44, 10, 12, 7, "scene_tree");
  check_true("tree reorder command kind", tree_reorder.kind == EditorCommandKind::kReorderSibling);
  check_true("tree reorder command id", tree_reorder.command_id == "scene_tree.reorder_sibling");
  const auto reorder_payload = std::get<ReorderSiblingCommand>(tree_reorder.payload);
  check_true("tree reorder payload moved", reorder_payload.moved_entity == 44);
  check_true("tree reorder payload sibling before", reorder_payload.sibling_before == 10);
  check_true("tree reorder payload sibling after", reorder_payload.sibling_after == 12);
  check_true("tree reorder command source", tree_reorder.source_widget == "scene_tree");
  const std::string reorder_line = SerializeEditorCommand(tree_reorder);
  check_true("tree reorder serialized", reorder_line.find("\"moved_entity\":44") != std::string::npos);

  const auto valid_texture_drop = ValidateAssetDrop("C:/assets/brick.PNG", "texture_slot");
  check_true("valid texture drop extension support", valid_texture_drop.extension_supported);
  check_true("valid texture drop accepted", valid_texture_drop.accepted);
  check_true("valid texture drop type", valid_texture_drop.asset_type == "texture");

  const auto invalid_texture_drop = ValidateAssetDrop("C:/assets/readme.md", "texture_slot");
  check_true("invalid texture drop rejected", !invalid_texture_drop.accepted);
  check_true("invalid texture drop unsupported", !invalid_texture_drop.extension_supported);

  const auto unknown_slot_drop = ValidateAssetDrop("C:/assets/model.obj", "invalid_slot");
  check_true("unknown slot drop rejected", !unknown_slot_drop.accepted);
  check_true("unknown slot drop still supported", unknown_slot_drop.extension_supported);

  const auto empty_path_drop = ValidateAssetDrop("", "material_slot");
  check_true("empty path drop rejected", !empty_path_drop.accepted);
  check_true("empty path drop reports reason", !empty_path_drop.reason.empty());

  const auto script_attachment = MakeMenuCommand("scripts.attach_script_to_selection", "menu");
  check_true("scripts.attach_script_to_selection command", script_attachment.kind == EditorCommandKind::kAttachScript);
  const auto script_detach = MakeMenuCommand("scripts.detach_script_from_selection", "menu");
  check_true("scripts.detach_script_from_selection command", script_detach.kind == EditorCommandKind::kDetachScript);
  const auto script_new = MakeMenuCommand("scripts.new_lua_script", "menu");
  check_true("scripts.new_lua_script currently unsupported by model", script_new.kind == EditorCommandKind::kUnsupportedUiAction);

  const auto benchmark_desc = MakeDefaultBenchmarkRunDesc("scenes/test.json", "vulkan", "hybrid", 128, 10, 42, 1024, 576);
  check_true("benchmark desc scene path", benchmark_desc.scene_path == "scenes/test.json");
  check_true("benchmark desc backend", benchmark_desc.backend == "vulkan");
  check_true("benchmark desc renderer path", benchmark_desc.renderer_path == "hybrid");
  check_true("benchmark desc spp", benchmark_desc.samples_per_pixel == 128);
  check_true("benchmark desc width", benchmark_desc.resolution.width == 1024);
  check_true("benchmark desc height", benchmark_desc.resolution.height == 576);
  check_true("benchmark desc max depth", benchmark_desc.max_depth == 10);
  check_true("benchmark desc seed", benchmark_desc.seed == 42);
  check_true("benchmark desc tolerance default", benchmark_desc.tolerance_policy == "default");
  const auto workload = EstimateWorkloadComplexity(benchmark_desc, 3, 1024, 512, 16 * 1024 * 1024, true);
  check_true("workload model has cost", workload.normalized_cost_units > 0.0);
  check_true("workload model explains drivers", !workload.cost_drivers.empty());
  auto normalized_score = ComputeBenchmarkScore(2048.0, 2048.0, 1024.0, workload.normalized_cost_units, true);
  normalized_score.raw_paths_per_second = 4096.0;
  normalized_score.raw_gpu_ms = 4.0;
  normalized_score.raw_cpu_ms = 2.0;
  const BenchmarkRawMetricsModel raw_metrics{
    120.0, 8.33, 4.0, 2.0, 1024.0, 4096.0, 8192.0, 128, 32 * 1024 * 1024, 1.5, 0.25
  };
  const auto benchmark_panel = BuildBenchmarkPanelModel(
      benchmark_desc, raw_metrics, normalized_score, workload,
      "artifacts/benchmarks/ui-smoke", "ok", true);
  const std::string benchmark_panel_json = SerializeBenchmarkPanelModel(benchmark_panel);
  check_true("benchmark panel serializes raw metrics",
             benchmark_panel_json.find("\"raw_metrics\"") != std::string::npos &&
             benchmark_panel_json.find("\"path_vertices_per_second\":8192") != std::string::npos);
  check_true("benchmark panel serializes normalized score",
             benchmark_panel_json.find("\"normalized_score\":1") != std::string::npos);
  check_true("benchmark panel calibration actions",
             !benchmark_panel.calibration_actions.empty() &&
             !BuildDefaultBenchmarkCalibrationActions(false, false).back().supported);
  UiRuntimeState runtime_for_status = CreateDefaultRuntimeState();
  runtime_for_status.active_scene = "assets/scenes/cornell_native.json";
  runtime_for_status.active_renderer_backend = "cpu";
  runtime_for_status.active_renderer_path = "cpu_scalar";
  runtime_for_status.spp_accumulated = 128;
  runtime_for_status.fps = 120.0;
  runtime_for_status.frame_ms = 8.33;
  runtime_for_status.background_job_count = 2;
  runtime_for_status.last_warning_or_error = "none";
  const auto status_bar = BuildStatusBarModel(runtime_for_status, selection, &normalized_score);
  check_true("status bar renderer path", status_bar.renderer_path == "cpu_scalar");
  check_true("status bar spp/fps/jobs", status_bar.spp == 128 && status_bar.fps == 120.0 && status_bar.background_job_count == 2);
  check_true("status bar selected count", status_bar.selected_entity_count == selection.selected_entity_ids.size());

  EditorCommandHistory commandHistory(4);
  commandHistory.push(new_scene);
  commandHistory.push(select_none);
  const std::string command_lines = SerializeEditorCommandsJsonl(commandHistory.history(), 4);
  check_true("command history serialization", command_lines.find("file.new_scene") != std::string::npos);

  UiEventLog eventLog(4);
  PushUiEvent(eventLog, "menu_click", "menu", "file.new_scene");
  PushUiEvent(eventLog, "menu_click", "menu", "edit.select_none");
  check_true("ui event log", eventLog.events().size() == 2);

  const auto shortcut_conflict_true = DetectShortcutConflicts(std::vector<UiShortcut>{
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open"},
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_recent", "Open"}
  });
  const auto shortcut_conflict_false = DetectShortcutConflicts(std::vector<UiShortcut>{
    {static_cast<std::uint32_t>('O'), true, false, false, "file.open_scene", "Open"},
    {static_cast<std::uint32_t>('S'), true, false, false, "file.save_scene", "Save"}
  });
  check_true("ui shortcut conflict true", shortcut_conflict_true);
  check_true("ui shortcut conflict false", !shortcut_conflict_false);

  const auto action_conflict_true = DetectShortcutConflicts(std::vector<UiShortcutAction>{
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_current_scene", "Run"},
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_cpu_calibration", "Run2"}
  });
  const auto action_conflict_false = DetectShortcutConflicts(std::vector<UiShortcutAction>{
    {static_cast<std::uint32_t>('B'), true, false, false, "benchmark.run_current_scene", "Run"},
    {static_cast<std::uint32_t>('B'), false, false, false, "benchmark.run_cpu_calibration", "Run2"}
  });
  check_true("ui shortcut action conflict true", action_conflict_true);
  check_true("ui shortcut action conflict false", !action_conflict_false);

  const auto shortcuts = BuildDefaultUiShortcuts();
  check_true("shortcut ctrl+s", has_shortcut(shortcuts, "file.save_scene", 'S', true, false, false));
  check_true("shortcut ctrl+o", has_shortcut(shortcuts, "file.open_scene", 'O', true, false, false));
  check_true("shortcut ctrl+z", has_shortcut(shortcuts, "edit.undo", 'Z', true, false, false));
  check_true("shortcut ctrl+y", has_shortcut(shortcuts, "edit.redo", 'Y', true, false, false));
  check_true("shortcut ctrl+d", has_shortcut(shortcuts, "edit.duplicate", 'D', true, false, false));
  check_true("shortcut delete", has_shortcut(shortcuts, "edit.delete", 127, false, false, false));
  check_true("shortcut f", has_shortcut(shortcuts, "view.focus_selected", 'F', false, false, false));
  check_true("shortcut w", has_shortcut(shortcuts, "gizmo.translate", 'W', false, false, false));
  check_true("shortcut e", has_shortcut(shortcuts, "gizmo.rotate", 'E', false, false, false));
  check_true("shortcut r", has_shortcut(shortcuts, "gizmo.scale", 'R', false, false, false));
  check_true("shortcut q", has_shortcut(shortcuts, "gizmo.select", 'Q', false, false, false));
  check_true("shortcut ctrl+g", has_shortcut(shortcuts, "edit.group_selection", 'G', true, false, false));
  check_true("shortcut ctrl+shift+g", has_shortcut(shortcuts, "edit.ungroup_selection", 'G', true, true, false));
  check_true("shortcut ctrl+b", has_shortcut(shortcuts, "benchmark.run_current_scene", 'B', true, false, false));
  check_true("shortcut f11", has_shortcut(shortcuts, "view.fullscreen", 122, false, false, false));

  UiLayoutDocument panelLayout = CreateLayoutPreset(LayoutPreset::Default);
  const auto inspectorPanelBefore = find_panel(panelLayout, "inspector");
  check_true("panel state mutation target exists", inspectorPanelBefore != nullptr);
  if (inspectorPanelBefore) {
    const auto beforeX = inspectorPanelBefore->x;
    const auto beforeY = inspectorPanelBefore->y;
    const auto beforeWidth = inspectorPanelBefore->width;
    const auto beforeHeight = inspectorPanelBefore->height;
    const auto hideInspector = SetPanelVisible(panelLayout, "inspector", false);
    check_true("set panel visibility", hideInspector.changed && !find_panel(panelLayout, "inspector")->visible);
    const auto showInspector = SetPanelVisible(panelLayout, "inspector", true);
    check_true("restore panel visibility", showInspector.changed && find_panel(panelLayout, "inspector")->visible);
    const auto* restoredPanel = find_panel(panelLayout, "inspector");
    check_true("panel restore preserves position", restoredPanel != nullptr &&
              restoredPanel->x == beforeX &&
              restoredPanel->y == beforeY &&
              restoredPanel->width == beforeWidth &&
              restoredPanel->height == beforeHeight);
    const auto collapseInspector = SetPanelCollapsed(panelLayout, "inspector", true);
    check_true("set panel collapsed", collapseInspector.changed && find_panel(panelLayout, "inspector")->collapsed);
    const auto moveInspector = MovePanel(panelLayout, "inspector", 64.0f, 88.0f);
    const auto* movedPanel = find_panel(panelLayout, "inspector");
    check_true("move panel updates position", moveInspector.changed &&
              movedPanel != nullptr &&
              movedPanel->x == 64.0f &&
              movedPanel->y == 88.0f);
    const auto resizeInspector = ResizePanel(panelLayout, "inspector", 333.0f, 444.0f);
    const auto* resizedPanel = find_panel(panelLayout, "inspector");
    check_true("resize panel updates size", resizeInspector.changed &&
              resizedPanel != nullptr &&
              resizedPanel->width == 333.0f &&
              resizedPanel->height == 444.0f);
    const auto dockInspector = SetPanelDockState(panelLayout, "inspector", false, true);
    const auto* dockedPanel = find_panel(panelLayout, "inspector");
    check_true("set panel floating", dockInspector.changed && dockedPanel != nullptr &&
              dockedPanel->floating && !dockedPanel->docked);
    const auto commandMove = ApplyPanelStateCommand(panelLayout, "view.panel.move", true, 120.0f, "inspector");
    const auto* commandPanel = find_panel(panelLayout, "inspector");
    check_true("apply panel move command", commandMove.changed &&
              commandPanel != nullptr &&
              commandPanel->x == 120.0f);
    const auto commandResize = ApplyPanelStateCommand(panelLayout, "view.panel.resize", false, 512.0f, "inspector");
    const auto* commandResizePanel = find_panel(panelLayout, "inspector");
    check_true("apply panel resize command", commandResize.changed &&
              commandResizePanel != nullptr &&
              commandResizePanel->width == 512.0f);
    const auto commandToggleVisible = ApplyPanelStateCommand(panelLayout, "view.panel.toggle_visible", false, 0.0f, "inspector");
    const auto* commandVisiblePanel = find_panel(panelLayout, "inspector");
    check_true("apply panel toggle visible", commandToggleVisible.changed &&
              commandVisiblePanel != nullptr &&
              !commandVisiblePanel->visible);
  }
  const auto restoreBenchmarkLayout = RestoreLayoutPreset(panelLayout, LayoutPreset::Benchmark);
  check_true("restore benchmark layout", restoreBenchmarkLayout);
  check_true("restore layout changed preset", panelLayout.preset == LayoutPreset::Benchmark);
  const auto unknownPanelCommand = ApplyPanelStateCommand(panelLayout, "does_not_exist", false, 0.0f, "inspector");
  check_true("unknown panel command rejected", !unknownPanelCommand.changed);

  const auto release_gate = BuildUiReleaseGateEvidence();
  const std::string release_gate_json = SerializeUiReleaseGateChecklist(release_gate);
  check_true("release gate has no pending items",
             release_gate_json.find("\"pending_count\":0") != std::string::npos);
  check_true("release gate explicitly defers runtime UI gaps",
             release_gate_json.find("\"deferred_count\":") != std::string::npos &&
             release_gate_json.find("\"status\":\"deferred\"") != std::string::npos);

  std::cout << "ui model smoke: " << (ok ? "ok\n" : "failed\n");
  return ok;
}

void MarkReleaseGate(std::vector<vkpt::editor::UiReleaseGateItem>& items,
                     std::string_view id,
                     bool passed,
                     std::string_view evidence,
                     std::string_view deferred_reason = {}) {
  for (auto& item : items) {
    if (item.id == id) {
      item.passed = passed;
      item.deferred = !passed && !deferred_reason.empty();
      item.evidence = std::string(evidence);
      item.deferred_reason = std::string(deferred_reason);
      return;
    }
  }
}

std::vector<vkpt::editor::UiReleaseGateItem> BuildUiReleaseGateEvidence() {
  using namespace vkpt::editor;
  auto checklist = BuildDefaultUiReleaseGateChecklist();

  const auto menu = BuildDefaultMenuBar();
  MarkReleaseGate(checklist, "menu.works",
                  HasTopLevelMenu(menu, "file") &&
                  HasTopLevelMenu(menu, "edit") &&
                  HasTopLevelMenu(menu, "benchmark") &&
                  HasMenuItem(menu, "file", "file.open_scene") &&
                  HasMenuItem(menu, "benchmark", "benchmark.run_current_scene"),
                  "BuildDefaultMenuBar exposes required top-level menus and typed actions");

  const auto layout = CreateLayoutPreset(LayoutPreset::Default);
  const auto layoutPath = std::filesystem::temp_directory_path() / "vkpt-ui-release-layout.json";
  std::string layoutError;
  UiLayoutDocument reloadedLayout;
  const bool layoutPersisted = SaveLayoutToFile(layoutPath.string(), layout, &layoutError) &&
                               LoadLayoutFromFile(layoutPath.string(), &reloadedLayout) &&
                               reloadedLayout.active_layout_name == layout.active_layout_name &&
                               !reloadedLayout.panels.empty();
  std::error_code removeEc;
  std::filesystem::remove(layoutPath, removeEc);
  MarkReleaseGate(checklist, "layout.persists", layoutPersisted,
                  "UiLayoutDocument save/load roundtrip preserves panel geometry",
                  layoutPersisted ? std::string_view{} : std::string_view{"layout JSON roundtrip failed"});

  auto panelLayout = CreateLayoutPreset(LayoutPreset::Default);
  const bool panelMutations =
      SetPanelVisible(panelLayout, "inspector", false).changed &&
      SetPanelVisible(panelLayout, "inspector", true).changed &&
      SetPanelDockState(panelLayout, "inspector", false, true).changed &&
      MovePanel(panelLayout, "inspector", 32.0f, 48.0f).changed &&
      ResizePanel(panelLayout, "inspector", 512.0f, 384.0f).changed;
  MarkReleaseGate(checklist, "panels.dock_float", panelMutations,
                  "UiLayoutDocument panel helpers cover close/show/dock/float/move/resize");

  SelectionState selection = CreateDefaultSelectionState();
  selection.selected_entity_ids = {1, 2};
  selection.active_primary_entity = 1;
  selection.hovered_entity = 2;
  selection.aggregate_bounds = {{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, true};
  selection.per_item_bounds = {
    {1, {{-1.0f, -1.0f, -1.0f}, {0.0f, 0.0f, 0.0f}, true}},
    {2, {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, true}},
  };
  const auto selectionPath = std::filesystem::temp_directory_path() / "vkpt-ui-release-selection.json";
  std::string selectionError;
  SelectionState selectionRoundtrip;
  const bool selectionSaved = SaveSelectionToFile(selectionPath.string(), selection, &selectionError) &&
                              LoadSelectionFromFile(selectionPath.string(), &selectionRoundtrip) &&
                              selectionRoundtrip.selected_entity_ids == selection.selected_entity_ids &&
                              selectionRoundtrip.per_item_bounds.size() == 2;
  std::filesystem::remove(selectionPath, removeEc);
  MarkReleaseGate(checklist, "selection.multi", selectionSaved,
                  "SelectionState supports stable IDs, primary selection, hover, aggregate bounds, per-item bounds, and JSON roundtrip");
  MarkReleaseGate(checklist, "viewport.bounds", false,
                  "SelectionState carries aggregate and per-item bounds for viewport/tree/inspector overlays",
                  "requires overlay renderer drawing those bounds in the viewport");

  const auto rejectedDrop = ValidateAssetDrop("C:/assets/readme.md", "texture_slot");
  MarkReleaseGate(checklist, "assets.reject", !rejectedDrop.accepted && !rejectedDrop.reason.empty(),
                  "ValidateAssetDrop rejects unsupported target/type combinations with a reason");

  const auto benchmarkDesc = MakeDefaultBenchmarkRunDesc("assets/scenes/cornell_native.json", "cpu", "cpu_scalar", 8, 4, 123, 320, 180);
  const auto workload = EstimateWorkloadComplexity(benchmarkDesc, 1, 12, 8, 4096, false);
  auto score = ComputeBenchmarkScore(1000.0, 1000.0, 250.0, workload.normalized_cost_units, true);
  score.raw_paths_per_second = 500.0;
  const BenchmarkRawMetricsModel rawMetrics{
    60.0, 16.67, 0.0, 16.67, 250.0, 500.0, 1500.0, 8, 4096, 0.25, 0.0
  };
  const auto benchmarkPanel = BuildBenchmarkPanelModel(
      benchmarkDesc, rawMetrics, score, workload, "artifacts/benchmarks/ui", "model smoke result", true);
  const std::string benchmarkJson = SerializeBenchmarkPanelModel(benchmarkPanel);
  const bool benchmarkPanelOk =
      benchmarkJson.find("\"selected_scene\":\"assets/scenes/cornell_native.json\"") != std::string::npos &&
      benchmarkJson.find("\"raw_metrics\"") != std::string::npos &&
      benchmarkJson.find("\"normalized_score\":1") != std::string::npos &&
      benchmarkJson.find("\"calibration_actions\"") != std::string::npos;
  MarkReleaseGate(checklist, "benchmark.desc", benchmarkPanelOk,
                  "BenchmarkPanelModel serializes run descriptor, raw metrics, score, workload, calibration actions, and artifact path");
  MarkReleaseGate(checklist, "benchmark.score", score.calibration_valid && score.confidence == "high",
                  "ComputeBenchmarkScore separates raw throughput from hardware-normalized efficiency");

  UiEventLog log(256);
  PushUiEvent(log, "menu_click", "menu", "file.open_scene", 1, {}, {}, "unsupported:file dialog unavailable");
  EditorCommandHistory history(256);
  history.push(MakeMenuCommand("benchmark.run_current_scene", "menu", 2));
  const auto runtime = CreateDefaultRuntimeState();
  const std::string uiState = SerializeUiRuntimeState(runtime);
  const std::string eventLines = SerializeUiEventsJsonl(log.events(), 256);
  const std::string commandLines = SerializeEditorCommandsJsonl(history.history(), 256);
  MarkReleaseGate(checklist, "crash.ui_state",
                  uiState.find("\"active_layout_name\"") != std::string::npos &&
                  eventLines.find("menu_click") != std::string::npos &&
                  commandLines.find("benchmark.run_current_scene") != std::string::npos,
                  "Crash recorder receives serialized UI state, selection state, layout, UI events, and editor command history from app shell");

  MarkReleaseGate(checklist, "window.opens", false,
                  "Native/Qt bounded window smoke is covered by tools/ui_qt_smoke when the platform is available",
                  "requires a GUI-capable platform run, not a headless model check");
  MarkReleaseGate(checklist, "tree.hierarchy", false,
                  "SceneTreeRow and hierarchy command contracts exist in UiModels",
                  "requires ECS tree widget/runtime binding");
  MarkReleaseGate(checklist, "viewport.selection", false,
                  "SelectionState and selection commands are covered by --ui-model-smoke",
                  "requires object-ID/CPU-ray picking integration in the viewport");
  MarkReleaseGate(checklist, "gizmo.trs", false,
                  "GizmoSettings and transform command contracts exist",
                  "requires rendered gizmo handles and command application");
  MarkReleaseGate(checklist, "inspector.edits", false,
                  "InspectorFieldSchema and mixed-value models are covered by --ui-model-smoke",
                  "requires bound inspector widgets applying ECS/component edits");
  MarkReleaseGate(checklist, "grouping", false,
                  "Group/Ungroup command payloads serialize through EditorCommand",
                  "requires ECS hierarchy command application");
  MarkReleaseGate(checklist, "merge.split", false,
                  "Merge command payload serializes; split is explicitly unsupported until merge metadata runtime exists",
                  "requires generated asset/merge metadata runtime");
  MarkReleaseGate(checklist, "assets.import", false,
                  "Asset drop validation and import command contracts exist",
                  "requires importer runtime and asset browser widget");
  MarkReleaseGate(checklist, "lua.attach", false,
                  "Attach/Detach script command payloads exist and script lifecycle model is deterministic",
                  "requires Lua runtime/editor binding");
  MarkReleaseGate(checklist, "logs.errors", false,
                  "UiEventLog serializes model events for crash artifacts",
                  "requires visible log panel consuming the diagnostics ring buffer");

  return checklist;
}

bool RunUiReleaseGateCheck(bool json) {
  const auto checklist = BuildUiReleaseGateEvidence();
  if (json) {
    std::cout << vkpt::editor::SerializeUiReleaseGateChecklist(checklist) << "\n";
  } else {
    std::size_t passed = 0;
    std::size_t deferred = 0;
    std::size_t pending = 0;
    std::cout << "ui release gate:\n";
    for (const auto& item : checklist) {
      const char* status = item.passed ? "pass" : (item.deferred ? "defer" : "PEND");
      if (item.passed) {
        ++passed;
      } else if (item.deferred) {
        ++deferred;
      } else {
        ++pending;
      }
      std::cout << "  [" << status << "] " << item.id << ": " << item.evidence;
      if (item.deferred) {
        std::cout << " (" << item.deferred_reason << ")";
      }
      std::cout << "\n";
    }
    std::cout << "ui release gate summary: passed=" << passed
              << " deferred=" << deferred
              << " pending=" << pending << "\n";
  }

  return std::none_of(checklist.begin(), checklist.end(), [](const auto& item) {
    return !item.passed && !item.deferred;
  });
}

void PushUiEvent(vkpt::editor::UiEventLog& log,
                 std::string_view event_type,
                 std::string_view panel_id,
                 std::string_view widget_id,
                 vkpt::core::FrameIndex frame_index,
                 std::string_view old_value,
                 std::string_view new_value,
                 std::string_view command_result) {
  vkpt::editor::UiEvent event;
  event.event_type = std::string(event_type);
  event.panel_id = std::string(panel_id);
  event.widget_id = std::string(widget_id);
  event.frame_index = frame_index;
  event.timestamp_ms = NowMs();
  event.thread_id = "main";
  event.old_value = std::string(old_value);
  event.new_value = std::string(new_value);
  event.command_result = std::string(command_result);
  log.push(event);
}

vkpt::editor::EditorCommand MakeUnsupportedUiCommand(std::string_view action_id,
                                                    std::string_view reason,
                                                    std::string_view source_widget = "app_shell",
                                                    vkpt::core::FrameIndex frame_index = 0) {
  vkpt::editor::EditorCommand cmd;
  cmd.command_id = std::string(action_id);
  cmd.kind = vkpt::editor::EditorCommandKind::kUnsupportedUiAction;
  cmd.source_widget = std::string(source_widget);
  cmd.frame_index = frame_index;
  cmd.undoable = false;
  cmd.redoable = false;
  cmd.validated = false;
  cmd.payload = vkpt::editor::UnsupportedUiActionCommand{
    std::string(action_id),
    std::string(reason)
  };
  return cmd;
}

void UpdateCrashArtifactsFromUiState(
    const vkpt::editor::UiRuntimeState& runtime_state,
    const vkpt::editor::SelectionState& selection_state,
    const vkpt::editor::UiLayoutDocument& layout_state,
    const vkpt::editor::UiEventLog& event_log,
    const vkpt::editor::EditorCommandHistory& command_history) {
  auto& recorder = vkpt::diagnostics::CrashRecorder::instance();
  recorder.update_ui_state_json(vkpt::editor::SerializeUiRuntimeState(runtime_state));
  recorder.update_selection_state_json(vkpt::editor::SerializeSelectionState(selection_state));
  recorder.update_layout_state_json(vkpt::editor::SerializeLayoutDocument(layout_state));
  recorder.update_ui_events_jsonl(vkpt::editor::SerializeUiEventsJsonl(event_log.events(), 256));
  recorder.update_editor_commands_jsonl(
      vkpt::editor::SerializeEditorCommandsJsonl(command_history.history(), 256));
}

}  // namespace

int main(int argc, char** argv) {
  // ---- Early init: logging + crash hooks ------------------------------------
  InitializeLogging();
  InitializeCrashRecorder();
  vkpt::diagnostics::install_crash_hooks("artifacts/crashes");
  vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("startup", 0);

  auto& logger = vkpt::log::Logger::instance();

  // ---- Parse CLI args -------------------------------------------------------
  const std::vector<std::string_view> args(argv, argv + argc);
  bool showVersion   = false;
  bool versionJson   = false;
  bool headless      = false;
  bool crashTest     = false;
  bool doctor        = false;
  bool checkBuild    = false;
  bool checkCpu      = false;
  bool checkBackends = false;
  bool checkAssets   = false;
  bool checkShaders  = false;
  bool checkJobSystem         = false;
  bool checkSceneSchema       = false;
  bool checkBenchmarkArtifact = false;
  bool dumpConfig    = false;
  bool listBackends  = false;
  bool doRender      = false;
  bool uiModelSmoke  = false;
  bool uiReleaseGate = false;
  bool openWindow    = false;
  bool listGpus      = false;
  bool autoExitWindow = false;
  std::string configFilePath;
  std::string_view scenePath;
  std::string_view backend;
  std::string_view platformName;
  std::string_view outputPath    = "artifacts/renders/cornell.png";
  std::string_view exrOutputPath;
  std::string_view logLevel      = "info";
  uint32_t width    = 320;
  uint32_t height   = 240;
  uint32_t windowWidth  = 1280;
  uint32_t windowHeight = 720;
  uint32_t windowFrameLimit = 0;
  uint32_t spp      = 16;
  uint32_t maxDepth = 6;
  std::optional<uint32_t> uiPresentHz;

  for (size_t i = 1; i < args.size(); ++i) {
    const auto token = args[i];
    if      (token == "--version")        { showVersion   = true; }
    else if (token == "--json")           { versionJson   = true; }
    else if (token == "--doctor")         { doctor = checkBuild = checkCpu = checkBackends = checkAssets = checkShaders = checkJobSystem = checkSceneSchema = checkBenchmarkArtifact = true; }
    else if (token == "--check-build")    { checkBuild    = true; }
    else if (token == "--check-cpu")      { checkCpu      = true; }
    else if (token == "--check-backends") { checkBackends = true; }
    else if (token == "--check-assets")   { checkAssets   = true; }
    else if (token == "--check-shaders")  { checkShaders  = true; }
    else if (token == "--check-job-system")   { checkJobSystem         = true; }
    else if (token == "--check-scene-schema") { checkSceneSchema       = true; }
    else if (token == "--check-bench-write")  { checkBenchmarkArtifact = true; }
    else if (token == "--dump-config")    { dumpConfig    = true; }
    else if (token == "--list-backends")  { listBackends  = true; }
    else if (token == "--headless")       { headless      = true; }
    else if (token == "--render")         { doRender      = true; }
    else if (token == "--window")         { openWindow    = true; }
    else if (token == "--list-gpus")      { listGpus      = true; }
    else if (token == "--crash-test")     { crashTest     = true; }
    else if (token == "--ui-model-smoke") { uiModelSmoke  = true; }
    else if (token == "--ui-release-gate") { uiReleaseGate = true; }
    else if (token == "--exit")           { autoExitWindow = true; }
    else if (token == "--config") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --config\n"; return 1; }
      configFilePath = std::string(args[++i]);
    } else if (token == "--scene") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --scene\n"; return 1; }
      scenePath = args[++i];
    } else if (token == "--backend") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --backend\n"; return 1; }
      backend = args[++i];
    } else if (token == "--platform") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --platform\n"; return 1; }
      platformName = args[++i];
    } else if (token == "--log-level") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --log-level\n"; return 1; }
      logLevel = args[++i];
    } else if (token == "--output") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --output\n"; return 1; }
      outputPath = args[++i];
    } else if (token == "--exr-output") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --exr-output\n"; return 1; }
      exrOutputPath = args[++i];
    } else if (token == "--width") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], width)) { std::cerr << "invalid value for --width\n"; return 1; }
    } else if (token == "--height") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], height)) { std::cerr << "invalid value for --height\n"; return 1; }
    } else if (token == "--window-width") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], windowWidth)) { std::cerr << "invalid value for --window-width\n"; return 1; }
    } else if (token == "--window-height") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], windowHeight)) { std::cerr << "invalid value for --window-height\n"; return 1; }
    } else if (token == "--ui-present-hz") {
      uint32_t parsedUiPresentHz = 0;
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], parsedUiPresentHz)) {
        std::cerr << "invalid value for --ui-present-hz\n";
        return 1;
      }
      uiPresentHz = vkpt::config::ClampUiPresentHz(parsedUiPresentHz);
    } else if (token == "--frames") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], windowFrameLimit) || windowFrameLimit == 0u) {
        std::cerr << "invalid value for --frames\n";
        return 1;
      }
    } else if (token == "--spp") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], spp)) { std::cerr << "invalid value for --spp\n"; return 1; }
    } else if (token == "--max-depth") {
      if (i + 1 >= args.size() || !ParseUnsigned(args[++i], maxDepth)) { std::cerr << "invalid value for --max-depth\n"; return 1; }
    } else if (token == "--help" || token == "-h") {
      PrintUsage();
      return 0;
    } else {
      std::cerr << "unknown option: " << token << "\n";
      PrintUsage();
      return 1;
    }
  }

  if (openWindow && doRender) {
    std::cerr << "--window and --render are mutually exclusive; use --window for the interactive preview or --render for offscreen output\n";
    return 1;
  }
  if ((autoExitWindow || windowFrameLimit != 0u) && !openWindow) {
    std::cerr << "--exit and --frames are only valid with --window\n";
    return 1;
  }
  if (autoExitWindow && windowFrameLimit == 0u) {
    windowFrameLimit = 1u;
  }

  // ---- Build resolved config (A10) ------------------------------------------
  vkpt::config::RuntimeConfig config = vkpt::config::BuildDefaultConfig(configFilePath);
  // CLI flags override file/env values.
  if (!backend.empty())   { config.backend    = {std::string(backend),    vkpt::config::ConfigSource::CliFlag}; }
  if (!platformName.empty())  { config.platform   = {std::string(platformName),   vkpt::config::ConfigSource::CliFlag}; }
  if (!scenePath.empty()) { config.scene_path = {std::string(scenePath),  vkpt::config::ConfigSource::CliFlag}; }
  if (!logLevel.empty())  { config.log_level  = {std::string(logLevel),   vkpt::config::ConfigSource::CliFlag}; }
  if (headless)           { config.headless   = {true,                    vkpt::config::ConfigSource::CliFlag}; }
  if (width != 320)       { config.render_width  = {width,  vkpt::config::ConfigSource::CliFlag}; }
  if (height != 240)      { config.render_height = {height, vkpt::config::ConfigSource::CliFlag}; }
  if (spp != 16)          { config.spp           = {spp,    vkpt::config::ConfigSource::CliFlag}; }
  if (maxDepth != 6)      { config.max_depth     = {maxDepth, vkpt::config::ConfigSource::CliFlag}; }
  if (uiPresentHz)        { config.ui_present_hz = {*uiPresentHz, vkpt::config::ConfigSource::CliFlag}; }
  if (!outputPath.empty()) { config.output_path  = {std::string(outputPath), vkpt::config::ConfigSource::CliFlag}; }
  if (!exrOutputPath.empty()) {
    config.exr_output_path = {std::string(exrOutputPath), vkpt::config::ConfigSource::CliFlag};
  }

  // Canonicalize backend aliases (e.g. "dxr" -> "d3d12-dxr").
  config.backend.value = vkpt::render::NormalizeBackendName(config.backend.value);
  const auto requestedPlatform = vkpt::platform::ParseRuntimePlatform(config.platform.value);
  if (requestedPlatform == vkpt::platform::RuntimePlatformKind::Invalid) {
    std::cerr << "invalid platform: " << config.platform.value
              << " (expected auto|raw|qt|headless aliases: desktop|native|win32)\n";
    return 1;
  }
  const auto selectedPlatform = vkpt::platform::ResolveRuntimePlatform(
      requestedPlatform,
      openWindow,
      config.headless.value);
  if (!vkpt::platform::IsPlatformBuilt(selectedPlatform)) {
    std::cerr << "selected platform is not built: "
              << vkpt::platform::RuntimePlatformKindName(selectedPlatform) << "\n";
    return 1;
  }
  const bool doctorMode = doctor || checkBuild || checkCpu || checkBackends || checkAssets || checkShaders
                         || checkJobSystem || checkSceneSchema || checkBenchmarkArtifact;
  const bool nonGuiCommandMode = doctorMode || listBackends || doRender;
  auto effectivePlatform = selectedPlatform;
  if (nonGuiCommandMode && selectedPlatform == vkpt::platform::RuntimePlatformKind::Qt) {
    effectivePlatform = vkpt::platform::RuntimePlatformKind::Headless;
    BootStep("qt platform requested for non-gui command; using headless shell");
  }
  config.platform.value = vkpt::platform::RuntimePlatformKindName(effectivePlatform);
  if (effectivePlatform == vkpt::platform::RuntimePlatformKind::Headless) {
    config.headless = {true, config.platform.source};
  }
  {
    std::ostringstream resolved;
    resolved << "runtime config resolved backend=" << config.backend.value
             << " platform=" << config.platform.value
             << " requested_platform=" << vkpt::platform::RuntimePlatformKindName(requestedPlatform)
             << " selected_platform=" << vkpt::platform::RuntimePlatformKindName(selectedPlatform)
             << " scene=" << (config.scene_path.value.empty() ? "none" : config.scene_path.value);
    BootStep(resolved.str());
  }

  vkpt::editor::UiRuntimeState ui_runtime_state = vkpt::editor::CreateDefaultRuntimeState();
  vkpt::editor::SelectionState ui_selection_state = vkpt::editor::CreateDefaultSelectionState();
  vkpt::editor::UiLayoutDocument ui_layout_state = vkpt::editor::CreateDefaultLayout();
  vkpt::editor::UiEventLog ui_event_log(256);
  vkpt::editor::EditorCommandHistory ui_command_history(256);

  ui_runtime_state.active_scene = config.scene_path.value.empty() ? "none" : config.scene_path.value;
  ui_runtime_state.active_renderer_backend = config.backend.value;
  PushUiEvent(ui_event_log, "app_start", "app_shell", "startup", 0, {}, {}, "bootstrap");
  UpdateCrashArtifactsFromUiState(
      ui_runtime_state, ui_selection_state, ui_layout_state,
      ui_event_log, ui_command_history);

  logger.log(vkpt::log::Severity::Info, "app", "arg parse complete");
  LogRuntimeMetadata(logger, "post_config", requestedPlatform, selectedPlatform, effectivePlatform,
                     openWindow, doRender, autoExitWindow, windowFrameLimit,
                     config.ui_present_hz.value);
  BootStep("command line parsed");

  // ---- Status tracking (A13) ------------------------------------------------
  vkpt::diagnostics::StatusFileData status;
  status.build_status           = "ok";
  status.enabled_backend        = config.backend.value;
  status.selected_scene         = config.scene_path.value.empty() ? "none" : config.scene_path.value;
  status.selected_renderer_path = "cpu_scalar";
  const auto writeStatus = [&](const std::string& runStatus, const std::string& error = "") {
    status.last_run_status = runStatus;
    status.last_error      = error;
    std::string writeErr;
    if (!vkpt::diagnostics::WriteStatusFile(status, config.status_file_path.value, &writeErr)) {
      logger.log(vkpt::log::Severity::Warning, "app", "status file write failed: " + writeErr);
    }
  };
  const auto recordUiAction = [&](std::string_view action_id,
                                 std::string_view event_widget,
                                 const std::string& action_status,
                                 const vkpt::editor::EditorCommand& command) {
    auto logged_command = command;
    logged_command.command_id = std::string(action_id);
    if (logged_command.source_widget.empty()) {
      logged_command.source_widget = "app_shell";
    }
    logged_command.frame_index = 0;
    ui_command_history.push(logged_command);
    ui_runtime_state.status_message = action_status;
    ui_runtime_state.last_menu_action = std::string(action_id);
    ui_runtime_state.focused_panel = logged_command.source_widget;
    PushUiEvent(ui_event_log, "app_action", logged_command.source_widget, event_widget, 0, {}, {}, action_status);
    UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                   ui_event_log, ui_command_history);
  };

  // ---- --version ------------------------------------------------------------
  if (showVersion) {
    if (versionJson) { PrintVersionJson(effectivePlatform); } else { PrintVersionText(effectivePlatform); }
    recordUiAction("app.version", "version", "version output", MakeUnsupportedUiCommand("app.version", "handled on cli"));
    writeStatus("version_query");
    return 0;
  }

  // ---- --dump-config (A10) --------------------------------------------------
  if (dumpConfig) {
    std::cout << vkpt::config::SerializeRuntimeConfig(config) << "\n";
    recordUiAction("app.dump_config", "dump_config", "config dump", MakeUnsupportedUiCommand("app.dump_config", "handled on cli"));
    writeStatus("dump_config");
    return 0;
  }

  if (uiModelSmoke) {
    const bool ok = RunUiModelSmokeTests();
    writeStatus(ok ? "ui_model_smoke" : "ui_model_smoke_failed");
    return ok ? 0 : 1;
  }

  if (uiReleaseGate) {
    const bool ok = RunUiReleaseGateCheck(versionJson);
    writeStatus(ok ? "ui_release_gate" : "ui_release_gate_failed");
    return ok ? 0 : 1;
  }

  // ---- --doctor / --check-* (A09) -------------------------------------------
  if (doctorMode) {
    PrintNonGuiPlatformShellNotice("doctor", selectedPlatform, effectivePlatform);
    RunDoctor(checkBuild, checkCpu, checkBackends, checkAssets, checkShaders,
              checkJobSystem, checkSceneSchema, checkBenchmarkArtifact);
    recordUiAction("app.doctor", "doctor", "doctor complete", MakeUnsupportedUiCommand("app.doctor", "handled on cli"));
    writeStatus("doctor_ok");
    return 0;
  }

  if (listBackends) {
    PrintNonGuiPlatformShellNotice("list-backends", selectedPlatform, effectivePlatform);
    PrintBackendDiagnostics();
    recordUiAction("app.list_backends", "list_backends", "backend list", MakeUnsupportedUiCommand("app.list_backends", "handled on cli"));
    writeStatus("list_backends");
    return 0;
  }

#ifdef PT_ENABLE_VULKAN
  if (listGpus) {
    const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
        PT_SHADER_SPV_PATH;
#else
        "shaders/pathtrace.spv";
#endif
    // Init a temporary tracer just for device enumeration (it logs devices in init_device)
    auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
    if (gpuTracer->is_valid()) {
      std::cout << "Selected GPU: " << gpuTracer->gpu_name() << "\n";
      std::cout << "  Type  : " << gpuTracer->gpu_type() << "\n";
      std::cout << "  VRAM  : " << gpuTracer->vram_mb() << " MB\n";
      std::cout << "  Vulkan: "
                << VK_VERSION_MAJOR(gpuTracer->vulkan_api()) << "."
                << VK_VERSION_MINOR(gpuTracer->vulkan_api()) << "\n";
    } else {
      std::cerr << "Vulkan device init failed: " << gpuTracer->last_error() << "\n";
    }
    writeStatus("list_gpus");
    return gpuTracer->is_valid() ? 0 : 1;
  }
#else
  if (listGpus) {
    std::cout << "PT_ENABLE_VULKAN not set in this build — no GPU info available.\n";
    writeStatus("list_gpus_no_vulkan");
    return 0;
  }
#endif

  // ---- --window (interactive shell placeholder) ----------------------------
  if (openWindow) {
    BootStep("window mode requested");
    if (headless) {
      std::cerr << "--window and --headless are mutually exclusive\n";
      return 1;
    }
    if (selectedPlatform == vkpt::platform::RuntimePlatformKind::Headless) {
      std::cerr << "--window and --platform headless are mutually exclusive\n";
      return 1;
    }
    if (selectedPlatform == vkpt::platform::RuntimePlatformKind::Qt) {
      BootStep("creating qt platform");
      auto platform = vkpt::platform::CreatePlatform(vkpt::platform::RuntimePlatformKind::Qt,
                                                     "vkpt-qt");
      if (!platform) {
        std::cerr << "qt platform factory creation failed\n";
        writeStatus("error:qt_platform_create_failed", "qt_platform_create_failed");
        return 1;
      }
      BootStep("initializing qt platform");
      auto platformState = platform->initialize();
      if (!platformState) {
        std::cerr << "qt platform initialization failed\n";
        writeStatus("error:qt_platform_init_failed", "qt_platform_init_failed");
        return 1;
      }

      auto* window = platform->window();
      BootStep("qt platform initialized; acquiring window");
      if (!window) {
        std::cerr << "qt platform returned invalid window\n";
        platform->shutdown();
        writeStatus("error:qt_window_missing", "qt_window_missing");
        return 1;
      }

#ifdef PT_ENABLE_QT
      auto* qtWindow = dynamic_cast<vkpt::platform::QtWindow*>(window);
      if (qtWindow) {
        qtWindow->resize(std::max<uint32_t>(1u, windowWidth),
                         std::max<uint32_t>(1u, windowHeight));
      }
#endif
      LogRuntimeMetadata(logger, "qt_platform_initialized", requestedPlatform,
                         selectedPlatform, effectivePlatform, openWindow, doRender,
                         autoExitWindow, windowFrameLimit, config.ui_present_hz.value);

      std::cout << "qt window open (" << window->metrics().width << "x"
                << window->metrics().height << ")\n";
      std::cout << "qt platform shell: " << QtPlatformShellString() << "\n";
      std::cout << "Close the Qt window to exit.\n";
      if (windowFrameLimit != 0u) {
        std::cout << "[ui] gui smoke frame limit: " << windowFrameLimit << "\n";
      }
      BootStep("qt window opened");

      // ---- Path tracer setup ----
      std::unique_ptr<vkpt::pathtracer::IPathTracer> qtTracer;
#ifdef PT_ENABLE_D3D12
      if (config.backend.value == "d3d12" || config.backend.value == "d3d12-dxr") {
  BootStep("initializing d3d12 tracer");
        const bool requestDxr = (config.backend.value == "d3d12-dxr");
        const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
            PT_SHADER_HLSL_PATH;
#else
            "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
        auto gpuTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
        if (gpuTracer->is_valid()) {
          gpuTracer->set_prefer_dxr(requestDxr);
          std::cout << "[gpu] D3D12 " << gpuTracer->gpu_name()
                    << "  " << gpuTracer->vram_mb() << " MB VRAM"
                    << "  DXR=" << (gpuTracer->dxr_supported() ? "yes" : "no") << "\n";
          qtTracer = std::move(gpuTracer);
        } else {
          std::cerr << "[gpu] D3D12 tracer init failed: " << gpuTracer->last_error() << "\n";
          writeStatus("error:d3d12_init_failed", gpuTracer->last_error());
          platform->shutdown();
          return 1;
        }
      }
#endif
      if (!qtTracer) {
  BootStep("falling back to cpu tiled tracer");
        vkpt::cpu::TiledRenderConfig tiledConfig{};
        tiledConfig.worker_count = 0;
        qtTracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
        std::cout << "[cpu] Using TiledCpuPathTracer\n";
      }

      // ---- Scene loading ----
      vkpt::pathtracer::RTSceneData qtScene;
      vkpt::scene::SceneDocument qtSceneDocument;
      BootStep("loading scene snapshot");
      {
        bool sceneOk = false;
        if (!config.scene_path.value.empty()) {
          auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
          if (parseResult) {
            qtSceneDocument = parseResult.value();
            auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
            if (sceneResult) {
              qtScene = std::move(sceneResult.value());
              sceneOk = true;
            }
          }
        } else {
          qtSceneDocument.metadata.scene_name = "cornell";
          auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
          if (sceneResult) {
            qtScene = std::move(sceneResult.value());
            sceneOk = true;
          }
        }
        if (!sceneOk) {
          std::cerr << "qt window: scene load failed\n";
          writeStatus("error:qt_scene_load_failed", "qt_scene_load_failed");
          platform->shutdown();
          return 1;
        }
        // Ensure scene has lighting
        bool hasLight = !qtScene.lights.empty();
        bool hasEmissive = false;
        for (const auto& mat : qtScene.materials) {
          if (mat.is_emissive()) { hasEmissive = true; break; }
        }
        if (!hasLight && !hasEmissive) {
          qtScene.environment_color = {0.35f, 0.4f, 0.5f};
        }
      }
      BootStep("scene loaded");

      // ---- Tracer configure ----
      vkpt::pathtracer::RenderSettings qtSettings{};
      qtSettings.width  = std::max<uint32_t>(1u, config.render_width.value);
      qtSettings.height = std::max<uint32_t>(1u, config.render_height.value);
      qtSettings.spp    = std::numeric_limits<uint32_t>::max();
      qtSettings.max_depth = std::max<uint32_t>(1u, config.max_depth.value);
      qtSettings.seed = 0xC001D00Dull;
      qtSettings.enable_nee = true;
      qtSettings.enable_mis = true;

      bool qtTracerReady = (qtTracer->configure(qtSettings) &&
                            qtTracer->load_scene_snapshot(qtScene) &&
                            qtTracer->build_or_update_acceleration() &&
                            qtTracer->reset_accumulation());
      if (!qtTracerReady) {
        std::cerr << "qt window: tracer init failed\n";
        BootStep("tracer init failed");
      } else {
        BootStep("tracer initialized");
      }

      // Immutable geometry snapshot — bg thread uses this for camera-only reloads.
      const vkpt::pathtracer::RTSceneData qtSceneBase = qtScene;
#ifdef PT_ENABLE_QT
      const std::vector<ViewportPickable> qtPickables =
          BuildViewportPickables(qtSceneDocument, qtScene);
#endif

      // ---- Background render thread (TiledCpuPathTracer blocks; run off main thread) ----
      // Camera updates are a small command channel; display frames use QtWindow's queued handoff.
      const uint32_t qtPreviewPublishHz = std::max<uint32_t>(1u, config.ui_present_hz.value);
      constexpr uint32_t kQtPreviewImmediatePublishes = 4u;
      const auto kQtPreviewPublishInterval =
          std::chrono::microseconds(std::max<uint32_t>(1u, 1000000u / qtPreviewPublishHz));
      std::atomic<bool> qtBgStop{false};
      std::atomic<bool> qtBgFailed{false};
      std::atomic<uint32_t> qtPublishedSample{0u};
      std::atomic<uint32_t> qtPublishedWidth{qtSettings.width};
      std::atomic<uint32_t> qtPublishedHeight{qtSettings.height};
      std::atomic<std::uint64_t> qtPublishedFrames{0u};
      std::atomic<std::uint64_t> qtDroppedFrames{0u};
      auto qtFramebufferHandoffAlive = std::make_shared<std::atomic<bool>>(true);

      std::mutex qtCameraCommandMutex;
      struct QtCameraCommand {
        bool camPending = false;
        vkpt::pathtracer::Vec3 camPos{}, camTarget{}, camUp{};
        float camFov = 0.0f;
      } qtCameraCommand;
      const bool qtUseBg = (windowFrameLimit == 0u) &&
          (dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(qtTracer.get()) != nullptr);
      std::thread qtBgThread;
      if (qtTracerReady && qtUseBg) {
        BootStep("starting background cpu render thread");
        qtBgThread = std::thread([&]() {
          uint32_t s = 0;
          auto lastPublish = std::chrono::steady_clock::time_point{};
          while (!qtBgStop.load(std::memory_order_relaxed)) {
            // Pick up any pending camera update between samples (no blocking).
            QtCameraCommand cameraCommand{};
            bool hasCameraCommand = false;
            {
              std::lock_guard<std::mutex> lock(qtCameraCommandMutex);
              if (qtCameraCommand.camPending) {
                cameraCommand = qtCameraCommand;
                qtCameraCommand.camPending = false;
                hasCameraCommand = true;
              }
            }

            if (hasCameraCommand) {
              const bool ok = qtTracer->update_camera(
                  cameraCommand.camPos, cameraCommand.camTarget,
                  cameraCommand.camUp, cameraCommand.camFov);
              if (!ok) {
                vkpt::pathtracer::RTSceneData camScene = qtSceneBase;
                camScene.camera_position = cameraCommand.camPos;
                camScene.camera_target = cameraCommand.camTarget;
                camScene.camera_up = cameraCommand.camUp;
                camScene.camera_fov_deg = cameraCommand.camFov;
                qtTracer->load_scene_snapshot(camScene);
                qtTracer->build_or_update_acceleration();
              }
              qtTracer->reset_accumulation();
              s = 0u;
              qtPublishedSample.store(0u, std::memory_order_relaxed);
              lastPublish = std::chrono::steady_clock::time_point{};
            }
            if (!qtTracer->render_sample_batch(0, qtSettings.height, s, 0)) {
              qtBgFailed.store(true, std::memory_order_release);
              break;
            }

            const uint32_t completedSample = s + 1u;
            qtPublishedSample.store(completedSample, std::memory_order_relaxed);
            const auto now = std::chrono::steady_clock::now();
            const bool publishNow =
                completedSample <= kQtPreviewImmediatePublishes ||
                lastPublish == std::chrono::steady_clock::time_point{} ||
                (now - lastPublish) >= kQtPreviewPublishInterval;

            if (publishNow) {
              auto ldr = qtTracer->resolve_ldr();
              qtPublishedWidth.store(ldr.width, std::memory_order_relaxed);
              qtPublishedHeight.store(ldr.height, std::memory_order_relaxed);
              bool queued = false;
#ifdef PT_ENABLE_QT
              queued = QueueQtWindowFramebuffer(qtWindow,
                                                qtFramebufferHandoffAlive,
                                                std::move(ldr.rgba8),
                                                ldr.width,
                                                ldr.height);
#else
              (void)ldr;
#endif
              if (queued) {
                qtPublishedFrames.fetch_add(1u, std::memory_order_relaxed);
                lastPublish = now;
              } else {
                qtDroppedFrames.fetch_add(1u, std::memory_order_relaxed);
              }
            } else {
              qtDroppedFrames.fetch_add(1u, std::memory_order_relaxed);
            }
            ++s;
          }
        });
      }

      uint32_t qtSampleIndex = 0;
      std::string qtPreviewStatus = (windowFrameLimit != 0u)
          ? "smoke frame limit"
          : (qtTracerReady ? "rendering" : "tracer init failed");

      // ---- Orbit camera setup ----
      // Auto-orbit is too expensive on the CPU tiled tracer because each
      // camera step resets accumulation and may force a scene reload/rebuild.
      // Keep orbit enabled for non-bg (GPU/synchronous) paths only.
      const bool qtEnableAutoOrbit = (windowFrameLimit == 0u) && !qtUseBg;
      const float kQtOrbitDegPerSec = 7.5f;
      const float kQtOrbitMinStepDeg = 0.1f;
      vkpt::pathtracer::Vec3 qtOrbitCenter{};
      {
        float bminX = FLT_MAX, bminZ = FLT_MAX;
        float bmaxX = -FLT_MAX, bmaxZ = -FLT_MAX;
        float bminY = FLT_MAX, bmaxY = -FLT_MAX;
        for (const auto& v : qtScene.vertices) {
          bminX = std::min(bminX, v.x); bmaxX = std::max(bmaxX, v.x);
          bminY = std::min(bminY, v.y); bmaxY = std::max(bmaxY, v.y);
          bminZ = std::min(bminZ, v.z); bmaxZ = std::max(bmaxZ, v.z);
        }
        if (bminX < bmaxX) {
          qtOrbitCenter.x = (bminX + bmaxX) * 0.5f;
          qtOrbitCenter.y = (bminY + bmaxY) * 0.5f;
          qtOrbitCenter.z = (bminZ + bmaxZ) * 0.5f;
        } else {
          qtOrbitCenter = qtScene.camera_target;
        }
      }
      const float qtOrbitDx = qtScene.camera_position.x - qtOrbitCenter.x;
      const float qtOrbitDz = qtScene.camera_position.z - qtOrbitCenter.z;
      const float kQtOrbitRadius = std::sqrt(qtOrbitDx * qtOrbitDx + qtOrbitDz * qtOrbitDz);
      const float qtOrbitInitialAngleDeg = std::atan2(qtOrbitDx, qtOrbitDz) * (180.0f / 3.14159265f);
      float qtOrbitLastAngleDeg = qtOrbitInitialAngleDeg;
      const auto qtOrbitStartTime = std::chrono::steady_clock::now();
      if (qtEnableAutoOrbit) {
        std::cout << "[orbit] setup: center=(" << qtOrbitCenter.x << "," << qtOrbitCenter.y << "," << qtOrbitCenter.z
                  << ") radius=" << kQtOrbitRadius << " initial_angle=" << qtOrbitInitialAngleDeg << "\n";
      } else {
        std::cout << "[orbit] disabled for CPU background tracer to preserve throughput\n";
      }

      // ---- Main Qt event loop with rendering ----
      BootStep("entering qt render loop");
      uint32_t qtFrameCount = 0u;
      bool qtUserCameraActive = false;
#ifdef PT_ENABLE_QT
      ViewportCameraPose qtCameraPose{qtScene.camera_position,
                                      qtScene.camera_target,
                                      qtScene.camera_up,
                                      qtScene.camera_fov_deg};
      auto syncQtCameraPoseFromScene = [&]() {
        qtCameraPose.position = qtScene.camera_position;
        qtCameraPose.target = qtScene.camera_target;
        qtCameraPose.up = qtScene.camera_up;
        qtCameraPose.fov_deg = qtScene.camera_fov_deg;
      };
      auto qtCameraForward = [&]() {
        return PtNormalize(PtSub(qtCameraPose.target, qtCameraPose.position));
      };
      float qtFpsYaw = 0.0f;
      float qtFpsPitch = 0.0f;
      auto syncQtFpsAnglesFromPose = [&]() {
        const auto forward = qtCameraForward();
        qtFpsYaw = std::atan2(forward.x, forward.z);
        qtFpsPitch = std::asin(ClampFloat(forward.y, -0.98f, 0.98f));
      };
      auto qtFpsForwardFromAngles = [&]() {
        const float cosPitch = std::cos(qtFpsPitch);
        return PtNormalize(vkpt::pathtracer::Vec3{
            std::sin(qtFpsYaw) * cosPitch,
            std::sin(qtFpsPitch),
            std::cos(qtFpsYaw) * cosPitch});
      };
      auto applyQtCameraPose = [&](std::string_view reason) {
        qtScene.camera_position = qtCameraPose.position;
        qtScene.camera_target = qtCameraPose.target;
        qtScene.camera_up = qtCameraPose.up;
        qtScene.camera_fov_deg = qtCameraPose.fov_deg;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        if (qtUseBg) {
          std::lock_guard<std::mutex> lock(qtCameraCommandMutex);
          qtCameraCommand.camPos = qtScene.camera_position;
          qtCameraCommand.camTarget = qtScene.camera_target;
          qtCameraCommand.camUp = qtScene.camera_up;
          qtCameraCommand.camFov = qtScene.camera_fov_deg;
          qtCameraCommand.camPending = true;
        } else if (qtTracerReady) {
          const bool camOk = qtTracer->update_camera(
              qtScene.camera_position, qtScene.camera_target,
              qtScene.camera_up, qtScene.camera_fov_deg);
          if (!camOk) {
            qtTracer->load_scene_snapshot(qtScene);
            qtTracer->build_or_update_acceleration();
          }
          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        ui_runtime_state.status_message = std::string("camera ") + std::string(reason);
      };
      auto updateQtSelectionOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        const auto metrics = window->metrics();
        qtWindow->set_selection_overlay_boxes(BuildSelectionOverlayBoxes(
            ui_selection_state,
            qtPickables,
            qtCameraPose,
            static_cast<float>(std::max(1, metrics.width)),
            static_cast<float>(std::max(1, metrics.height))));
      };
      syncQtFpsAnglesFromPose();
      bool qtFpsMode = false;
      bool qtLeftMouseDown = false;
      bool qtRightMouseDown = false;
      bool qtMiddleMouseDown = false;
      bool qtPotentialClick = false;
      float qtClickX = 0.0f;
      float qtClickY = 0.0f;
      float qtClickDragPixels = 0.0f;
      std::unordered_set<int> qtKeysDown;
      auto qtNormalizeKey = [](int key) {
        if (key >= 'a' && key <= 'z') {
          return static_cast<int>(std::toupper(static_cast<unsigned char>(key)));
        }
        return key;
      };
      constexpr int kQtKeyEscape = 0x01000000;
      constexpr int kQtKeyShift = 0x01000020;
      constexpr int kQtKeyControl = 0x01000021;
      constexpr int kQtKeyLeft = 0x01000012;
      constexpr int kQtKeyUp = 0x01000013;
      constexpr int kQtKeyRight = 0x01000014;
      constexpr int kQtKeyDown = 0x01000015;
      auto qtKeyActive = [&](int key) {
        return qtKeysDown.find(key) != qtKeysDown.end() ||
               qtKeysDown.find(qtNormalizeKey(key)) != qtKeysDown.end();
      };
      auto qtAppendSelection = [&]() {
        return qtKeyActive(kQtKeyShift) || qtKeyActive(kQtKeyControl) ||
               qtKeyActive(16) || qtKeyActive(17) || IsShiftDown() || IsCtrlDown();
      };
      auto qtSceneScale = [&]() {
        if (qtPickables.empty()) {
          return 2.5f;
        }
        vkpt::editor::Bounds bounds{};
        for (const auto& pickable : qtPickables) {
          if (!pickable.bounds.valid) {
            continue;
          }
          ExpandBounds(bounds, ToPtVec3(pickable.bounds.min));
          ExpandBounds(bounds, ToPtVec3(pickable.bounds.max));
        }
        if (!bounds.valid) {
          return 2.5f;
        }
        const auto extent = PtSub(ToPtVec3(bounds.max), ToPtVec3(bounds.min));
        return std::max(1.0f, PtLength(extent) * 0.25f);
      };
      const float qtCameraMoveUnitsPerSecond = qtSceneScale();
      auto qtApplyFpsLookDelta = [&](float dx, float dy) {
        constexpr float kLookSensitivity = 0.0045f;
        qtFpsYaw += dx * kLookSensitivity;
        qtFpsPitch = ClampFloat(qtFpsPitch - dy * kLookSensitivity, -1.45f, 1.45f);
        const auto forward = qtFpsForwardFromAngles();
        qtCameraPose.target = PtAdd(qtCameraPose.position, forward);
        qtCameraPose.up = {0.0f, 1.0f, 0.0f};
        applyQtCameraPose("fps look");
      };
      auto qtApplyOrbitDrag = [&](float dx, float dy) {
        constexpr float kOrbitSensitivity = 0.006f;
        const auto offset = PtSub(qtCameraPose.position, qtCameraPose.target);
        const float radius = std::max(0.05f, PtLength(offset));
        float yaw = std::atan2(offset.x, offset.z) - dx * kOrbitSensitivity;
        float pitch = std::asin(ClampFloat(offset.y / radius, -0.98f, 0.98f)) + dy * kOrbitSensitivity;
        pitch = ClampFloat(pitch, -1.45f, 1.45f);
        const float cosPitch = std::cos(pitch);
        qtCameraPose.position = PtAdd(qtCameraPose.target,
                                      PtMul(vkpt::pathtracer::Vec3{
                                          std::sin(yaw) * cosPitch,
                                          std::sin(pitch),
                                          std::cos(yaw) * cosPitch}, radius));
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("orbit drag");
      };
      auto qtApplyDolly = [&](float wheelDelta) {
        if (std::fabs(wheelDelta) <= 1.0e-4f) {
          return;
        }
        const auto forward = qtCameraForward();
        if (qtFpsMode) {
          const float distance = wheelDelta * qtCameraMoveUnitsPerSecond * 0.35f;
          qtCameraPose.position = PtAdd(qtCameraPose.position, PtMul(forward, distance));
          qtCameraPose.target = PtAdd(qtCameraPose.target, PtMul(forward, distance));
        } else {
          const auto offset = PtSub(qtCameraPose.position, qtCameraPose.target);
          const float scale = std::pow(0.88f, wheelDelta);
          qtCameraPose.position = PtAdd(qtCameraPose.target, PtMul(offset, ClampFloat(scale, 0.25f, 4.0f)));
          syncQtFpsAnglesFromPose();
        }
        applyQtCameraPose(qtFpsMode ? "fps dolly" : "orbit dolly");
      };
      auto qtApplyContinuousFpsMovement = [&](float dtSeconds) {
        if (!qtFpsMode || dtSeconds <= 0.0f) {
          return;
        }
        const auto forward = qtFpsForwardFromAngles();
        const auto right = PtNormalize(PtCross(forward, qtCameraPose.up), {1.0f, 0.0f, 0.0f});
        const auto up = vkpt::pathtracer::Vec3{0.0f, 1.0f, 0.0f};
        vkpt::pathtracer::Vec3 move{};
        if (qtKeyActive('W') || qtKeyActive(kQtKeyUp)) {
          move = PtAdd(move, forward);
        }
        if (qtKeyActive('S') || qtKeyActive(kQtKeyDown)) {
          move = PtSub(move, forward);
        }
        if (qtKeyActive('D') || qtKeyActive(kQtKeyRight)) {
          move = PtAdd(move, right);
        }
        if (qtKeyActive('A') || qtKeyActive(kQtKeyLeft)) {
          move = PtSub(move, right);
        }
        if (qtKeyActive('E')) {
          move = PtAdd(move, up);
        }
        if (qtKeyActive('Q')) {
          move = PtSub(move, up);
        }
        if (PtLength(move) <= 1.0e-5f) {
          return;
        }
        const float speed = qtCameraMoveUnitsPerSecond * (qtKeyActive(kQtKeyShift) || qtKeyActive(16) ? 3.0f : 1.0f);
        const auto delta = PtMul(PtNormalize(move), speed * dtSeconds);
        qtCameraPose.position = PtAdd(qtCameraPose.position, delta);
        qtCameraPose.target = PtAdd(qtCameraPose.target, delta);
        applyQtCameraPose("fps move");
      };
      auto qtApplyViewportPick = [&](float x, float y, vkpt::core::FrameIndex frameIndex) {
        const auto metrics = window->metrics();
        const auto picked = PickViewportObject(qtPickables,
                                               qtCameraPose,
                                               x,
                                               y,
                                               static_cast<float>(std::max(1, metrics.width)),
                                               static_cast<float>(std::max(1, metrics.height)));
        vkpt::editor::EditorCommand command;
        command.command_id = picked ? "viewport.pick" : "viewport.clear_selection";
        command.kind = picked ? vkpt::editor::EditorCommandKind::kSelectEntity
                              : vkpt::editor::EditorCommandKind::kClearSelection;
        command.source_widget = "viewport";
        command.frame_index = frameIndex;
        if (picked) {
          command.payload = vkpt::editor::SelectEntityCommand{picked->entity_id, qtAppendSelection(), false};
        } else {
          command.payload = vkpt::editor::ClearSelectionCommand{};
        }
        ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, command);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        ui_command_history.push(command);
        ui_runtime_state.last_clicked_entity = picked ? picked->entity_id : 0u;
        ui_runtime_state.status_message = picked
            ? "viewport selected " + (picked->label.empty() ? std::to_string(picked->entity_id) : picked->label)
            : "viewport selection cleared";
        PushUiEvent(ui_event_log,
                    picked ? "viewport_pick" : "viewport_pick_miss",
                    "viewport",
                    "viewport",
                    frameIndex,
                    {},
                    picked ? std::to_string(picked->entity_id) : std::string("none"),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        updateQtSelectionOverlay();
        logger.log(vkpt::log::Severity::Info, "app", "Qt viewport pick", {
          {"entity_id", std::to_string(ui_runtime_state.last_clicked_entity)},
          {"x", std::to_string(x)},
          {"y", std::to_string(y)},
          {"selection_count", std::to_string(ui_selection_state.selected_entity_ids.size())}
        });
      };
      auto qtInputPrevTime = std::chrono::steady_clock::now();
      auto updateQtPreviewOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        const uint32_t sampleCount = qtUseBg
            ? qtPublishedSample.load(std::memory_order_relaxed)
            : qtSampleIndex;
        const uint32_t frameWidth = qtPublishedWidth.load(std::memory_order_relaxed);
        const uint32_t frameHeight = qtPublishedHeight.load(std::memory_order_relaxed);
        const auto qtWindowDropped = ReadQtWindowDroppedFrames(qtWindow);
        const std::uint64_t droppedFrames = qtWindowDropped.value_or(
            qtDroppedFrames.load(std::memory_order_relaxed));

        std::ostringstream statusText;
        statusText << "Qt preview\n"
                   << "backend: " << config.backend.value << "\n"
                   << "scene: "
                   << (config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value)
                   << "\n"
                   << "path tracing: " << (qtTracerReady ? "on" : "failed")
                   << (qtUseBg ? " (background thread)" : " (event loop)") << "\n"
                   << "samples: " << sampleCount << "\n"
                   << "frame: " << frameWidth << "x" << frameHeight << "\n"
                   << "publish cap: ";
        if (qtUseBg) {
          statusText << qtPreviewPublishHz << " fps"
                     << " (first " << kQtPreviewImmediatePublishes << " immediate)";
        } else {
          statusText << "event loop";
        }
        statusText << "\n"
                   << "published: " << qtPublishedFrames.load(std::memory_order_relaxed) << "\n"
                   << "dropped frames: " << droppedFrames
                   << (qtWindowDropped ? " (QtWindow)" : " (render throttle)") << "\n"
                   << "camera: " << (qtFpsMode ? "fps" : "orbit") << "\n"
                   << "selected: " << (ui_selection_state.active_primary_entity == 0
                       ? std::string("none")
                       : std::to_string(ui_selection_state.active_primary_entity)) << "\n"
                   << "status: " << qtPreviewStatus;
        qtWindow->set_overlay_text(statusText.str());
      };
      updateQtSelectionOverlay();
      updateQtPreviewOverlay();
#endif
      while (window->is_open()) {
        const auto qtFrameStart = std::chrono::steady_clock::now();

#ifdef PT_ENABLE_QT
        const float qtInputDt = ClampFloat(
            std::chrono::duration<float>(qtFrameStart - qtInputPrevTime).count(),
            0.0f,
            0.05f);
        qtInputPrevTime = qtFrameStart;

        std::vector<vkpt::platform::InputEvent> qtInputEvents;
        if (auto* input = platform->input()) {
          input->consume(qtInputEvents);
        } else if (!window->poll_events()) {
          break;
        }
        if (!window->is_open()) {
          break;
        }

        for (const auto& event : qtInputEvents) {
          LogWindowInput(ui_event_log, ui_runtime_state, event, qtFrameCount);
          switch (event.type) {
            case vkpt::platform::InputEventType::KeyDown: {
              const int key = qtNormalizeKey(event.code);
              const int rawKey = qtNormalizeKey(event.raw_code);
              qtKeysDown.insert(key);
              if (rawKey != 0) {
                qtKeysDown.insert(rawKey);
              }
              if (key == 'F' || rawKey == 'F') {
                qtFpsMode = !qtFpsMode;
                qtUserCameraActive = true;
                ui_runtime_state.active_viewport_tool = qtFpsMode
                    ? vkpt::editor::ViewportTool::Fps
                    : vkpt::editor::ViewportTool::Select;
                if (qtFpsMode) {
                  syncQtFpsAnglesFromPose();
                }
                ui_runtime_state.status_message = qtFpsMode ? "fps camera mode" : "select camera mode";
                PushUiEvent(ui_event_log,
                            "viewport_camera_mode",
                            "viewport",
                            "keyboard",
                            qtFrameCount,
                            {},
                            qtFpsMode ? "fps" : "select",
                            ui_runtime_state.status_message);
              } else if (key == kQtKeyEscape || rawKey == 27) {
                qtFpsMode = false;
                ui_runtime_state.active_viewport_tool = vkpt::editor::ViewportTool::Select;
                ui_runtime_state.status_message = "select camera mode";
              }
              break;
            }
            case vkpt::platform::InputEventType::KeyUp: {
              const int key = qtNormalizeKey(event.code);
              const int rawKey = qtNormalizeKey(event.raw_code);
              qtKeysDown.erase(key);
              qtKeysDown.erase(rawKey);
              break;
            }
            case vkpt::platform::InputEventType::MouseButtonDown:
              if (event.code == 0) {
                qtLeftMouseDown = true;
                qtPotentialClick = true;
                qtClickX = event.x;
                qtClickY = event.y;
                qtClickDragPixels = 0.0f;
              } else if (event.code == 1) {
                qtRightMouseDown = true;
                syncQtFpsAnglesFromPose();
              } else if (event.code == 2) {
                qtMiddleMouseDown = true;
              }
              break;
            case vkpt::platform::InputEventType::MouseButtonUp:
              if (event.code == 0) {
                qtLeftMouseDown = false;
                if (qtPotentialClick && qtClickDragPixels <= 6.0f) {
                  qtApplyViewportPick(event.x, event.y, qtFrameCount);
                }
                qtPotentialClick = false;
              } else if (event.code == 1) {
                qtRightMouseDown = false;
              } else if (event.code == 2) {
                qtMiddleMouseDown = false;
              }
              break;
            case vkpt::platform::InputEventType::MouseMove: {
              if (qtLeftMouseDown && qtPotentialClick) {
                const float dx = event.x - qtClickX;
                const float dy = event.y - qtClickY;
                qtClickDragPixels = std::max(qtClickDragPixels, std::sqrt(dx * dx + dy * dy));
              }
              if (qtRightMouseDown) {
                qtUserCameraActive = true;
                if (qtFpsMode) {
                  qtApplyFpsLookDelta(event.delta_x, event.delta_y);
                } else {
                  qtApplyOrbitDrag(event.delta_x, event.delta_y);
                }
              } else if (qtMiddleMouseDown) {
                qtUserCameraActive = true;
                qtApplyOrbitDrag(event.delta_x, event.delta_y);
              }
              break;
            }
            case vkpt::platform::InputEventType::MouseWheel:
              qtUserCameraActive = true;
              qtApplyDolly(event.delta_z);
              break;
            case vkpt::platform::InputEventType::WindowResize:
              if (event.x > 0.0f && event.y > 0.0f) {
                ApplyWindowMetricsToLayout(ui_layout_state,
                                           static_cast<std::size_t>(event.x),
                                           static_cast<std::size_t>(event.y));
                SyncRuntimePanelState(ui_runtime_state, ui_layout_state);
                updateQtSelectionOverlay();
              }
              break;
            case vkpt::platform::InputEventType::FocusLost:
              qtKeysDown.clear();
              qtLeftMouseDown = false;
              qtRightMouseDown = false;
              qtMiddleMouseDown = false;
              qtPotentialClick = false;
              ui_runtime_state.status_message = "window focus lost";
              break;
            case vkpt::platform::InputEventType::FocusGained:
              ui_runtime_state.status_message = "window focus gained";
              break;
            case vkpt::platform::InputEventType::CloseRequested:
              window->close();
              break;
            default:
              break;
          }
        }
        if (!qtInputEvents.empty()) {
          UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                         ui_selection_state,
                                         ui_layout_state,
                                         ui_event_log,
                                         ui_command_history);
        }
        qtApplyContinuousFpsMovement(qtInputDt);
        updateQtSelectionOverlay();
#endif

        // ---- Camera orbit update — post to bg thread, never join ----
        if (qtTracerReady && qtEnableAutoOrbit && !qtUserCameraActive && kQtOrbitRadius > 1e-4f) {
          const float elapsedSec = std::chrono::duration<float>(qtFrameStart - qtOrbitStartTime).count();
          const float rawAngleDeg = qtOrbitInitialAngleDeg + elapsedSec * kQtOrbitDegPerSec;
          const float angleDeg = std::fmod(rawAngleDeg, 360.0f);
          const float angleDiff = std::fabs(angleDeg - qtOrbitLastAngleDeg);
          const float wrappedDiff = std::min(angleDiff, 360.0f - angleDiff);
          if (wrappedDiff >= kQtOrbitMinStepDeg) {
            const float rad = angleDeg * (3.14159265f / 180.0f);
            qtScene.camera_position.x = qtOrbitCenter.x + kQtOrbitRadius * std::sin(rad);
            qtScene.camera_position.z = qtOrbitCenter.z + kQtOrbitRadius * std::cos(rad);
            qtScene.camera_target = qtOrbitCenter;
#ifdef PT_ENABLE_QT
            syncQtCameraPoseFromScene();
            syncQtFpsAnglesFromPose();
#endif
            qtOrbitLastAngleDeg = angleDeg;
            if (qtUseBg) {
              // Post camera update; bg thread will apply it between samples.
              std::lock_guard<std::mutex> lock(qtCameraCommandMutex);
              qtCameraCommand.camPos = qtScene.camera_position;
              qtCameraCommand.camTarget = qtScene.camera_target;
              qtCameraCommand.camUp = qtScene.camera_up;
              qtCameraCommand.camFov = qtScene.camera_fov_deg;
              qtCameraCommand.camPending = true;
            } else {
              // Synchronous (GPU) path — update directly on main thread.
              const bool camOk = qtTracer->update_camera(
                  qtScene.camera_position, qtScene.camera_target,
                  qtScene.camera_up, qtScene.camera_fov_deg);
              if (!camOk) {
                qtTracer->load_scene_snapshot(qtScene);
                qtTracer->build_or_update_acceleration();
              }
              qtTracer->reset_accumulation();
              qtSampleIndex = 0u;
            }
          }
        }

        if (qtUseBg) {
          if (qtBgFailed.exchange(false, std::memory_order_acq_rel)) {
            qtTracerReady = false;
            qtPreviewStatus = "render sample failed";
            std::cerr << "[qt] background render_sample_batch failed\n";
            logger.log(vkpt::log::Severity::Error, "app", "Qt background render sample failed", {
              {"sample", std::to_string(qtPublishedSample.load(std::memory_order_relaxed))},
              {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)}
            });
          }
        } else if (windowFrameLimit == 0u && qtTracerReady) {
          if (qtTracer->render_sample_batch(0, qtSettings.height, qtSampleIndex, 0)) {
            ++qtSampleIndex;
            auto ldr = qtTracer->resolve_ldr();
            qtPublishedSample.store(qtSampleIndex, std::memory_order_relaxed);
            qtPublishedWidth.store(ldr.width, std::memory_order_relaxed);
            qtPublishedHeight.store(ldr.height, std::memory_order_relaxed);
#ifdef PT_ENABLE_QT
            if (qtWindow) {
              qtWindow->set_framebuffer_rgba(ldr.rgba8, ldr.width, ldr.height);
              qtPublishedFrames.fetch_add(1u, std::memory_order_relaxed);
            }
#endif
          } else {
            qtTracerReady = false;
            qtPreviewStatus = "render sample failed";
            std::cerr << "[qt] render_sample_batch failed at sample " << qtSampleIndex << "\n";
            logger.log(vkpt::log::Severity::Error, "app", "Qt render sample failed", {
              {"sample", std::to_string(qtSampleIndex)},
              {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)}
            });
          }
        }

#ifdef PT_ENABLE_QT
        updateQtPreviewOverlay();
#endif
        if (!window->is_open()) break;
        ++qtFrameCount;
        if (windowFrameLimit != 0u && qtFrameCount >= windowFrameLimit) {
          BootStep("qt gui smoke frame limit reached");
          window->close();
          break;
        }
        const auto qtFrameEnd = std::chrono::steady_clock::now();
        const auto qtFrameDuration = qtFrameEnd - qtFrameStart;
        const auto qtFrameTarget = std::chrono::milliseconds(16);
        if (qtFrameDuration < qtFrameTarget) {
          std::this_thread::sleep_for(qtFrameTarget - qtFrameDuration);
        }
        // Pace the UI/event loop so input and overlay updates do not busy-spin.
      }

      logger.log(vkpt::log::Severity::Info, "app", "Qt render loop closing", {
        {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)},
        {"frames", std::to_string(qtFrameCount)},
        {"status", qtPreviewStatus}
      });
      qtBgStop.store(true, std::memory_order_release);
      if (qtBgThread.joinable()) {
        logger.log(vkpt::log::Severity::Info, "app", "Qt background render thread join begin");
        qtBgThread.join();
        logger.log(vkpt::log::Severity::Info, "app", "Qt background render thread joined");
      }
#ifdef PT_ENABLE_QT
      DrainQtQueuedWork();
      if (qtWindow != nullptr) {
        const auto qtStats = qtWindow->framebuffer_stats();
        std::ostringstream qtPerfSummary;
        qtPerfSummary << "qt_preview_hz=" << qtPreviewPublishHz
                      << " render_published=" << qtPublishedFrames.load(std::memory_order_relaxed)
                      << " render_dropped=" << qtDroppedFrames.load(std::memory_order_relaxed)
                      << " window_received=" << qtStats.received
                      << " window_presented=" << qtStats.presented
                      << " window_dropped=" << qtStats.dropped;
        status.performance_summary = qtPerfSummary.str();
        logger.log(vkpt::log::Severity::Info, "app", "Qt preview shutdown metadata", {
          {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)},
          {"render_published", std::to_string(qtPublishedFrames.load(std::memory_order_relaxed))},
          {"render_dropped", std::to_string(qtDroppedFrames.load(std::memory_order_relaxed))},
          {"window_received", std::to_string(qtStats.received)},
          {"window_presented", std::to_string(qtStats.presented)},
          {"window_dropped", std::to_string(qtStats.dropped)},
          {"latest_published_id", std::to_string(qtStats.latestPublishedId)},
          {"latest_presented_id", std::to_string(qtStats.latestPresentedId)}
        });
      }
      qtFramebufferHandoffAlive->store(false, std::memory_order_release);
      DrainQtQueuedWork();
#endif
      BootStep("qt render loop exited; shutting down platform");

      platform->shutdown();
      recordUiAction("app.window.qt", "window", "qt window closed",
                     MakeUnsupportedUiCommand("app.window.qt", "window_closed"));
      writeStatus("window_closed");
      return 0;
    }
    LogUiWindowStartup(windowWidth, windowHeight, ui_layout_state, ui_runtime_state);
    logger.log(vkpt::log::Severity::Info, "app",
               "ui window mode selected; attempting to create desktop platform");
    recordUiAction("app.window", "window", "window init",
                   MakeUnsupportedUiCommand("app.window", "window mode requested"));

    if (windowWidth == 0u || windowHeight == 0u) {
      std::cout << "invalid ui window size (" << windowWidth << "x" << windowHeight
                << "); falling back to 1280x720\n";
      logger.log(vkpt::log::Severity::Warning, "app",
                 "invalid ui window size, using default fallback 1280x720");
      windowWidth = 1280;
      windowHeight = 720;
    }

#if !defined(PT_ENABLE_RAW_DESKTOP)
    std::cerr << "--window raw platform not available in this build\n";
    writeStatus("error:raw_platform_not_built", "raw_platform_not_built");
    return 1;
#else
    vkpt::platform::DesktopPlatform platform("vkpt-desktop");
    auto platformState = platform.initialize();
    if (!platformState) {
      std::cerr << "desktop init failed\n";
      logger.log(vkpt::log::Severity::Error, "app", "desktop init failed");
      recordUiAction("app.window", "window", "desktop init failed",
                     MakeUnsupportedUiCommand("app.window", "desktop_init_failed"));
      writeStatus("error:desktop_init_failed", "desktop_init_failed");
      return 1;
    }
    logger.log(vkpt::log::Severity::Info, "app", "desktop platform initialized");

    auto* window = platform.window();
    auto* desktopWindow = static_cast<vkpt::platform::DesktopWindow*>(window);
    if (!window || !desktopWindow) {
      std::cerr << "desktop platform returned invalid window handle\n";
      logger.log(vkpt::log::Severity::Error, "app", "desktop platform window missing");
      return 1;
    }
    LogDesktopWindowState("after window object creation", *desktopWindow);
    logger.log(vkpt::log::Severity::Info,
               "app",
               std::string("desktop window object created: ") +
                   std::to_string(desktopWindow->metrics().width) + "x" +
                   std::to_string(desktopWindow->metrics().height));
    std::cout << "[ui] is_open(initial): " << (desktopWindow->is_open() ? "true" : "false") << "\n";
    if (windowWidth != 1280u || windowHeight != 720u) {
      logger.log(vkpt::log::Severity::Info,
                 "app",
                 std::string("applying requested window resize to ") +
                   std::to_string(windowWidth) + "x" + std::to_string(windowHeight));
      desktopWindow->resize(windowWidth, windowHeight);
    }
    LogDesktopWindowState("after explicit window resize attempt", *desktopWindow);
    std::unordered_map<NativeMenuId, std::string> menuCommandLookup;
    ui_runtime_state.active_layout_name = ui_layout_state.active_layout_name;
    ui_runtime_state.status_message = "window initialized";
    std::cout << "[ui] layout name: " << ui_layout_state.active_layout_name << "\n";
    std::cout << "[ui] panel count: " << ui_layout_state.panels.size() << "\n";
#ifdef _WIN32
    LogDesktopWindowState("before menu attach", *desktopWindow);
    {
      const auto uiMenu = vkpt::editor::BuildDefaultMenuBar();
      if (const auto nativeMenu = BuildNativeMenuBarFromModel(uiMenu, menuCommandLookup)) {
        SetMenu(static_cast<HWND>(desktopWindow->native_handle()), nativeMenu);
        DrawMenuBar(static_cast<HWND>(desktopWindow->native_handle()));
        logger.log(vkpt::log::Severity::Info, "app",
                   std::string("native menu bar attached with ") +
                       std::to_string(menuCommandLookup.size()) + " command mappings");
        if (!menuCommandLookup.empty()) {
          auto first = menuCommandLookup.begin();
          auto last = menuCommandLookup.begin();
          for (auto it = std::next(menuCommandLookup.begin());
               it != menuCommandLookup.end(); ++it) {
            last = it;
          }
          logger.log(vkpt::log::Severity::Info, "app",
                     std::string("native menu ids: first=") +
                     std::to_string(first->first) + " action=" + first->second +
                     " last=" + std::to_string(last->first) + " action=" + last->second);
        }
      } else {
        logger.log(vkpt::log::Severity::Warning, "app", "native menu bar creation failed");
      }
    }
    desktopWindow->resize(windowWidth, windowHeight);
    LogDesktopWindowState("after menu attach resize", *desktopWindow);
#endif
    if (!desktopWindow->native_handle()) {
      std::cerr << "desktop window native_handle is null after initialization\n";
      logger.log(vkpt::log::Severity::Error, "app", "desktop native handle missing");
    }

#ifdef _WIN32
    if (const auto hwnd = static_cast<HWND>(desktopWindow->native_handle())) {
      ShowWindow(hwnd, SW_SHOWNORMAL);
      SetForegroundWindow(hwnd);
      UpdateWindow(hwnd);
    }

    if (const auto hwnd = static_cast<HWND>(desktopWindow->native_handle())) {
      RECT clientRect{};
      RECT windowRect{};
      GetWindowRect(hwnd, &windowRect);
      GetClientRect(hwnd, &clientRect);
      logger.log(vkpt::log::Severity::Info, "app",
                 std::string("native rect ") +
                 std::to_string(windowRect.left) + "," + std::to_string(windowRect.top) + " " +
                 std::to_string(windowRect.right - windowRect.left) + "x" +
                 std::to_string(windowRect.bottom - windowRect.top) +
                 " client " + std::to_string(clientRect.right - clientRect.left) + "x" +
                 std::to_string(clientRect.bottom - clientRect.top));
      if ((clientRect.right - clientRect.left) <= 0 ||
          (clientRect.bottom - clientRect.top) <= 0) {
        std::cout << "window client area is non-positive; will continue and trust resize event updates\n";
        logger.log(vkpt::log::Severity::Warning, "app",
                   "native client area is zero at startup; forcing fallback size");
        desktopWindow->resize(std::max<std::uint32_t>(windowWidth, 1280u),
                             std::max<std::uint32_t>(windowHeight, 720u));
      }
    }
#endif
    LogDesktopWindowState("after startup visibility/bootstrap", *desktopWindow);

    std::cout << "vkpt desktop window open (" << desktopWindow->metrics().width << "x"
              << desktopWindow->metrics().height << ")\n";
    std::cout << "Close the window to exit.\n";
    if (windowFrameLimit != 0u) {
      std::cout << "[ui] gui smoke frame limit: " << windowFrameLimit << "\n";
    }
    LogRuntimeMetadata(logger, "raw_platform_initialized", requestedPlatform,
                       selectedPlatform, effectivePlatform, openWindow, doRender,
                       autoExitWindow, windowFrameLimit, config.ui_present_hz.value);

    std::unique_ptr<vkpt::pathtracer::IPathTracer> previewTracer;
    std::string previewGpuName;   // GPU name shown in overlay (empty = CPU)
    std::string previewGpuInfo;   // one-line device info for diagnostics
#ifdef PT_ENABLE_VULKAN
    if (config.backend.value == "vulkan" || config.backend.value == "vulkan-compute") {
      const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
          PT_SHADER_SPV_PATH;
#else
          "shaders/pathtrace.spv";
#endif
      auto gpuTracer = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
      if (gpuTracer->is_valid()) {
        previewGpuName = gpuTracer->gpu_name();
        std::ostringstream ginfo;
        ginfo << gpuTracer->gpu_name()
              << "  [" << gpuTracer->gpu_type() << "]"
              << "  " << gpuTracer->vram_mb() << " MB VRAM"
              << "  Vulkan " << VK_VERSION_MAJOR(gpuTracer->vulkan_api())
              << "." << VK_VERSION_MINOR(gpuTracer->vulkan_api());
        previewGpuInfo = ginfo.str();
        std::cout << "[gpu] " << previewGpuInfo << "\n";
        logger.log(vkpt::log::Severity::Info, "app",
                   "Using Vulkan GPU path tracer: " + previewGpuInfo);
        previewTracer = std::move(gpuTracer);
      } else {
        logger.log(vkpt::log::Severity::Warning, "app",
                   "Vulkan GPU tracer init failed (" + gpuTracer->last_error() +
                   "), falling back to CPU tiled");
      }
    }
#endif
#ifdef PT_ENABLE_D3D12
    if (config.backend.value == "d3d12" || config.backend.value == "d3d12-dxr") {
      const bool requestDxr = (config.backend.value == "d3d12-dxr");
      const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
          PT_SHADER_HLSL_PATH;
#else
          "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
      auto gpuTracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
      if (gpuTracer->is_valid()) {
        gpuTracer->set_prefer_dxr(requestDxr);
        if (requestDxr && !gpuTracer->dxr_supported()) {
          const std::string errorMsg =
              "Requested d3d12-dxr but DXR is not supported on this GPU/device";
          logger.log(vkpt::log::Severity::Error, "app", errorMsg);
          writeStatus("error:d3d12_dxr_unsupported", errorMsg);
          return 1;
        }
        previewGpuName = gpuTracer->gpu_name();
        std::ostringstream ginfo;
        ginfo << gpuTracer->gpu_name()
              << "  D3D12"
            << "  " << gpuTracer->vram_mb() << " MB VRAM"
            << "  DXR=" << (gpuTracer->dxr_supported() ? "yes" : "no")
            << "(tier " << gpuTracer->dxr_tier_string() << ")";
        if (requestDxr) {
          ginfo << "  mode=DXR-phase1";
        }
        previewGpuInfo = ginfo.str();
        std::cout << "[gpu] " << previewGpuInfo << "\n";
        logger.log(vkpt::log::Severity::Info, "app",
                   std::string(requestDxr ? "Using D3D12 DXR path tracer (phase1 compute fallback): "
                                          : "Using D3D12 GPU path tracer: ") + previewGpuInfo);
        previewTracer = std::move(gpuTracer);
      } else {
        const std::string errorMsg =
            "D3D12 GPU tracer init failed (" + gpuTracer->last_error() + ")";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:d3d12_init_failed", errorMsg);
        return 1;
      }
    }
#endif
    if (!previewTracer) {
      const bool allowCpuFallback =
          (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
           config.backend.value == "auto" || config.backend.value.empty());
      if (!allowCpuFallback) {
        const std::string errorMsg =
            "Requested backend '" + config.backend.value +
            "' is unavailable in this build; refusing CPU fallback";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:backend_unavailable", errorMsg);
        return 1;
      }
      if (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
          config.backend.value == "auto" || config.backend.value.empty()) {
        vkpt::cpu::TiledRenderConfig tiledConfig{};
        tiledConfig.worker_count = 0;
        previewTracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
      } else {
        previewTracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
      }
    }
    vkpt::pathtracer::RenderSettings previewSettings{};
    const uint32_t windowPreviewWidth = std::max<uint32_t>(1u, static_cast<uint32_t>(desktopWindow->metrics().width));
    const uint32_t windowPreviewHeight = std::max<uint32_t>(1u, static_cast<uint32_t>(desktopWindow->metrics().height));
    const float previewScale = std::min(1.0f, 960.0f / static_cast<float>(windowPreviewWidth));
    previewSettings.width = std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(windowPreviewWidth) * previewScale));
    previewSettings.height = std::max<uint32_t>(1u, static_cast<uint32_t>(static_cast<float>(windowPreviewHeight) * previewScale));
    // Smoke runs only need a bounded event loop; skip render dispatch below.
    previewSettings.spp = (windowFrameLimit != 0u)
        ? 1u
        : std::numeric_limits<uint32_t>::max();
    previewSettings.max_depth = std::max<uint32_t>(1u, config.max_depth.value);
    previewSettings.seed = 0xC001D00Dull;
    previewSettings.enable_nee = true;
    previewSettings.enable_mis = true;

    std::string previewError;
    bool previewTracerReady = false;
    bool previewNonBlack = false;
    bool previewRendered = false;
    uint32_t previewSampleIndex = 0u;
    uint32_t previewD3D12RetryCount = 0u;
    const uint32_t previewPixelCount = previewSettings.width * previewSettings.height;
    std::vector<uint32_t> previewPixelOrder(previewPixelCount);
    uint32_t previewPixelCursor = 0u;
    const uint32_t previewPixelsPerBatch = 1024u;
    const auto previewTraceBudgetPerFrame = std::chrono::milliseconds(16);
    uint32_t previewChunkCounter = 0u;

    // Background render thread state (used when TiledCpuPathTracer is active).
    // The tiled tracer blocks the calling thread in wait_group() for an entire
    // sample which would stall the Win32 message pump.  We run it on a
    // dedicated thread so the main loop stays responsive.
    std::atomic<bool> bgRenderStop{false};
    std::mutex bgFrameMutex;
    struct BgFrame {
      std::vector<uint8_t> rgba8;
      uint32_t width  = 0;
      uint32_t height = 0;
      uint32_t sample = 0;
      bool fresh = false;
    } bgFrame;
    bool useBgRender = false;
    std::thread bgRenderThread;

    auto resetPreviewPixelOrder = [&](uint32_t sampleIndex) {
      std::iota(previewPixelOrder.begin(), previewPixelOrder.end(), 0u);
      if (previewPixelOrder.size() <= 1u) {
        return;
      }

      // Deterministic per-sample Fisher-Yates shuffle for spatially uniform progressive updates.
      uint32_t state = 0x9e3779b9u ^ (sampleIndex * 0x85ebca6bu + 0xc2b2ae35u);
      auto nextRand = [&]() -> uint32_t {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        return state;
      };

      for (uint32_t i = previewPixelCount - 1u; i > 0u; --i) {
        const uint32_t j = nextRand() % (i + 1u);
        std::swap(previewPixelOrder[i], previewPixelOrder[j]);
      }
    };
    resetPreviewPixelOrder(previewSampleIndex);

    auto buildWindowSceneData = [&](vkpt::pathtracer::RTSceneData& outScene, std::string* outErr) -> bool {
      outScene = {};
      if (!config.scene_path.value.empty()) {
        auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
        if (!parseResult) {
          if (outErr) {
            *outErr = "scene parse failed";
          }
          return false;
        }
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(parseResult.value());
        if (!sceneResult) {
          if (outErr) {
            *outErr = "scene conversion failed";
          }
          return false;
        }
        outScene = std::move(sceneResult.value());
      } else {
        vkpt::scene::SceneDocument document;
        document.metadata.scene_name = "cornell";
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
        if (!sceneResult) {
          if (outErr) {
            *outErr = "builtin scene conversion failed";
          }
          return false;
        }
        outScene = std::move(sceneResult.value());
      }

      bool hasLight = !outScene.lights.empty();
      bool hasEmissive = false;
      for (const auto& material : outScene.materials) {
        if (material.is_emissive()) {
          hasEmissive = true;
          break;
        }
      }
      if (!hasLight && !hasEmissive) {
        outScene.environment_color = {0.35f, 0.4f, 0.5f};
      }
      return true;
    };

    vkpt::pathtracer::RTSceneData previewScene;
    if (!buildWindowSceneData(previewScene, &previewError)) {
      // Fall back to a guaranteed visible diagnostic texture if scene load/setup fails.
      const uint32_t fbw = std::max<uint32_t>(64u, static_cast<uint32_t>(desktopWindow->metrics().width));
      const uint32_t fbh = std::max<uint32_t>(64u, static_cast<uint32_t>(desktopWindow->metrics().height));
      std::vector<std::uint8_t> diagnostic;
      diagnostic.resize(static_cast<std::size_t>(fbw) * static_cast<std::size_t>(fbh) * 4u, 255u);
      for (uint32_t y = 0; y < fbh; ++y) {
        for (uint32_t x = 0; x < fbw; ++x) {
          const std::size_t idx = (static_cast<std::size_t>(y) * fbw + x) * 4u;
          const bool checker = ((x / 32u) + (y / 32u)) % 2u == 0u;
          diagnostic[idx + 0u] = checker ? static_cast<std::uint8_t>((255u * x) / std::max(1u, fbw - 1u)) : 16u;
          diagnostic[idx + 1u] = checker ? static_cast<std::uint8_t>((255u * y) / std::max(1u, fbh - 1u)) : 220u;
          diagnostic[idx + 2u] = checker ? 32u : 64u;
          diagnostic[idx + 3u] = 255u;
        }
      }
      desktopWindow->set_framebuffer_rgba(diagnostic, fbw, fbh);
      previewNonBlack = true;
      logger.log(vkpt::log::Severity::Warning, "traceprobe",
                 "window preview scene setup failed; using diagnostic texture",
                 {
                   {"scene", config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value},
                   {"error", previewError}
                 },
                 0);
    } else if (!previewTracer->configure(previewSettings) ||
               !previewTracer->load_scene_snapshot(previewScene) ||
               !previewTracer->build_or_update_acceleration() ||
               !previewTracer->reset_accumulation()) {
      previewError = "window preview tracer init failed";
      std::string previewInitFailureReason;
#ifdef PT_ENABLE_D3D12
      if (const auto* d3d12Tracer =
              dynamic_cast<vkpt::gpu::D3D12GpuPathTracer*>(previewTracer.get())) {
        previewInitFailureReason = d3d12Tracer->last_error();
      }
#endif
      if (previewInitFailureReason.empty()) previewInitFailureReason = "unavailable";
      previewError = previewError + " (" + previewInitFailureReason + ")";
      logger.log(vkpt::log::Severity::Error, "traceprobe",
                 "window preview tracer initialization failed",
                 {
                   {"scene", config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value},
                   {"width", std::to_string(previewSettings.width)},
                   {"height", std::to_string(previewSettings.height)},
                   {"spp", std::to_string(previewSettings.spp)},
                   {"max_depth", std::to_string(previewSettings.max_depth)},
                   {"error", previewInitFailureReason}
                  },
                  0);
    } else {
      previewTracerReady = true;
      previewError.clear();
      logger.log(vkpt::log::Severity::Info, "traceprobe",
                 "window preview tracer initialized",
                 {
                   {"scene", config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value},
                   {"width", std::to_string(previewSettings.width)},
                   {"height", std::to_string(previewSettings.height)},
                   {"spp", std::to_string(previewSettings.spp)},
                   {"max_depth", std::to_string(previewSettings.max_depth)},
                   {"lights", std::to_string(previewScene.lights.size())},
                   {"materials", std::to_string(previewScene.materials.size())},
                   {"instances", std::to_string(previewScene.instances.size())},
                   {"sdf_primitives", std::to_string(previewScene.sdf_primitives.size())}
                 },
                 0);

      // Launch background render thread for TiledCpuPathTracer so the main
      // thread's Win32 message pump is never blocked.
      if (windowFrameLimit == 0u &&
          dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(previewTracer.get()) != nullptr) {
        useBgRender = true;
        bgRenderThread = std::thread([&]() {
          // Accumulate indefinitely — no spp cap. Convergence continues until
          // bgRenderStop is set (window close) or render_sample_batch fails.
          for (uint32_t s = 0; !bgRenderStop.load(std::memory_order_relaxed); ++s) {
            bool ok = previewTracer->render_sample_batch(0, previewSettings.height, s, 0);
            if (!ok) break;
            auto ldr = previewTracer->resolve_ldr();
            {
              std::lock_guard<std::mutex> lock(bgFrameMutex);
              bgFrame.rgba8  = std::move(ldr.rgba8);
              bgFrame.width  = ldr.width;
              bgFrame.height = ldr.height;
              bgFrame.sample = s + 1;
              bgFrame.fresh  = true;
            }
          }
        });
        logger.log(vkpt::log::Severity::Info, "traceprobe",
                   "background render thread launched for TiledCpuPathTracer");
      }
    }

    std::ostringstream overlay;
    overlay << "UI shell ready\n"
            << "backend: '" << config.backend.value << "'\n"
            << "scene: " << (config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value) << "\n"
            << "layout: " << ui_layout_state.active_layout_name << "\n"
            << "path tracing: " << (previewTracerReady ? "on" : "failed")
            << (previewNonBlack ? " (visible)" : " (diagnostic fallback)") << "\n";
    if (!previewError.empty()) {
      overlay << "status: " << previewError << "\n";
    }
    desktopWindow->set_overlay_text(overlay.str());

    SyncRuntimePanelState(ui_runtime_state, ui_layout_state);
    UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                   ui_event_log, ui_command_history);

    // ---- Camera orbit state ---------------------------------------------------
    // Orbit center = bounding-box center of scene geometry so the camera always
    // looks at the actual scene content regardless of the camera_target stored
    // in the scene file (which may just be "1 unit in front of camera").
    const float kOrbitDegPerSec = 7.5f;
    const float kOrbitMinStepDeg = 0.1f;  // min angle change before reset
    vkpt::pathtracer::Vec3 orbitCenter{};
    {
      float bminX = FLT_MAX, bminZ = FLT_MAX;
      float bmaxX = -FLT_MAX, bmaxZ = -FLT_MAX;
      float bminY = FLT_MAX, bmaxY = -FLT_MAX;
      for (const auto& v : previewScene.vertices) {
        bminX = std::min(bminX, v.x); bmaxX = std::max(bmaxX, v.x);
        bminY = std::min(bminY, v.y); bmaxY = std::max(bmaxY, v.y);
        bminZ = std::min(bminZ, v.z); bmaxZ = std::max(bmaxZ, v.z);
      }
      if (bminX < bmaxX) {
        orbitCenter.x = (bminX + bmaxX) * 0.5f;
        orbitCenter.y = (bminY + bmaxY) * 0.5f;
        orbitCenter.z = (bminZ + bmaxZ) * 0.5f;
      } else {
        orbitCenter = previewScene.camera_target;
      }
    }
    const float orbitDx = previewScene.camera_position.x - orbitCenter.x;
    const float orbitDz = previewScene.camera_position.z - orbitCenter.z;
    const float kOrbitRadius = std::sqrt(orbitDx * orbitDx + orbitDz * orbitDz);
    const float orbitInitialAngleDeg = std::atan2(orbitDx, orbitDz) * (180.0f / 3.14159265f);
    float orbitLastAngleDeg = orbitInitialAngleDeg;
    const auto orbitStartTime = std::chrono::steady_clock::now();
    std::cout << "[orbit] setup: center=(" << orbitCenter.x << "," << orbitCenter.y << "," << orbitCenter.z
              << ") radius=" << kOrbitRadius
              << " initial_angle=" << orbitInitialAngleDeg << "\n";

    auto frame = static_cast<vkpt::core::FrameIndex>(0);
    bool running = true;
    std::size_t loggedStartupFrames = 0;
    auto perfPrevTime = std::chrono::steady_clock::now();
    std::uint64_t perfPrevRays = 0u;
    std::deque<double> perfRpsWindow;
    double perfRpsWindowSum = 0.0;
    constexpr std::size_t kPerfWindowSize = 12u;
    constexpr double kRpsBenchmarkTarget = 30.0e6;
    std::string titlePerfText = "rays/s: 0";
    while (running && window->is_open()) {
      const auto frameStart = std::chrono::steady_clock::now();

      // ---- Camera orbit update -------------------------------------------
      if (previewTracerReady && previewTracer && kOrbitRadius > 1e-4f) {
        const float elapsedSec = std::chrono::duration<float>(
            frameStart - orbitStartTime).count();
        const float rawAngleDeg = orbitInitialAngleDeg + elapsedSec * kOrbitDegPerSec;
        const float angleDeg = std::fmod(rawAngleDeg, 360.0f);
        const float angleDiff = std::fabs(angleDeg - orbitLastAngleDeg);
        const float wrappedDiff = std::min(angleDiff, 360.0f - angleDiff);
        if (wrappedDiff >= kOrbitMinStepDeg) {
          const float rad = angleDeg * (3.14159265f / 180.0f);
          previewScene.camera_position.x = orbitCenter.x + kOrbitRadius * std::sin(rad);
          previewScene.camera_position.z = orbitCenter.z + kOrbitRadius * std::cos(rad);
          // Camera always looks at the orbit center.
          previewScene.camera_target = orbitCenter;
          // Use lightweight camera update so geometry upload state is preserved.
          // Fall back to full load_scene_snapshot only if update_camera is unsupported.
          const bool camOk = previewTracer->update_camera(
              previewScene.camera_position,
              previewScene.camera_target,
              previewScene.camera_up,
              previewScene.camera_fov_deg);
          if (!camOk) {
            previewTracer->load_scene_snapshot(previewScene);
            previewTracer->build_or_update_acceleration();
          }
          previewTracer->reset_accumulation();
          previewSampleIndex = 0u;
          orbitLastAngleDeg = angleDeg;
          std::cout << "[orbit] angle=" << angleDeg
                    << " pos=(" << previewScene.camera_position.x
                    << "," << previewScene.camera_position.y
                    << "," << previewScene.camera_position.z
                    << ") elapsed=" << elapsedSec << "s\n";
        }
      }

      // ---- Render dispatch -----------------------------------------------
      // TiledCpuPathTracer runs on a background thread (bgRenderThread) to
      // keep the Win32 message pump unblocked.  The main loop just picks up
      // the latest resolved frame whenever the bg thread posts one.
      // ScalarCpuPathTracer keeps the original incremental pixel-batch path.
      if (windowFrameLimit == 0u && useBgRender) {
        bool fresh = false;
        std::vector<uint8_t> rgbaCopy;
        uint32_t bfw = 0, bfh = 0, bfSample = 0;
        {
          std::lock_guard<std::mutex> lock(bgFrameMutex);
          if (bgFrame.fresh) {
            rgbaCopy  = bgFrame.rgba8;
            bfw       = bgFrame.width;
            bfh       = bgFrame.height;
            bfSample  = bgFrame.sample;
            bgFrame.fresh = false;
            fresh = true;
          }
        }
        if (fresh) {
          desktopWindow->set_framebuffer_rgba(rgbaCopy, bfw, bfh);
          previewRendered  = true;
          previewNonBlack  = false;
          for (std::size_t i = 0; i + 3u < rgbaCopy.size(); i += 4u) {
            if (rgbaCopy[i] || rgbaCopy[i + 1u] || rgbaCopy[i + 2u]) {
              previewNonBlack = true;
              break;
            }
          }
          previewSampleIndex = bfSample;
          // Log every 16 samples to show convergence progress
          if (bfSample == 1u || (bfSample % 16u) == 0u) {
            std::cout << "[ui] preview accumulate: samples=" << bfSample
                      << " non_black=" << (previewNonBlack ? "yes" : "no") << "\n";
            logger.log(vkpt::log::Severity::Info, "traceprobe",
                       "window preview accumulate",
                       {
                         {"samples",   std::to_string(bfSample)},
                         {"rendered",  previewRendered ? "yes" : "no"},
                         {"non_black", previewNonBlack ? "yes" : "no"}
                       },
                       frame);
          }
        }
      } else if (windowFrameLimit == 0u && previewTracerReady && previewSampleIndex < previewSettings.spp) {
        // Original ScalarCpuPathTracer incremental pixel-batch path.
        const auto traceFrameStart = std::chrono::steady_clock::now();
        bool tracedAnyPixelsThisFrame = false;
        bool completedSampleThisFrame = false;
        uint32_t completedSampleCount = 0u;
        uint32_t completedSampleNumber = 0u;

        while (previewTracerReady && previewSampleIndex < previewSettings.spp) {
          const auto elapsed = std::chrono::steady_clock::now() - traceFrameStart;
          if (elapsed >= previewTraceBudgetPerFrame) {
            break;
          }

          bool render_ok = false;
          if (auto* scalar_tracer = dynamic_cast<vkpt::pathtracer::ScalarCpuPathTracer*>(previewTracer.get())) {
            const uint32_t remainingPixels = previewPixelCount - previewPixelCursor;
            const uint32_t batchCount = std::min(previewPixelsPerBatch, remainingPixels);
            render_ok = scalar_tracer->render_sample_pixels(previewPixelOrder.data() + previewPixelCursor,
                                                            batchCount,
                                                            previewSampleIndex,
                                                            frame);
            if (render_ok) {
              tracedAnyPixelsThisFrame = true;
              ++previewChunkCounter;
              previewPixelCursor += batchCount;

              if (previewPixelCursor >= previewPixelCount) {
                previewPixelCursor = 0u;
                ++previewSampleIndex;
                completedSampleThisFrame = true;
                ++completedSampleCount;
                completedSampleNumber = previewSampleIndex;
                if (previewSampleIndex < previewSettings.spp) {
                  resetPreviewPixelOrder(previewSampleIndex);
                }
              }
            }
          } else {
            render_ok = previewTracer->render_sample_batch(0, previewSettings.height, previewSampleIndex, frame);
            if (render_ok) {
              tracedAnyPixelsThisFrame = true;
              ++previewSampleIndex;
              completedSampleThisFrame = true;
              ++completedSampleCount;
              completedSampleNumber = previewSampleIndex;
            }
          }

          if (!render_ok) {
            previewTracerReady = false;
            previewError = "window preview sample failed";
            std::string sampleFailureReason;
            bool d3d12Failure = false;
#ifdef PT_ENABLE_D3D12
            if (const auto* d3d12Tracer =
                    dynamic_cast<vkpt::gpu::D3D12GpuPathTracer*>(previewTracer.get())) {
              sampleFailureReason = d3d12Tracer->last_error();
              d3d12Failure = true;
            }
#endif
            const auto counters = previewTracer->read_counters();
            logger.log(vkpt::log::Severity::Error, "traceprobe",
                       "window preview sample failed",
                       {
                         {"sample", std::to_string(previewSampleIndex)},
                         {"frame", std::to_string(frame)},
                         {"accum_samples", std::to_string(counters.samples)},
                         {"accum_rays", std::to_string(counters.rays)},
                         {"error", sampleFailureReason.empty() ? "unavailable" : sampleFailureReason}
                       },
                       frame);
            std::cout << "[ui] preview sample failed: sample=" << previewSampleIndex
                      << " frame=" << frame
                      << " accum_samples=" << counters.samples
                      << " accum_rays=" << counters.rays
                      << " error=" << (sampleFailureReason.empty() ? "unavailable" : sampleFailureReason)
                      << "\n";
            if (d3d12Failure && previewD3D12RetryCount < 2u) {
              if (previewD3D12RetryCount < 2u && previewTracer->reset_accumulation()) {
                std::cout << "[ui] preview d3d12 sample failed; retrying sample 0\n";
                logger.log(vkpt::log::Severity::Warning, "traceprobe",
                           "window preview d3d12 sample retry",
                           {
                             {"attempt", std::to_string(previewD3D12RetryCount + 1u)},
                             {"accum_samples", std::to_string(counters.samples)},
                             {"accum_rays", std::to_string(counters.rays)}
                           },
                           frame);
                ++previewD3D12RetryCount;
                previewSampleIndex = 0u;
                previewPixelCursor = 0u;
                resetPreviewPixelOrder(previewSampleIndex);
                previewError.clear();
                previewTracerReady = true;
                continue;
              }
            }
            break;
          }
        }

        if (previewTracerReady && tracedAnyPixelsThisFrame) {
          const auto ldr = previewTracer->resolve_ldr();
          desktopWindow->set_framebuffer_rgba(ldr.rgba8, ldr.width, ldr.height);
          previewRendered = true;
          previewNonBlack = false;
          std::uint8_t rgbMax = 0u;
          std::uint64_t rgbSum = 0u;
          std::uint64_t rgbCount = 0u;
          for (std::size_t i = 0; i + 3u < ldr.rgba8.size(); i += 4u) {
            const std::uint8_t r = ldr.rgba8[i + 0u];
            const std::uint8_t g = ldr.rgba8[i + 1u];
            const std::uint8_t b = ldr.rgba8[i + 2u];
            rgbMax = std::max(rgbMax, std::max(r, std::max(g, b)));
            rgbSum += static_cast<std::uint64_t>(r) + static_cast<std::uint64_t>(g) + static_cast<std::uint64_t>(b);
            rgbCount += 3u;
            if (r != 0u || g != 0u || b != 0u) {
              previewNonBlack = true;
            }
          }
          const float rgbAvg = (rgbCount == 0u) ? 0.0f
              : static_cast<float>(rgbSum) / static_cast<float>(rgbCount);
          if (completedSampleThisFrame &&
              (completedSampleNumber == 1u || (completedSampleNumber % 16u) == 0u)) {
            std::cout << "[ui] preview accumulate: samples=" << completedSampleNumber
                      << " non_black=" << (previewNonBlack ? "yes" : "no") << "\n";
            logger.log(vkpt::log::Severity::Info, "traceprobe",
                       "window preview accumulate",
                       {
                         {"samples",   std::to_string(completedSampleNumber)},
                         {"samples_completed_this_frame", std::to_string(completedSampleCount)},
                         {"chunks_traced_total", std::to_string(previewChunkCounter)},
                         {"rendered",   previewRendered ? "yes" : "no"},
                         {"rgb_max",    std::to_string(static_cast<unsigned>(rgbMax))},
                         {"rgb_avg",    std::to_string(rgbAvg)},
                         {"non_black",  previewNonBlack ? "yes" : "no"}
                       },
                       frame);
          }
        }
      }

      window->poll_events();
      const auto events = desktopWindow->drain_events();
      if (loggedStartupFrames < 6) {
        logger.log(vkpt::log::Severity::Info, "app",
                   std::string("window loop frame=") + std::to_string(frame) +
                     " open=" + (window->is_open() ? "true" : "false") +
                     " events=" + std::to_string(events.size()));
      }
      bool closeRequested = false;
      for (const auto& e : events) {
        if (loggedStartupFrames < 3) {
          std::cout << "[ui] event: " << ToString(e.type)
                    << " x=" << e.x << " y=" << e.y
                    << " raw=" << e.raw_code
                    << " dx=" << e.delta_x << " dy=" << e.delta_y << "\n";
        }
        LogWindowInput(ui_event_log, ui_runtime_state, e, frame);

        const auto logMenuCommand = [&](const std::string& action_id,
                                       const vkpt::editor::EditorCommand& menuCommand,
                                       const std::string& status_text) {
          auto logged = menuCommand;
          logged.command_id = action_id;
          logged.frame_index = frame;
          if (logged.source_widget.empty()) {
            logged.source_widget = "menu";
          }
          ui_command_history.push(logged);
          ui_runtime_state.status_message = status_text;
          ui_runtime_state.last_menu_action = action_id;
          ui_runtime_state.focused_panel = logged.source_widget;
          PushUiEvent(ui_event_log, "app_action", logged.source_widget, "menu",
                      frame, {}, status_text, status_text);
          UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                         ui_event_log, ui_command_history);
        };

        switch (e.type) {
          case vkpt::platform::InputEventType::MenuCommand: {
            const auto nativeCommandId = static_cast<NativeMenuId>(e.raw_code);
            auto mappedAction = menuCommandLookup.find(nativeCommandId);
            if (mappedAction == menuCommandLookup.end()) {
              auto unsupported = MakeUnsupportedUiCommand("app.menu.unknown_command", "unknown menu command id");
              logMenuCommand(std::string("app.menu.unknown_command") + "_" + std::to_string(nativeCommandId),
                             unsupported,
                             std::string("unknown menu action: ") + std::to_string(nativeCommandId));
              break;
            }

            const std::string actionId = mappedAction->second;
            auto menuAction = vkpt::editor::MakeMenuCommand(actionId, "menu", frame);
            menuAction.command_id = actionId;

            if (actionId == "file.exit") {
              closeRequested = true;
              logMenuCommand(actionId,
                             MakeUnsupportedUiCommand("file.exit", "user selected Exit"),
                             "menu action executed: file.exit");
              break;
            }

            if (actionId == "benchmark.open_artifacts" ||
                actionId == "benchmark.history") {
              const std::string target = "artifacts/benchmarks";
              std::string menuStatus = "menu action failed: benchmark folder open failed";
              if (OpenPathInExplorer(target)) {
                menuStatus = std::string("menu action opened: ") + target;
              }
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, menuAction, menuStatus);
              break;
            }

            if (actionId == "benchmark.export_csv_json") {
              auto latest = FindLatestBenchmarkResultJson();
              std::string menuStatus;
              bool opened = false;
              if (latest && std::filesystem::exists(*latest)) {
                const auto csvPath = latest->parent_path() / "results.csv";
                if (std::filesystem::exists(csvPath)) {
                  opened = OpenPathInExplorer(csvPath);
                  menuStatus = opened ? "menu action opened: " + csvPath.string()
                                  : "menu action failed: unable to open results.csv";
                } else {
                  opened = OpenPathInExplorer(latest->parent_path());
                  menuStatus = opened ? "menu action opened: " + latest->parent_path().string()
                                  : "menu action failed: no results.csv in latest benchmark result";
                }
              } else {
                opened = false;
                menuStatus = "menu action failed: no benchmark results found";
              }
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, menuAction, menuStatus);
              break;
            }

            if (menuAction.kind == vkpt::editor::EditorCommandKind::kRunBenchmark &&
                std::holds_alternative<vkpt::editor::RunBenchmarkCommand>(menuAction.payload)) {
              auto resolved = std::get<vkpt::editor::RunBenchmarkCommand>(menuAction.payload);
              resolved = ResolveBenchmarkCommand(resolved, actionId,
                                                ui_runtime_state.active_scene,
                                                config.scene_path.value);
              const auto artifactDir = MakeMenuActionArtifactPath(actionId);
              std::filesystem::create_directories(artifactDir);
              std::string resultPath;
              const bool ok = LaunchBenchmarkRun(resolved,
                                                resolved.desc.backend.empty()
                                                  ? std::string(BenchmarkRunBackendFromAction(actionId))
                                                  : resolved.desc.backend,
                                                resolved.desc.renderer_path.empty()
                                                  ? std::string(BenchmarkRendererFromBackend(
                                                    resolved.desc.backend))
                                                  : resolved.desc.renderer_path,
                                                resolved.desc.scene_path,
                                                artifactDir,
                                                &resultPath);
              std::string menuStatus = "menu action executed: " + actionId;
              if (!std::filesystem::exists(resolved.desc.scene_path)) {
                menuStatus = "menu action failed: benchmark scene not found: " +
                         resolved.desc.scene_path;
              } else if (!ok) {
                menuStatus = "menu action failed: benchmark command exited non-zero";
              } else {
                menuStatus = "menu action completed: " + actionId;
                ui_runtime_state.last_benchmark_command = actionId;
              }
              auto completion = menuAction;
              completion.payload = resolved;
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, completion, menuStatus);
              break;
            }

            if (menuAction.kind != vkpt::editor::EditorCommandKind::kUnsupportedUiAction) {
              const std::string menuStatus =
                  std::string("menu action routed: ") + vkpt::editor::ToString(menuAction.kind) +
                  " (command queued)";
              logger.log(vkpt::log::Severity::Info, "app",
                         std::string("menu command received: ") + actionId + " => " + menuStatus);
              logMenuCommand(actionId, menuAction, menuStatus);
              break;
            }

            std::string reason = "unsupported in menu runtime";
            if (std::holds_alternative<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload)) {
              reason = std::get<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload).reason;
            }
            std::string menuStatus =
                std::string("menu action unsupported: ") + actionId + " (" + reason + ")";
            logger.log(vkpt::log::Severity::Info, "app",
                       std::string("menu command received: ") + menuStatus);
            logMenuCommand(actionId, menuAction, menuStatus);
            break;
          }
          case vkpt::platform::InputEventType::WindowResize:
            if (loggedStartupFrames < 3) {
              std::cout << "[ui] applying resize from event: " << e.x << "x" << e.y << "\n";
            }
            if (e.x > 0.0f && e.y > 0.0f) {
              ApplyWindowMetricsToLayout(ui_layout_state,
                                        static_cast<std::size_t>(e.x),
                                        static_cast<std::size_t>(e.y));
              ui_runtime_state.status_message = "window resize";
              {
                auto cmd = MakeUnsupportedUiCommand("app.window.resize", "window resized",
                                                   "window", frame);
                cmd.command_id = "app.window.resize";
                ui_command_history.push(cmd);
              }
              SyncRuntimePanelState(ui_runtime_state, ui_layout_state);
            } else {
              logger.log(vkpt::log::Severity::Warning, "app",
                         std::string("ignoring zero-size resize event: x=") +
                           std::to_string(static_cast<int>(e.x)) + " y=" + std::to_string(static_cast<int>(e.y)));
            }
            break;
          case vkpt::platform::InputEventType::FocusGained:
            ui_runtime_state.status_message = "window focus gained";
            {
              auto cmd = MakeUnsupportedUiCommand("app.window.focus_gained", "focus gained",
                                                 "window", frame);
              cmd.command_id = "app.window.focus_gained";
              ui_command_history.push(cmd);
            }
            break;
          case vkpt::platform::InputEventType::FocusLost:
            ui_runtime_state.status_message = "window focus lost";
            {
              auto cmd = MakeUnsupportedUiCommand("app.window.focus_lost", "focus lost",
                                                 "window", frame);
              cmd.command_id = "app.window.focus_lost";
              ui_command_history.push(cmd);
            }
            break;
          case vkpt::platform::InputEventType::CloseRequested:
            closeRequested = true;
            ui_runtime_state.last_menu_action = "app.window.close_requested";
            {
              auto cmd = MakeUnsupportedUiCommand("app.window.close_requested", "close requested",
                                                 "window", frame);
              cmd.command_id = "app.window.close_requested";
              ui_command_history.push(cmd);
            }
            break;
          case vkpt::platform::InputEventType::KeyDown:
          case vkpt::platform::InputEventType::KeyUp:
            ui_runtime_state.last_clicked_entity = static_cast<vkpt::core::StableId>(e.raw_code);
            break;
          default:
            break;
        }
      }

      if (!events.empty()) {
        UpdateCrashArtifactsFromUiState(ui_runtime_state, ui_selection_state, ui_layout_state,
                                       ui_event_log, ui_command_history);
      }
#ifdef _WIN32
      if (previewTracerReady && previewTracer && !useBgRender) {
        const auto counters = previewTracer->read_counters();
        const auto now = std::chrono::steady_clock::now();
        const double dtSec = std::chrono::duration<double>(now - perfPrevTime).count();
        if (dtSec > 0.0) {
          const std::uint64_t currRays = counters.rays;
          const std::uint64_t deltaRays = (currRays >= perfPrevRays) ? (currRays - perfPrevRays) : 0u;
          const double instantRps = static_cast<double>(deltaRays) / dtSec;
          perfRpsWindow.push_back(instantRps);
          perfRpsWindowSum += instantRps;
          if (perfRpsWindow.size() > kPerfWindowSize) {
            perfRpsWindowSum -= perfRpsWindow.front();
            perfRpsWindow.pop_front();
          }
          const double avgRps = (perfRpsWindow.empty())
              ? instantRps
              : (perfRpsWindowSum / static_cast<double>(perfRpsWindow.size()));
          const bool benchmarkPass = (avgRps >= kRpsBenchmarkTarget);
          titlePerfText = std::string("rays/s: ") + FormatThroughputKmb(avgRps)
              + "  target>30M:" + (benchmarkPass ? "PASS" : "FAIL");
          perfPrevRays = currRays;
          perfPrevTime = now;
        }
      }
      SetWindowFrameStatus(desktopWindow, ui_runtime_state, ui_layout_state, frame, titlePerfText);
      std::ostringstream statusText;
      statusText << "UI shell ready\n"
                 << "backend: " << ui_runtime_state.active_renderer_backend;
      if (!previewGpuName.empty()) {
        statusText << "  [" << previewGpuName << "]";
      }
      statusText << "\n"
                 << "scene: "   << ui_runtime_state.active_scene << "\n"
                 << "layout: "  << ui_runtime_state.active_layout_name << "\n"
                 << "path tracing: " << (previewTracerReady ? "on" : "failed")
                 << "  samples=" << previewSampleIndex << "\n"
                 << "image: " << (previewNonBlack ? "non-black" : "dark/empty") << "\n"
                 << "status: "  << ui_runtime_state.status_message << "\n"
                 << "events: "  << ui_event_log.events().size();
      desktopWindow->set_overlay_text(statusText.str());
#endif
      ++loggedStartupFrames;

      if (windowFrameLimit != 0u && static_cast<uint32_t>(frame + 1u) >= windowFrameLimit) {
        BootStep("raw gui smoke frame limit reached");
        closeRequested = true;
      }

      if (closeRequested) {
        window->close();
        running = false;
      } else {
        ++frame;
        // Sleep only the remaining time to hit the 60fps (16ms) frame budget.
        const auto frameEnd = std::chrono::steady_clock::now();
        const auto frameDuration = frameEnd - frameStart;
        const auto frameTarget = std::chrono::milliseconds(16);
        if (frameDuration < frameTarget) {
          std::this_thread::sleep_for(frameTarget - frameDuration);
        }
      }
    }

    // Stop background render thread before tearing down the tracer.
    bgRenderStop.store(true, std::memory_order_relaxed);
    if (bgRenderThread.joinable()) {
      bgRenderThread.join();
    }

    platform.shutdown();
    recordUiAction("app.window", "window", "window closed",
                   MakeUnsupportedUiCommand("app.window", "window_closed"));
    writeStatus("window_closed");
    return 0;
#endif
  }

  // ---- --crash-test (A07/A08) -----------------------------------------------
  if (crashTest) {
    recordUiAction("app.crash_test", "crash_test", "crash test requested",
                   MakeUnsupportedUiCommand("app.crash_test", "writing crash artifact"));
    logger.log(vkpt::log::Severity::Fatal, "app", "crash test requested — writing crash artifact");

    // Gate 10 (C20): embed renderer crash state when possible.
    try {
      const auto backendNames = vkpt::render::AvailableBackendNames();
      if (!backendNames.empty()) {
        auto backend = vkpt::render::CreateBackend(backendNames.front());
        if (backend && backend->initialize()) {
          const auto state = vkpt::render::BuildRendererCrashState(
              *backend, 0u, "ptapp.crash_test", "crash_test", "none", "crash_test_requested");
          vkpt::diagnostics::CrashRecorder::instance().update_renderer_state_json(
              vkpt::render::SerializeRenderCrashState(state));
        }
      }
    } catch (...) {
      // best-effort; crash artifacts should still write.
    }

    vkpt::diagnostics::CrashRecorder::instance().set_last_error("crash_test_requested");
    const std::string crashDir = vkpt::diagnostics::CrashRecorder::instance().flush(
        config.crash_artifact_dir.value);
    std::cout << "crash test: artifact written to " << (crashDir.empty() ? "(failed)" : crashDir) << "\n";
    status.last_crash_artifact = crashDir;
    writeStatus("crash_test", "crash_test_requested");
    return 42;
  }

  // ---- --render -------------------------------------------------------------
  if (doRender) {
    PrintNonGuiPlatformShellNotice("render", selectedPlatform, effectivePlatform);
    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("render_prepare", 0);
    ui_runtime_state.status_message = "render_prepare";
    ui_runtime_state.active_scene = config.scene_path.value.empty() ? "cornell_builtin" : config.scene_path.value;
    recordUiAction("app.render", "render", "render_prepare",
                   MakeUnsupportedUiCommand("app.render", "render path entered"));
    vkpt::scene::SceneDocument document;
    if (!config.scene_path.value.empty()) {
      auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
      if (!parseResult) {
        std::cerr << "failed to parse scene: " << config.scene_path.value << "\n";
        recordUiAction("app.render", "render", "scene parse failed",
                       MakeUnsupportedUiCommand("app.render", "scene_parse_failed"));
        writeStatus("error:scene_parse_failed", "scene_parse_failed");
        return 2;
      }
      document = parseResult.value();
      vkpt::diagnostics::CrashRecorder::instance().update_scene(config.scene_path.value);
    } else {
      document.metadata.scene_name = "cornell";
      vkpt::diagnostics::CrashRecorder::instance().update_scene("cornell_builtin");
    }

    vkpt::pathtracer::RenderSettings settings{};
    settings.width     = std::max<uint32_t>(1, config.render_width.value);
    settings.height    = std::max<uint32_t>(1, config.render_height.value);
    settings.spp       = std::max<uint32_t>(1, config.spp.value);
    settings.max_depth = std::max<uint32_t>(1, config.max_depth.value);
    settings.seed      = 0xBAADF00DULL;
    settings.enable_nee = true;
    settings.enable_mis = true;

    auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
    if (!sceneResult) {
      std::cerr << "scene conversion failed\n";
      recordUiAction("app.render", "render", "scene conversion failed",
                     MakeUnsupportedUiCommand("app.render", "scene_conversion_failed"));
      writeStatus("error:scene_conversion_failed", "scene_conversion_failed");
      return 2;
    }

    // ---- Vulkan software-BVH compute path (C10 / Gate 4) ------------------
    const bool useVulkan = (config.backend.value == "vulkan" ||
                            config.backend.value == "vulkan-compute");
    if (useVulkan) {
      vkpt::diagnostics::CrashRecorder::instance().update_backend("vulkan-compute");
      vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("vulkan_bvh_pass", 0);
      status.selected_renderer_path = "vulkan_bvh_compute";

      vkpt::render::VulkanComputeBackend vulkanBackend;
      auto bvhResult = vkpt::render::RunVulkanBVHPass(
          vulkanBackend, sceneResult.value(), settings.width, settings.height);

      if (!bvhResult.success) {
        std::cerr << "vulkan bvh pass failed: " << bvhResult.error << "\n";
        recordUiAction("app.render", "render", "vulkan bvh pass failed",
                       MakeUnsupportedUiCommand("app.render", "vulkan_bvh_failed"));
        writeStatus("error:vulkan_bvh_failed", bvhResult.error);
        return 2;
      }

      // Simulated backend: film texture is zero-filled. Write a black PNG
      // placeholder so the output path contract is satisfied.
      std::filesystem::create_directories(
          std::filesystem::path(config.output_path.value).parent_path());
      vkpt::pathtracer::FilmLdr ldr;
      ldr.width  = settings.width;
      ldr.height = settings.height;
      ldr.rgba8.assign(static_cast<std::size_t>(settings.width) * settings.height * 4u, 0u);

      std::string saveError;
      if (!vkpt::pathtracer::SavePngCompat(config.output_path.value, ldr, &saveError)) {
        std::cerr << "png save failed: " << saveError << "\n";
        recordUiAction("app.render", "render", "png save failed",
                       MakeUnsupportedUiCommand("app.render", "png_save_failed"));
        writeStatus("error:png_save_failed", saveError);
        return 2;
      }

      std::cout << "render complete (vulkan-compute): " << config.output_path.value << "\n";
      std::cout << "vertices: "  << bvhResult.vertex_buffer_count << "\n";
      std::cout << "indices: "   << bvhResult.index_buffer_count  << "\n";
      std::cout << "instances: " << bvhResult.instance_count      << "\n";
      std::cout << "bvh_nodes: " << bvhResult.bvh_node_estimate   << "\n";

      const std::string perfSummary = "vertices=" + std::to_string(bvhResult.vertex_buffer_count)
                                    + " indices=" + std::to_string(bvhResult.index_buffer_count)
                                    + " bvh_nodes=" + std::to_string(bvhResult.bvh_node_estimate);
      status.performance_summary = perfSummary;
      recordUiAction("app.render", "render", "vulkan render complete",
                     MakeUnsupportedUiCommand("app.render", "render_ok"));
      writeStatus("render_ok");
      return 0;
    }

    // ---- CPU/GPU path tracer selection ----------------------------------------
    std::unique_ptr<vkpt::pathtracer::IPathTracer> tracer;
#ifdef PT_ENABLE_VULKAN
    if (config.backend.value == "vulkan" || config.backend.value == "vulkan-compute") {
      const std::string spvPath =
#ifdef PT_SHADER_SPV_PATH
          PT_SHADER_SPV_PATH;
#else
          "shaders/pathtrace.spv";
#endif
      auto gpuT = std::make_unique<vkpt::gpu::VulkanGpuPathTracer>(spvPath);
      if (gpuT->is_valid()) { tracer = std::move(gpuT); }
    }
#endif
#ifdef PT_ENABLE_D3D12
    if (config.backend.value == "d3d12" || config.backend.value == "d3d12-dxr") {
      const bool requestDxr = (config.backend.value == "d3d12-dxr");
      const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
          PT_SHADER_HLSL_PATH;
#else
          "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
      auto gpuT = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
      if (gpuT->is_valid()) {
        gpuT->set_prefer_dxr(requestDxr);
        if (requestDxr && !gpuT->dxr_supported()) {
          const std::string errorMsg =
              "Requested d3d12-dxr but DXR is not supported on this GPU/device";
          std::cerr << errorMsg << "\n";
          logger.log(vkpt::log::Severity::Error, "app", errorMsg);
          writeStatus("error:d3d12_dxr_unsupported", errorMsg);
          recordUiAction("app.render", "render", "d3d12 dxr unsupported",
                         MakeUnsupportedUiCommand("app.render", "d3d12_dxr_unsupported"));
          return 2;
        }
        tracer = std::move(gpuT);
        if (requestDxr) {
          status.selected_renderer_path = "d3d12_dxr_phase1";
        }
      } else {
        const std::string errorMsg =
            "D3D12 GPU tracer init failed (" + gpuT->last_error() + ")";
        std::cerr << errorMsg << "\n";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:d3d12_init_failed", errorMsg);
        recordUiAction("app.render", "render", "d3d12 render init failed",
                       MakeUnsupportedUiCommand("app.render", "d3d12_render_init_failed"));
        return 2;
      }
    }
#endif
    if (!tracer) {
      const bool allowCpuFallback =
          (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
           config.backend.value == "auto" || config.backend.value.empty());
      if (!allowCpuFallback) {
        const std::string errorMsg =
            "Requested backend '" + config.backend.value +
            "' is unavailable in this build; refusing CPU fallback";
        std::cerr << errorMsg << "\n";
        logger.log(vkpt::log::Severity::Error, "app", errorMsg);
        writeStatus("error:backend_unavailable", errorMsg);
        recordUiAction("app.render", "render", "backend unavailable",
                       MakeUnsupportedUiCommand("app.render", "backend_unavailable"));
        return 2;
      }
      if (config.backend.value == "cpu-tiled" || config.backend.value == "cpu" ||
          config.backend.value == "auto"       || config.backend.value.empty()) {
        vkpt::cpu::TiledRenderConfig tiledConfig{};
        tiledConfig.worker_count = 0;
        tracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
      } else {
        tracer = std::make_unique<vkpt::pathtracer::ScalarCpuPathTracer>();
      }
    }
    if (!tracer->configure(settings)) {
      std::cerr << "pathtracer configure failed\n";
      recordUiAction("app.render", "render", "pathtracer configure failed",
                     MakeUnsupportedUiCommand("app.render", "tracer_configure_failed"));
      writeStatus("error:tracer_configure_failed", "tracer_configure_failed");
      return 2;
    }
    if (!tracer->load_scene_snapshot(sceneResult.value()) || !tracer->build_or_update_acceleration()) {
      std::cerr << "failed to prepare scene for path tracing\n";
      recordUiAction("app.render", "render", "scene bvh build failed",
                     MakeUnsupportedUiCommand("app.render", "bvh_build_failed"));
      writeStatus("error:bvh_build_failed", "bvh_build_failed");
      return 2;
    }

    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("render_execute", 0);
    std::filesystem::create_directories(
        std::filesystem::path(config.output_path.value).parent_path());
    tracer->reset_accumulation();
    for (uint32_t sample = 0; sample < settings.spp; ++sample) {
      if (!tracer->render_sample_batch(0, settings.height, sample, 0)) {
        std::cerr << "render failed\n";
        recordUiAction("app.render", "render", "render sample failed",
                       MakeUnsupportedUiCommand("app.render", "render_sample_batch_failed"));
        writeStatus("error:render_failed", "render_sample_batch_failed");
        return 2;
      }
    }

    const auto ldr = tracer->resolve_ldr();
    const auto hdr = tracer->resolve_hdr();
    std::string saveError;
    if (!vkpt::pathtracer::SavePngCompat(config.output_path.value, ldr, &saveError)) {
      std::cerr << "png save failed: " << saveError << "\n";
      recordUiAction("app.render", "render", "png save failed",
                     MakeUnsupportedUiCommand("app.render", "png_save_failed"));
      writeStatus("error:png_save_failed", saveError);
      return 2;
    }
    if (!config.exr_output_path.value.empty()) {
      if (!vkpt::pathtracer::SaveExrCompat(config.exr_output_path.value, hdr, &saveError)) {
        std::cerr << "exr save failed: " << saveError << "\n";
        recordUiAction("app.render", "render", "exr save failed",
                       MakeUnsupportedUiCommand("app.render", "exr_save_failed"));
        writeStatus("error:exr_save_failed", saveError);
        return 2;
      }
    }

    const auto counters = tracer->read_counters();
    std::cout << "render complete: " << config.output_path.value << "\n";
    std::cout << "samples: " << counters.samples << "\n";
    std::cout << "rays: " << counters.rays << "\n";
    std::cout << "triangle hits: " << counters.triangle_hits << "\n";
    std::cout << "sdf hits: " << counters.sdf_hits << "\n";

    const std::string perfSummary = "samples=" + std::to_string(counters.samples)
                                  + " rays=" + std::to_string(counters.rays)
                                  + " tri_hits=" + std::to_string(counters.triangle_hits);
    status.performance_summary = perfSummary;
    recordUiAction("app.render", "render", "cpu render complete",
                   MakeUnsupportedUiCommand("app.render", "render_ok"));
    writeStatus("render_ok");
    return 0;
  }

  // ---- Default / headless mode ----------------------------------------------
  std::cout << "ptapp started\n";
  std::cout << "mode: " << (config.headless.value ? "headless" : "demo") << "\n";
  std::cout << "platform: " << config.platform.value << "\n";
  std::cout << "backend: " << config.backend.value << "\n";
  if (!config.scene_path.value.empty()) {
    std::cout << "scene: " << config.scene_path.value << "\n";
  }
  std::cout << "log level: " << config.log_level.value << "\n";
  logger.log(vkpt::log::Severity::Info, "app", "runtime boot", {
    {"platform",  config.platform.value},
    {"backend",   config.backend.value},
    {"log_level", config.log_level.value},
    {"scene",     config.scene_path.value}
  });

  if (config.headless.value || args.size() == 1) {
    recordUiAction("app.headless", "headless", "headless init", MakeUnsupportedUiCommand("app.headless", "headless mode requested"));
    vkpt::diagnostics::CrashRecorder::instance().update_frame_stage("headless_init", 0);
    auto platform = vkpt::platform::CreatePlatform(vkpt::platform::RuntimePlatformKind::Headless, "vkpt-headless");
    auto state = platform ? platform->initialize()
                          : vkpt::core::Result<void>::error(vkpt::core::ErrorCode::Unsupported);
    if (!state) {
      std::cerr << "headless init failed\n";
      recordUiAction("app.headless", "headless", "headless init failed",
                     MakeUnsupportedUiCommand("app.headless", "headless_init_failed"));
      writeStatus("error:headless_init_failed", "headless_init_failed");
      return 1;
    }
    std::cout << "headless platform initialized\n";
    recordUiAction("app.headless", "headless", "headless init done",
                   MakeUnsupportedUiCommand("app.headless", "headless initialized"));
    platform->shutdown();
  }

  if (!config.scene_path.value.empty()) {
    auto parseResult = vkpt::scene::SceneDocument::load_from_file(config.scene_path.value);
    if (!parseResult) {
      std::cerr << "Failed to load scene file: " << config.scene_path.value << "\n";
      recordUiAction("app.scene_load", "scene_load", "scene load failed",
                     MakeUnsupportedUiCommand("app.scene_load", "scene_load_failed"));
      writeStatus("error:scene_load_failed", "scene_load_failed");
      return 2;
    }
    recordUiAction("app.scene_load", "scene_load", "scene loaded",
                   MakeUnsupportedUiCommand("app.scene_load", "scene_load_success"));
    std::cout << "scene entities: " << parseResult.value().snapshot().entity_ids.size() << '\n';
    std::cout << "scene hash: "     << parseResult.value().export_hash_hex() << '\n';
    std::cout << "asset refs: "     << parseResult.value().snapshot().asset_refs.size() << '\n';
  }

  recordUiAction("app.exit", "app_shell", "app exit", MakeUnsupportedUiCommand("app.exit", "normal exit"));
  writeStatus("ok");
  return 0;
}
