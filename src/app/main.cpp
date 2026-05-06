#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "app/DoctorChecks.h"
#include "app/UiValidation.h"
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
#include "app/QtDockPanels.h"
#include "app/ViewportInteraction.h"
#endif
#include "platform/PlatformFactory.h"
#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"
#include "scripting/ScriptRuntime.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/D3D12Backend.h"
#include "render/backends/VulkanBackend.h"
#include "render/RenderCoordinator.h"
#include "render/interface/RenderContracts.h"
#include "jobs/JobSystem.h"

namespace {

using vkpt::app::RunDoctor;
using vkpt::app::RunUiModelSmokeTests;
using vkpt::app::RunUiReleaseGateCheck;

#ifdef PT_ENABLE_QT
using namespace vkpt::app;
#endif

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

#ifdef PT_ENABLE_QT
void AppendQtMenuItemsFromModel(std::vector<vkpt::platform::QtMenuItem>& out,
                                const std::vector<vkpt::editor::MenuItem>& items,
                                NativeMenuId& commandId,
                                std::unordered_map<NativeMenuId, std::string>& menuCommands) {
  for (const auto& item : items) {
    vkpt::platform::QtMenuItem menuItem{};
    menuItem.label = item.label;
    menuItem.enabled = item.enabled;
    if (item.children.empty()) {
      menuItem.command_id = static_cast<std::uint32_t>(commandId);
      menuCommands.emplace(commandId, item.id);
      ++commandId;
    } else {
      AppendQtMenuItemsFromModel(menuItem.children, item.children, commandId, menuCommands);
    }
    out.push_back(std::move(menuItem));
  }
}

std::vector<vkpt::platform::QtMenuItem> BuildQtMenuBarFromModel(
    const vkpt::editor::MenuBar& menuModel,
    std::unordered_map<NativeMenuId, std::string>& menuCommands) {
  menuCommands.clear();
  NativeMenuId commandId = kFirstMenuCommandId;
  std::vector<vkpt::platform::QtMenuItem> menus;
  menus.reserve(menuModel.top_level_menus.size());
  AppendQtMenuItemsFromModel(menus, menuModel.top_level_menus, commandId, menuCommands);
  return menus;
}
#endif

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

bool IsUsefulTitleMetadata(std::string_view value) {
  return !value.empty() &&
         value != "unknown" &&
         value != "(none)" &&
         value != "disabled" &&
         value != "unavailable";
}

std::string BuildWindowTitleBase(std::string_view shell_name) {
  std::ostringstream out;
  out << "vkPathTracer";
  if (IsUsefulTitleMetadata(vkpt::build::kProjectVersion)) {
    out << ' ' << vkpt::build::kProjectVersion;
  }
  if (IsUsefulTitleMetadata(vkpt::build::kGitHash)) {
    out << " | git " << vkpt::build::kGitHash;
  }
  if (IsUsefulTitleMetadata(vkpt::build::kBuildType)) {
    out << " | " << vkpt::build::kBuildType;
  }
  if (IsUsefulTitleMetadata(vkpt::build::kTargetOs) ||
      IsUsefulTitleMetadata(vkpt::build::kTargetArch)) {
    out << " | target ";
    out << (IsUsefulTitleMetadata(vkpt::build::kTargetOs) ? vkpt::build::kTargetOs : "unknown");
    out << '/';
    out << (IsUsefulTitleMetadata(vkpt::build::kTargetArch) ? vkpt::build::kTargetArch : "unknown");
  }
  if (!shell_name.empty()) {
    out << " | " << shell_name;
  }
  if (shell_name == "qt" && IsUsefulTitleMetadata(vkpt::build::kQtVersion)) {
    out << " | Qt " << vkpt::build::kQtVersion;
  }
  return out.str();
}

std::string BuildWindowRuntimeTitle(std::string_view title_base,
                                    const vkpt::editor::UiRuntimeState& runtime_state,
                                    const vkpt::editor::UiLayoutDocument& layout_state) {
  std::ostringstream out;
  out << title_base
      << " | layout=" << layout_state.active_layout_name
      << " | scene=" << (runtime_state.active_scene.empty() ? "none" : runtime_state.active_scene)
      << " | backend=" << runtime_state.active_renderer_backend;
  if (!runtime_state.status_message.empty()) {
    out << " | status=" << runtime_state.status_message;
  }
  return out.str();
}

#if defined(PT_ENABLE_RAW_DESKTOP)
void SetWindowFrameStatus(vkpt::platform::DesktopWindow* window,
                         std::string_view title_base,
                         const vkpt::editor::UiRuntimeState& runtime_state,
                         const vkpt::editor::UiLayoutDocument& layout_state,
                         vkpt::core::FrameIndex frame_index,
                         std::string_view perf_text) {
  std::ostringstream out;
  out << BuildWindowRuntimeTitle(title_base, runtime_state, layout_state)
      << " | frame=" << frame_index;
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

#ifdef _WIN32
void RedirectStandardStreamsToConsole() {
  FILE* stream = nullptr;
  freopen_s(&stream, "CONOUT$", "w", stdout);
  freopen_s(&stream, "CONOUT$", "w", stderr);
  freopen_s(&stream, "CONIN$", "r", stdin);
  std::ios::sync_with_stdio(true);
  std::cout.clear();
  std::cerr.clear();
  std::cin.clear();
}

void EnableOptionalConsole(bool requested) {
  if (!requested) {
    return;
  }
  if (GetConsoleWindow() == nullptr) {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
      AllocConsole();
    }
  }
  RedirectStandardStreamsToConsole();
}
#else
void EnableOptionalConsole(bool /*requested*/) {}
#endif

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
  std::cout << "  --render              Render using scalar CPU path tracer\n";
  std::cout << "  --output <path>       Render output PNG path\n";
  std::cout << "  --exr-output <path>   Render output EXR path\n";
  std::cout << "  --width <px>          Render width\n";
  std::cout << "  --height <px>         Render height\n";
  std::cout << "  --spp <samples>       Samples per pixel\n";
  std::cout << "  --max-depth <depth>   Max ray depth\n";
  std::cout << "  --denoiser            Enable GPU denoiser for D3D12 renders\n";
  std::cout << "  --temporal-aa         Enable temporal reuse for D3D12 renders\n";
}

// ---- ptdoctor checks -------------------------------------------------------

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

void PrintAcceleratorDiagnostics(uint32_t width, uint32_t height) {
  std::cout << "accelerators:\n";
  const auto accelerators = vkpt::render::EnumerateD3D12Accelerators(true, true);
  if (accelerators.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto& accelerator : accelerators) {
      std::cout << "  " << vkpt::render::AcceleratorKindToString(accelerator.accelerator_kind)
                << " -> " << accelerator.name << "\n";
      std::cout << "    " << vkpt::render::SerializeAcceleratorCapabilities(accelerator) << "\n";
    }
  }

  vkpt::render::RayBudgetRequest request;
  request.width = width;
  request.height = height;
  request.polygon_frame_budget_ms = 16.6667;
  request.reserved_polygon_ms = 5.0;
  request.merge_budget_ms = 1.0;
  request.include_cpu = true;
  request.include_integrated_gpu = true;
  request.include_warp = false;
  request.require_ray_tracing = false;
  const auto printPlan = [](std::string_view label, const vkpt::render::RayBudgetRequest& planRequest) {
    const auto plan = vkpt::render::BuildD3D12RayBudgetPlan(planRequest);
    std::cout << label << " ray budget plan:\n";
    std::cout << "  request=" << vkpt::render::SerializeRayBudgetRequest(planRequest) << "\n";
    std::cout << "  " << vkpt::render::SerializeRayBudgetPlan(plan) << "\n";
  };
  request.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::Auto;
  printPlan("auto", request);
  request.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::HighPerformance;
  printPlan("high-performance", request);
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

#ifndef PT_ENABLE_QT
std::optional<vkpt::scene::SceneWorld> BuildSceneWorldSnapshot(
    const vkpt::scene::SceneDocument& document) {
  auto worldResult = document.to_world();
  if (!worldResult) {
    return std::nullopt;
  }
  auto world = std::move(worldResult.value());
  world.recompute_world_transforms();
  return world;
}

vkpt::scene::TransformComponent ResolveEntityWorldTransform(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* world) {
  if (world != nullptr) {
    if (const auto* worldTransform = world->world_transform(entity.id)) {
      vkpt::scene::TransformComponent transform = *worldTransform;
      transform.dirty = entity.has_transform ? entity.transform.dirty : false;
      return transform;
    }
  }
  return entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
}

int RunDynamicPhysicsPerformanceGate(std::string scenePath,
                                     std::string backend,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t frames) {
  (void)scenePath;
  (void)backend;
  (void)width;
  (void)height;
  (void)frames;
  std::cerr << "dynamic physics gate requires a Qt/D3D12-enabled build\n";
  return 2;
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
  EnableOptionalConsole(ShouldEnableOptionalConsole(argc, argv));
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
  bool listAccelerators = false;
  bool doRender      = false;
  bool uiModelSmoke  = false;
  bool uiReleaseGate = false;
  bool dynamicPhysicsGate = false;
  bool openWindow    = false;
  bool listGpus      = false;
  bool autoExitWindow = false;
  std::string configFilePath;
  std::string envFilePath = ".env";
  bool envFileExplicit = false;
  bool envFileEnabled = true;
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
  bool gpuDenoiser = false;
  bool temporalAa = false;
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
    else if (token == "--list-accelerators") { listAccelerators = true; }
    else if (token == "--headless")       { headless      = true; }
    else if (token == "--render")         { doRender      = true; }
    else if (token == "--window")         { openWindow    = true; }
    else if (IsConsoleOptInArg(token))     { /* handled before logging init */ }
    else if (token == "--denoiser")       { gpuDenoiser   = true; }
    else if (token == "--temporal-aa")    { temporalAa    = true; }
    else if (token == "--list-gpus")      { listGpus      = true; }
    else if (token == "--crash-test")     { crashTest     = true; }
    else if (token == "--ui-model-smoke") { uiModelSmoke  = true; }
    else if (token == "--ui-release-gate") { uiReleaseGate = true; }
    else if (token == "--dynamic-physics-gate") { dynamicPhysicsGate = true; }
    else if (token == "--exit")           { autoExitWindow = true; }
    else if (token == "--config") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --config\n"; return 1; }
      configFilePath = std::string(args[++i]);
    } else if (token == "--env-file") {
      if (i + 1 >= args.size()) { std::cerr << "missing value for --env-file\n"; return 1; }
      envFilePath = std::string(args[++i]);
      envFileExplicit = true;
      envFileEnabled = true;
    } else if (token == "--no-env-file") {
      envFileEnabled = false;
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

  if (envFileEnabled) {
    std::error_code envFileEc;
    const bool envFileExists = std::filesystem::exists(envFilePath, envFileEc);
    if (envFileExplicit || (envFileExists && !envFileEc)) {
      std::string envFileError;
      if (!vkpt::config::LoadDotEnvFile(envFilePath, false, &envFileError)) {
        std::cerr << envFileError << "\n";
        return 1;
      }
      BootStep("loaded dotenv file: " + envFilePath);
    }
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
  const bool nonGuiCommandMode = doctorMode || listBackends || listAccelerators || doRender || dynamicPhysicsGate;
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

  if (listAccelerators) {
    PrintNonGuiPlatformShellNotice("list-accelerators", selectedPlatform, effectivePlatform);
    PrintAcceleratorDiagnostics(config.render_width.value, config.render_height.value);
    recordUiAction("app.list_accelerators", "list_accelerators", "accelerator list",
                   MakeUnsupportedUiCommand("app.list_accelerators", "handled on cli"));
    writeStatus("list_accelerators");
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
      const std::string qtTitleBase = BuildWindowTitleBase("qt");
      auto platform = vkpt::platform::CreatePlatform(vkpt::platform::RuntimePlatformKind::Qt,
                                                     qtTitleBase);
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

      std::function<void(std::string_view)> qtStartupStep = [&](std::string_view phase) {
        BootStep(phase);
      };

#ifdef PT_ENABLE_QT
      auto* qtWindow = dynamic_cast<vkpt::platform::QtWindow*>(window);
      std::unordered_map<NativeMenuId, std::string> qtMenuCommandLookup;
      auto rebuildQtMenuBar = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        const auto uiMenu = vkpt::editor::BuildDefaultMenuBar(ui_selection_state);
        qtWindow->set_menu_bar(BuildQtMenuBarFromModel(uiMenu, qtMenuCommandLookup));
      };
      qtStartupStep = [&](std::string_view phase) {
        BootStep(phase);
        if (qtWindow == nullptr) {
          return;
        }
        std::ostringstream overlay;
        overlay << "vkPathTracer\n"
                << "Starting: " << phase << "\n"
                << "Scene: " << (config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value) << "\n"
                << "Backend: " << config.backend.value << "\n"
                << "Logs: artifacts/logs/ptapp.log";
        qtWindow->set_title(BuildWindowRuntimeTitle(qtTitleBase, ui_runtime_state, ui_layout_state));
        qtWindow->set_overlay_text(overlay.str());
        qtWindow->set_startup_splash_text(phase);
        vkpt::platform::QtStatusBarText status;
        status.message = "Starting: " + std::string(phase);
        status.fields.push_back(vkpt::platform::QtStatusBarField{
            "startup.backend", "Backend: " + config.backend.value, 0});
        status.fields.push_back(vkpt::platform::QtStatusBarField{
            "startup.scene",
            config.scene_path.value.empty() ? std::string("Scene: builtin") : "Scene: " + config.scene_path.value,
            0});
        qtWindow->set_status_bar_text(status);
        DrainQtQueuedWork(4);
      };
      if (qtWindow) {
        qtWindow->resize(std::max<uint32_t>(1u, windowWidth),
                         std::max<uint32_t>(1u, windowHeight));
        qtStartupStep("preparing Qt shell");
        rebuildQtMenuBar();
        logger.log(vkpt::log::Severity::Info,
                   "app",
                   std::string("Qt menu bar attached with ") +
                       std::to_string(qtMenuCommandLookup.size()) + " command mappings");
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
      qtStartupStep("qt window opened");

      // ---- Path tracer setup ----
      std::unique_ptr<vkpt::pathtracer::IPathTracer> qtTracer;
#ifdef PT_ENABLE_D3D12
      if (config.backend.value == "d3d12" || config.backend.value == "d3d12-dxr") {
        qtStartupStep("initializing d3d12 tracer");
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
        qtStartupStep("falling back to cpu tiled tracer");
        vkpt::cpu::TiledRenderConfig tiledConfig{};
        tiledConfig.worker_count = 0;
        qtTracer = std::make_unique<vkpt::cpu::TiledCpuPathTracer>(tiledConfig);
        std::cout << "[cpu] Using TiledCpuPathTracer\n";
      }

      // ---- Scene loading ----
      vkpt::pathtracer::RTSceneData qtScene;
      vkpt::scene::SceneDocument qtSceneDocument;
      qtStartupStep("loading scene snapshot");
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
      qtStartupStep("scene loaded");

      // ---- Tracer configure ----
      vkpt::pathtracer::RenderSettings qtSettings{};
      qtSettings.width  = std::max<uint32_t>(1u, config.render_width.value);
      qtSettings.height = std::max<uint32_t>(1u, config.render_height.value);
      qtSettings.spp    = std::numeric_limits<uint32_t>::max();
      qtSettings.max_depth = std::max<uint32_t>(1u, config.max_depth.value);
      qtSettings.seed = 0xC001D00Dull;
      qtSettings.enable_nee = true;
      qtSettings.enable_mis = true;
      qtSettings.enable_denoiser = true;
      qtSettings.enable_temporal_aa = true;

      qtStartupStep("configuring renderer and acceleration");
      bool qtTracerReady = (qtTracer->configure(qtSettings) &&
                            qtTracer->load_scene_snapshot(qtScene) &&
                            qtTracer->build_or_update_acceleration() &&
                            qtTracer->reset_accumulation());
      if (!qtTracerReady) {
        std::cerr << "qt window: tracer init failed\n";
        qtStartupStep("tracer init failed");
#ifdef PT_ENABLE_QT
        if (qtWindow != nullptr) {
          qtWindow->finish_startup_splash();
        }
#endif
      } else {
        qtStartupStep("tracer initialized");
      }

#ifdef PT_ENABLE_QT
      std::vector<ViewportPickable> qtPickables =
          BuildViewportPickables(qtSceneDocument, qtScene);
      FpsCollisionWorker qtFpsCollisionWorker;
      qtFpsCollisionWorker.set_pickables(qtPickables);
#endif
      auto qtBuildPhysicsBodies = [&](const vkpt::scene::SceneWorld* world) {
        std::vector<vkpt::physics::PhysicsBodySync> bodies;
        bodies.reserve(qtSceneDocument.entities.size());
        for (const auto& entity : qtSceneDocument.entities) {
          if (!entity.has_physics_body) {
            continue;
          }
          vkpt::physics::PhysicsBodySync sync;
          sync.entity = entity.id;
          sync.body = entity.physics_body;
          sync.transform = ResolveEntityWorldTransform(entity, world);
          bodies.push_back(std::move(sync));
        }
        return bodies;
      };
      auto qtPhysics = vkpt::physics::CreatePhysicsWorld();
      auto qtSyncPhysicsSceneDocumentNow = [&]() {
        if (auto world = BuildSceneWorldSnapshot(qtSceneDocument)) {
          return qtPhysics->sync_from_scene_world(*world);
        }
        return qtPhysics->sync_from_bodies(qtBuildPhysicsBodies(nullptr),
                                           qtSceneDocument.entities.size());
      };
      auto qtPhysicsSummary = qtSyncPhysicsSceneDocumentNow();
      const auto qtPhysicsInfo = qtPhysics->engine_info();
      bool qtPhysicsRuntimeDirty = false;
      auto qtSyncPhysicsFromSceneDocument = [&]() {
        qtPhysicsSummary = qtSyncPhysicsSceneDocumentNow();
        qtPhysicsRuntimeDirty = true;
      };
#ifndef PT_ENABLE_QT
      (void)qtSyncPhysicsFromSceneDocument;
#endif
      const bool qtPhysicsRuntimeEnabled =
          qtPhysicsSummary.enabled_bodies > 0u && qtPhysicsSummary.dynamic_bodies > 0u;
      std::cout << "[physics] " << qtPhysicsInfo.engine_name
                << " bodies=" << qtPhysicsSummary.backend_bodies
                << " dynamic=" << qtPhysicsSummary.dynamic_bodies
                << " static=" << qtPhysicsSummary.static_bodies
                << " worker=" << (qtPhysicsInfo.runs_on_worker_thread ? "yes" : "no") << "\n";

      // ---- Background render coordinator (TiledCpuPathTracer blocks; run off main thread) ----
      // Qt posts coalesced commands and consumes latest-wins display frames.
      const uint32_t qtPreviewPublishHz = std::max<uint32_t>(1u, config.ui_present_hz.value);
      constexpr uint32_t kQtPreviewImmediatePublishes = 4u;
      std::atomic<uint32_t> qtPublishedSample{0u};
      std::atomic<uint32_t> qtPublishedWidth{qtSettings.width};
      std::atomic<uint32_t> qtPublishedHeight{qtSettings.height};
      std::atomic<std::uint64_t> qtPublishedRays{0u};
      std::atomic<std::uint64_t> qtPublishedFrames{0u};
      std::atomic<std::uint64_t> qtDroppedFrames{0u};
      std::unique_ptr<vkpt::render::RenderCoordinator> qtRenderCoordinator;
      const bool qtUseBg =
          (dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(qtTracer.get()) != nullptr);
#ifdef PT_ENABLE_QT
      QtDockDeviceStats qtDeviceStats;
      qtDeviceStats.selected_backend = config.backend.value.empty() ? "cpu" : config.backend.value;
      qtDeviceStats.active_renderer_path = qtUseBg ? "cpu_tiled_background" : qtDeviceStats.selected_backend;
      {
        qtDeviceStats.backend_caps.backend_name = qtDeviceStats.selected_backend;
        const auto normalizedBackend = vkpt::render::NormalizeBackendName(qtDeviceStats.selected_backend);
        if (normalizedBackend.find("d3d12") != std::string::npos ||
            normalizedBackend.find("dxr") != std::string::npos) {
          qtDeviceStats.accelerators = vkpt::render::EnumerateD3D12Accelerators(true, true);
        }
        const auto selectedIt = std::find_if(qtDeviceStats.accelerators.begin(),
                                             qtDeviceStats.accelerators.end(),
                                             [](const vkpt::render::AcceleratorCapabilities& accelerator) {
                                               return accelerator.selected_by_default;
                                             });
        if (selectedIt != qtDeviceStats.accelerators.end()) {
          qtDeviceStats.has_selected_accelerator = true;
          qtDeviceStats.selected_accelerator = *selectedIt;
          qtDeviceStats.backend_caps = selectedIt->backend_caps;
        } else if (!qtDeviceStats.accelerators.empty()) {
          qtDeviceStats.has_selected_accelerator = true;
          qtDeviceStats.selected_accelerator = qtDeviceStats.accelerators.front();
          qtDeviceStats.backend_caps = qtDeviceStats.accelerators.front().backend_caps;
        } else if (auto probeBackend = vkpt::render::CreateBackend(qtDeviceStats.selected_backend)) {
          if (probeBackend->initialize()) {
            qtDeviceStats.backend_caps = probeBackend->capabilities();
            probeBackend->shutdown();
          }
        }
      }
#endif
      if (qtTracerReady && qtUseBg) {
        qtStartupStep("starting background cpu render coordinator");
        vkpt::render::RenderCoordinatorConfig coordinatorConfig{};
        coordinatorConfig.publish_hz = qtPreviewPublishHz;
        coordinatorConfig.immediate_publish_count = kQtPreviewImmediatePublishes;
        qtRenderCoordinator = std::make_unique<vkpt::render::RenderCoordinator>(
            std::move(qtTracer),
            qtSettings,
            qtScene,
            coordinatorConfig);
        if (!qtRenderCoordinator->start()) {
          qtTracerReady = false;
          qtStartupStep("background cpu render coordinator start failed");
        }
      }

      uint32_t qtSampleIndex = 0;
      std::string qtPreviewStatus = (windowFrameLimit != 0u)
          ? "smoke frame limit"
          : (qtTracerReady ? "rendering" : "tracer init failed");

      // ---- Orbit camera setup ----
      // Auto-orbit is too expensive on the CPU tiled tracer because each
      // camera step resets accumulation and may force a scene reload/rebuild.
      // Keep orbit enabled for non-bg (GPU/synchronous) paths only.
      const bool qtEnableAutoOrbit = (windowFrameLimit == 0u) && !qtUseBg && !qtPhysicsRuntimeEnabled;
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
      } else if (qtPhysicsRuntimeEnabled) {
        std::cout << "[orbit] disabled for dynamic physics scene; using authored camera\n";
      } else {
        std::cout << "[orbit] disabled for CPU background tracer to preserve throughput\n";
      }

      // ---- Main Qt event loop with rendering ----
      qtStartupStep("entering qt render loop");
      uint32_t qtFrameCount = 0u;
      bool qtUserCameraActive = false;
      constexpr auto kQtInteractiveFrameTarget = std::chrono::milliseconds(16);
      constexpr uint32_t kQtMaxGpuBatchesPerTick = 64u;
      uint32_t qtLastGpuBatchesPerTick = 0u;
      double qtSmoothedGpuBatchMs = 0.0;
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
      std::function<void()> qtSyncActiveCameraObjectFromPose;
      std::function<std::string()> qtActiveCameraObjectName;
      std::chrono::steady_clock::time_point qtLastFpsCameraObjectSync{};
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
      auto qtLastCameraInputTime = std::chrono::steady_clock::time_point{};
      auto applyQtCameraPose = [&](std::string_view reason, bool syncSceneCameraObject = true) {
        qtLastCameraInputTime = std::chrono::steady_clock::now();
        if (syncSceneCameraObject && qtSyncActiveCameraObjectFromPose) {
          qtSyncActiveCameraObjectFromPose();
        }
        qtScene.camera_position = qtCameraPose.position;
        qtScene.camera_target = qtCameraPose.target;
        qtScene.camera_up = qtCameraPose.up;
        qtScene.camera_fov_deg = qtCameraPose.fov_deg;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_camera(vkpt::render::RenderCameraCommand{
              qtScene.camera_position,
              qtScene.camera_target,
              qtScene.camera_up,
              qtScene.camera_fov_deg});
        } else if (!qtUseBg && qtTracerReady) {
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
      auto qtRenderAspect = [&]() {
        const float renderWidth = static_cast<float>(
            std::max(1u, qtPublishedWidth.load(std::memory_order_relaxed)));
        const float renderHeight = static_cast<float>(
            std::max(1u, qtPublishedHeight.load(std::memory_order_relaxed)));
        return renderWidth / renderHeight;
      };
      vkpt::pathtracer::Vec3 qtCameraFocusPoint = qtCameraPose.target;
      float qtCameraFocusDistance =
          std::max(0.25f, PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
      struct QtSavedCameraShot {
        bool valid = false;
        ViewportCameraPose pose{};
        float focus_distance = 1.0f;
      };
      std::array<QtSavedCameraShot, 4> qtSavedCameraShots{};
      int qtActiveCameraShotSlot = 0;
      struct QtViewportImageRect {
        float x = 0.0f;
        float y = 0.0f;
        float width = 1.0f;
        float height = 1.0f;
      };
      auto qtViewportImageRect = [&]() {
        const auto metrics = window->metrics();
        const float viewportWidth = static_cast<float>(std::max(1, metrics.width));
        const float viewportHeight = static_cast<float>(std::max(1, metrics.height));
        const float aspect = std::max(0.01f, qtRenderAspect());
        QtViewportImageRect out{};
        const float viewportAspect = viewportWidth / viewportHeight;
        if (viewportAspect > aspect) {
          out.height = viewportHeight;
          out.width = viewportHeight * aspect;
          out.x = (viewportWidth - out.width) * 0.5f;
          out.y = 0.0f;
        } else {
          out.width = viewportWidth;
          out.height = viewportWidth / aspect;
          out.x = 0.0f;
          out.y = (viewportHeight - out.height) * 0.5f;
        }
        out.width = std::max(1.0f, out.width);
        out.height = std::max(1.0f, out.height);
        return out;
      };
      auto qtViewportLocalPoint = [&](float x, float y) -> std::optional<std::pair<float, float>> {
        const auto frameRect = qtViewportImageRect();
        if (x < frameRect.x || y < frameRect.y ||
            x > frameRect.x + frameRect.width ||
            y > frameRect.y + frameRect.height) {
          return std::nullopt;
        }
        return std::pair<float, float>{x - frameRect.x, y - frameRect.y};
      };
      auto qtOffsetOverlayBoxes = [](std::vector<vkpt::platform::QtSelectionOverlayBox> boxes,
                                     const QtViewportImageRect& frameRect) {
        for (auto& box : boxes) {
          box.x += frameRect.x;
          box.y += frameRect.y;
          for (auto& line : box.lines) {
            line.x0 += frameRect.x;
            line.y0 += frameRect.y;
            line.x1 += frameRect.x;
            line.y1 += frameRect.y;
          }
          for (auto& point : box.points) {
            point.x += frameRect.x;
            point.y += frameRect.y;
          }
        }
        return boxes;
      };
      std::optional<ViewportGizmoHit> qtHoveredGizmoHit;
      auto updateQtSelectionOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        const auto frameRect = qtViewportImageRect();
        auto boxes = BuildSelectionOverlayBoxes(
            ui_selection_state,
            qtPickables,
            qtCameraPose,
            frameRect.width,
            frameRect.height,
            qtRenderAspect(),
            ui_runtime_state.active_gizmo_mode,
            qtHoveredGizmoHit);
        if (qtCameraFocusDistance > 0.0f &&
            (qtScene.camera_focus_distance > 0.0f || qtScene.camera_aperture_radius > 0.0f)) {
          vkpt::platform::QtSelectionOverlayBox focusBox{};
          focusBox.label = "focus";
          focusBox.primary = false;
          const auto forward = PtNormalize(PtSub(qtCameraPose.target, qtCameraPose.position));
          const auto right = PtNormalize(PtCross(forward, qtCameraPose.up), {1.0f, 0.0f, 0.0f});
          const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
          const float half = std::max(0.05f, qtCameraFocusDistance * 0.08f);
          constexpr OverlayColor kFocus{80u, 220u, 255u, 170u};
          AddWorldOverlayLine(focusBox,
                              qtCameraPose,
                              frameRect.width,
                              frameRect.height,
                              qtRenderAspect(),
                              PtSub(qtCameraFocusPoint, PtMul(right, half)),
                              PtAdd(qtCameraFocusPoint, PtMul(right, half)),
                              kFocus,
                              1.5f);
          AddWorldOverlayLine(focusBox,
                              qtCameraPose,
                              frameRect.width,
                              frameRect.height,
                              qtRenderAspect(),
                              PtSub(qtCameraFocusPoint, PtMul(up, half)),
                              PtAdd(qtCameraFocusPoint, PtMul(up, half)),
                              kFocus,
                              1.5f);
          AddWorldOverlayPoint(focusBox,
                               qtCameraPose,
                               frameRect.width,
                               frameRect.height,
                               qtRenderAspect(),
                               qtCameraFocusPoint,
                               kFocus,
                               3.0f);
          if (!focusBox.lines.empty() || !focusBox.points.empty()) {
            boxes.push_back(std::move(focusBox));
          }
        }
        qtWindow->set_selection_overlay_boxes(qtOffsetOverlayBoxes(std::move(boxes), frameRect));
      };
      syncQtFpsAnglesFromPose();
      bool qtFpsMode = false;
      FpsPlayerState qtFpsPlayer{};
      bool qtLeftMouseDown = false;
      bool qtRightMouseDown = false;
      bool qtMiddleMouseDown = false;
      bool qtPotentialClick = false;
      float qtClickX = 0.0f;
      float qtClickY = 0.0f;
      float qtClickDragPixels = 0.0f;
      float qtLastMouseX = 0.0f;
      float qtLastMouseY = 0.0f;
      float qtPendingFpsLookDx = 0.0f;
      float qtPendingFpsLookDy = 0.0f;
      float qtPendingFpsMoveDt = 0.0f;
      std::uint64_t qtFpsMoveSequence = 0u;
      std::uint64_t qtFpsAppliedMoveSequence = 0u;
      bool qtDockPanelsDirty = true;
      auto qtLastFpsDockRefresh = std::chrono::steady_clock::time_point{};
      constexpr auto kQtFpsDockRefreshInterval = std::chrono::milliseconds(250);
      auto qtMarkFpsDockPanelsDirty = [&](bool immediate) {
        const auto now = std::chrono::steady_clock::now();
        if (immediate ||
            qtLastFpsDockRefresh == std::chrono::steady_clock::time_point{} ||
            now - qtLastFpsDockRefresh >= kQtFpsDockRefreshInterval) {
          qtDockPanelsDirty = true;
          qtLastFpsDockRefresh = now;
        }
      };
      auto qtDiscardFpsMovementResults = [&]() {
        qtPendingFpsMoveDt = 0.0f;
        qtFpsAppliedMoveSequence = qtFpsMoveSequence;
        qtFpsCollisionWorker.discard_pending_results();
      };
      struct QtGizmoDragEntityStart {
        vkpt::core::StableId entity_id = 0;
        bool sdf_primitive = false;
        vkpt::scene::TransformComponent transform;
        vkpt::pathtracer::Vec3 local_pivot{};
      };
      struct QtGizmoDragState {
        bool active = false;
        ViewportGizmoHit hit;
        float start_x = 0.0f;
        float start_y = 0.0f;
        std::vector<QtGizmoDragEntityStart> entities;
      } qtGizmoDrag;
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
      constexpr int kQtKeySpace = 0x20;
      auto qtKeyActive = [&](int key) {
        return qtKeysDown.find(key) != qtKeysDown.end() ||
               qtKeysDown.find(qtNormalizeKey(key)) != qtKeysDown.end();
      };
      auto qtAppendSelection = [&]() {
        return qtKeyActive(kQtKeyShift) || qtKeyActive(kQtKeyControl) ||
               qtKeyActive(16) || qtKeyActive(17) || IsShiftDown() || IsCtrlDown();
      };
      auto qtActiveSelectionBounds = [&]() -> std::optional<vkpt::editor::Bounds> {
        if (ui_selection_state.active_primary_entity == 0u) {
          return std::nullopt;
        }
        for (const auto& item : ui_selection_state.per_item_bounds) {
          if (item.entity_id == ui_selection_state.active_primary_entity && item.bounds.valid) {
            return item.bounds;
          }
        }
        return std::nullopt;
      };
      auto qtSetViewportCursor = [&](vkpt::platform::QtViewportCursor cursor) {
        if (qtWindow != nullptr) {
          qtWindow->set_viewport_cursor(cursor);
        }
      };
      auto qtSetViewportMouseLocked = [&](bool locked) {
        if (qtWindow != nullptr) {
          qtWindow->set_viewport_mouse_locked(locked);
        }
      };
      auto qtUpdateGizmoHoverCursor = [&](float x, float y) {
        auto clearHover = [&]() {
          if (qtHoveredGizmoHit) {
            qtHoveredGizmoHit = std::nullopt;
            updateQtSelectionOverlay();
          }
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        };
        if (qtFpsMode ||
            qtLeftMouseDown ||
            qtRightMouseDown ||
            qtMiddleMouseDown ||
            ui_runtime_state.active_gizmo_mode == vkpt::editor::GizmoMode::None) {
          clearHover();
          return;
        }
        const auto activeBounds = qtActiveSelectionBounds();
        if (!activeBounds) {
          clearHover();
          return;
        }
        const auto frameRect = qtViewportImageRect();
        const auto localPoint = qtViewportLocalPoint(x, y);
        if (!localPoint) {
          clearHover();
          return;
        }
        auto hit = PickSelectionGizmoHandle(
            *activeBounds,
            qtCameraPose,
            frameRect.width,
            frameRect.height,
            qtRenderAspect(),
            ui_runtime_state.active_gizmo_mode,
            localPoint->first,
            localPoint->second);
        if (!hit) {
          hit = PickSelectionBoundsFreeform(
              *activeBounds,
              qtCameraPose,
              frameRect.width,
              frameRect.height,
              qtRenderAspect(),
              ui_runtime_state.active_gizmo_mode,
              localPoint->first,
              localPoint->second);
        }
        qtSetViewportCursor(hit ? CursorForGizmoHit(*hit)
                                : vkpt::platform::QtViewportCursor::Default);
        if (!SameGizmoHandle(qtHoveredGizmoHit, hit)) {
          qtHoveredGizmoHit = hit;
          updateQtSelectionOverlay();
        }
      };
      auto qtFindEntity = [&](vkpt::core::StableId id) -> vkpt::scene::SceneEntityDefinition* {
        const auto it = std::find_if(qtSceneDocument.entities.begin(),
                                     qtSceneDocument.entities.end(),
                                     [id](const vkpt::scene::SceneEntityDefinition& entity) {
                                       return entity.id == id;
                                     });
        return it == qtSceneDocument.entities.end() ? nullptr : &*it;
      };
      auto qtFindRtInstanceIndex = [&](vkpt::core::StableId entityId) -> uint32_t {
        for (std::size_t index = 0; index < qtScene.instances.size(); ++index) {
          if (qtScene.instances[index].entity_id == entityId) {
            return static_cast<uint32_t>(index);
          }
        }
        return vkpt::pathtracer::kInvalidRTInstanceIndex;
      };
      auto qtFindSdfPrimitive = [&](vkpt::core::StableId id) -> vkpt::scene::SceneSdfPrimitiveDefinition* {
        const auto it = std::find_if(qtSceneDocument.sdf_primitives.begin(),
                                     qtSceneDocument.sdf_primitives.end(),
                                     [id](const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
                                       return primitive.id == id;
                                     });
        return it == qtSceneDocument.sdf_primitives.end() ? nullptr : &*it;
      };
      auto qtFindTransformEntry = [&](vkpt::core::StableId id) -> const vkpt::scene::SceneTransformEntry* {
        const auto it = std::find_if(qtSceneDocument.transforms.begin(),
                                     qtSceneDocument.transforms.end(),
                                     [id](const vkpt::scene::SceneTransformEntry& entry) {
                                       return entry.id == id;
                                     });
        return it == qtSceneDocument.transforms.end() ? nullptr : &*it;
      };
      auto qtNextSceneObjectId = [&]() {
        vkpt::core::StableId next = 1u;
        auto observe = [&](vkpt::core::StableId id) {
          if (id >= next) {
            next = id + 1u;
          }
        };
        for (const auto& entity : qtSceneDocument.entities) {
          observe(entity.id);
        }
        for (const auto& entry : qtSceneDocument.transforms) {
          observe(entry.id);
        }
        for (const auto& camera : qtSceneDocument.cameras) {
          observe(camera.id);
        }
        for (const auto& light : qtSceneDocument.lights) {
          observe(light.id);
        }
        for (const auto& primitive : qtSceneDocument.sdf_primitives) {
          observe(primitive.id);
        }
        return next;
      };
      auto qtApplyLegacyTransformToEntity = [&](vkpt::scene::SceneEntityDefinition& entity) {
        if (const auto* entry = qtFindTransformEntry(entity.id)) {
          if (!entity.has_transform) {
            entity.has_transform = true;
            entity.transform = entry->transform;
          }
          if (entry->parent != 0u && !entity.has_hierarchy) {
            entity.has_hierarchy = true;
            entity.hierarchy.parent = entry->parent;
          }
        }
      };
      auto qtRemoveLegacyTransformEntry = [&](vkpt::core::StableId id) {
        qtSceneDocument.transforms.erase(
            std::remove_if(qtSceneDocument.transforms.begin(),
                           qtSceneDocument.transforms.end(),
                           [id](const vkpt::scene::SceneTransformEntry& entry) {
                             return entry.id == id;
                           }),
            qtSceneDocument.transforms.end());
      };
      auto qtEnsureSceneObjectEntity = [&](vkpt::core::StableId requestedId,
                                           std::string_view fallbackName)
          -> vkpt::scene::SceneEntityDefinition& {
        const auto id = requestedId != 0u ? requestedId : qtNextSceneObjectId();
        if (auto* entity = qtFindEntity(id)) {
          if (entity->name.empty()) {
            entity->name = std::string(fallbackName);
          }
          qtApplyLegacyTransformToEntity(*entity);
          return *entity;
        }
        vkpt::scene::SceneEntityDefinition entity{};
        entity.id = id;
        entity.name = std::string(fallbackName);
        qtApplyLegacyTransformToEntity(entity);
        qtSceneDocument.entities.push_back(std::move(entity));
        return qtSceneDocument.entities.back();
      };
      auto qtPromoteSceneObjectCamerasAndLights = [&]() {
        if (!qtSceneDocument.cameras.empty()) {
          const auto legacyCameras = std::move(qtSceneDocument.cameras);
          qtSceneDocument.cameras.clear();
          for (const auto& camera : legacyCameras) {
            auto& entity = qtEnsureSceneObjectEntity(camera.id, "Camera");
            entity.has_camera = true;
            entity.camera = camera.camera;
            qtRemoveLegacyTransformEntry(entity.id);
          }
        }
        if (!qtSceneDocument.lights.empty()) {
          const auto legacyLights = std::move(qtSceneDocument.lights);
          qtSceneDocument.lights.clear();
          for (const auto& light : legacyLights) {
            auto& entity = qtEnsureSceneObjectEntity(light.id, "Light");
            entity.has_light = true;
            entity.light = light.light;
            qtRemoveLegacyTransformEntry(entity.id);
          }
        }
      };
      auto qtDocumentHasLightObject = [&]() {
        return std::any_of(qtSceneDocument.entities.begin(),
                           qtSceneDocument.entities.end(),
                           [](const vkpt::scene::SceneEntityDefinition& entity) {
                             return entity.has_light;
                           });
      };
      auto qtRuntimeSceneHasEmissiveMaterial = [&]() {
        return std::any_of(qtScene.materials.begin(),
                           qtScene.materials.end(),
                           [](const vkpt::pathtracer::RTMaterial& material) {
                             return material.is_emissive();
                           });
      };
      auto qtEnsureRuntimeLightObject = [&]() {
        qtPromoteSceneObjectCamerasAndLights();
        if (qtDocumentHasLightObject()) {
          return;
        }
        vkpt::scene::SceneEntityDefinition* entity = nullptr;
        if (!qtScene.lights.empty()) {
          const auto& runtimeLight = qtScene.lights.front();
          auto& lightEntity = qtEnsureSceneObjectEntity(0u, "Key Light");
          lightEntity.has_transform = true;
          lightEntity.transform.translation = ToSceneVec3(runtimeLight.position);
          lightEntity.transform.dirty = true;
          lightEntity.has_light = true;
          lightEntity.light.type = "point";
          lightEntity.light.color = ToSceneVec3(runtimeLight.color);
          lightEntity.light.intensity = runtimeLight.intensity;
          lightEntity.light.radius = runtimeLight.radius;
          lightEntity.light.direction = ToSceneVec3(runtimeLight.direction);
          entity = &lightEntity;
        } else if (!qtRuntimeSceneHasEmissiveMaterial() &&
                   (PtLength(qtScene.environment_color) > 1.0e-6f)) {
          auto& lightEntity = qtEnsureSceneObjectEntity(0u, "Environment Light");
          lightEntity.has_light = true;
          lightEntity.light.type = "environment";
          lightEntity.light.color = ToSceneVec3(qtScene.environment_color);
          lightEntity.light.intensity = 1.0f;
          entity = &lightEntity;
        }
        if (entity != nullptr) {
          qtRemoveLegacyTransformEntry(entity->id);
        }
      };
      auto qtFindActiveCameraEntity = [&]() -> vkpt::scene::SceneEntityDefinition* {
        const auto it = std::find_if(qtSceneDocument.entities.begin(),
                                     qtSceneDocument.entities.end(),
                                     [](const vkpt::scene::SceneEntityDefinition& entity) {
                                       return entity.has_camera;
                                     });
        return it == qtSceneDocument.entities.end() ? nullptr : &*it;
      };
      auto qtEnsureActiveCameraEntity = [&]() -> vkpt::scene::SceneEntityDefinition* {
        qtPromoteSceneObjectCamerasAndLights();
        if (auto* entity = qtFindActiveCameraEntity()) {
          if (entity->name.empty()) {
            entity->name = "Camera";
          }
          return entity;
        }
        auto& entity = qtEnsureSceneObjectEntity(0u, "Camera");
        entity.has_camera = true;
        entity.camera.fov = qtCameraPose.fov_deg;
        entity.camera.focus_distance = qtCameraFocusDistance;
        return &entity;
      };
      auto qtWriteActiveCameraObjectFromPose = [&]() {
        auto* entity = qtEnsureActiveCameraEntity();
        if (entity == nullptr) {
          return;
        }
        entity->has_camera = true;
        entity->camera.fov = qtCameraPose.fov_deg;
        entity->camera.focus_distance = qtCameraFocusDistance;
        entity->has_transform = true;
        entity->transform.translation = ToSceneVec3(qtCameraPose.position);
        entity->transform.rotation = QuatFromCameraForwardUp(qtCameraForward(), qtCameraPose.up);
        entity->transform.dirty = true;
        qtRemoveLegacyTransformEntry(entity->id);
      };
      qtSyncActiveCameraObjectFromPose = qtWriteActiveCameraObjectFromPose;
      qtActiveCameraObjectName = [&]() {
        qtPromoteSceneObjectCamerasAndLights();
        if (const auto* entity = qtFindActiveCameraEntity()) {
          return QtEntityDisplayName(*entity) + " #" + std::to_string(entity->id);
        }
        return std::string("runtime camera");
      };
      auto qtMaybeSyncFpsCameraObject = [&](bool force) {
        if (!qtSyncActiveCameraObjectFromPose) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            qtLastFpsCameraObjectSync != std::chrono::steady_clock::time_point{} &&
            now - qtLastFpsCameraObjectSync < std::chrono::milliseconds(250)) {
          return;
        }
        qtSyncActiveCameraObjectFromPose();
        qtLastFpsCameraObjectSync = now;
      };
      qtPromoteSceneObjectCamerasAndLights();
      qtEnsureRuntimeLightObject();
      qtSyncActiveCameraObjectFromPose();
      auto qtApplySceneLightingFallback = [&]() {
        bool hasLight = !qtScene.lights.empty();
        bool hasEmissive = false;
        for (const auto& mat : qtScene.materials) {
          if (mat.is_emissive()) {
            hasEmissive = true;
            break;
          }
        }
        if (!hasLight && !hasEmissive) {
          qtScene.environment_color = {0.35f, 0.4f, 0.5f};
        }
      };
      auto qtScriptRuntime = vkpt::scripting::CreateScriptRuntime();
      QtDockScriptRuntimeState qtScriptState;
      const auto qtScriptRuntimeStartTime = std::chrono::steady_clock::now();
      auto qtReloadScriptsFromDocument = [&](std::string_view reason) {
        auto worldResult = qtSceneDocument.to_world();
        if (!worldResult) {
          qtScriptState.status = "world build failed";
          ui_runtime_state.last_warning_or_error = "script reload failed: world build";
          qtDockPanelsDirty = true;
          return false;
        }
        auto world = std::move(worldResult.value());
        world.recompute_world_transforms();
        qtScriptState.binding_summary = qtScriptRuntime->reload_bindings(world);
        qtScriptState.bindings = qtScriptRuntime->bindings();
        qtScriptState.diagnostics = qtScriptRuntime->diagnostics();
        qtScriptState.status = std::string("bindings reloaded") +
            (reason.empty() ? std::string{} : " (" + std::string(reason) + ")");
        qtScriptState.last_hook = "reload";
        qtScriptState.last_frame = qtFrameCount;
        qtDockPanelsDirty = true;
        return true;
      };
      auto qtDispatchScriptHook =
          [&](vkpt::scripting::ScriptLifecycleHook hook,
              vkpt::core::FrameIndex frameIndex,
              double dtSeconds,
              std::string_view source) {
        auto worldResult = qtSceneDocument.to_world();
        if (!worldResult) {
          qtScriptState.status = "dispatch failed: world build";
          ui_runtime_state.last_warning_or_error = qtScriptState.status;
          qtDockPanelsDirty = true;
          return false;
        }
        auto world = std::move(worldResult.value());
        world.recompute_world_transforms();
        vkpt::scene::WorldCommandBuffer commands;
        vkpt::scripting::ScriptExecutionContext context;
        context.frame = frameIndex;
        context.delta_seconds = dtSeconds;
        context.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - qtScriptRuntimeStartTime).count();
        context.scripts_enabled = qtScriptState.scripts_enabled;
        context.benchmark_mode = qtSceneDocument.benchmark.enabled;
        context.allow_benchmark_scripts = qtScriptState.benchmark_scripts_allowed;
        qtScriptState.last_dispatch =
            qtScriptRuntime->dispatch_hook(world, hook, context, commands);
        qtScriptState.binding_summary = vkpt::scripting::SummarizeScriptBindings(
            qtScriptRuntime->bindings(),
            qtScriptRuntime->lua_compiled_in(),
            qtScriptRuntime->execution_available());
        qtScriptState.bindings = qtScriptRuntime->bindings();
        qtScriptState.diagnostics = qtScriptRuntime->diagnostics();
        qtScriptState.last_hook = std::string(vkpt::scripting::to_string(hook));
        qtScriptState.last_frame = frameIndex;
        ++qtScriptState.dispatch_count;
        qtScriptState.status =
            std::string(source) + " " + qtScriptState.last_hook +
            " runnable=" + std::to_string(qtScriptState.last_dispatch.runnable_count) +
            " skipped=" + std::to_string(qtScriptState.last_dispatch.skipped_count);
        if (!commands.commands().empty()) {
          qtScriptState.status += " commands=pending";
        }
        ui_runtime_state.status_message = qtScriptState.status;
        qtDockPanelsDirty = true;
        return true;
      };
      qtReloadScriptsFromDocument("startup");
      auto qtLastScriptAutoDispatch = std::chrono::steady_clock::time_point{};
      auto qtApplyScriptPlayback =
          [&](std::chrono::steady_clock::time_point now,
              vkpt::core::FrameIndex frameIndex,
              double dtSeconds) {
        if (!qtScriptState.playing) {
          return false;
        }
        const auto interval = qtScriptRuntime->execution_available()
            ? std::chrono::milliseconds(16)
            : std::chrono::milliseconds(500);
        if (qtLastScriptAutoDispatch != std::chrono::steady_clock::time_point{} &&
            now - qtLastScriptAutoDispatch < interval) {
          return false;
        }
        qtLastScriptAutoDispatch = now;
        return qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate,
                                    frameIndex,
                                    dtSeconds,
                                    "script playback");
      };
      auto qtReloadEditedScene = [&](std::string_view reason) {
        if (qtFpsMode) {
          qtMaybeSyncFpsCameraObject(true);
        }
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
        if (!sceneResult) {
          ui_runtime_state.status_message = "scene edit failed: rebuild scene data";
          qtPreviewStatus = "scene edit failed";
          return false;
        }
        qtScene = std::move(sceneResult.value());
        qtApplySceneLightingFallback();
        syncQtCameraPoseFromScene();
        syncQtFpsAnglesFromPose();
        qtCameraFocusDistance = std::max(
            0.25f,
            qtScene.camera_focus_distance > 0.0f
                ? qtScene.camera_focus_distance
                : PtLength(PtSub(qtCameraPose.target, qtCameraPose.position)));
        qtCameraFocusPoint =
            PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_scene(qtScene);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        qtDockPanelsDirty = true;
        qtSyncPhysicsFromSceneDocument();
        qtReloadScriptsFromDocument("scene edited");
        qtPreviewStatus = qtTracerReady ? "scene edited" : "scene edit renderer reload failed";
        ui_runtime_state.status_message = reason == std::string_view{"animation playback"}
            ? std::string("animation playback")
            : std::string("gizmo ") + std::string(reason);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
      std::unordered_map<vkpt::core::StableId, vkpt::scene::TransformComponent>
          qtAnimationBaseTransforms;
      auto qtRefreshAnimationBaseTransforms = [&](bool replaceExisting) {
        for (const auto& entity : qtSceneDocument.entities) {
          if (entity.animation.clip.empty()) {
            continue;
          }
          if (!replaceExisting && qtAnimationBaseTransforms.contains(entity.id)) {
            continue;
          }
          qtAnimationBaseTransforms[entity.id] =
              entity.has_transform ? entity.transform : vkpt::scene::TransformComponent{};
        }
      };
      auto qtEntityHasAnimatedTransformPath =
          [&](const vkpt::scene::SceneEntityDefinition& entity) {
        if (!entity.animation.clip.empty()) {
          return true;
        }
        std::unordered_set<vkpt::core::StableId> visited;
        vkpt::core::StableId parent = entity.has_hierarchy ? entity.hierarchy.parent : 0u;
        while (parent != 0u && visited.insert(parent).second) {
          const auto* parentEntity = static_cast<const vkpt::scene::SceneEntityDefinition*>(nullptr);
          for (const auto& candidate : qtSceneDocument.entities) {
            if (candidate.id == parent) {
              parentEntity = &candidate;
              break;
            }
          }
          if (parentEntity == nullptr) {
            break;
          }
          if (!parentEntity->animation.clip.empty()) {
            return true;
          }
          parent = parentEntity->has_hierarchy ? parentEntity->hierarchy.parent : 0u;
        }
        return false;
      };
      uint32_t qtAnimationTransformRevision = 1u;
      const auto qtAnimationStartTime = std::chrono::steady_clock::now();
      auto qtApplySceneAnimation = [&](std::chrono::steady_clock::time_point now) {
        if (qtGizmoDrag.active) {
          return false;
        }
        qtRefreshAnimationBaseTransforms(false);
        bool animated = false;
        const float elapsedSeconds =
            std::chrono::duration<float>(now - qtAnimationStartTime).count();
        for (auto& entity : qtSceneDocument.entities) {
          if (entity.animation.clip.empty() ||
              !AnimationHasAuthoredMotion(entity.animation)) {
            continue;
          }
          const auto baseIt = qtAnimationBaseTransforms.find(entity.id);
          if (baseIt == qtAnimationBaseTransforms.end()) {
            continue;
          }
          entity.has_transform = true;
          entity.transform = SampleAnimationTransform(baseIt->second,
                                                      entity.animation,
                                                      elapsedSeconds);
          animated = true;
        }
        if (!animated) {
          return false;
        }

        const auto worldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
        const auto* world = worldSnapshot ? &worldSnapshot.value() : nullptr;
        std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
        for (const auto& entity : qtSceneDocument.entities) {
          if (!entity.has_mesh || !qtEntityHasAnimatedTransformPath(entity)) {
            continue;
          }
          const uint32_t instanceIndex = qtFindRtInstanceIndex(entity.id);
          if (instanceIndex == vkpt::pathtracer::kInvalidRTInstanceIndex) {
            continue;
          }
          const auto worldTransform = ResolveEntityWorldTransform(entity, world);
          vkpt::pathtracer::RTInstanceTransformUpdate update;
          update.entity_id = entity.id;
          update.instance_index = instanceIndex;
          update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                         vkpt::pathtracer::kRTInstanceFlagTransformDirty;
          update.transform_revision = qtAnimationTransformRevision++;
          update.translation = ToPtVec3(worldTransform.translation);
          update.rotation = ToPtQuat4(worldTransform.rotation);
          update.scale = ToPtVec3(worldTransform.scale);
          updates.push_back(update);
        }

        if (!updates.empty() && !qtUseBg && qtTracerReady &&
            qtTracer->update_instance_transforms(updates)) {
          for (const auto& update : updates) {
            if (update.instance_index >= qtScene.instances.size()) {
              continue;
            }
            auto& instance = qtScene.instances[update.instance_index];
            instance.translation = update.translation;
            instance.rotation = update.rotation;
            instance.scale = update.scale;
            instance.flags |= update.flags;
            instance.transform_revision = update.transform_revision;
          }
          qtTracerReady = qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
          qtPublishedSample.store(0u, std::memory_order_relaxed);
          qtPublishedRays.store(0u, std::memory_order_relaxed);
          qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
          qtFpsCollisionWorker.set_pickables(qtPickables);
          RebuildSelectionBounds(ui_selection_state, qtPickables);
          updateQtSelectionOverlay();
          qtPreviewStatus = "animation playback";
          return qtTracerReady;
        }
        return qtReloadEditedScene("animation playback");
      };
      qtRefreshAnimationBaseTransforms(true);
      auto qtReloadRenderSettings = [&](std::string_view reason) {
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        qtPublishedWidth.store(qtSettings.width, std::memory_order_relaxed);
        qtPublishedHeight.store(qtSettings.height, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_settings(qtSettings, qtScene);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->configure(qtSettings) &&
                          qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        qtDockPanelsDirty = true;
        qtPreviewStatus = qtTracerReady ? std::string(reason) : "render settings reload failed";
        return qtTracerReady;
      };
      auto qtSetAuthoredCameraFocusDistance = [&](float distance, std::string_view reason) {
        const float focusDistance = ClampFloat(distance, 0.0f, 100000.0f);
        bool wroteAuthoredCamera = false;
        if (auto* entity = qtEnsureActiveCameraEntity()) {
          entity->camera.focus_distance = focusDistance;
          wroteAuthoredCamera = true;
        }
        qtCameraFocusDistance = std::max(0.25f, focusDistance);
        qtCameraFocusPoint = PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        if (wroteAuthoredCamera) {
          return qtReloadEditedScene(reason);
        }

        qtScene.camera_focus_distance = focusDistance;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        if (qtUseBg && qtRenderCoordinator) {
          qtRenderCoordinator->post_scene(qtScene);
        } else if (!qtUseBg && qtTracerReady) {
          qtTracerReady = qtTracer->load_scene_snapshot(qtScene) &&
                          qtTracer->build_or_update_acceleration() &&
                          qtTracer->reset_accumulation();
          qtSampleIndex = 0u;
        }
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
      auto qtSaveCameraShot = [&](int slot) {
        const int clampedSlot = std::clamp(slot, 0, 3);
        qtSavedCameraShots[static_cast<std::size_t>(clampedSlot)] =
            QtSavedCameraShot{true, qtCameraPose, qtCameraFocusDistance};
        qtActiveCameraShotSlot = clampedSlot;
        qtDockPanelsDirty = true;
        ui_runtime_state.status_message = "camera shot saved";
      };
      auto qtRecallCameraShot = [&](int slot) {
        const int clampedSlot = std::clamp(slot, 0, 3);
        const auto& shot = qtSavedCameraShots[static_cast<std::size_t>(clampedSlot)];
        if (!shot.valid) {
          ui_runtime_state.status_message = "camera shot empty";
          qtDockPanelsDirty = true;
          return false;
        }
        qtActiveCameraShotSlot = clampedSlot;
        qtCameraPose = shot.pose;
        qtCameraFocusDistance = std::max(0.25f, shot.focus_distance);
        qtCameraFocusPoint = PtAdd(qtCameraPose.position, PtMul(qtCameraForward(), qtCameraFocusDistance));
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("shot recall");
        qtSetAuthoredCameraFocusDistance(qtCameraFocusDistance, "shot recall");
        qtDockPanelsDirty = true;
        return true;
      };
      std::function<bool(vkpt::core::FrameIndex)> qtDockAutoFocus;
      std::function<bool(vkpt::core::FrameIndex)> qtDockFocusSelected;
      std::function<void(bool, vkpt::core::FrameIndex, std::string_view)> qtSetFpsMode;
      auto qtApplyDockPropertyEdit = [&](const vkpt::platform::QtDockPropertyEdit& edit,
                                         vkpt::core::FrameIndex frameIndex) {
        const auto parts = QtSplitPropertyPath(edit.property_id);
        bool changed = false;
        bool renderAffecting = false;
        bool settingsAffecting = false;

        auto failEdit = [&](std::string message) {
          ui_runtime_state.status_message = "property edit rejected: " + std::move(message);
          ui_runtime_state.last_warning_or_error = ui_runtime_state.status_message;
          qtDockPanelsDirty = true;
          PushUiEvent(ui_event_log,
                      "dock_property_edit_rejected",
                      edit.panel_id,
                      "dock",
                      frameIndex,
                      {},
                      edit.property_id,
                      ui_runtime_state.status_message);
          return false;
        };

        if (parts.size() >= 2u && parts[0] == "script") {
          if (parts.size() >= 3u && parts[1] == "runtime") {
            if (parts[2] == "enabled") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              qtScriptState.scripts_enabled = value;
              qtScriptState.status = value ? "scripts enabled" : "scripts disabled";
            } else if (parts[2] == "playing") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              qtScriptState.playing = value;
              qtScriptState.status = value ? "script playback running" : "script playback paused";
            } else if (parts[2] == "play") {
              qtScriptState.playing = true;
              qtScriptState.scripts_enabled = true;
              qtScriptState.status = "script playback running";
            } else if (parts[2] == "pause") {
              qtScriptState.playing = false;
              qtScriptState.status = "script playback paused";
            } else if (parts[2] == "step") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnUpdate,
                                   frameIndex,
                                   1.0 / 60.0,
                                   "script step");
            } else if (parts[2] == "reload") {
              qtReloadScriptsFromDocument("dock");
            } else if (parts[2] == "dispatch_on_load") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLoad,
                                   frameIndex,
                                   0.0,
                                   "script manual");
            } else if (parts[2] == "dispatch_fixed_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnFixedUpdate,
                                   frameIndex,
                                   1.0 / 60.0,
                                   "script manual");
            } else if (parts[2] == "dispatch_late_update") {
              qtDispatchScriptHook(vkpt::scripting::ScriptLifecycleHook::OnLateUpdate,
                                   frameIndex,
                                   0.0,
                                   "script manual");
            } else {
              return failEdit("unknown script runtime property");
            }
          } else {
            return failEdit("unknown script property");
          }
          changed = true;
          qtDockPanelsDirty = true;
        } else if (parts.size() >= 2u && parts[0] == "camera") {
          if (parts.size() == 3u && parts[1] == "mode" && parts[2] == "fps_toggle") {
            if (qtSetFpsMode) {
              qtSetFpsMode(!qtFpsMode, frameIndex, "dock");
            } else {
              qtFpsMode = !qtFpsMode;
            }
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "slot") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric camera shot slot");
            }
            qtActiveCameraShotSlot = static_cast<int>(ClampFloat(std::round(value), 1.0f, 4.0f)) - 1;
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "save") {
            bool value = true;
            if (edit.value != "clicked" && !QtParseBool(edit.value, value)) {
              return failEdit("expected true, false, or clicked");
            }
            if (value) {
              qtSaveCameraShot(qtActiveCameraShotSlot);
            }
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "recall") {
            bool value = true;
            if (edit.value != "clicked" && !QtParseBool(edit.value, value)) {
              return failEdit("expected true, false, or clicked");
            }
            if (value && !qtRecallCameraShot(qtActiveCameraShotSlot)) {
              return failEdit("camera shot slot is empty");
            }
          } else if (parts.size() == 3u && parts[1] == "focus" && parts[2] == "pick") {
            if (!qtDockAutoFocus || !qtDockAutoFocus(frameIndex)) {
              return failEdit("auto focus missed");
            }
          } else if (parts.size() == 3u && parts[1] == "focus" && parts[2] == "selected") {
            if (!qtDockFocusSelected || !qtDockFocusSelected(frameIndex)) {
              return failEdit("no active selection to focus");
            }
          } else {
            return failEdit("unknown camera command property");
          }
          changed = true;
        } else if (parts.size() >= 2u && parts[0] == "render") {
          if (parts.size() == 3u && parts[1] == "resolution") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric render resolution");
            }
            const auto dimension =
                static_cast<uint32_t>(ClampFloat(std::round(value), 16.0f, 8192.0f));
            if (parts[2] == "width") {
              qtSettings.width = dimension;
              config.render_width = {dimension, vkpt::config::ConfigSource::CliFlag};
            } else if (parts[2] == "height") {
              qtSettings.height = dimension;
              config.render_height = {dimension, vkpt::config::ConfigSource::CliFlag};
            } else {
              return failEdit("unknown render resolution dimension");
            }
          } else if (parts.size() == 2u && parts[1] == "max_depth") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric max depth");
            }
            qtSettings.max_depth =
                static_cast<uint32_t>(ClampFloat(std::round(value), 1.0f, 256.0f));
          } else if (parts.size() == 2u && parts[1] == "nee") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_nee = value;
          } else if (parts.size() == 2u && parts[1] == "mis") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_mis = value;
          } else if (parts.size() == 2u && parts[1] == "denoiser") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_denoiser = value;
          } else if (parts.size() == 2u && parts[1] == "temporal_aa") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.enable_temporal_aa = value;
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "exposure") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric exposure");
            }
            qtSettings.film_resolve.exposure = ClampFloat(value, 0.0f, 64.0f);
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "tone_map") {
            vkpt::pathtracer::ToneMapMode mode{};
            if (!QtParseToneMapMode(edit.value, mode)) {
              return failEdit("unknown tone mapper");
            }
            qtSettings.film_resolve.tone_map = mode;
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "output_transform") {
            vkpt::pathtracer::OutputTransformMode mode{};
            if (!QtParseOutputTransformMode(edit.value, mode)) {
              return failEdit("unknown output transform");
            }
            qtSettings.film_resolve.output_transform = mode;
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "gamma") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric gamma");
            }
            qtSettings.film_resolve.gamma = ClampFloat(value, 0.01f, 8.0f);
          } else if (parts.size() == 3u && parts[1] == "film" && parts[2] == "clamp_output") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            qtSettings.film_resolve.clamp_output = value;
          } else {
            return failEdit("unknown render settings property");
          }
          changed = true;
          settingsAffecting = true;
        } else if (parts.size() >= 3u && parts[0] == "entity") {
          vkpt::core::StableId entityId = 0u;
          if (!QtParseStableId(parts[1], entityId)) {
            return failEdit("invalid entity id");
          }
          auto* entity = qtFindEntity(entityId);
          if (entity == nullptr) {
            return failEdit("entity not found");
          }
          auto ensurePhysicsBody = [&]() -> vkpt::scene::PhysicsBodyComponent& {
            if (!entity->has_physics_body) {
              entity->has_physics_body = true;
              entity->physics_body = QtDefaultDynamicPhysicsBody();
            }
            return entity->physics_body;
          };

          if (parts.size() == 3u && parts[2] == "name") {
            const std::string value = QtTrim(edit.value);
            if (entity->name != value) {
              entity->name = value;
              changed = true;
            }
          } else if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "transform") {
            if (!entity->has_transform) {
              entity->has_transform = true;
              entity->transform = {};
            }
            if (parts.size() == 5u) {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric transform value");
              }
              const auto& field = parts[3];
              const auto& component = parts[4];
              if (field == "translation") {
                if (component == "x") {
                  entity->transform.translation.x = value;
                } else if (component == "y") {
                  entity->transform.translation.y = value;
                } else if (component == "z") {
                  entity->transform.translation.z = value;
                } else {
                  return failEdit("unknown translation component");
                }
              } else if (field == "rotation") {
                if (component == "x") {
                  entity->transform.rotation.x = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "y") {
                  entity->transform.rotation.y = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "z") {
                  entity->transform.rotation.z = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "w") {
                  entity->transform.rotation.w = ClampFloat(value, -1.0f, 1.0f);
                } else {
                  return failEdit("unknown rotation component");
                }
                const float lenSq =
                    entity->transform.rotation.x * entity->transform.rotation.x +
                    entity->transform.rotation.y * entity->transform.rotation.y +
                    entity->transform.rotation.z * entity->transform.rotation.z +
                    entity->transform.rotation.w * entity->transform.rotation.w;
                if (lenSq <= 1.0e-8f) {
                  entity->transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
                } else {
                  const float invLen = 1.0f / std::sqrt(lenSq);
                  entity->transform.rotation.x *= invLen;
                  entity->transform.rotation.y *= invLen;
                  entity->transform.rotation.z *= invLen;
                  entity->transform.rotation.w *= invLen;
                }
              } else if (field == "scale") {
                value = std::fabs(value) <= 1.0e-5f ? 1.0e-5f : value;
                if (component == "x") {
                  entity->transform.scale.x = value;
                } else if (component == "y") {
                  entity->transform.scale.y = value;
                } else if (component == "z") {
                  entity->transform.scale.z = value;
                } else {
                  return failEdit("unknown scale component");
                }
              } else {
                return failEdit("unknown transform field");
              }
              changed = true;
              renderAffecting = true;
            } else if (parts[3] == "translation") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for position");
              }
              entity->transform.translation = value;
              changed = true;
              renderAffecting = true;
            } else if (parts[3] == "rotation") {
              vkpt::scene::Quat value{};
              if (!QtParseQuat(edit.value, value)) {
                return failEdit("expected four numbers for rotation quaternion");
              }
              entity->transform.rotation = value;
              changed = true;
              renderAffecting = true;
            } else if (parts[3] == "scale") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for scale");
              }
              value.x = std::fabs(value.x) <= 1.0e-5f ? 1.0e-5f : value.x;
              value.y = std::fabs(value.y) <= 1.0e-5f ? 1.0e-5f : value.y;
              value.z = std::fabs(value.z) <= 1.0e-5f ? 1.0e-5f : value.z;
              entity->transform.scale = value;
              changed = true;
              renderAffecting = true;
            } else if (!qtUseBg) {
              return failEdit("unknown transform field");
            }
            entity->transform.dirty = true;
          } else if (parts.size() == 4u && parts[2] == "mesh" && parts[3] == "material_id") {
            if (!entity->has_mesh) {
              return failEdit("entity has no mesh renderer");
            }
            vkpt::core::StableId materialId = 0u;
            if (!QtParseStableId(edit.value, materialId)) {
              return failEdit("invalid material id");
            }
            if (materialId != 0u && FindQtSceneMaterial(qtSceneDocument, materialId) == nullptr) {
              return failEdit("material id does not exist");
            }
            entity->mesh.material_id = materialId;
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "mesh" && parts[3] == "mesh_id") {
            if (!entity->has_mesh) {
              return failEdit("entity has no mesh renderer");
            }
            vkpt::core::StableId geometryId = 0u;
            if (!QtParseStableId(edit.value, geometryId)) {
              return failEdit("invalid mesh id");
            }
            if (geometryId != 0u && FindQtSceneGeometry(qtSceneDocument, geometryId) == nullptr) {
              return failEdit("mesh id does not exist");
            }
            entity->mesh.mesh_id = geometryId;
            changed = true;
            renderAffecting = true;
          } else if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "sdf_primitive") {
            if (!entity->has_sdf_primitive) {
              return failEdit("entity has no SDF primitive");
            }
            const auto& field = parts[3];
            if (parts.size() == 5u) {
              return failEdit("unknown SDF property component");
            }
            if (field == "shape") {
              entity->sdf_primitive.shape = QtTrim(edit.value);
              if (entity->sdf_primitive.shape.empty()) {
                entity->sdf_primitive.shape = "sphere";
              }
            } else if (field == "radius" || field == "param_a" || field == "param_b") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric SDF value");
              }
              if (field == "radius") {
                entity->sdf_primitive.radius = std::max(0.01f, value);
              } else if (field == "param_a") {
                entity->sdf_primitive.param_a = value;
              } else {
                entity->sdf_primitive.param_b = value;
              }
            } else {
              return failEdit("unknown SDF property");
            }
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "material" && parts[3] == "material_id") {
            vkpt::core::StableId materialId = 0u;
            if (!QtParseStableId(edit.value, materialId)) {
              return failEdit("invalid material id");
            }
            if (materialId != 0u && FindQtSceneMaterial(qtSceneDocument, materialId) == nullptr) {
              return failEdit("material id does not exist");
            }
            entity->material.material_id = materialId;
            changed = true;
            renderAffecting = true;
          } else if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "light") {
            if (!entity->has_light) {
              return failEdit("entity has no light");
            }
            const auto& field = parts[3];
            if (parts.size() == 5u && (field == "color" || field == "direction")) {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric light vector component");
              }
              value = field == "color" ? ClampFloat(value, 0.0f, 10.0f)
                                       : ClampFloat(value, -1.0f, 1.0f);
              auto& target = field == "color" ? entity->light.color : entity->light.direction;
              if (parts[4] == "x") {
                target.x = value;
              } else if (parts[4] == "y") {
                target.y = value;
              } else if (parts[4] == "z") {
                target.z = value;
              } else {
                return failEdit("unknown light vector component");
              }
            } else if (parts.size() != 4u) {
              return failEdit("unknown light property component");
            } else if (field == "type") {
              entity->light.type = QtTrim(edit.value);
              if (entity->light.type.empty()) {
                entity->light.type = "point";
              }
            } else if (field == "color") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for light color");
              }
              entity->light.color = {
                  ClampFloat(value.x, 0.0f, 10.0f),
                  ClampFloat(value.y, 0.0f, 10.0f),
                  ClampFloat(value.z, 0.0f, 10.0f)};
            } else if (field == "direction") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for light direction");
              }
              entity->light.direction = {
                  ClampFloat(value.x, -1.0f, 1.0f),
                  ClampFloat(value.y, -1.0f, 1.0f),
                  ClampFloat(value.z, -1.0f, 1.0f)};
            } else if (field == "intensity" || field == "radius" ||
                       field == "beam_angle" || field == "blend") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric light value");
              }
              if (field == "intensity") {
                entity->light.intensity = std::max(0.0f, value);
              } else if (field == "radius") {
                entity->light.radius = std::max(0.0f, value);
              } else if (field == "beam_angle") {
                entity->light.beam_angle_degrees = ClampFloat(value, 1.0f, 120.0f);
              } else {
                entity->light.blend = ClampFloat(value, 0.0f, 1.0f);
              }
            } else {
              return failEdit("unknown light property");
            }
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "camera") {
            if (!entity->has_camera) {
              return failEdit("entity has no camera");
            }
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric camera value");
            }
            const auto& field = parts[3];
            if (field == "fov") {
              entity->camera.fov = ClampFloat(value, 1.0f, 179.0f);
            } else if (field == "near_plane") {
              entity->camera.near_plane = std::max(0.001f, value);
              if (entity->camera.far_plane <= entity->camera.near_plane) {
                entity->camera.far_plane = entity->camera.near_plane + 1.0f;
              }
            } else if (field == "far_plane") {
              entity->camera.far_plane = std::max(value, entity->camera.near_plane + 0.001f);
            } else if (field == "focal_length_mm") {
              entity->camera.focal_length_mm = ClampFloat(value, 1.0f, 1000.0f);
            } else if (field == "sensor_width_mm") {
              entity->camera.sensor_width_mm = ClampFloat(value, 1.0f, 200.0f);
            } else if (field == "sensor_height_mm") {
              entity->camera.sensor_height_mm = ClampFloat(value, 1.0f, 200.0f);
            } else if (field == "aperture_radius") {
              entity->camera.aperture_radius = ClampFloat(value, 0.0f, 100.0f);
            } else if (field == "focus_distance") {
              entity->camera.focus_distance = ClampFloat(value, 0.0f, 100000.0f);
            } else if (field == "f_stop") {
              entity->camera.f_stop = ClampFloat(value, 0.0f, 256.0f);
            } else if (field == "shutter_seconds") {
              entity->camera.shutter_seconds = ClampFloat(value, 0.000001f, 60.0f);
            } else if (field == "iso") {
              entity->camera.iso = ClampFloat(value, 1.0f, 1048576.0f);
            } else if (field == "exposure_compensation") {
              entity->camera.exposure_compensation = ClampFloat(value, -32.0f, 32.0f);
            } else if (field == "white_balance_kelvin") {
              entity->camera.white_balance_kelvin = ClampFloat(value, 1000.0f, 40000.0f);
            } else if (field == "iris_blade_count") {
              entity->camera.iris_blade_count =
                  static_cast<std::uint32_t>(ClampFloat(std::round(value), 0.0f, 64.0f));
            } else if (field == "iris_rotation_degrees") {
              entity->camera.iris_rotation_degrees = ClampFloat(value, -360.0f, 360.0f);
            } else if (field == "iris_roundness") {
              entity->camera.iris_roundness = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "anamorphic_squeeze") {
              entity->camera.anamorphic_squeeze = ClampFloat(value, 0.01f, 100.0f);
            } else {
              return failEdit("unknown camera property");
            }
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "physics") {
            const auto& field = parts[3];
            if (field == "enabled") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              if (value || entity->has_physics_body) {
                auto& body = ensurePhysicsBody();
                body.enabled = value;
                changed = true;
              }
            } else if (field == "body_type") {
              auto& body = ensurePhysicsBody();
              std::string value = QtTrim(edit.value);
              if (value.empty()) {
                value = "static";
              }
              body.dynamic = value == "dynamic";
              body.body_type = value;
              changed = true;
            } else if (field == "shape") {
              auto& body = ensurePhysicsBody();
              body.shape = QtTrim(edit.value);
              if (body.shape.empty()) {
                body.shape = "box";
              }
              changed = true;
            } else if (field == "mass" ||
                       field == "friction" ||
                       field == "restitution" ||
                       field == "gravity_scale") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric physics value");
              }
              auto& body = ensurePhysicsBody();
              if (field == "mass") {
                body.mass = std::max(0.01f, value);
              } else if (field == "friction") {
                body.friction = std::max(0.0f, value);
              } else if (field == "restitution") {
                body.restitution = std::max(0.0f, value);
              } else {
                body.gravity_scale = value;
              }
              changed = true;
            } else if (field == "trigger" ||
                       field == "allow_sleeping" ||
                       field == "continuous_collision") {
              bool value = false;
              if (!QtParseBool(edit.value, value)) {
                return failEdit("expected true or false");
              }
              auto& body = ensurePhysicsBody();
              if (field == "trigger") {
                body.trigger = value;
              } else if (field == "allow_sleeping") {
                body.allow_sleeping = value;
              } else {
                body.continuous_collision = value;
              }
              changed = true;
            } else {
              return failEdit("unknown physics property");
            }
            renderAffecting = changed;
          } else {
            return failEdit("unknown entity property");
          }
        } else if (parts.size() >= 3u && parts[0] == "sdf") {
          vkpt::core::StableId sdfId = 0u;
          if (!QtParseStableId(parts[1], sdfId)) {
            return failEdit("invalid SDF id");
          }
          auto* primitive = qtFindSdfPrimitive(sdfId);
          if (primitive == nullptr) {
            return failEdit("SDF primitive not found");
          }

          if ((parts.size() == 4u || parts.size() == 5u) && parts[2] == "transform") {
            if (parts.size() == 5u) {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric SDF transform value");
              }
              const auto& field = parts[3];
              const auto& component = parts[4];
              if (field == "translation") {
                if (component == "x") {
                  primitive->transform.translation.x = value;
                } else if (component == "y") {
                  primitive->transform.translation.y = value;
                } else if (component == "z") {
                  primitive->transform.translation.z = value;
                } else {
                  return failEdit("unknown SDF translation component");
                }
              } else if (field == "rotation") {
                if (component == "x") {
                  primitive->transform.rotation.x = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "y") {
                  primitive->transform.rotation.y = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "z") {
                  primitive->transform.rotation.z = ClampFloat(value, -1.0f, 1.0f);
                } else if (component == "w") {
                  primitive->transform.rotation.w = ClampFloat(value, -1.0f, 1.0f);
                } else {
                  return failEdit("unknown SDF rotation component");
                }
                const float lenSq =
                    primitive->transform.rotation.x * primitive->transform.rotation.x +
                    primitive->transform.rotation.y * primitive->transform.rotation.y +
                    primitive->transform.rotation.z * primitive->transform.rotation.z +
                    primitive->transform.rotation.w * primitive->transform.rotation.w;
                if (lenSq <= 1.0e-8f) {
                  primitive->transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
                } else {
                  const float invLen = 1.0f / std::sqrt(lenSq);
                  primitive->transform.rotation.x *= invLen;
                  primitive->transform.rotation.y *= invLen;
                  primitive->transform.rotation.z *= invLen;
                  primitive->transform.rotation.w *= invLen;
                }
              } else if (field == "scale") {
                value = std::fabs(value) <= 1.0e-5f ? 1.0e-5f : value;
                if (component == "x") {
                  primitive->transform.scale.x = value;
                } else if (component == "y") {
                  primitive->transform.scale.y = value;
                } else if (component == "z") {
                  primitive->transform.scale.z = value;
                } else {
                  return failEdit("unknown SDF scale component");
                }
              } else {
                return failEdit("unknown SDF transform field");
              }
            } else if (parts[3] == "translation") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for SDF position");
              }
              primitive->transform.translation = value;
            } else if (parts[3] == "rotation") {
              vkpt::scene::Quat value{};
              if (!QtParseQuat(edit.value, value)) {
                return failEdit("expected four numbers for SDF rotation quaternion");
              }
              primitive->transform.rotation = value;
            } else if (parts[3] == "scale") {
              vkpt::scene::Vec3 value{};
              if (!QtParseVec3(edit.value, value)) {
                return failEdit("expected three numbers for SDF scale");
              }
              value.x = std::fabs(value.x) <= 1.0e-5f ? 1.0e-5f : value.x;
              value.y = std::fabs(value.y) <= 1.0e-5f ? 1.0e-5f : value.y;
              value.z = std::fabs(value.z) <= 1.0e-5f ? 1.0e-5f : value.z;
              primitive->transform.scale = value;
            } else {
              return failEdit("unknown SDF transform field");
            }
            primitive->transform.dirty = true;
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 3u && parts[2] == "shape") {
            primitive->shape = QtTrim(edit.value);
            if (primitive->shape.empty()) {
              primitive->shape = "sphere";
            }
            primitive->primitive.shape = primitive->shape;
            changed = true;
            renderAffecting = true;
          } else if (parts.size() == 4u && parts[2] == "primitive") {
            const auto& field = parts[3];
            if (field == "shape") {
              primitive->primitive.shape = QtTrim(edit.value);
              if (primitive->primitive.shape.empty()) {
                primitive->primitive.shape = "sphere";
              }
              primitive->shape = primitive->primitive.shape;
            } else if (field == "radius" || field == "param_a" || field == "param_b") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric SDF value");
              }
              if (field == "radius") {
                primitive->primitive.radius = std::max(0.01f, value);
              } else if (field == "param_a") {
                primitive->primitive.param_a = value;
              } else {
                primitive->primitive.param_b = value;
              }
            } else {
              return failEdit("unknown SDF primitive property");
            }
            changed = true;
            renderAffecting = true;
          } else {
            return failEdit("unknown SDF property");
          }
        } else if ((parts.size() == 3u || parts.size() == 4u) && parts[0] == "material") {
          vkpt::core::StableId materialId = 0u;
          if (!QtParseStableId(parts[1], materialId)) {
            return failEdit("invalid material id");
          }
          auto* material = FindQtMutableSceneMaterial(qtSceneDocument, materialId);
          if (material == nullptr) {
            return failEdit("material not found");
          }

          const auto& field = parts[2];
          if (parts.size() == 4u && (field == "albedo" || field == "emission")) {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric material component");
            }
            auto& color = field == "albedo" ? material->albedo : material->emission;
            value = field == "albedo" ? ClampFloat(value, 0.0f, 1.0f) : std::max(0.0f, value);
            if (parts[3] == "x") {
              color.x = value;
            } else if (parts[3] == "y") {
              color.y = value;
            } else if (parts[3] == "z") {
              color.z = value;
            } else {
              return failEdit("unknown material color component");
            }
          } else if (parts.size() != 3u) {
            return failEdit("unknown material property component");
          } else if (field == "name") {
            material->name = QtTrim(edit.value);
          } else if (field == "family") {
            material->family = QtTrim(edit.value);
            if (material->family.empty()) {
              material->family = "diffuse";
            }
            vkpt::scene::ApplyMaterialFamilyPreset(
                *material,
                vkpt::scene::SceneMaterialPresetPolicy::Override);
          } else if (field == "albedo") {
            vkpt::scene::Vec3 value{};
            if (!QtParseVec3(edit.value, value)) {
              return failEdit("expected three numbers for base color");
            }
            material->albedo = value;
          } else if (field == "emission") {
            vkpt::scene::Vec3 value{};
            if (!QtParseVec3(edit.value, value)) {
              return failEdit("expected three numbers for emission");
            }
            material->emission = value;
          } else if (field == "roughness" ||
                     field == "metallic" ||
                     field == "ior" ||
                     field == "transmission" ||
                     field == "clearcoat" ||
                     field == "sheen" ||
                     field == "anisotropy" ||
                     field == "alpha" ||
                     field == "emission_intensity") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric material value");
            }
            if (field == "roughness") {
              material->roughness = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "metallic") {
              material->metallic = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "ior") {
              material->ior = std::max(1.01f, value);
            } else if (field == "transmission") {
              material->transmission = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "clearcoat") {
              material->clearcoat = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "sheen") {
              material->sheen = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "anisotropy") {
              material->anisotropy = ClampFloat(value, -1.0f, 1.0f);
            } else if (field == "alpha") {
              material->alpha = ClampFloat(value, 0.0f, 1.0f);
            } else if (field == "emission_intensity") {
              material->emission_intensity = std::max(0.0f, value);
            }
          } else if (field == "double_sided") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            material->double_sided = value;
          } else {
            return failEdit("unknown material property");
          }
          changed = true;
          renderAffecting = true;
        } else {
          return failEdit("unknown property id");
        }

        if (!changed) {
          qtDockPanelsDirty = true;
          return true;
        }

        bool reloadOk = true;
        if (settingsAffecting) {
          reloadOk = qtReloadRenderSettings("render settings edited");
        } else if (renderAffecting) {
          reloadOk = qtReloadEditedScene("property edit");
        }
        qtDockPanelsDirty = true;
        if (reloadOk) {
          ui_runtime_state.status_message = "property edited: " + edit.property_id;
          ui_runtime_state.last_warning_or_error.clear();
        }
        PushUiEvent(ui_event_log,
                    reloadOk ? "dock_property_edit" : "dock_property_edit_failed",
                    edit.panel_id,
                    "dock",
                    frameIndex,
                    {},
                    edit.property_id,
                    edit.value);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        updateQtSelectionOverlay();
        return reloadOk;
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
      const float qtFpsUnitScale = ClampFloat(qtCameraMoveUnitsPerSecond * 0.10f, 0.65f, 2.25f);
      const float qtFpsStandEyeHeight = 1.62f * qtFpsUnitScale;
      const float qtFpsCrouchEyeHeight = 1.05f * qtFpsUnitScale;
      const float qtFpsRadius = 0.32f * qtFpsUnitScale;
      const float qtFpsStepHeight = 0.38f * qtFpsUnitScale;
      const float qtFpsSkin = 0.03f * qtFpsUnitScale;
      const float qtFpsWalkSpeed = ClampFloat(qtCameraMoveUnitsPerSecond * 0.45f,
                                              1.45f * qtFpsUnitScale,
                                              4.25f * qtFpsUnitScale);
      const float qtFpsRunSpeed = qtFpsWalkSpeed * 1.65f;
      const float qtFpsCrouchSpeed = qtFpsWalkSpeed * 0.45f;
      const float qtFpsAirControlScale = 0.45f;
      const float qtFpsGravity = 18.0f * qtFpsUnitScale;
      const float qtFpsJumpSpeed = 5.2f * std::sqrt(qtFpsUnitScale);
      auto qtFpsEyePosition = [&]() {
        return PtAdd(qtFpsPlayer.feet_position, {0.0f, qtFpsPlayer.eye_height, 0.0f});
      };
      auto qtFpsFlatForward = [&]() {
        return PtNormalize(vkpt::pathtracer::Vec3{
            std::sin(qtFpsYaw),
            0.0f,
            std::cos(qtFpsYaw)}, {0.0f, 0.0f, -1.0f});
      };
      auto qtFpsFlatRight = [&]() {
        return PtNormalize(PtCross(qtFpsFlatForward(), {0.0f, 1.0f, 0.0f}), {1.0f, 0.0f, 0.0f});
      };
      auto qtRefreshCameraFocusFromPose = [&]() {
        const auto forward = qtCameraForward();
        const float targetDistance = PtLength(PtSub(qtCameraPose.target, qtCameraPose.position));
        const float sceneScale = qtSceneScale();
        const float minimumSceneFocusDistance = std::max(1.0f, sceneScale * 0.25f);
        const float projectedSceneDistance = PtDot(PtSub(qtOrbitCenter, qtCameraPose.position), forward);

        float focusDistance = std::max(0.25f, targetDistance);
        if (targetDistance < minimumSceneFocusDistance &&
            projectedSceneDistance > minimumSceneFocusDistance) {
          focusDistance = projectedSceneDistance;
        }

        qtCameraFocusDistance = std::max(0.25f, focusDistance);
        qtCameraFocusPoint = PtAdd(qtCameraPose.position, PtMul(forward, qtCameraFocusDistance));
      };
      auto qtMoveCameraFocusBy = [&](const vkpt::pathtracer::Vec3& delta) {
        qtCameraFocusPoint = PtAdd(qtCameraFocusPoint, delta);
      };
      qtRefreshCameraFocusFromPose();
      auto qtSyncCameraFromFpsPlayer = [&](std::string_view reason) {
        const auto eye = qtFpsEyePosition();
        const auto forward = qtFpsForwardFromAngles();
        qtCameraPose.position = eye;
        qtCameraPose.target = PtAdd(eye, PtMul(forward, std::max(1.0f, qtCameraFocusDistance)));
        qtCameraPose.up = {0.0f, 1.0f, 0.0f};
        qtCameraFocusPoint = qtCameraPose.target;
        qtCameraFocusDistance = PtLength(PtSub(qtCameraFocusPoint, qtCameraPose.position));
        qtMaybeSyncFpsCameraObject(false);
        applyQtCameraPose(reason, false);
      };
      auto qtInitializeFpsPlayerFromPose = [&]() {
        qtFpsPlayer = {};
        qtFpsPlayer.initialized = true;
        qtFpsPlayer.eye_height = qtFpsStandEyeHeight;
        qtFpsPlayer.feet_position = {
            qtCameraPose.position.x,
            qtCameraPose.position.y - qtFpsStandEyeHeight,
            qtCameraPose.position.z};
        const auto groundProbeOrigin = PtAdd(qtCameraPose.position, {0.0f, qtFpsStepHeight, 0.0f});
        const auto ground = TraceFpsGround(qtPickables,
                                           groundProbeOrigin,
                                           qtFpsStandEyeHeight + qtFpsStepHeight + qtFpsSkin,
                                           0.62f);
        if (ground.hit) {
          qtFpsPlayer.feet_position.y = ground.position.y;
          qtFpsPlayer.grounded = true;
        }
        qtFpsPlayer.current_speed = 0.0f;
      };
      auto qtResolveFpsHorizontalDelta = [&](const vkpt::pathtracer::Vec3& feetPosition,
                                             const vkpt::pathtracer::Vec3& desiredDelta) {
        return ResolveFpsHorizontalDeltaForPlayer(qtPickables,
                                                  feetPosition,
                                                  desiredDelta,
                                                  qtFpsRadius,
                                                  qtFpsSkin,
                                                  qtFpsPlayer.eye_height);
      };
      auto qtApplyFpsLookDelta = [&](float dx, float dy) {
        constexpr float kLookSensitivity = 0.0045f;
        qtFpsYaw -= dx * kLookSensitivity;
        qtFpsPitch = ClampFloat(qtFpsPitch - dy * kLookSensitivity, -1.45f, 1.45f);
        const auto forward = qtFpsForwardFromAngles();
        qtCameraPose.target = PtAdd(qtCameraPose.position,
                                    PtMul(forward, std::max(1.0f, qtCameraFocusDistance)));
        qtCameraFocusPoint = qtCameraPose.target;
        qtCameraFocusDistance = PtLength(PtSub(qtCameraFocusPoint, qtCameraPose.position));
        qtCameraPose.up = {0.0f, 1.0f, 0.0f};
        qtMaybeSyncFpsCameraObject(false);
        applyQtCameraPose("fps look", false);
      };
      auto qtApplyOrbitDrag = [&](float dx, float dy) {
        constexpr float kOrbitSensitivity = 0.006f;
        const auto offset = PtSub(qtCameraPose.position, qtCameraFocusPoint);
        const float radius = std::max(0.05f, PtLength(offset));
        float yaw = std::atan2(offset.x, offset.z) - dx * kOrbitSensitivity;
        float pitch = std::asin(ClampFloat(offset.y / radius, -0.98f, 0.98f)) + dy * kOrbitSensitivity;
        pitch = ClampFloat(pitch, -1.45f, 1.45f);
        const float cosPitch = std::cos(pitch);
        qtCameraPose.position = PtAdd(qtCameraFocusPoint,
                                      PtMul(vkpt::pathtracer::Vec3{
                                          std::sin(yaw) * cosPitch,
                                          std::sin(pitch),
                                          std::cos(yaw) * cosPitch}, radius));
        qtCameraPose.target = qtCameraFocusPoint;
        qtCameraFocusDistance = radius;
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("orbit drag");
      };
      auto qtApplyPanDrag = [&](float dx, float dy) {
        const auto frameRect = qtViewportImageRect();
        const float viewportHeight = std::max(1.0f, frameRect.height);
        const auto forward = qtCameraForward();
        const auto right = PtNormalize(PtCross(forward, qtCameraPose.up), {1.0f, 0.0f, 0.0f});
        const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
        const float focusDistance = std::max(0.05f, qtCameraFocusDistance);
        const float worldPerPixel =
            (2.0f * focusDistance * std::tan(0.5f * DegToRad(std::max(1.0f, qtCameraPose.fov_deg)))) /
            viewportHeight;
        const auto delta = PtAdd(PtMul(right, -dx * worldPerPixel),
                                 PtMul(up, dy * worldPerPixel));
        qtCameraPose.position = PtAdd(qtCameraPose.position, delta);
        qtCameraPose.target = PtAdd(qtCameraPose.target, delta);
        qtMoveCameraFocusBy(delta);
        applyQtCameraPose("pan drag");
      };
      auto qtApplyDolly = [&](float wheelDelta) {
        if (std::fabs(wheelDelta) <= 1.0e-4f) {
          return;
        }
        if (qtFpsMode) {
          if (!qtFpsPlayer.initialized) {
            qtInitializeFpsPlayerFromPose();
          }
          const auto delta = qtResolveFpsHorizontalDelta(
              qtFpsPlayer.feet_position,
              PtMul(qtFpsFlatForward(), wheelDelta * qtFpsWalkSpeed * 0.35f));
          qtFpsPlayer.feet_position = PtAdd(qtFpsPlayer.feet_position, delta);
          qtFpsPlayer.current_speed = 0.0f;
          qtDiscardFpsMovementResults();
          qtSyncCameraFromFpsPlayer("fps dolly");
          qtMarkFpsDockPanelsDirty(true);
          return;
        } else {
          const auto offset = PtSub(qtCameraPose.position, qtCameraFocusPoint);
          const float scale = std::pow(0.88f, wheelDelta);
          const float currentDistance = std::max(0.05f, PtLength(offset));
          const float newDistance = ClampFloat(currentDistance * ClampFloat(scale, 0.25f, 4.0f),
                                               0.05f,
                                               std::max(currentDistance * 32.0f, qtSceneScale() * 64.0f));
          qtCameraPose.position = PtAdd(qtCameraFocusPoint,
                                        PtMul(PtNormalize(offset), newDistance));
          qtCameraPose.target = qtCameraFocusPoint;
          qtCameraFocusDistance = newDistance;
          syncQtFpsAnglesFromPose();
        }
        applyQtCameraPose(qtFpsMode ? "fps dolly" : "orbit dolly");
      };
      auto qtApplyContinuousFpsMovement = [&](float dtSeconds) {
        if (!qtFpsMode || dtSeconds <= 0.0f) {
          return;
        }
        if (!qtFpsPlayer.initialized) {
          qtInitializeFpsPlayerFromPose();
        }

        vkpt::pathtracer::Vec3 wishMove{};
        if (qtKeyActive('W') || qtKeyActive(kQtKeyUp)) {
          wishMove = PtAdd(wishMove, qtFpsFlatForward());
        }
        if (qtKeyActive('S') || qtKeyActive(kQtKeyDown)) {
          wishMove = PtSub(wishMove, qtFpsFlatForward());
        }
        if (qtKeyActive('D') || qtKeyActive(kQtKeyRight)) {
          wishMove = PtAdd(wishMove, qtFpsFlatRight());
        }
        if (qtKeyActive('A') || qtKeyActive(kQtKeyLeft)) {
          wishMove = PtSub(wishMove, qtFpsFlatRight());
        }

        if (auto result = qtFpsCollisionWorker.take_latest_result()) {
          const bool freshCollision = result->collision_revision ==
              qtFpsCollisionWorker.collision_revision();
          const bool freshSequence = result->sequence > qtFpsAppliedMoveSequence;
          if (freshCollision && freshSequence) {
            const bool pendingJump = qtFpsPlayer.jump_queued;
            qtFpsAppliedMoveSequence = result->sequence;
            qtFpsPlayer = result->player;
            qtFpsPlayer.jump_queued = qtFpsPlayer.jump_queued || pendingJump;
            if (result->pose_changed || result->state_changed) {
              qtUserCameraActive = true;
              qtSyncCameraFromFpsPlayer("fps physics");
              qtMarkFpsDockPanelsDirty(result->state_changed);
            }
          }
        }

        const bool wantsCrouch =
            qtKeyActive(kQtKeyControl) || qtKeyActive(17) || qtKeyActive('C');
        const bool wantsRun =
            !wantsCrouch && (qtKeyActive(kQtKeyShift) || qtKeyActive(16));
        const float targetEyeHeight = wantsCrouch ? qtFpsCrouchEyeHeight : qtFpsStandEyeHeight;
        const bool hasWishMove = PtLength(wishMove) > 1.0e-5f;
        const bool needsMovementStep =
            hasWishMove ||
            qtFpsPlayer.jump_queued ||
            !qtFpsPlayer.grounded ||
            qtFpsPlayer.crouching != wantsCrouch ||
            qtFpsPlayer.running != wantsRun ||
            std::fabs(qtFpsPlayer.eye_height - targetEyeHeight) > 1.0e-4f;
        if (!needsMovementStep) {
          qtPendingFpsMoveDt = 0.0f;
          if (qtFpsPlayer.current_speed != 0.0f) {
            qtFpsPlayer.current_speed = 0.0f;
            qtMarkFpsDockPanelsDirty(false);
          }
          return;
        }

        qtPendingFpsMoveDt = ClampFloat(qtPendingFpsMoveDt + dtSeconds, 0.0f, 0.12f);
        if (qtFpsCollisionWorker.has_work() || qtPendingFpsMoveDt <= 1.0e-5f) {
          return;
        }

        FpsMovementRequest request{};
        request.sequence = ++qtFpsMoveSequence;
        request.player = qtFpsPlayer;
        request.wish_move = wishMove;
        request.tuning.stand_eye_height = qtFpsStandEyeHeight;
        request.tuning.crouch_eye_height = qtFpsCrouchEyeHeight;
        request.tuning.radius = qtFpsRadius;
        request.tuning.step_height = qtFpsStepHeight;
        request.tuning.skin = qtFpsSkin;
        request.tuning.walk_speed = qtFpsWalkSpeed;
        request.tuning.run_speed = qtFpsRunSpeed;
        request.tuning.crouch_speed = qtFpsCrouchSpeed;
        request.tuning.air_control_scale = qtFpsAirControlScale;
        request.tuning.gravity = qtFpsGravity;
        request.tuning.jump_speed = qtFpsJumpSpeed;
        request.dt_seconds = ClampFloat(qtPendingFpsMoveDt, 0.0f, 0.08f);
        request.crouching = wantsCrouch;
        request.running = wantsRun;
        qtPendingFpsMoveDt = 0.0f;
        qtFpsCollisionWorker.submit(request);
        qtFpsPlayer.jump_queued = false;
      };
      qtSetFpsMode = [&](bool enabled,
                         vkpt::core::FrameIndex frameIndex,
                         std::string_view source) {
        if (qtFpsMode == enabled && (!enabled || qtFpsPlayer.initialized)) {
          return;
        }
        qtFpsMode = enabled;
        qtDiscardFpsMovementResults();
        qtUserCameraActive = true;
        ui_runtime_state.active_viewport_tool = qtFpsMode
            ? vkpt::editor::ViewportTool::Fps
            : vkpt::editor::ViewportTool::Select;
        if (qtFpsMode) {
          syncQtFpsAnglesFromPose();
          qtInitializeFpsPlayerFromPose();
          qtLeftMouseDown = false;
          qtRightMouseDown = false;
          qtMiddleMouseDown = false;
          qtPotentialClick = false;
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
          qtGizmoDrag = {};
          qtHoveredGizmoHit = std::nullopt;
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
          qtSetViewportMouseLocked(true);
          qtSyncCameraFromFpsPlayer("fps enter");
        } else {
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
          qtMaybeSyncFpsCameraObject(true);
          qtFpsPlayer.jump_queued = false;
          syncQtFpsAnglesFromPose();
          qtSetViewportMouseLocked(false);
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        }
        ui_runtime_state.status_message = qtFpsMode ? "fps camera mode" : "select camera mode";
        PushUiEvent(ui_event_log,
                    "viewport_camera_mode",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    qtFpsMode ? "fps" : "select",
                    ui_runtime_state.status_message);
        qtDockPanelsDirty = true;
        updateQtSelectionOverlay();
      };
      auto qtCaptureGizmoDrag = [&](const ViewportGizmoHit& hit, float x, float y) {
        qtGizmoDrag = {};
        qtGizmoDrag.active = true;
        qtGizmoDrag.hit = hit;
        qtGizmoDrag.start_x = x;
        qtGizmoDrag.start_y = y;
        qtSetViewportCursor(CursorForGizmoHit(hit));
        for (const auto selectedId : ui_selection_state.selected_entity_ids) {
          if (auto* entity = qtFindEntity(selectedId)) {
            if (!entity->has_transform) {
              entity->has_transform = true;
              entity->transform = {};
            }
            qtGizmoDrag.entities.push_back({
                selectedId,
                false,
                entity->transform,
                InverseSceneTransformPoint(hit.pivot, entity->transform)});
            continue;
          }
          if (auto* primitive = qtFindSdfPrimitive(selectedId)) {
            qtGizmoDrag.entities.push_back({
                selectedId,
                true,
                primitive->transform,
                InverseSceneTransformPoint(hit.pivot, primitive->transform)});
          }
        }
        if (qtGizmoDrag.entities.empty()) {
          qtGizmoDrag = {};
        }
      };
      auto qtWriteGizmoTransform = [&](const QtGizmoDragEntityStart& start,
                                       const vkpt::scene::TransformComponent& transform) {
        if (start.sdf_primitive) {
          if (auto* primitive = qtFindSdfPrimitive(start.entity_id)) {
            primitive->transform = transform;
            primitive->transform.dirty = true;
          }
          return;
        }
        if (auto* entity = qtFindEntity(start.entity_id)) {
          entity->has_transform = true;
          entity->transform = transform;
          entity->transform.dirty = true;
        }
      };
      auto qtApplyGizmoDrag = [&](float x, float y, vkpt::core::FrameIndex frameIndex) {
        if (!qtGizmoDrag.active) {
          return;
        }
        const float dx = x - qtGizmoDrag.start_x;
        const float dy = y - qtGizmoDrag.start_y;
        const float dragAlongAxis = dx * qtGizmoDrag.hit.screen_axis_x +
                                    dy * qtGizmoDrag.hit.screen_axis_y;
        vkpt::pathtracer::Vec3 freeformDelta{};
        bool hasFreeformDelta = false;
        if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::FreeformTranslate) {
          const auto frameRect = qtViewportImageRect();
          const float viewportWidth = frameRect.width;
          const float viewportHeight = frameRect.height;
          const auto startWorld = ScreenPointOnCameraPlane(
              qtCameraPose,
              qtGizmoDrag.start_x - frameRect.x,
              qtGizmoDrag.start_y - frameRect.y,
              viewportWidth,
              viewportHeight,
              qtRenderAspect(),
              qtGizmoDrag.hit.pivot);
          const auto currentWorld = ScreenPointOnCameraPlane(
              qtCameraPose,
              x - frameRect.x,
              y - frameRect.y,
              viewportWidth,
              viewportHeight,
              qtRenderAspect(),
              qtGizmoDrag.hit.pivot);
          if (startWorld && currentWorld) {
            freeformDelta = PtSub(*currentWorld, *startWorld);
            hasFreeformDelta = true;
          }
        }
        for (const auto& start : qtGizmoDrag.entities) {
          auto transform = start.transform;
          if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::Translate) {
            const float units = dragAlongAxis / std::max(1.0f, qtGizmoDrag.hit.pixels_per_unit);
            const auto delta = PtMul(qtGizmoDrag.hit.axis, units);
            transform.translation.x = start.transform.translation.x + delta.x;
            transform.translation.y = start.transform.translation.y + delta.y;
            transform.translation.z = start.transform.translation.z + delta.z;
          } else if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::FreeformTranslate) {
            if (!hasFreeformDelta) {
              continue;
            }
            transform.translation.x = start.transform.translation.x + freeformDelta.x;
            transform.translation.y = start.transform.translation.y + freeformDelta.y;
            transform.translation.z = start.transform.translation.z + freeformDelta.z;
          } else if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::ScaleAxis) {
            const float units = dragAlongAxis / std::max(1.0f, qtGizmoDrag.hit.pixels_per_unit);
            const float factor = ClampFloat(
                (std::max(1.0e-4f, qtGizmoDrag.hit.axis_world_length) + units) /
                    std::max(1.0e-4f, qtGizmoDrag.hit.axis_world_length),
                0.05f,
                20.0f);
            switch (qtGizmoDrag.hit.axis_index) {
              case 0:
                transform.scale.x = start.transform.scale.x * factor;
                break;
              case 1:
                transform.scale.y = start.transform.scale.y * factor;
                break;
              case 2:
                transform.scale.z = start.transform.scale.z * factor;
                break;
              default:
                transform.scale.x = start.transform.scale.x * factor;
                transform.scale.y = start.transform.scale.y * factor;
                transform.scale.z = start.transform.scale.z * factor;
                break;
            }
            const auto scaledPivot = vkpt::pathtracer::Vec3{
                start.local_pivot.x * transform.scale.x,
                start.local_pivot.y * transform.scale.y,
                start.local_pivot.z * transform.scale.z};
            const auto rotatedPivot = RotatePointByQuat(scaledPivot, transform.rotation);
            const auto newTranslation = PtSub(qtGizmoDrag.hit.pivot, rotatedPivot);
            transform.translation = {newTranslation.x, newTranslation.y, newTranslation.z};
          } else if (qtGizmoDrag.hit.kind == ViewportGizmoDragKind::Rotate) {
            const float angle = dragAlongAxis * 0.012f;
            transform.rotation = QuatMultiply(QuatFromAxisAngle(qtGizmoDrag.hit.axis, angle),
                                              start.transform.rotation);
            const auto scaledPivot = vkpt::pathtracer::Vec3{
                start.local_pivot.x * transform.scale.x,
                start.local_pivot.y * transform.scale.y,
                start.local_pivot.z * transform.scale.z};
            const auto rotatedPivot = RotatePointByQuat(scaledPivot, transform.rotation);
            const auto newTranslation = PtSub(qtGizmoDrag.hit.pivot, rotatedPivot);
            transform.translation = {newTranslation.x, newTranslation.y, newTranslation.z};
          }
          qtWriteGizmoTransform(start, transform);
        }
        PushUiEvent(ui_event_log,
                    "viewport_gizmo_drag",
                    "viewport",
                    "mouse",
                    frameIndex,
                    {},
                    vkpt::editor::ToString(ui_runtime_state.active_gizmo_mode),
                    "gizmo manipulating selection");
        qtReloadEditedScene("drag");
      };
      auto qtSetGizmoMode = [&](vkpt::editor::GizmoMode mode,
                                std::string_view source,
                                vkpt::core::FrameIndex frameIndex) {
        if (ui_runtime_state.active_gizmo_mode == mode) {
          updateQtSelectionOverlay();
          return;
        }
        ui_runtime_state.active_gizmo_mode = mode;
        qtHoveredGizmoHit = std::nullopt;
        qtDockPanelsDirty = true;
        ui_runtime_state.status_message =
            std::string("gizmo ") + vkpt::editor::ToString(mode);
        PushUiEvent(ui_event_log,
                    "viewport_gizmo_mode",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    vkpt::editor::ToString(mode),
                    ui_runtime_state.status_message);
        updateQtSelectionOverlay();
        if (mode == vkpt::editor::GizmoMode::None) {
          qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
        }
      };
      auto qtApplyViewportPick = [&](float x, float y, vkpt::core::FrameIndex frameIndex) {
        if (qtFpsMode) {
          qtPotentialClick = false;
          qtGizmoDrag = {};
          qtHoveredGizmoHit = std::nullopt;
          updateQtSelectionOverlay();
          return;
        }
        const auto frameRect = qtViewportImageRect();
        const auto localPoint = qtViewportLocalPoint(x, y);
        const auto picked = localPoint
            ? PickViewportObject(qtPickables,
                                 qtCameraPose,
                                 localPoint->first,
                                 localPoint->second,
                                 frameRect.width,
                                 frameRect.height,
                                 qtRenderAspect())
            : std::optional<ViewportPickResult>{};
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
        qtHoveredGizmoHit = std::nullopt;
        qtDockPanelsDirty = true;
        ui_command_history.push(command);
        ui_runtime_state.last_clicked_entity = picked ? picked->entity_id : 0u;
        if (picked && ui_runtime_state.active_gizmo_mode == vkpt::editor::GizmoMode::None) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::Universal;
        } else if (!picked && !qtAppendSelection()) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
        }
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
        rebuildQtMenuBar();
        updateQtSelectionOverlay();
        qtUpdateGizmoHoverCursor(x, y);
        logger.log(vkpt::log::Severity::Info, "app", "Qt viewport pick", {
          {"entity_id", std::to_string(ui_runtime_state.last_clicked_entity)},
          {"x", std::to_string(x)},
          {"y", std::to_string(y)},
          {"selection_count", std::to_string(ui_selection_state.selected_entity_ids.size())}
        });
      };
      auto qtApplySceneGraphActivation =
          [&](const vkpt::platform::QtDockRowActivation& activation,
              vkpt::core::FrameIndex frameIndex) {
        if (activation.entity_id == 0u ||
            (activation.panel_id != "scene_graph" && activation.panel_id != "scene_tree")) {
          return;
        }

        vkpt::editor::EditorCommand command;
        command.command_id = "scene_tree.select";
        command.kind = vkpt::editor::EditorCommandKind::kSelectEntity;
        command.source_widget = "scene_tree";
        command.frame_index = frameIndex;
        command.payload = vkpt::editor::SelectEntityCommand{
            activation.entity_id,
            activation.append,
            activation.range_mode};

        ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, command);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtHoveredGizmoHit = std::nullopt;
        qtDockPanelsDirty = true;
        ui_command_history.push(command);
        ui_runtime_state.last_clicked_entity = activation.entity_id;
        ui_runtime_state.focused_panel = "scene_tree";
        ui_runtime_state.last_scene_tree_operation =
            "selected " + std::to_string(activation.entity_id);
        if (ui_selection_state.selected_entity_ids.empty()) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
        } else if (ui_runtime_state.active_gizmo_mode == vkpt::editor::GizmoMode::None) {
          ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::Universal;
        }
        ui_runtime_state.status_message =
            "scene graph selected " + std::to_string(activation.entity_id);
        PushUiEvent(ui_event_log,
                    "scene_tree_select",
                    "scene_tree",
                    activation.row_id.empty() ? std::string("row") : activation.row_id,
                    frameIndex,
                    {},
                    std::to_string(activation.entity_id),
                    ui_runtime_state.status_message);
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
        rebuildQtMenuBar();
        updateQtSelectionOverlay();
        qtUpdateGizmoHoverCursor(qtLastMouseX, qtLastMouseY);
        logger.log(vkpt::log::Severity::Info, "app", "Qt scene graph selection", {
          {"entity_id", std::to_string(activation.entity_id)},
          {"row_id", activation.row_id},
          {"append", activation.append ? "true" : "false"},
          {"range_mode", activation.range_mode ? "true" : "false"},
          {"selection_count", std::to_string(ui_selection_state.selected_entity_ids.size())}
        });
      };
      auto qtFocusSelected = [&](vkpt::core::FrameIndex frameIndex, std::string_view source) {
        const auto bounds = qtActiveSelectionBounds();
        if (!bounds) {
          ui_runtime_state.status_message = "focus selected: no active selection";
          qtDockPanelsDirty = true;
          return false;
        }
        const auto center = PtMul(PtAdd(ToPtVec3(bounds->min), ToPtVec3(bounds->max)), 0.5f);
        const auto extent = PtSub(ToPtVec3(bounds->max), ToPtVec3(bounds->min));
        const float radius = std::max(0.05f, PtLength(extent) * 0.5f);
        const auto forward = qtCameraForward();
        const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, qtCameraPose.fov_deg)));
        const float distance = std::max(0.25f, (radius / std::max(0.05f, tanHalfFov)) * 1.35f);
        qtCameraFocusPoint = center;
        qtCameraFocusDistance = distance;
        qtCameraPose.target = center;
        qtCameraPose.position = PtSub(center, PtMul(forward, distance));
        syncQtFpsAnglesFromPose();
        applyQtCameraPose("focus selected");
        qtSetAuthoredCameraFocusDistance(distance, "focus selected");
        PushUiEvent(ui_event_log,
                    "viewport_focus_selected",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    std::to_string(ui_selection_state.active_primary_entity),
                    "focused selected entity");
        return true;
      };
      auto qtPickFocusAt = [&](float x,
                               float y,
                               vkpt::core::FrameIndex frameIndex,
                               std::string_view source,
                               bool reportMiss) {
        const auto frameRect = qtViewportImageRect();
        const auto localPoint = qtViewportLocalPoint(x, y);
        const auto picked = localPoint
            ? PickViewportObject(qtPickables,
                                 qtCameraPose,
                                 localPoint->first,
                                 localPoint->second,
                                 frameRect.width,
                                 frameRect.height,
                                 qtRenderAspect())
            : std::optional<ViewportPickResult>{};
        if (!picked || picked->distance <= 0.0f) {
          if (reportMiss) {
            ui_runtime_state.status_message = "pick focus missed";
            qtDockPanelsDirty = true;
          }
          return false;
        }
        const auto ray = BuildViewportRay(qtCameraPose,
                                          localPoint->first,
                                          localPoint->second,
                                          frameRect.width,
                                          frameRect.height,
                                          qtRenderAspect());
        qtCameraFocusDistance = std::max(0.25f, picked->distance);
        qtCameraFocusPoint = PtAdd(ray.origin, PtMul(ray.direction, qtCameraFocusDistance));
        qtSetAuthoredCameraFocusDistance(qtCameraFocusDistance, "pick focus");
        PushUiEvent(ui_event_log,
                    "viewport_pick_focus",
                    "viewport",
                    std::string(source),
                    frameIndex,
                    {},
                    std::to_string(picked->entity_id),
                    "picked camera focus");
        return true;
      };
      qtDockFocusSelected = [&](vkpt::core::FrameIndex frameIndex) {
        return qtFocusSelected(frameIndex, "dock");
      };
      qtDockAutoFocus = [&](vkpt::core::FrameIndex frameIndex) {
        if (qtViewportLocalPoint(qtLastMouseX, qtLastMouseY) &&
            qtPickFocusAt(qtLastMouseX, qtLastMouseY, frameIndex, "dock", false)) {
          return true;
        }
        const auto frameRect = qtViewportImageRect();
        const float centerX = frameRect.x + frameRect.width * 0.5f;
        const float centerY = frameRect.y + frameRect.height * 0.5f;
        if (qtPickFocusAt(centerX, centerY, frameIndex, "dock", false)) {
          return true;
        }
        if (qtFocusSelected(frameIndex, "dock")) {
          return true;
        }
        ui_runtime_state.status_message = "auto focus missed";
        qtDockPanelsDirty = true;
        return false;
      };
      auto qtLogMenuCommand = [&](const std::string& actionId,
                                  const vkpt::editor::EditorCommand& menuCommand,
                                  const std::string& statusText,
                                  vkpt::core::FrameIndex frameIndex) {
        auto logged = menuCommand;
        logged.command_id = actionId;
        logged.frame_index = frameIndex;
        if (logged.source_widget.empty()) {
          logged.source_widget = "menu";
        }
        ui_command_history.push(logged);
        ui_runtime_state.status_message = statusText;
        ui_runtime_state.last_menu_action = actionId;
        ui_runtime_state.focused_panel = logged.source_widget;
        PushUiEvent(ui_event_log,
                    "app_action",
                    logged.source_widget,
                    "menu",
                    frameIndex,
                    {},
                    actionId,
                    statusText);
        qtDockPanelsDirty = true;
        UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                       ui_selection_state,
                                       ui_layout_state,
                                       ui_event_log,
                                       ui_command_history);
      };
      auto qtInputPrevTime = std::chrono::steady_clock::now();
      double qtLastUiFrameMs = 0.0;
      QtRayMetricAccumulator qtComputerRayMetrics;
      std::unordered_map<std::string, QtRayMetricAccumulator> qtRayMetricsByDevice;
      std::uint64_t qtActiveRenderLastRays = 0u;
      std::uint64_t qtActiveRenderAccumulatedRays = 0u;
      double qtActiveRenderSeconds = 0.0;
      double qtActiveRenderInstantRps = 0.0;
      double qtActiveRenderRollingRps = 0.0;
      double qtActiveRenderAverageRps = 0.0;
      auto qtRecordActiveRenderBatch = [&](std::uint64_t beforeRays,
                                           std::uint64_t afterRays,
                                           double batchMs) {
        if (afterRays < beforeRays || afterRays < qtActiveRenderLastRays) {
          qtActiveRenderLastRays = beforeRays;
          qtActiveRenderAccumulatedRays = 0u;
          qtActiveRenderSeconds = 0.0;
          qtActiveRenderInstantRps = 0.0;
          qtActiveRenderRollingRps = 0.0;
          qtActiveRenderAverageRps = 0.0;
        }
        if (afterRays <= beforeRays || batchMs <= 0.0) {
          qtActiveRenderLastRays = afterRays;
          return;
        }
        const std::uint64_t deltaRays = afterRays - beforeRays;
        const double dt = batchMs / 1000.0;
        qtActiveRenderAccumulatedRays += deltaRays;
        qtActiveRenderSeconds += dt;
        qtActiveRenderInstantRps = static_cast<double>(deltaRays) / dt;
        const double alpha = std::clamp(dt / 2.0, 0.05, 0.35);
        qtActiveRenderRollingRps = qtActiveRenderRollingRps <= 0.0
            ? qtActiveRenderInstantRps
            : (qtActiveRenderRollingRps * (1.0 - alpha) +
               qtActiveRenderInstantRps * alpha);
        qtActiveRenderAverageRps = qtActiveRenderSeconds > 0.0
            ? static_cast<double>(qtActiveRenderAccumulatedRays) / qtActiveRenderSeconds
            : 0.0;
        qtActiveRenderLastRays = afterRays;
      };
      auto qtLastDockPanelSync = std::chrono::steady_clock::time_point{};
      std::vector<QtDockPanelContent> qtLastDockPanels;
      std::string qtLastWindowTitle;
      auto qtBuildFrameStats = [&]() {
        QtDockFrameStats stats;
        stats.sample_count = qtUseBg
            ? qtPublishedSample.load(std::memory_order_relaxed)
            : qtSampleIndex;
        stats.frame_width = qtPublishedWidth.load(std::memory_order_relaxed);
        stats.frame_height = qtPublishedHeight.load(std::memory_order_relaxed);
        if (qtWindow != nullptr) {
          const auto metrics = qtWindow->metrics();
          stats.canvas_width = static_cast<std::uint32_t>(std::max(0, metrics.width));
          stats.canvas_height = static_cast<std::uint32_t>(std::max(0, metrics.height));
          const auto imageRect = qtViewportImageRect();
          stats.displayed_image_width = static_cast<std::uint32_t>(
              std::max(0, static_cast<int>(std::round(imageRect.width))));
          stats.displayed_image_height = static_cast<std::uint32_t>(
              std::max(0, static_cast<int>(std::round(imageRect.height))));
        }
        stats.preview_publish_hz = qtPreviewPublishHz;
        stats.gpu_batches_per_tick = qtLastGpuBatchesPerTick;
        stats.gpu_batch_ms = qtSmoothedGpuBatchMs;
        stats.ui_frame_ms = qtLastUiFrameMs;
        stats.total_rays = qtPublishedRays.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const auto computerMetric = qtComputerRayMetrics.update(
            "computer",
            "Computer",
            stats.total_rays,
            stats.sample_count,
            now);
        stats.instant_rays_per_second = computerMetric.instant_rays_per_second;
        stats.rolling_rays_per_second = computerMetric.rolling_rays_per_second;
        stats.accumulated_rays_per_second = computerMetric.accumulated_rays_per_second;
        const std::string activeDeviceKey = QtDockActiveRayDeviceKey(qtDeviceStats);
        QtDockRayDeviceMetric activeMetric;
        if (qtActiveRenderAverageRps > 0.0) {
          stats.instant_rays_per_second = qtActiveRenderInstantRps;
          stats.rolling_rays_per_second = qtActiveRenderRollingRps;
          stats.accumulated_rays_per_second = qtActiveRenderAverageRps;
          activeMetric.device_key = activeDeviceKey;
          activeMetric.device_name = QtDockActiveRayDeviceName(qtDeviceStats);
          activeMetric.sample_count = stats.sample_count;
          activeMetric.total_rays = stats.total_rays;
          activeMetric.instant_rays_per_second = qtActiveRenderInstantRps;
          activeMetric.rolling_rays_per_second = qtActiveRenderRollingRps;
          activeMetric.accumulated_rays_per_second = qtActiveRenderAverageRps;
          activeMetric.measured = true;
        } else {
          auto& activeAccumulator = qtRayMetricsByDevice[activeDeviceKey];
          activeMetric = activeAccumulator.update(
              activeDeviceKey,
              QtDockActiveRayDeviceName(qtDeviceStats),
              stats.total_rays,
              stats.sample_count,
              now);
        }
        qtDeviceStats.active_device_key = activeDeviceKey;
        QtDockUpsertRayMetric(qtDeviceStats.ray_metrics, std::move(activeMetric));
        stats.render_published = qtPublishedFrames.load(std::memory_order_relaxed);
        stats.render_dropped = qtDroppedFrames.load(std::memory_order_relaxed);
        stats.background_thread = qtUseBg;
        stats.tracer_ready = qtTracerReady;
        stats.preview_status = qtPreviewStatus;
        stats.render_mode = qtTracerReady
            ? (qtUseBg ? std::string("on (background thread)") : std::string("on (event loop)"))
            : std::string("off");
        stats.publish_cap = qtUseBg
            ? (std::to_string(qtPreviewPublishHz) + " fps")
            : std::string("event loop");
        stats.camera_mode = qtFpsMode
            ? std::string("fps")
            : (qtEnableAutoOrbit ? std::string("orbit") : std::string("authored"));
        stats.fps_player_grounded = qtFpsPlayer.grounded;
        stats.fps_player_crouching = qtFpsPlayer.crouching;
        stats.fps_player_running = qtFpsPlayer.running;
        stats.fps_player_speed = qtFpsPlayer.current_speed;
        stats.fps_player_eye_height = qtFpsPlayer.eye_height;
        if (qtWindow != nullptr) {
          const auto windowStats = qtWindow->framebuffer_stats();
          stats.window_received = windowStats.received;
          stats.window_presented = windowStats.presented;
          stats.window_dropped = windowStats.dropped;
        }
        return stats;
      };
      auto qtBuildBenchmarkPanel = [&](const QtDockFrameStats& frameStats) {
        const std::string scenePath =
            config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value;
        const std::string rendererPath = qtUseBg
            ? "cpu_tiled_background"
            : (config.backend.value.empty() ? std::string("event_loop") : config.backend.value);
        auto desc = vkpt::editor::MakeDefaultBenchmarkRunDesc(
            scenePath,
            config.backend.value.empty() ? "cpu" : config.backend.value,
            rendererPath,
            std::max<std::uint32_t>(1u, config.spp.value),
            std::max<std::uint32_t>(1u, config.max_depth.value),
            42u,
            std::max<std::uint32_t>(1u, frameStats.frame_width),
            std::max<std::uint32_t>(1u, frameStats.frame_height));
        vkpt::editor::BenchmarkRawMetricsModel raw;
        raw.fps = ui_runtime_state.fps;
        raw.frame_ms = ui_runtime_state.frame_ms;
        raw.spp_accumulated = frameStats.sample_count;
        raw.samples_per_second = frameStats.ui_frame_ms > 0.0
            ? (static_cast<double>(std::max<std::uint32_t>(1u, frameStats.sample_count)) * 1000.0 /
               frameStats.ui_frame_ms)
            : 0.0;
        raw.paths_per_second = raw.samples_per_second;
        raw.path_vertices_per_second = raw.samples_per_second *
            static_cast<double>(std::max<std::size_t>(1u, qtScene.indices.size() / 3u));
        raw.memory_estimate_bytes =
            static_cast<std::uint64_t>(qtScene.vertices.size() * sizeof(vkpt::pathtracer::Vec3)) +
            static_cast<std::uint64_t>(qtScene.indices.size() * sizeof(std::uint32_t)) +
            static_cast<std::uint64_t>(qtScene.materials.size() * sizeof(vkpt::pathtracer::RTMaterial));
        const auto workload = vkpt::editor::EstimateWorkloadComplexity(
            desc,
            static_cast<std::uint32_t>(qtScene.lights.size()),
            static_cast<std::uint64_t>(qtScene.indices.size() / 3u),
            static_cast<std::uint64_t>(qtScene.instances.size()),
            0u,
            false);
        const double expected = raw.samples_per_second > 0.0 ? raw.samples_per_second : 1.0;
        auto score = vkpt::editor::ComputeBenchmarkScore(
            raw.samples_per_second,
            expected,
            raw.samples_per_second,
            workload.normalized_cost_units,
            false);
        score.raw_paths_per_second = raw.paths_per_second;
        return vkpt::editor::BuildBenchmarkPanelModel(
            desc,
            raw,
            score,
            workload,
            "benchmarks/",
            "Live preview defaults; benchmark not running",
            frameStats.tracer_ready,
            frameStats.tracer_ready ? std::string_view{} : std::string_view{"tracer is not ready"});
      };
      auto syncQtDockPanels = [&](bool force) {
        if (qtWindow == nullptr) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            !qtDockPanelsDirty &&
            qtLastDockPanelSync != std::chrono::steady_clock::time_point{} &&
            now - qtLastDockPanelSync < std::chrono::milliseconds(5000)) {
          return;
        }
        const auto frameStats = qtBuildFrameStats();
        ui_runtime_state.active_scene =
            config.scene_path.value.empty() ? "builtin:preview" : config.scene_path.value;
        ui_runtime_state.active_camera = qtActiveCameraObjectName ? qtActiveCameraObjectName() : "runtime camera";
        ui_runtime_state.active_renderer_backend = config.backend.value;
        ui_runtime_state.active_renderer_path = qtUseBg ? "cpu_tiled_background" : config.backend.value;
        ui_runtime_state.spp_accumulated = frameStats.sample_count;
        ui_runtime_state.frame_ms = frameStats.ui_frame_ms;
        ui_runtime_state.fps = frameStats.ui_frame_ms > 0.0 ? 1000.0 / frameStats.ui_frame_ms : 0.0;
        ui_runtime_state.background_job_count = qtUseBg ? 1u : 0u;
        const auto benchmarkPanel = qtBuildBenchmarkPanel(frameStats);
        const auto statusBar = vkpt::editor::BuildStatusBarModel(
            ui_runtime_state,
            ui_selection_state,
            &benchmarkPanel.score);
        ApplyQtStatusBarToWindow(qtWindow, BuildQtStatusBarText(statusBar));
        qtLastDockPanels = BuildQtDockPanels(qtSceneDocument,
                                             qtScene,
                                             qtSettings,
                                             ui_runtime_state,
                                             ui_selection_state,
                                             ui_layout_state,
                                             benchmarkPanel,
                                             frameStats,
                                             qtDeviceStats,
                                             qtActiveCameraShotSlot,
                                             std::array<bool, 4>{
                                                 qtSavedCameraShots[0].valid,
                                                 qtSavedCameraShots[1].valid,
                                                 qtSavedCameraShots[2].valid,
                                                 qtSavedCameraShots[3].valid},
                                             &qtScriptState);
        ApplyQtDockPanelsToWindow(qtWindow, qtLastDockPanels);
        qtLastDockPanelSync = now;
        qtDockPanelsDirty = false;
      };
      auto updateQtWindowTitle = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        std::string nextTitle =
            BuildWindowRuntimeTitle(qtTitleBase, ui_runtime_state, ui_layout_state);
        if (nextTitle == qtLastWindowTitle) {
          return;
        }
        qtWindow->set_title(nextTitle);
        qtLastWindowTitle = std::move(nextTitle);
      };
      auto emitQtShellReadyMarker = [&]() {
        constexpr std::string_view requiredDockIds[] = {
            "scene_graph",
            "inspector",
            "materials",
            "lights",
            "camera",
            "render_settings",
            "diagnostics",
            "performance",
            "device",
        };
        std::ostringstream docks;
        for (std::size_t i = 0; i < qtLastDockPanels.size(); ++i) {
          if (i > 0u) {
            docks << ",";
          }
          docks << qtLastDockPanels[i].id;
        }
        bool requiredDocksPresent = true;
        for (const auto required : requiredDockIds) {
          const auto found = std::find_if(qtLastDockPanels.begin(),
                                          qtLastDockPanels.end(),
                                          [required](const QtDockPanelContent& panel) {
                                            return panel.id == required;
                                          });
          requiredDocksPresent = requiredDocksPresent && found != qtLastDockPanels.end();
        }
        const std::string message =
            "qt shell ready menu_bar=true status_bar=true dock_count=" +
            std::to_string(qtLastDockPanels.size()) +
            " required_docks=" + (requiredDocksPresent ? std::string("true") : std::string("false")) +
            " docks=" + docks.str();
        std::cout << message << "\n";
        logger.log(vkpt::log::Severity::Info, "app", message);
      };
      auto updateQtPreviewOverlay = [&]() {
        if (qtWindow == nullptr) {
          return;
        }
        qtWindow->set_overlay_text(std::string_view{});
        syncQtDockPanels(false);
        updateQtWindowTitle();
      };
      vkpt::physics::PhysicsStepConfig qtPhysicsStepConfig;
      qtPhysicsStepConfig.fixed_dt = 1.0f / 60.0f;
      qtPhysicsStepConfig.collision_steps = 6;
      qtPhysicsStepConfig.deterministic = true;
      bool qtPhysicsCollisionDetectionEnabled = true;
      float qtPhysicsAccumulatorSeconds = 0.0f;
      auto qtPhysicsPrevTime = std::chrono::steady_clock::now();
      auto qtLastPhysicsScenePublish = std::chrono::steady_clock::time_point{};
      bool qtPhysicsScenePublishPending = false;
      bool qtPhysicsTransformUpdateSupported = false;
      uint32_t qtPhysicsTransformRevision = 1u;
      std::unordered_map<vkpt::core::StableId, vkpt::pathtracer::RTInstanceTransformUpdate>
          qtPendingPhysicsInstanceUpdates;
      struct QtPhysicsRenderPose {
        vkpt::scene::TransformComponent previous;
        vkpt::scene::TransformComponent current;
        bool valid = false;
      };
      std::unordered_map<vkpt::core::StableId, QtPhysicsRenderPose> qtPhysicsRenderPoses;
      double qtPhysicsSceneReloadMs = 0.0;
      auto qtLerpSceneVec3 = [](const vkpt::scene::Vec3& lhs,
                                const vkpt::scene::Vec3& rhs,
                                float alpha) {
        return vkpt::scene::Vec3{
            lhs.x + (rhs.x - lhs.x) * alpha,
            lhs.y + (rhs.y - lhs.y) * alpha,
            lhs.z + (rhs.z - lhs.z) * alpha};
      };
      auto qtNlerpSceneQuat = [](const vkpt::scene::Quat& lhs,
                                 const vkpt::scene::Quat& rhs,
                                 float alpha) {
        vkpt::scene::Quat out{
            lhs.x + (rhs.x - lhs.x) * alpha,
            lhs.y + (rhs.y - lhs.y) * alpha,
            lhs.z + (rhs.z - lhs.z) * alpha,
            lhs.w + (rhs.w - lhs.w) * alpha};
        const float len2 = out.x*out.x + out.y*out.y + out.z*out.z + out.w*out.w;
        if (len2 > 1.0e-12f) {
          const float invLen = 1.0f / std::sqrt(len2);
          out.x *= invLen;
          out.y *= invLen;
          out.z *= invLen;
          out.w *= invLen;
        } else {
          out = {};
        }
        return out;
      };
      auto qtPhysicsInterpolationAlpha = [&]() {
        if (qtPhysicsStepConfig.fixed_dt <= 0.0f) {
          return 1.0f;
        }
        return ClampFloat(qtPhysicsAccumulatorSeconds / qtPhysicsStepConfig.fixed_dt,
                          0.0f,
                          1.0f);
      };
      auto qtFallbackTransformFromUpdate =
          [](const vkpt::pathtracer::RTInstanceTransformUpdate& update) {
        vkpt::scene::TransformComponent transform;
        transform.translation = {update.translation.x, update.translation.y, update.translation.z};
        transform.rotation = {update.rotation.x, update.rotation.y, update.rotation.z, update.rotation.w};
        transform.scale = {update.scale.x, update.scale.y, update.scale.z};
        transform.dirty = true;
        return transform;
      };
      auto qtInterpolatedPhysicsTransform = [&](vkpt::core::StableId entityId,
                                                const vkpt::pathtracer::RTInstanceTransformUpdate& fallback) {
        const auto found = qtPhysicsRenderPoses.find(entityId);
        if (found == qtPhysicsRenderPoses.end() || !found->second.valid) {
          return qtFallbackTransformFromUpdate(fallback);
        }
        const float alpha = qtPhysicsInterpolationAlpha();
        vkpt::scene::TransformComponent transform;
        transform.translation = qtLerpSceneVec3(found->second.previous.translation,
                                                found->second.current.translation,
                                                alpha);
        transform.rotation = qtNlerpSceneQuat(found->second.previous.rotation,
                                              found->second.current.rotation,
                                              alpha);
        transform.scale = qtLerpSceneVec3(found->second.previous.scale,
                                          found->second.current.scale,
                                          alpha);
        transform.dirty = true;
        return transform;
      };
      auto qtPhysicsPublishInterval = [&](bool cameraRecentlyActive) {
        if (qtPhysicsTransformUpdateSupported) {
          return std::chrono::milliseconds(16);
        }
        if (cameraRecentlyActive) {
          const double targetMs = qtPhysicsSceneReloadMs > 0.0
              ? qtPhysicsSceneReloadMs * 1.25
              : 80.0;
          return std::chrono::milliseconds(static_cast<int>(
              std::round(std::clamp(targetMs, 50.0, 240.0))));
        }
        const double targetMs = qtPhysicsSceneReloadMs > 0.0
            ? qtPhysicsSceneReloadMs * 2.5
            : 250.0;
        return std::chrono::milliseconds(static_cast<int>(
            std::round(std::clamp(targetMs, 180.0, 1000.0))));
      };
      auto qtBuildPendingPhysicsInstanceUpdates = [&]() {
        std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
        updates.reserve(qtPendingPhysicsInstanceUpdates.size());
        for (auto& [entityId, update] : qtPendingPhysicsInstanceUpdates) {
          if (update.instance_index == vkpt::pathtracer::kInvalidRTInstanceIndex) {
            update.instance_index = qtFindRtInstanceIndex(entityId);
          }
          if (update.instance_index != vkpt::pathtracer::kInvalidRTInstanceIndex) {
            const auto renderTransform = qtInterpolatedPhysicsTransform(entityId, update);
            update.translation = ToPtVec3(renderTransform.translation);
            update.rotation = ToPtQuat4(renderTransform.rotation);
            update.scale = ToPtVec3(renderTransform.scale);
            updates.push_back(update);
          }
        }
        std::sort(updates.begin(), updates.end(),
                  [](const vkpt::pathtracer::RTInstanceTransformUpdate& lhs,
                     const vkpt::pathtracer::RTInstanceTransformUpdate& rhs) {
                    if (lhs.instance_index != rhs.instance_index) {
                      return lhs.instance_index < rhs.instance_index;
                    }
                    return lhs.entity_id < rhs.entity_id;
                  });
        return updates;
      };
      auto qtQueuePhysicsRenderPoseUpdates = [&]() {
        for (const auto& [entityId, pose] : qtPhysicsRenderPoses) {
          if (!pose.valid) {
            continue;
          }
          auto& pending = qtPendingPhysicsInstanceUpdates[entityId];
          pending.entity_id = entityId;
          pending.instance_index = qtFindRtInstanceIndex(entityId);
          pending.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                          vkpt::pathtracer::kRTInstanceFlagPhysicsControlled |
                          vkpt::pathtracer::kRTInstanceFlagTransformDirty;
          pending.transform_revision = qtPhysicsTransformRevision++;
          pending.translation = ToPtVec3(pose.current.translation);
          pending.rotation = ToPtQuat4(pose.current.rotation);
          pending.scale = ToPtVec3(pose.current.scale);
        }
        qtPhysicsScenePublishPending =
            qtPhysicsScenePublishPending || !qtPendingPhysicsInstanceUpdates.empty();
      };
      auto qtTryPublishPhysicsInstanceUpdates = [&]() {
        if (qtUseBg || !qtTracerReady || qtPendingPhysicsInstanceUpdates.empty()) {
          return false;
        }
        const auto updates = qtBuildPendingPhysicsInstanceUpdates();
        if (updates.empty() || !qtTracer->update_instance_transforms(updates)) {
          qtPhysicsTransformUpdateSupported = false;
          return false;
        }
        for (const auto& update : updates) {
          if (update.instance_index >= qtScene.instances.size()) {
            continue;
          }
          auto& instance = qtScene.instances[update.instance_index];
          instance.translation = update.translation;
          instance.rotation = update.rotation;
          instance.scale = update.scale;
          instance.flags |= update.flags;
          instance.transform_revision = update.transform_revision;
        }
        qtTracerReady = qtTracer->reset_accumulation();
        if (!qtTracerReady) {
          qtPhysicsTransformUpdateSupported = false;
          return false;
        }
        qtSampleIndex = 0u;
        qtPublishedSample.store(0u, std::memory_order_relaxed);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
        qtFpsCollisionWorker.set_pickables(qtPickables);
        RebuildSelectionBounds(ui_selection_state, qtPickables);
        qtDockPanelsDirty = true;
        qtPendingPhysicsInstanceUpdates.clear();
        qtPhysicsTransformUpdateSupported = true;
        updateQtSelectionOverlay();
        return true;
      };
      auto qtApplyPhysicsSimulation = [&](std::chrono::steady_clock::time_point now) {
        auto nearlyEqual = [](float lhs, float rhs, float epsilon) {
          return std::abs(lhs - rhs) <= epsilon;
        };
        if (qtPhysicsRuntimeDirty) {
          qtPhysicsRenderPoses.clear();
          qtPendingPhysicsInstanceUpdates.clear();
          qtPhysicsTransformUpdateSupported = true;
          qtPhysicsTransformRevision++;
          qtPhysicsAccumulatorSeconds = 0.0f;
          qtPhysicsPrevTime = now;
          qtLastPhysicsScenePublish = std::chrono::steady_clock::time_point{};
          qtPhysicsRuntimeDirty = false;
        }
        const bool physicsOn =
            qtPhysicsSummary.enabled_bodies > 0u && qtPhysicsSummary.dynamic_bodies > 0u;
        if (!physicsOn || qtGizmoDrag.active) {
          qtPhysicsPrevTime = now;
          return;
        }

        const float dt = ClampFloat(
            std::chrono::duration<float>(now - qtPhysicsPrevTime).count(),
            0.0f,
            0.05f);
        qtPhysicsPrevTime = now;
        qtPhysicsAccumulatorSeconds =
            std::min(0.25f, qtPhysicsAccumulatorSeconds + dt);

        bool stepped = false;
        int steps = 0;
        constexpr int kMaxPhysicsStepsPerUiFrame = 4;
        qtPhysicsStepConfig.collision_detection_enabled = qtPhysicsCollisionDetectionEnabled;
        while (qtPhysicsAccumulatorSeconds >= qtPhysicsStepConfig.fixed_dt &&
               steps < kMaxPhysicsStepsPerUiFrame) {
          const auto stepResult = qtPhysics->step_fixed(qtPhysicsStepConfig);
          if (!stepResult) {
            qtPreviewStatus = "physics step failed";
            ui_runtime_state.status_message = "physics step failed";
            qtPhysicsAccumulatorSeconds = 0.0f;
            return;
          }
          qtPhysicsAccumulatorSeconds -= qtPhysicsStepConfig.fixed_dt;
          stepped = true;
          ++steps;
        }
        if (steps == kMaxPhysicsStepsPerUiFrame) {
          qtPhysicsAccumulatorSeconds = 0.0f;
        }
        if (!stepped) {
          if (!qtPhysicsRenderPoses.empty() &&
              qtPhysicsTransformUpdateSupported &&
              !qtUseBg &&
              qtTracerReady) {
            qtQueuePhysicsRenderPoseUpdates();
          } else {
            return;
          }
        }

        if (stepped) {
          bool sceneChanged = false;
          const auto preWriteWorldSnapshot = BuildSceneWorldSnapshot(qtSceneDocument);
          const auto* preWriteWorld =
              preWriteWorldSnapshot ? &preWriteWorldSnapshot.value() : nullptr;
          for (const auto& write : qtPhysics->extract_transform_writes()) {
            auto* entity = qtFindEntity(write.entity);
            if (entity == nullptr) {
              continue;
            }
            const auto currentWorld = ResolveEntityWorldTransform(*entity, preWriteWorld);
            const auto& next = write.transform;
            const bool transformChanged =
                !nearlyEqual(currentWorld.translation.x, next.translation.x, 0.0005f) ||
                !nearlyEqual(currentWorld.translation.y, next.translation.y, 0.0005f) ||
                !nearlyEqual(currentWorld.translation.z, next.translation.z, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.x, next.rotation.x, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.y, next.rotation.y, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.z, next.rotation.z, 0.0005f) ||
                !nearlyEqual(currentWorld.rotation.w, next.rotation.w, 0.0005f) ||
                !nearlyEqual(currentWorld.scale.x, next.scale.x, 0.0005f) ||
                !nearlyEqual(currentWorld.scale.y, next.scale.y, 0.0005f) ||
                !nearlyEqual(currentWorld.scale.z, next.scale.z, 0.0005f);
            if (!transformChanged) {
              continue;
            }
            auto& renderPose = qtPhysicsRenderPoses[write.entity];
            renderPose.previous = currentWorld;
            renderPose.current = next;
            renderPose.valid = true;
            entity->has_transform = true;
            entity->transform = ConvertWorldTransformToDocumentLocal(*entity, preWriteWorld, next);
            entity->transform.dirty = true;
            auto& pending = qtPendingPhysicsInstanceUpdates[write.entity];
            pending.entity_id = write.entity;
            pending.instance_index = qtFindRtInstanceIndex(write.entity);
            pending.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                            vkpt::pathtracer::kRTInstanceFlagPhysicsControlled |
                            vkpt::pathtracer::kRTInstanceFlagTransformDirty;
            pending.transform_revision = qtPhysicsTransformRevision++;
            pending.translation = ToPtVec3(next.translation);
            pending.rotation = ToPtQuat4(next.rotation);
            pending.scale = ToPtVec3(next.scale);
            sceneChanged = true;
          }
          if (!sceneChanged) {
            qtPhysicsRenderPoses.clear();
          }
          qtPhysicsScenePublishPending = qtPhysicsScenePublishPending || sceneChanged;
          if (!qtPhysicsScenePublishPending) {
            return;
          }
        } else if (!qtPhysicsScenePublishPending) {
          return;
        }

        const bool cameraRecentlyActive =
            qtLastCameraInputTime != std::chrono::steady_clock::time_point{} &&
            now - qtLastCameraInputTime < std::chrono::milliseconds(250);

        const auto publishInterval = qtPhysicsPublishInterval(cameraRecentlyActive);
        if (qtLastPhysicsScenePublish == std::chrono::steady_clock::time_point{} ||
            now - qtLastPhysicsScenePublish >= publishInterval) {
          const auto publishStart = std::chrono::steady_clock::now();
          qtLastPhysicsScenePublish = now;
          const bool transformUpdatePublished = qtTryPublishPhysicsInstanceUpdates();
          if (transformUpdatePublished) {
            const double publishMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - publishStart).count();
            qtPhysicsSceneReloadMs = qtPhysicsSceneReloadMs <= 0.0
                ? publishMs
                : (qtPhysicsSceneReloadMs * 0.75 + publishMs * 0.25);
            qtPhysicsScenePublishPending = false;
            qtPreviewStatus = "physics transform update";
            ui_runtime_state.status_message = "physics transform update";
          } else if (qtReloadEditedScene("physics simulation")) {
            const double publishMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - publishStart).count();
            qtPhysicsSceneReloadMs = qtPhysicsSceneReloadMs <= 0.0
                ? publishMs
                : (qtPhysicsSceneReloadMs * 0.75 + publishMs * 0.25);
            qtPhysicsScenePublishPending = false;
            qtPendingPhysicsInstanceUpdates.clear();
            qtPhysicsTransformUpdateSupported = false;
            qtPreviewStatus = "physics simulation";
            ui_runtime_state.status_message = "physics simulation";
          }
        }
      };
      syncQtDockPanels(true);
      emitQtShellReadyMarker();
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
            case vkpt::platform::InputEventType::MenuCommand: {
              const auto nativeCommandId = static_cast<NativeMenuId>(event.raw_code);
              const auto mappedAction = qtMenuCommandLookup.find(nativeCommandId);
              if (mappedAction == qtMenuCommandLookup.end()) {
                const auto actionId =
                    std::string("app.menu.unknown_command_") + std::to_string(nativeCommandId);
                qtLogMenuCommand(actionId,
                                 MakeUnsupportedUiCommand(actionId, "unknown Qt menu command id", "menu", qtFrameCount),
                                 std::string("unknown menu action: ") + std::to_string(nativeCommandId),
                                 qtFrameCount);
                break;
              }

              const std::string actionId = mappedAction->second;
              auto menuAction = vkpt::editor::MakeMenuCommand(actionId, "menu", qtFrameCount);
              menuAction.command_id = actionId;

              if (actionId == "file.exit") {
                qtLogMenuCommand(actionId,
                                 MakeUnsupportedUiCommand("file.exit", "user selected Exit", "menu", qtFrameCount),
                                 "menu action executed: file.exit",
                                 qtFrameCount);
                window->close();
                break;
              }

              if (actionId == "view.focus_selected") {
                const bool focused = qtFocusSelected(qtFrameCount, "menu");
                qtLogMenuCommand(actionId,
                                 menuAction,
                                 focused ? "menu action executed: view.focus_selected"
                                         : "menu action failed: no active selection",
                                 qtFrameCount);
                break;
              }

              if (menuAction.kind == vkpt::editor::EditorCommandKind::kClearSelection) {
                ui_selection_state = vkpt::editor::ApplySelectionCommand(ui_selection_state, menuAction);
                RebuildSelectionBounds(ui_selection_state, qtPickables);
                ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
                qtHoveredGizmoHit = std::nullopt;
                qtLogMenuCommand(actionId,
                                 menuAction,
                                 "menu action executed: " + actionId,
                                 qtFrameCount);
                rebuildQtMenuBar();
                updateQtSelectionOverlay();
                qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
                break;
              }

              if (menuAction.kind != vkpt::editor::EditorCommandKind::kUnsupportedUiAction) {
                const std::string menuStatus =
                    std::string("menu action routed: ") +
                    vkpt::editor::ToString(menuAction.kind) +
                    " (command queued)";
                qtLogMenuCommand(actionId, menuAction, menuStatus, qtFrameCount);
                break;
              }

              std::string reason = "unsupported in Qt menu runtime";
              if (std::holds_alternative<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload)) {
                reason = std::get<vkpt::editor::UnsupportedUiActionCommand>(menuAction.payload).reason;
              }
              qtLogMenuCommand(actionId,
                               menuAction,
                               "menu action unsupported: " + actionId + " (" + reason + ")",
                               qtFrameCount);
              break;
            }
            case vkpt::platform::InputEventType::KeyDown: {
              const int key = qtNormalizeKey(event.code);
              const int rawKey = qtNormalizeKey(event.raw_code);
              qtKeysDown.insert(key);
              if (rawKey != 0) {
                qtKeysDown.insert(rawKey);
              }
              if (key == 'F' || rawKey == 'F') {
                qtFocusSelected(qtFrameCount, "keyboard");
              } else if (key == 'P' || rawKey == 'P') {
                qtPickFocusAt(qtLastMouseX, qtLastMouseY, qtFrameCount, "keyboard", true);
              } else if (key == 'V' || rawKey == 'V') {
                if (qtSetFpsMode) {
                  qtSetFpsMode(!qtFpsMode, qtFrameCount, "keyboard");
                }
              } else if (qtFpsMode && (key == kQtKeySpace || rawKey == kQtKeySpace)) {
                qtFpsPlayer.jump_queued = true;
                qtUserCameraActive = true;
              } else if (!qtFpsMode && (key == 'C' || rawKey == 'C')) {
                qtPhysicsCollisionDetectionEnabled = !qtPhysicsCollisionDetectionEnabled;
                qtPhysicsStepConfig.collision_detection_enabled = qtPhysicsCollisionDetectionEnabled;
                qtDockPanelsDirty = true;
                ui_runtime_state.status_message = qtPhysicsCollisionDetectionEnabled
                    ? "physics collision detection on"
                    : "physics collision detection off";
                PushUiEvent(ui_event_log,
                            "physics_collision_detection_toggle",
                            "viewport",
                            "keyboard",
                            qtFrameCount,
                            {},
                            qtPhysicsCollisionDetectionEnabled ? "on" : "off",
                            ui_runtime_state.status_message);
              } else if (!qtFpsMode && (key == 'T' || rawKey == 'T')) {
                qtSetGizmoMode(vkpt::editor::GizmoMode::Translate, "keyboard", qtFrameCount);
              } else if (!qtFpsMode && (key == 'R' || rawKey == 'R')) {
                qtSetGizmoMode(vkpt::editor::GizmoMode::Rotate, "keyboard", qtFrameCount);
              } else if (!qtFpsMode && (key == 'S' || rawKey == 'S')) {
                qtSetGizmoMode(vkpt::editor::GizmoMode::Scale, "keyboard", qtFrameCount);
              } else if (!qtFpsMode && (key == 'G' || rawKey == 'G')) {
                qtSetGizmoMode(vkpt::editor::GizmoMode::Universal, "keyboard", qtFrameCount);
              } else if (key == kQtKeyEscape || rawKey == 27) {
                if (qtSetFpsMode) {
                  qtSetFpsMode(false, qtFrameCount, "keyboard");
                } else {
                  qtFpsMode = false;
                }
                ui_runtime_state.active_viewport_tool = vkpt::editor::ViewportTool::Select;
                ui_runtime_state.active_gizmo_mode = vkpt::editor::GizmoMode::None;
                qtHoveredGizmoHit = std::nullopt;
                ui_runtime_state.status_message = "select camera mode";
                updateQtSelectionOverlay();
                qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
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
              qtLastMouseX = event.x;
              qtLastMouseY = event.y;
              if (qtFpsMode) {
                qtUserCameraActive = true;
                qtLeftMouseDown = false;
                qtRightMouseDown = false;
                qtMiddleMouseDown = false;
                qtPotentialClick = false;
                qtGizmoDrag = {};
                break;
              }
              if (event.code == 0) {
                qtUserCameraActive = true;
                qtLeftMouseDown = true;
                qtClickX = event.x;
                qtClickY = event.y;
                qtClickDragPixels = 0.0f;
                qtPotentialClick = true;
                if (!qtFpsMode &&
                    ui_runtime_state.active_gizmo_mode != vkpt::editor::GizmoMode::None &&
                    ui_selection_state.active_primary_entity != 0u) {
                  const auto activeBounds = qtActiveSelectionBounds();
                  if (activeBounds) {
                    const auto frameRect = qtViewportImageRect();
                    const auto localPoint = qtViewportLocalPoint(event.x, event.y);
                    if (localPoint) {
                      auto hit = PickSelectionGizmoHandle(
                          *activeBounds,
                          qtCameraPose,
                          frameRect.width,
                          frameRect.height,
                          qtRenderAspect(),
                          ui_runtime_state.active_gizmo_mode,
                          localPoint->first,
                          localPoint->second);
                      if (!hit) {
                        hit = PickSelectionBoundsFreeform(
                            *activeBounds,
                            qtCameraPose,
                            frameRect.width,
                            frameRect.height,
                            qtRenderAspect(),
                            ui_runtime_state.active_gizmo_mode,
                            localPoint->first,
                            localPoint->second);
                      }
                      if (hit) {
                        qtCaptureGizmoDrag(*hit, event.x, event.y);
                        if (qtGizmoDrag.active) {
                          qtPotentialClick = false;
                          ui_runtime_state.status_message = "gizmo drag started";
                        }
                      }
                    }
                  }
                }
              } else if (event.code == 1) {
                qtRightMouseDown = true;
                syncQtFpsAnglesFromPose();
              } else if (event.code == 2) {
                qtMiddleMouseDown = true;
              }
              break;
            case vkpt::platform::InputEventType::MouseButtonUp:
              if (qtFpsMode) {
                qtLeftMouseDown = false;
                qtRightMouseDown = false;
                qtMiddleMouseDown = false;
                qtPotentialClick = false;
                qtGizmoDrag = {};
                break;
              }
              if (event.code == 0) {
                qtLeftMouseDown = false;
                if (qtGizmoDrag.active) {
                  qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                  qtGizmoDrag = {};
                  ui_runtime_state.status_message = "gizmo drag committed";
                  qtDockPanelsDirty = true;
                } else if (qtPotentialClick && qtClickDragPixels <= 6.0f) {
                  qtApplyViewportPick(event.x, event.y, qtFrameCount);
                }
                qtPotentialClick = false;
                qtUpdateGizmoHoverCursor(event.x, event.y);
              } else if (event.code == 1) {
                qtRightMouseDown = false;
                qtUpdateGizmoHoverCursor(event.x, event.y);
              } else if (event.code == 2) {
                qtMiddleMouseDown = false;
                qtUpdateGizmoHoverCursor(event.x, event.y);
              }
              break;
            case vkpt::platform::InputEventType::MouseMove: {
              qtLastMouseX = event.x;
              qtLastMouseY = event.y;
              if (qtFpsMode) {
                qtUserCameraActive = true;
                qtPendingFpsLookDx += event.delta_x;
                qtPendingFpsLookDy += event.delta_y;
                break;
              }
              if (qtLeftMouseDown && qtGizmoDrag.active) {
                qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                qtClickDragPixels = std::max(qtClickDragPixels,
                                             ScreenDistance(event.x, event.y, qtClickX, qtClickY));
              } else if (qtLeftMouseDown && qtPotentialClick) {
                const float dx = event.x - qtClickX;
                const float dy = event.y - qtClickY;
                qtClickDragPixels = std::max(qtClickDragPixels, std::sqrt(dx * dx + dy * dy));
              }
              if ((qtRightMouseDown || qtFpsMode) && !qtGizmoDrag.active) {
                qtUserCameraActive = true;
                if (qtFpsMode) {
                  qtApplyFpsLookDelta(event.delta_x, event.delta_y);
                } else {
                  qtApplyOrbitDrag(event.delta_x, event.delta_y);
                }
              } else if (qtMiddleMouseDown && !qtGizmoDrag.active) {
                qtUserCameraActive = true;
                qtApplyPanDrag(event.delta_x, event.delta_y);
              }
              if (!qtGizmoDrag.active && !qtRightMouseDown && !qtMiddleMouseDown) {
                qtUpdateGizmoHoverCursor(event.x, event.y);
              }
              break;
            }
            case vkpt::platform::InputEventType::MouseWheel:
              if (qtFpsMode) {
                break;
              }
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
              if (qtSetFpsMode && qtFpsMode) {
                qtSetFpsMode(false, qtFrameCount, "focus");
              } else {
                qtSetViewportMouseLocked(false);
              }
              qtPendingFpsLookDx = 0.0f;
              qtPendingFpsLookDy = 0.0f;
              qtKeysDown.clear();
              qtLeftMouseDown = false;
              qtRightMouseDown = false;
              qtMiddleMouseDown = false;
              qtPotentialClick = false;
              qtGizmoDrag = {};
              qtHoveredGizmoHit = std::nullopt;
              qtSetViewportCursor(vkpt::platform::QtViewportCursor::Default);
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
        std::vector<vkpt::platform::QtDockPropertyEdit> qtPropertyEdits;
        if (qtWindow != nullptr) {
          qtPropertyEdits = qtWindow->drain_dock_property_edits();
        }
        for (const auto& edit : qtPropertyEdits) {
          qtApplyDockPropertyEdit(edit, qtFrameCount);
        }
        std::vector<vkpt::platform::QtDockRowActivation> qtDockRowActivations;
        if (qtWindow != nullptr) {
          qtDockRowActivations = qtWindow->drain_dock_row_activations();
        }
        for (const auto& activation : qtDockRowActivations) {
          qtApplySceneGraphActivation(activation, qtFrameCount);
        }
        if (!qtInputEvents.empty() || !qtPropertyEdits.empty() || !qtDockRowActivations.empty()) {
          UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                         ui_selection_state,
                                         ui_layout_state,
                                         ui_event_log,
                                         ui_command_history);
        }
        if (qtFpsMode &&
            (std::fabs(qtPendingFpsLookDx) > 0.0f || std::fabs(qtPendingFpsLookDy) > 0.0f)) {
          qtApplyFpsLookDelta(qtPendingFpsLookDx, qtPendingFpsLookDy);
          qtPendingFpsLookDx = 0.0f;
          qtPendingFpsLookDy = 0.0f;
        }
        qtApplyContinuousFpsMovement(qtInputDt);
        qtApplyPhysicsSimulation(qtFrameStart);
        qtApplySceneAnimation(qtFrameStart);
        qtApplyScriptPlayback(qtFrameStart, qtFrameCount, qtInputDt);
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
            qtRefreshCameraFocusFromPose();
            syncQtFpsAnglesFromPose();
#endif
            qtOrbitLastAngleDeg = angleDeg;
            if (qtUseBg && qtRenderCoordinator) {
              // Post camera update; coordinator will apply it between samples.
              qtRenderCoordinator->post_camera(vkpt::render::RenderCameraCommand{
                  qtScene.camera_position,
                  qtScene.camera_target,
                  qtScene.camera_up,
                  qtScene.camera_fov_deg});
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

        if (qtUseBg && qtRenderCoordinator) {
          const auto coordinatorStats = qtRenderCoordinator->stats();
          qtPublishedFrames.store(coordinatorStats.handoff.published, std::memory_order_relaxed);
          qtDroppedFrames.store(coordinatorStats.handoff.dropped, std::memory_order_relaxed);
          qtPublishedRays.store(coordinatorStats.counters.rays, std::memory_order_relaxed);
          if (coordinatorStats.failed) {
            qtTracerReady = false;
            qtPreviewStatus = coordinatorStats.error.empty() ? "render sample failed" : coordinatorStats.error;
            std::cerr << "[qt] background render coordinator failed\n";
            logger.log(vkpt::log::Severity::Error, "app", "Qt background render coordinator failed", {
              {"sample", std::to_string(coordinatorStats.sample_count)},
              {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)}
            });
          }
          if (auto frame = qtRenderCoordinator->acquire_latest_frame()) {
            qtPublishedSample.store(frame->sample_count, std::memory_order_relaxed);
            qtPublishedWidth.store(frame->width, std::memory_order_relaxed);
            qtPublishedHeight.store(frame->height, std::memory_order_relaxed);
            qtPublishedRays.store(frame->counters.rays, std::memory_order_relaxed);
#ifdef PT_ENABLE_QT
            if (qtWindow != nullptr) {
              qtWindow->set_framebuffer_rgba(frame->rgba8, frame->width, frame->height);
            }
#endif
          }
        } else if (!qtUseBg && windowFrameLimit == 0u && qtTracerReady) {
          bool renderedThisTick = false;
          qtLastGpuBatchesPerTick = 0u;
          while (qtTracerReady && qtLastGpuBatchesPerTick < kQtMaxGpuBatchesPerTick) {
            const auto now = std::chrono::steady_clock::now();
            if (qtLastGpuBatchesPerTick > 0u) {
              if (now >= qtFrameStart + kQtInteractiveFrameTarget) {
                break;
              }
              if (qtSmoothedGpuBatchMs > 0.0) {
                const auto estimatedNextBatch =
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double, std::milli>(qtSmoothedGpuBatchMs));
                if (now + estimatedNextBatch > qtFrameStart + kQtInteractiveFrameTarget) {
                  break;
                }
              }
            }

#ifdef PT_ENABLE_QT
            const auto countersBefore = qtTracer->read_counters();
#endif
            const auto batchStart = std::chrono::steady_clock::now();
            if (!qtTracer->render_sample_batch(0, qtSettings.height, qtSampleIndex, qtFrameCount)) {
              qtTracerReady = false;
              qtPreviewStatus = "render sample failed";
              std::cerr << "[qt] render_sample_batch failed at sample " << qtSampleIndex << "\n";
              logger.log(vkpt::log::Severity::Error, "app", "Qt render sample failed", {
                {"sample", std::to_string(qtSampleIndex)},
                {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)}
              });
              break;
            }

            const auto batchMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - batchStart).count();
#ifdef PT_ENABLE_QT
            const auto countersAfter = qtTracer->read_counters();
            qtRecordActiveRenderBatch(countersBefore.rays, countersAfter.rays, batchMs);
#endif
            qtSmoothedGpuBatchMs = (qtSmoothedGpuBatchMs <= 0.0)
                ? batchMs
                : (qtSmoothedGpuBatchMs * 0.75 + batchMs * 0.25);
            ++qtSampleIndex;
            ++qtLastGpuBatchesPerTick;
            renderedThisTick = true;
          }

          if (renderedThisTick && qtTracerReady) {
            qtPublishedRays.store(qtTracer->read_counters().rays, std::memory_order_relaxed);
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
          }
        }

#ifdef PT_ENABLE_QT
        qtLastUiFrameMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - qtFrameStart).count();
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
        if (qtFrameDuration < kQtInteractiveFrameTarget) {
          std::this_thread::sleep_for(kQtInteractiveFrameTarget - qtFrameDuration);
        }
        // Pace the UI/event loop so input and overlay updates do not busy-spin.
      }

      logger.log(vkpt::log::Severity::Info, "app", "Qt render loop closing", {
        {"effective_preview_present_hz", std::to_string(qtPreviewPublishHz)},
        {"frames", std::to_string(qtFrameCount)},
        {"status", qtPreviewStatus}
      });
      if (qtRenderCoordinator) {
        logger.log(vkpt::log::Severity::Info, "app", "Qt background render coordinator stop begin");
        qtRenderCoordinator->stop();
        logger.log(vkpt::log::Severity::Info, "app", "Qt background render coordinator stopped");
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
    const std::string rawTitleBase = BuildWindowTitleBase("raw");
    vkpt::platform::DesktopPlatform platform(rawTitleBase);
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
    // Smoke runs use a bounded sample cap so coordinator shutdown is exercised.
    previewSettings.spp = (windowFrameLimit != 0u)
        ? 1u
        : std::numeric_limits<uint32_t>::max();
    previewSettings.max_depth = std::max<uint32_t>(1u, config.max_depth.value);
    previewSettings.seed = 0xC001D00Dull;
    previewSettings.enable_nee = true;
    previewSettings.enable_mis = true;
    previewSettings.enable_denoiser = true;
    previewSettings.enable_temporal_aa = true;

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

    // Background tiled CPU preview is owned by RenderCoordinator.  The main
    // thread posts commands and consumes immutable frames; it never mutates the
    // active tiled tracer after the coordinator starts.
    std::unique_ptr<vkpt::render::RenderCoordinator> bgRenderCoordinator;
    bool useBgRender = false;

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

      // Launch a coordinator for TiledCpuPathTracer so the main thread's Win32
      // message pump is never blocked and never mutates the live tracer.
      if (dynamic_cast<vkpt::cpu::TiledCpuPathTracer*>(previewTracer.get()) != nullptr) {
        useBgRender = true;
        vkpt::render::RenderCoordinatorConfig coordinatorConfig{};
        coordinatorConfig.publish_hz = 60u;
        coordinatorConfig.immediate_publish_count = 4u;
        bgRenderCoordinator = std::make_unique<vkpt::render::RenderCoordinator>(
            std::move(previewTracer),
            previewSettings,
            previewScene,
            coordinatorConfig);
        if (!bgRenderCoordinator->start()) {
          useBgRender = false;
          previewTracerReady = false;
          previewError = "window preview background coordinator failed";
        }
        logger.log(vkpt::log::Severity::Info, "traceprobe",
                   "background render coordinator launched for TiledCpuPathTracer");
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
      if (previewTracerReady &&
          ((useBgRender && bgRenderCoordinator) || previewTracer) &&
          kOrbitRadius > 1e-4f) {
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
          if (useBgRender && bgRenderCoordinator) {
            bgRenderCoordinator->post_camera(vkpt::render::RenderCameraCommand{
                previewScene.camera_position,
                previewScene.camera_target,
                previewScene.camera_up,
                previewScene.camera_fov_deg});
          } else {
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
          }
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
      // TiledCpuPathTracer runs behind RenderCoordinator to keep the Win32
      // message pump unblocked. The main loop only consumes latest-wins frames.
      // ScalarCpuPathTracer keeps the original incremental pixel-batch path.
      if (useBgRender && bgRenderCoordinator) {
        const auto coordinatorStats = bgRenderCoordinator->stats();
        if (coordinatorStats.failed) {
          previewTracerReady = false;
          previewError = coordinatorStats.error.empty()
              ? "window preview background render failed"
              : coordinatorStats.error;
          logger.log(vkpt::log::Severity::Error,
                     "traceprobe",
                     previewError);
        }
        if (auto bgFrame = bgRenderCoordinator->acquire_latest_frame()) {
          desktopWindow->set_framebuffer_rgba(bgFrame->rgba8, bgFrame->width, bgFrame->height);
          previewRendered  = true;
          previewNonBlack  = false;
          for (std::size_t i = 0; i + 3u < bgFrame->rgba8.size(); i += 4u) {
            if (bgFrame->rgba8[i] || bgFrame->rgba8[i + 1u] || bgFrame->rgba8[i + 2u]) {
              previewNonBlack = true;
              break;
            }
          }
          previewSampleIndex = bgFrame->sample_count;
          // Log every 16 samples to show convergence progress
          if (previewSampleIndex == 1u || (previewSampleIndex % 16u) == 0u) {
            std::cout << "[ui] preview accumulate: samples=" << previewSampleIndex
                      << " non_black=" << (previewNonBlack ? "yes" : "no") << "\n";
            logger.log(vkpt::log::Severity::Info, "traceprobe",
                       "window preview accumulate",
                       {
                         {"samples",   std::to_string(previewSampleIndex)},
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
      SetWindowFrameStatus(desktopWindow, rawTitleBase, ui_runtime_state, ui_layout_state, frame, titlePerfText);
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

    // Stop the background coordinator before tearing down platform resources.
    if (bgRenderCoordinator) {
      bgRenderCoordinator->stop();
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
        auto rendererBackend = vkpt::render::CreateBackend(backendNames.front());
        if (rendererBackend && rendererBackend->initialize()) {
          const auto state = vkpt::render::BuildRendererCrashState(
              *rendererBackend, 0u, "ptapp.crash_test", "crash_test", "none", "crash_test_requested");
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

  if (dynamicPhysicsGate) {
    PrintNonGuiPlatformShellNotice("dynamic-physics-gate", selectedPlatform, effectivePlatform);
    return RunDynamicPhysicsPerformanceGate(config.scene_path.value,
                                            config.backend.value,
                                            config.render_width.value,
                                            config.render_height.value,
                                            std::max<uint32_t>(1u, config.spp.value));
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
    settings.enable_denoiser = gpuDenoiser;
    settings.enable_temporal_aa = temporalAa;

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
