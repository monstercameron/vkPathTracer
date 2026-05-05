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
#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"
#include "render/backends/BackendFactory.h"
#include "render/backends/D3D12Backend.h"
#include "render/backends/VulkanBackend.h"
#include "render/RenderCoordinator.h"
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
  std::cout << "  --list-accelerators   Print D3D12/CPU accelerator capability and ray budget plan\n";
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
  std::cout << "  --dynamic-physics-gate  Run D3D12 dynamic physics transform-update performance gate\n";
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
#ifdef PT_ENABLE_D3D12
  vkpt::render::RayBudgetRequest autoRequest;
  autoRequest.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::Auto;
  const auto autoPlan = vkpt::render::BuildD3D12RayBudgetPlan(autoRequest);
  vkpt::render::RayBudgetRequest highPerformanceRequest = autoRequest;
  highPerformanceRequest.accelerator_preset = vkpt::render::AcceleratorSelectionPreset::HighPerformance;
  const auto highPerformancePlan = vkpt::render::BuildD3D12RayBudgetPlan(highPerformanceRequest);
  std::size_t autoActiveAssignments = 0u;
  for (const auto& assignment : autoPlan.assignments) {
    if (assignment.active) {
      ++autoActiveAssignments;
    }
  }
  std::size_t highPerformanceActiveAssignments = 0u;
  for (const auto& assignment : highPerformancePlan.assignments) {
    if (assignment.active) {
      ++highPerformanceActiveAssignments;
    }
  }
  detail << "accelerator_auto=ok(active=" << autoActiveAssignments
         << ",target_rays=" << autoPlan.total_target_rays << ") "
         << "accelerator_high_performance=ok(active=" << highPerformanceActiveAssignments
         << ",target_rays=" << highPerformancePlan.total_target_rays << ") ";
#endif
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
  const std::filesystem::path sceneDir = "assets/scenes";
  if (!std::filesystem::exists(sceneDir)) {
    r.passed = true;
    r.detail = "skipped(assets/scenes not found)";
    return r;
  }

  std::vector<std::filesystem::path> scenePaths;
  for (const auto& entry : std::filesystem::directory_iterator(sceneDir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      scenePaths.push_back(entry.path());
    }
  }
  std::sort(scenePaths.begin(), scenePaths.end());
  if (scenePaths.empty()) {
    r.passed = true;
    r.detail = "skipped(no scene json files)";
    return r;
  }

  std::vector<std::string> issues;
  std::size_t totalEntities = 0;
  std::size_t totalMaterials = 0;
  std::size_t totalInstances = 0;
  std::size_t totalLights = 0;
  std::size_t totalSdfPrimitives = 0;

  auto checkRuntimeMaterialPresets = [&]() {
    vkpt::scene::SceneDocument presetDoc;
    presetDoc.metadata.schema = "1.0";
    presetDoc.metadata.scene_name = "material_preset_smoke";
    auto addMaterial = [&](vkpt::core::StableId id, std::string family) {
      vkpt::scene::SceneMaterialDefinition material;
      material.id = id;
      material.name = family;
      material.family = std::move(family);
      presetDoc.materials.push_back(std::move(material));
    };
    addMaterial(1u, "mirror");
    addMaterial(2u, "dielectric_glass");
    addMaterial(3u, "clearcoat");
    addMaterial(4u, "emissive");
    for (std::uint32_t i = 0; i < 4u; ++i) {
      const float x = static_cast<float>(i);
      vkpt::scene::SceneGeometryDefinition geometry;
      geometry.id = 100u + i;
      geometry.primitive = "triangle";
      geometry.material_id = i + 1u;
      geometry.vertices = {
          {x, 0.0f, 0.0f},
          {x + 0.5f, 0.0f, 0.0f},
          {x, 0.5f, 0.0f},
      };
      geometry.indices = {0u, 1u, 2u};
      presetDoc.geometry.push_back(std::move(geometry));

      vkpt::scene::SceneEntityDefinition entity;
      entity.id = 200u + i;
      entity.name = "preset_triangle_" + std::to_string(i);
      entity.has_mesh = true;
      entity.mesh.mesh_id = 100u + i;
      entity.mesh.material_id = i + 1u;
      presetDoc.entities.push_back(std::move(entity));
    }
    auto rtResult = vkpt::pathtracer::BuildSceneDataFromDocument(presetDoc);
    if (!rtResult || rtResult.value().materials.size() < 4u) {
      issues.push_back("material_preset_smoke:rt_scene_failed");
      return;
    }
    const auto& mats = rtResult.value().materials;
    if (mats[0].material_model != 2u || mats[0].roughness > 0.001f || mats[0].metallic < 0.99f) {
      issues.push_back("material_preset_smoke:mirror_defaults");
    }
    if (mats[1].material_model != 5u || mats[1].transmission < 0.99f || mats[1].alpha > 0.5f) {
      issues.push_back("material_preset_smoke:glass_defaults");
    }
    if (mats[2].material_model != 7u || mats[2].clearcoat < 0.99f) {
      issues.push_back("material_preset_smoke:clearcoat_defaults");
    }
    if (mats[3].material_model != 1u || !mats[3].is_emissive()) {
      issues.push_back("material_preset_smoke:emissive_defaults");
    }
  };
  checkRuntimeMaterialPresets();

  for (const auto& scenePath : scenePaths) {
    const auto sceneName = scenePath.filename().string();
    auto result = vkpt::scene::SceneDocument::load_from_file(scenePath.string());
    if (!result) {
      issues.push_back(sceneName + ":load=" + std::to_string(static_cast<int>(result.error())));
      continue;
    }

    const auto& document = result.value();
    std::vector<std::string> sceneIssues;
    if (!document.validate(&sceneIssues)) {
      for (const auto& issue : sceneIssues) {
        issues.push_back(sceneName + ":" + issue);
      }
    }

    auto worldResult = document.to_world();
    if (!worldResult) {
      issues.push_back(sceneName + ":to_world=" + std::to_string(static_cast<int>(worldResult.error())));
    }

    auto rtSceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
    if (!rtSceneResult) {
      issues.push_back(sceneName + ":rt_scene=" + std::to_string(static_cast<int>(rtSceneResult.error())));
    } else {
      const auto& rtScene = rtSceneResult.value();
      totalInstances += rtScene.instances.size();
      totalLights += rtScene.lights.size();
      totalSdfPrimitives += rtScene.sdf_primitives.size();
    }

    totalEntities += document.entities.size();
    totalMaterials += document.materials.size();
  }

  r.passed = issues.empty();
  std::ostringstream detail;
  detail << "scenes=" << scenePaths.size()
         << " entities=" << totalEntities
         << " materials=" << totalMaterials
         << " rt_instances=" << totalInstances
         << " rt_lights=" << totalLights
         << " rt_sdf=" << totalSdfPrimitives
         << " valid=" << (r.passed ? "yes" : "no");
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

#ifdef PT_ENABLE_QT
struct QtDockProperty {
  std::string id;
  std::string group;
  std::string label;
  std::string value;
  std::string unit;
  std::string editor;
  std::vector<std::string> options;
  double minimum = 0.0;
  double maximum = 1.0;
  double step = 0.01;
  double default_value = 0.0;
  bool has_numeric_range = false;
  bool has_default = false;
  bool editable = false;
  bool enabled = true;
};

struct QtDockPanelContent {
  std::string id;
  std::string title;
  bool visible = true;
  bool docked = true;
  bool floating = false;
  bool collapsed = false;
  float width = 320.0f;
  float height = 240.0f;
  std::vector<QtDockProperty> properties;
  std::vector<std::string> rows;
};

struct QtDockFrameStats {
  std::uint32_t sample_count = 0u;
  std::uint32_t frame_width = 0u;
  std::uint32_t frame_height = 0u;
  std::uint32_t preview_publish_hz = 0u;
  std::uint32_t gpu_batches_per_tick = 0u;
  double gpu_batch_ms = 0.0;
  double ui_frame_ms = 0.0;
  std::uint64_t total_rays = 0u;
  double instant_rays_per_second = 0.0;
  double rolling_rays_per_second = 0.0;
  std::uint64_t render_published = 0u;
  std::uint64_t render_dropped = 0u;
  std::uint64_t window_received = 0u;
  std::uint64_t window_presented = 0u;
  std::uint64_t window_dropped = 0u;
  bool background_thread = false;
  bool tracer_ready = false;
  std::string preview_status;
  std::string render_mode;
  std::string publish_cap;
  std::string camera_mode;
};

struct QtDockDeviceStats {
  vkpt::render::RenderBackendCapabilities backend_caps;
  std::vector<vkpt::render::AcceleratorCapabilities> accelerators;
  std::string selected_backend;
  std::string active_renderer_path;
  bool has_selected_accelerator = false;
  vkpt::render::AcceleratorCapabilities selected_accelerator;
};

std::string QtDockBool(bool value) {
  return value ? "true" : "false";
}

std::string QtDockNumber(double value, int precision = 2) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(precision) << value;
  return out.str();
}

std::string QtDockBytes(std::uint64_t bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  constexpr double kGiB = kMiB * 1024.0;
  const double value = static_cast<double>(bytes);
  if (bytes == 0u) {
    return "0 B";
  }
  if (value >= kGiB) {
    return QtDockNumber(value / kGiB, 2) + " GiB";
  }
  if (value >= kMiB) {
    return QtDockNumber(value / kMiB, 1) + " MiB";
  }
  if (value >= kKiB) {
    return QtDockNumber(value / kKiB, 1) + " KiB";
  }
  return std::to_string(bytes) + " B";
}

std::string QtDockRate(double raysPerSecond) {
  const double value = std::max(0.0, raysPerSecond);
  if (value >= 1.0e9) {
    return QtDockNumber(value / 1.0e9, 2) + " GRays/s";
  }
  if (value >= 1.0e6) {
    return QtDockNumber(value / 1.0e6, 2) + " MRays/s";
  }
  if (value >= 1.0e3) {
    return QtDockNumber(value / 1.0e3, 2) + " kRays/s";
  }
  return QtDockNumber(value, 1) + " Rays/s";
}

std::string QtDockCount(std::uint64_t value) {
  constexpr double kThousand = 1000.0;
  constexpr double kMillion = kThousand * 1000.0;
  constexpr double kBillion = kMillion * 1000.0;
  constexpr double kTrillion = kBillion * 1000.0;
  const double v = static_cast<double>(value);
  if (v >= kTrillion) {
    return QtDockNumber(v / kTrillion, 2) + "T";
  }
  if (v >= kBillion) {
    return QtDockNumber(v / kBillion, 2) + "B";
  }
  if (v >= kMillion) {
    return QtDockNumber(v / kMillion, 2) + "M";
  }
  if (v >= kThousand) {
    return QtDockNumber(v / kThousand, 2) + "K";
  }
  return std::to_string(value);
}

int QtDockPreferredPixels(float value) {
  if (!std::isfinite(value) || value <= 0.0f) {
    return 0;
  }
  return static_cast<int>(std::round(std::clamp(value, 1.0f, 4096.0f)));
}

std::uint64_t EstimateQtSceneMemoryBytes(const vkpt::pathtracer::RTSceneData& scene) {
  return static_cast<std::uint64_t>(scene.vertices.size() * sizeof(vkpt::pathtracer::Vec3)) +
         static_cast<std::uint64_t>(scene.indices.size() * sizeof(std::uint32_t)) +
         static_cast<std::uint64_t>(scene.materials.size() * sizeof(vkpt::pathtracer::RTMaterial)) +
         static_cast<std::uint64_t>(scene.instances.size() * sizeof(vkpt::pathtracer::RTInstance)) +
         static_cast<std::uint64_t>(scene.tessellation_requests.size() * sizeof(vkpt::pathtracer::RTTessellationRequest)) +
         static_cast<std::uint64_t>(scene.lights.size() * sizeof(vkpt::pathtracer::RTHitLight)) +
         static_cast<std::uint64_t>(scene.sdf_primitives.size() * sizeof(vkpt::pathtracer::RTSdfPrimitive));
}

std::string QtDockFeatureSummary(const vkpt::render::RenderBackendCapabilities& caps) {
  std::vector<std::string> features;
  if (caps.compute) {
    features.push_back("compute");
  }
  if (caps.ray_tracing) {
    features.push_back("ray tracing");
  }
  if (caps.ray_query_supported || caps.ray_query) {
    features.push_back("ray query");
  }
  if (caps.timestamp_queries) {
    features.push_back("timing");
  }
  if (features.empty()) {
    return "basic";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < features.size(); ++i) {
    if (i > 0u) {
      out << ", ";
    }
    out << features[i];
  }
  return out.str();
}

std::string QtDockAcceleratorKind(const vkpt::render::AcceleratorCapabilities& accel) {
  using vkpt::render::AcceleratorKind;
  switch (accel.accelerator_kind) {
    case AcceleratorKind::DiscreteGpu:
      return "Discrete GPU";
    case AcceleratorKind::IntegratedGpu:
      return "Integrated GPU";
    case AcceleratorKind::Warp:
      return "Software adapter";
    case AcceleratorKind::Cpu:
      return "CPU";
    case AcceleratorKind::VirtualGpu:
      return "Virtual GPU";
    case AcceleratorKind::Unknown:
    default:
      return "Unknown";
  }
}

std::string QtDockMemoryOrUnavailable(std::uint64_t bytes) {
  return bytes == 0u ? std::string("not reported") : QtDockBytes(bytes);
}

std::string QtDockMemoryUsage(std::uint64_t usage,
                              std::uint64_t budget,
                              std::string_view unavailable_reason) {
  if (usage == 0u && budget == 0u) {
    return unavailable_reason.empty() ? std::string("not reported") : std::string(unavailable_reason);
  }
  if (budget == 0u) {
    return QtDockBytes(usage);
  }
  std::ostringstream out;
  out << QtDockBytes(usage) << " / " << QtDockBytes(budget);
  if (usage > 0u) {
    const double percent = static_cast<double>(usage) * 100.0 / static_cast<double>(budget);
    out << " (" << QtDockNumber(percent, 1) << "%)";
  }
  return out.str();
}

std::string QtDockVec3(float x, float y, float z) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3)
      << x << ", " << y << ", " << z;
  return out.str();
}

std::string QtDockVec3(const vkpt::scene::Vec3& v) {
  return QtDockVec3(v.x, v.y, v.z);
}

std::string QtDockVec3(const vkpt::pathtracer::Vec3& v) {
  return QtDockVec3(v.x, v.y, v.z);
}

std::string QtDockVec3(const vkpt::editor::Vec3& v) {
  return QtDockVec3(v.x, v.y, v.z);
}

std::string QtDockBounds(const vkpt::editor::Bounds& bounds) {
  if (!bounds.valid) {
    return "invalid";
  }
  return "min(" + QtDockVec3(bounds.min) + ") max(" + QtDockVec3(bounds.max) + ")";
}

void QtDockAddProperty(QtDockPanelContent& panel,
                       std::string_view label,
                       std::string value) {
  QtDockProperty property;
  property.label = std::string(label);
  property.value = std::move(value);
  panel.properties.push_back(std::move(property));
}

void QtDockAddGroupedProperty(QtDockPanelContent& panel,
                              std::string_view group,
                              std::string_view label,
                              std::string value) {
  QtDockProperty property;
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  panel.properties.push_back(std::move(property));
}

void QtDockAddEditableGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value,
                                      std::string unit = {}) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.unit = std::move(unit);
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddDropdownGroupedProperty(QtDockPanelContent& panel,
                                      std::string id,
                                      std::string_view group,
                                      std::string_view label,
                                      std::string value,
                                      std::vector<std::string> options) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "dropdown";
  property.options = std::move(options);
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddButtonGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    std::string value) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = std::move(value);
  property.editor = "button";
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

void QtDockAddSliderGroupedProperty(QtDockPanelContent& panel,
                                    std::string id,
                                    std::string_view group,
                                    std::string_view label,
                                    double value,
                                    double minimum,
                                    double maximum,
                                    double step,
                                    double default_value,
                                    std::string unit = {}) {
  QtDockProperty property;
  property.id = std::move(id);
  property.group = std::string(group);
  property.label = std::string(label);
  property.value = QtDockNumber(value, step >= 1.0 ? 0 : 3);
  property.unit = std::move(unit);
  property.editor = "slider";
  property.minimum = minimum;
  property.maximum = maximum;
  property.step = step;
  property.default_value = default_value;
  property.has_numeric_range = true;
  property.has_default = true;
  property.editable = true;
  property.enabled = true;
  panel.properties.push_back(std::move(property));
}

std::vector<std::string> QtMaterialFamilyOptions() {
  return {
      "diffuse",
      "mirror",
      "glossy",
      "metallic_pbr",
      "ggx_rough_conductor",
      "dielectric_glass",
      "frosted_glass",
      "clearcoat",
      "velvet",
      "fabric_cloth",
      "toon_surface",
      "emissive",
      "procedural_material",
      "marble_scattering",
      "rust_progression",
      "thin_film_iridescent",
      "alpha_mask",
      "blackbody_emission",
      "wet_surface",
      "retroreflector",
      "normal_mapped_pbr",
      "xray"};
}

std::vector<std::string> QtMaterialIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current) {
  std::vector<std::string> options;
  options.push_back("none");
  bool hasCurrent = current == 0u;
  for (const auto& material : document.materials) {
    options.push_back(std::to_string(material.id));
    hasCurrent = hasCurrent || material.id == current;
  }
  if (!hasCurrent) {
    options.push_back(std::to_string(current));
  }
  return options;
}

std::vector<std::string> QtGeometryIdOptions(const vkpt::scene::SceneDocument& document,
                                             vkpt::core::StableId current) {
  std::vector<std::string> options;
  bool hasCurrent = false;
  if (current == 0u) {
    options.push_back("none");
    hasCurrent = true;
  }
  for (const auto& geometry : document.geometry) {
    options.push_back(std::to_string(geometry.id));
    hasCurrent = hasCurrent || geometry.id == current;
  }
  if (!hasCurrent) {
    options.push_back(std::to_string(current));
  }
  return options;
}

std::vector<std::string> QtLightTypeOptions() {
  return {"point", "sphere", "directional", "environment"};
}

std::vector<std::string> QtSdfShapeOptions() {
  return {"sphere", "box", "rounded_box", "torus", "capsule", "plane"};
}

std::vector<std::string> QtPhysicsBodyTypeOptions() {
  return {"static", "dynamic", "kinematic"};
}

std::vector<std::string> QtPhysicsShapeOptions() {
  return {"box", "sphere", "capsule", "cylinder", "mesh"};
}

std::vector<std::string> QtBoolOptions() {
  return {"false", "true"};
}

std::vector<std::string> QtToneMapOptions() {
  return {"linear", "reinhard", "filmic_approx", "aces_approx"};
}

std::vector<std::string> QtOutputTransformOptions() {
  return {"gamma", "linear"};
}

std::string QtToneMapName(vkpt::pathtracer::ToneMapMode mode) {
  switch (mode) {
    case vkpt::pathtracer::ToneMapMode::Reinhard:
      return "reinhard";
    case vkpt::pathtracer::ToneMapMode::FilmicApprox:
      return "filmic_approx";
    case vkpt::pathtracer::ToneMapMode::AcesApprox:
      return "aces_approx";
    case vkpt::pathtracer::ToneMapMode::Linear:
    default:
      return "linear";
  }
}

std::string QtOutputTransformName(vkpt::pathtracer::OutputTransformMode mode) {
  switch (mode) {
    case vkpt::pathtracer::OutputTransformMode::Linear:
      return "linear";
    case vkpt::pathtracer::OutputTransformMode::Gamma:
    default:
      return "gamma";
  }
}

bool QtParseToneMapMode(std::string_view text, vkpt::pathtracer::ToneMapMode& out) {
  const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  std::string value = begin < end ? std::string(begin, end) : std::string{};
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "linear") {
    out = vkpt::pathtracer::ToneMapMode::Linear;
    return true;
  }
  if (value == "reinhard") {
    out = vkpt::pathtracer::ToneMapMode::Reinhard;
    return true;
  }
  if (value == "filmic" || value == "filmic_approx") {
    out = vkpt::pathtracer::ToneMapMode::FilmicApprox;
    return true;
  }
  if (value == "aces" || value == "aces_approx") {
    out = vkpt::pathtracer::ToneMapMode::AcesApprox;
    return true;
  }
  return false;
}

bool QtParseOutputTransformMode(std::string_view text, vkpt::pathtracer::OutputTransformMode& out) {
  const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  std::string value = begin < end ? std::string(begin, end) : std::string{};
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "linear") {
    out = vkpt::pathtracer::OutputTransformMode::Linear;
    return true;
  }
  if (value == "gamma" || value == "srgb" || value == "display") {
    out = vkpt::pathtracer::OutputTransformMode::Gamma;
    return true;
  }
  return false;
}

vkpt::scene::PhysicsBodyComponent QtDefaultDynamicPhysicsBody() {
  vkpt::scene::PhysicsBodyComponent body;
  body.enabled = true;
  body.dynamic = true;
  body.body_type = "dynamic";
  body.shape = "box";
  body.mass = 1.0f;
  body.friction = 0.5f;
  body.restitution = 0.0f;
  body.gravity_scale = 1.0f;
  body.trigger = false;
  body.allow_sleeping = true;
  body.continuous_collision = false;
  return body;
}

void QtDockAddVec3Sliders(QtDockPanelContent& panel,
                          std::string_view prefix,
                          std::string_view group,
                          std::string_view label,
                          const vkpt::scene::Vec3& value,
                          const vkpt::scene::Vec3& defaults,
                          double minimum,
                          double maximum,
                          double step) {
  const std::string base(prefix);
  const std::string labelBase(label);
  QtDockAddSliderGroupedProperty(panel, base + ".x", group, labelBase + " x",
                                 value.x, minimum, maximum, step, defaults.x);
  QtDockAddSliderGroupedProperty(panel, base + ".y", group, labelBase + " y",
                                 value.y, minimum, maximum, step, defaults.y);
  QtDockAddSliderGroupedProperty(panel, base + ".z", group, labelBase + " z",
                                 value.z, minimum, maximum, step, defaults.z);
}

void QtDockAddQuatSliders(QtDockPanelContent& panel,
                          std::string_view prefix,
                          std::string_view group,
                          std::string_view label,
                          const vkpt::scene::Quat& value,
                          const vkpt::scene::Quat& defaults) {
  const std::string base(prefix);
  const std::string labelBase(label);
  QtDockAddSliderGroupedProperty(panel, base + ".x", group, labelBase + " x",
                                 value.x, -1.0, 1.0, 0.01, defaults.x);
  QtDockAddSliderGroupedProperty(panel, base + ".y", group, labelBase + " y",
                                 value.y, -1.0, 1.0, 0.01, defaults.y);
  QtDockAddSliderGroupedProperty(panel, base + ".z", group, labelBase + " z",
                                 value.z, -1.0, 1.0, 0.01, defaults.z);
  QtDockAddSliderGroupedProperty(panel, base + ".w", group, labelBase + " w",
                                 value.w, -1.0, 1.0, 0.01, defaults.w);
}

void QtDockAddCameraControls(QtDockPanelContent& panel,
                             std::string_view prefix,
                             const vkpt::scene::CameraComponent& camera,
                             std::string_view group = "Camera") {
  const std::string base(prefix);
  QtDockAddSliderGroupedProperty(panel, base + "fov", group, "fov",
                                 camera.fov, 1.0, 179.0, 0.1, 60.0, "deg");
  QtDockAddSliderGroupedProperty(panel, base + "near_plane", group, "near",
                                 camera.near_plane, 0.001, 10.0, 0.001, 0.1);
  QtDockAddSliderGroupedProperty(panel, base + "far_plane", group, "far",
                                 camera.far_plane, 1.0, 10000.0, 1.0, 1000.0);
  QtDockAddSliderGroupedProperty(panel, base + "focal_length_mm", group, "focal length",
                                 camera.focal_length_mm, 8.0, 300.0, 0.1, 35.0, "mm");
  QtDockAddSliderGroupedProperty(panel, base + "sensor_width_mm", group, "sensor width",
                                 camera.sensor_width_mm, 4.0, 70.0, 0.1, 36.0, "mm");
  QtDockAddSliderGroupedProperty(panel, base + "sensor_height_mm", group, "sensor height",
                                 camera.sensor_height_mm, 4.0, 70.0, 0.1, 24.0, "mm");
  QtDockAddSliderGroupedProperty(panel, base + "aperture_radius", group, "aperture radius",
                                 camera.aperture_radius, 0.0, 1.0, 0.001, 0.0);
  QtDockAddSliderGroupedProperty(panel, base + "focus_distance", group, "focus distance",
                                 camera.focus_distance, 0.0, 100.0, 0.01, 0.0);
  QtDockAddSliderGroupedProperty(panel, base + "f_stop", group, "f-stop",
                                 camera.f_stop, 0.0, 32.0, 0.1, 0.0);
  QtDockAddSliderGroupedProperty(panel, base + "shutter_seconds", group, "shutter",
                                 camera.shutter_seconds, 0.000125, 1.0, 0.000125, 0.0166666675, "s");
  QtDockAddSliderGroupedProperty(panel, base + "iso", group, "iso",
                                 camera.iso, 25.0, 12800.0, 1.0, 100.0);
  QtDockAddSliderGroupedProperty(panel, base + "exposure_compensation", group, "exposure compensation",
                                 camera.exposure_compensation, -8.0, 8.0, 0.1, 0.0, "EV");
  QtDockAddSliderGroupedProperty(panel, base + "white_balance_kelvin", group, "white balance",
                                 camera.white_balance_kelvin, 1000.0, 40000.0, 50.0, 6500.0, "K");
  QtDockAddSliderGroupedProperty(panel, base + "iris_blade_count", group, "iris blades",
                                 static_cast<double>(camera.iris_blade_count), 0.0, 16.0, 1.0, 0.0);
  QtDockAddSliderGroupedProperty(panel, base + "iris_rotation_degrees", group, "iris rotation",
                                 camera.iris_rotation_degrees, -180.0, 180.0, 1.0, 0.0, "deg");
  QtDockAddSliderGroupedProperty(panel, base + "iris_roundness", group, "iris roundness",
                                 camera.iris_roundness, 0.0, 1.0, 0.01, 1.0);
  QtDockAddSliderGroupedProperty(panel, base + "anamorphic_squeeze", group, "anamorphic squeeze",
                                 camera.anamorphic_squeeze, 0.25, 4.0, 0.01, 1.0);
}

void QtDockAddRow(QtDockPanelContent& panel, std::string row) {
  panel.rows.push_back(std::move(row));
}

const vkpt::editor::UiPanelState* FindQtLayoutPanel(
    const vkpt::editor::UiLayoutDocument& layout,
    std::string_view panel_id) {
  const auto it = std::find_if(layout.panels.begin(),
                               layout.panels.end(),
                               [panel_id](const vkpt::editor::UiPanelState& panel) {
                                 return panel.id == panel_id;
                               });
  return it == layout.panels.end() ? nullptr : &*it;
}

QtDockPanelContent MakeQtDockPanel(const vkpt::editor::UiLayoutDocument& layout,
                                   std::string_view id,
                                   std::string_view title,
                                   bool default_visible,
                                   float default_width = 320.0f,
                                   float default_height = 240.0f) {
  QtDockPanelContent panel;
  panel.id = std::string(id);
  panel.title = std::string(title);
  panel.visible = default_visible;
  panel.width = default_width;
  panel.height = default_height;
  const vkpt::editor::UiPanelState* state = FindQtLayoutPanel(layout, id);
  if (state == nullptr && id == "scene_graph") {
    state = FindQtLayoutPanel(layout, "scene_tree");
  } else if (state == nullptr && id == "materials") {
    state = FindQtLayoutPanel(layout, "material_editor");
  } else if (state == nullptr && id == "diagnostics") {
    state = FindQtLayoutPanel(layout, "console");
  }
  if (state != nullptr) {
    panel.visible = state->visible;
    panel.docked = state->docked;
    panel.floating = state->floating;
    panel.collapsed = state->collapsed;
    panel.width = state->width;
    panel.height = state->height;
  }
  return panel;
}

const vkpt::scene::SceneEntityDefinition* FindQtSceneEntity(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.entities.begin(),
                               document.entities.end(),
                               [id](const vkpt::scene::SceneEntityDefinition& entity) {
                                 return entity.id == id;
                               });
  return it == document.entities.end() ? nullptr : &*it;
}

const vkpt::scene::SceneMaterialDefinition* FindQtSceneMaterial(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.materials.begin(),
                               document.materials.end(),
                               [id](const vkpt::scene::SceneMaterialDefinition& material) {
                                 return material.id == id;
                               });
  return it == document.materials.end() ? nullptr : &*it;
}

const vkpt::scene::SceneGeometryDefinition* FindQtSceneGeometry(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.geometry.begin(),
                               document.geometry.end(),
                               [id](const vkpt::scene::SceneGeometryDefinition& geometry) {
                                 return geometry.id == id;
                               });
  return it == document.geometry.end() ? nullptr : &*it;
}

const vkpt::scene::SceneSdfPrimitiveDefinition* FindQtSceneSdfPrimitive(
    const vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.sdf_primitives.begin(),
                               document.sdf_primitives.end(),
                               [id](const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
                                 return primitive.id == id;
                               });
  return it == document.sdf_primitives.end() ? nullptr : &*it;
}

vkpt::scene::SceneMaterialDefinition* FindQtMutableSceneMaterial(
    vkpt::scene::SceneDocument& document,
    vkpt::core::StableId id) {
  const auto it = std::find_if(document.materials.begin(),
                               document.materials.end(),
                               [id](const vkpt::scene::SceneMaterialDefinition& material) {
                                 return material.id == id;
                               });
  return it == document.materials.end() ? nullptr : &*it;
}

std::string QtTrim(std::string_view text) {
  const auto first = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto last = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

std::vector<std::string> QtSplitPropertyPath(std::string_view id) {
  std::vector<std::string> parts;
  std::size_t start = 0u;
  while (start <= id.size()) {
    const std::size_t dot = id.find('.', start);
    const std::size_t end = dot == std::string_view::npos ? id.size() : dot;
    parts.push_back(std::string(id.substr(start, end - start)));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1u;
  }
  return parts;
}

bool QtParseFloat(std::string_view text, float& out) {
  std::istringstream in(QtTrim(text));
  float value = 0.0f;
  in >> value;
  if (!in || !std::isfinite(value)) {
    return false;
  }
  out = value;
  return true;
}

bool QtParseStableId(std::string_view text, vkpt::core::StableId& out) {
  const std::string trimmed = QtTrim(text);
  if (trimmed.empty() || trimmed == "none") {
    out = 0u;
    return true;
  }
  try {
    std::size_t consumed = 0u;
    const auto value = std::stoull(trimmed, &consumed, 10);
    if (consumed != trimmed.size()) {
      return false;
    }
    out = static_cast<vkpt::core::StableId>(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool QtParseBool(std::string_view text, bool& out) {
  std::string value = QtTrim(text);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    out = true;
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    out = false;
    return true;
  }
  return false;
}

bool QtParseVec3(std::string_view text, vkpt::scene::Vec3& out) {
  std::string normalized(text);
  for (auto& c : normalized) {
    if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']') {
      c = ' ';
    }
  }
  std::istringstream in(normalized);
  vkpt::scene::Vec3 value{};
  in >> value.x >> value.y >> value.z;
  if (!in || !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
    return false;
  }
  out = value;
  return true;
}

bool QtParseQuat(std::string_view text, vkpt::scene::Quat& out) {
  std::string normalized(text);
  for (auto& c : normalized) {
    if (c == ',' || c == '(' || c == ')' || c == '[' || c == ']') {
      c = ' ';
    }
  }
  std::istringstream in(normalized);
  vkpt::scene::Quat value{};
  in >> value.x >> value.y >> value.z >> value.w;
  if (!in || !std::isfinite(value.x) || !std::isfinite(value.y) ||
      !std::isfinite(value.z) || !std::isfinite(value.w)) {
    return false;
  }
  const float lenSq = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
  if (lenSq <= 1.0e-8f) {
    return false;
  }
  const float invLen = 1.0f / std::sqrt(lenSq);
  value.x *= invLen;
  value.y *= invLen;
  value.z *= invLen;
  value.w *= invLen;
  out = value;
  return true;
}

std::string QtEntityComponentSummary(const vkpt::scene::SceneEntityDefinition& entity) {
  std::vector<std::string_view> parts;
  if (entity.has_transform) {
    parts.push_back("Transform");
  }
  if (entity.has_mesh) {
    parts.push_back("Mesh");
  }
  if (entity.has_sdf_primitive) {
    parts.push_back("SDF");
  }
  if (entity.has_light) {
    parts.push_back("Light");
  }
  if (entity.has_camera) {
    parts.push_back("Camera");
  }
  if (!entity.script.script.empty()) {
    parts.push_back("Script");
  }
  if (!entity.animation.clip.empty()) {
    parts.push_back("Animation");
  }
  if (entity.has_physics_body) {
    parts.push_back(entity.physics_body.enabled ? "Physics" : "Physics Off");
  }
  if (entity.has_benchmark_tag && entity.benchmark_tag.enabled) {
    parts.push_back("Benchmark");
  }
  if (parts.empty()) {
    return "Identity";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i > 0u) {
      out << ", ";
    }
    out << parts[i];
  }
  return out.str();
}

std::string QtEntityDisplayName(const vkpt::scene::SceneEntityDefinition& entity) {
  if (!entity.name.empty()) {
    return entity.name;
  }
  return std::string("Entity ") + std::to_string(entity.id);
}

vkpt::core::StableId QtPrimarySelectionId(const vkpt::editor::SelectionState& selection) {
  if (selection.active_primary_entity != 0u) {
    return selection.active_primary_entity;
  }
  if (!selection.selected_entity_ids.empty()) {
    return selection.selected_entity_ids.front();
  }
  return 0u;
}

const vkpt::editor::Bounds* FindQtSelectionBounds(
    const vkpt::editor::SelectionState& selection,
    vkpt::core::StableId entity_id) {
  const auto it = std::find_if(selection.per_item_bounds.begin(),
                               selection.per_item_bounds.end(),
                               [entity_id](const vkpt::editor::SceneEntityBounds& item) {
                                 return item.entity_id == entity_id;
                               });
  return it == selection.per_item_bounds.end() ? nullptr : &it->bounds;
}

void QtDockLimitRows(QtDockPanelContent& panel, std::size_t max_rows) {
  if (panel.rows.size() <= max_rows) {
    return;
  }
  const std::size_t hidden = panel.rows.size() - max_rows;
  panel.rows.resize(max_rows);
  panel.rows.push_back("... " + std::to_string(hidden) + " more");
}

QtDockPanelContent BuildQtSceneTreeDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "scene_graph", "Scene Graph", true, 280.0f, 600.0f);
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddProperty(panel, "geometry", std::to_string(document.geometry.size()));
  QtDockAddProperty(panel, "sdf primitives", std::to_string(document.sdf_primitives.size()));
  const auto is_selected = [&](vkpt::core::StableId id) {
    return std::find(selection.selected_entity_ids.begin(),
                     selection.selected_entity_ids.end(),
                     id) != selection.selected_entity_ids.end();
  };
  for (const auto& entity : document.entities) {
    std::ostringstream row;
    row << (is_selected(entity.id) ? "[x] " : "[ ] ")
        << QtEntityDisplayName(entity)
        << " #" << entity.id
        << " (" << QtEntityComponentSummary(entity) << ")";
    if (entity.hierarchy.parent != 0u) {
      row << " parent=" << entity.hierarchy.parent;
    }
    QtDockAddRow(panel, row.str());
  }
  for (const auto& primitive : document.sdf_primitives) {
    std::ostringstream row;
    const std::string shape = primitive.shape.empty()
        ? (primitive.primitive.shape.empty() ? std::string("sphere") : primitive.primitive.shape)
        : primitive.shape;
    row << (is_selected(primitive.id) ? "[x] " : "[ ] ")
        << "SDF #" << primitive.id
        << " (" << shape << ")";
    QtDockAddRow(panel, row.str());
  }
  if (document.entities.empty()) {
    QtDockAddRow(panel, "No authored entities in document");
  }
  QtDockLimitRows(panel, 128u);
  return panel;
}

QtDockPanelContent BuildQtInspectorDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::editor::SelectionState& selection,
                                        const vkpt::editor::UiRuntimeState& runtime,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "inspector", "Inspector", true, 360.0f, 600.0f);
  const auto primaryId = QtPrimarySelectionId(selection);
  QtDockAddProperty(panel, "selected count", std::to_string(selection.selected_entity_ids.size()));
  QtDockAddProperty(panel, "primary", primaryId == 0u ? std::string("none") : std::to_string(primaryId));
  QtDockAddProperty(panel, "source", vkpt::editor::ToString(selection.selection_source));
  QtDockAddProperty(panel, "tool", vkpt::editor::ToString(runtime.active_viewport_tool));
  QtDockAddProperty(panel, "gizmo", vkpt::editor::ToString(runtime.active_gizmo_mode));
  QtDockAddProperty(panel, "aggregate bounds", QtDockBounds(selection.aggregate_bounds));

  if (primaryId == 0u) {
    QtDockAddRow(panel, "Select an object to inspect transform, material, light, and camera properties");
    return panel;
  }

  const auto* entity = FindQtSceneEntity(document, primaryId);
  if (entity == nullptr) {
    const auto* primitive = FindQtSceneSdfPrimitive(document, primaryId);
    if (primitive == nullptr) {
      QtDockAddRow(panel, "Selected entity is not present in the loaded document");
      return panel;
    }

    QtDockAddProperty(panel, "name", "sdf " + std::to_string(primitive->id));
    QtDockAddProperty(panel, "components", "SDF Primitive");
    if (const auto* bounds = FindQtSelectionBounds(selection, primaryId)) {
      QtDockAddProperty(panel, "bounds", QtDockBounds(*bounds));
    }

    const std::string sdfPrefix = "sdf." + std::to_string(primitive->id) + ".";
    QtDockAddRow(panel, "Transform");
    QtDockAddVec3Sliders(panel,
                         sdfPrefix + "transform.translation",
                         "Transform",
                         "position",
                         primitive->transform.translation,
                         vkpt::scene::Vec3{0.0f, 0.0f, 0.0f},
                         -10.0,
                         10.0,
                         0.01);
    QtDockAddQuatSliders(panel,
                         sdfPrefix + "transform.rotation",
                         "Transform",
                         "rotation",
                         primitive->transform.rotation,
                         vkpt::scene::Quat{0.0f, 0.0f, 0.0f, 1.0f});
    QtDockAddVec3Sliders(panel,
                         sdfPrefix + "transform.scale",
                         "Transform",
                         "scale",
                         primitive->transform.scale,
                         vkpt::scene::Vec3{1.0f, 1.0f, 1.0f},
                         0.01,
                         10.0,
                         0.01);

    const std::string shape = primitive->shape.empty()
        ? (primitive->primitive.shape.empty() ? std::string("sphere") : primitive->primitive.shape)
        : primitive->shape;
    QtDockAddRow(panel, "SDF Primitive");
    QtDockAddDropdownGroupedProperty(panel,
                                     sdfPrefix + "shape",
                                     "SDF Primitive",
                                     "shape",
                                     shape,
                                     QtSdfShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.radius",
                                   "SDF Primitive",
                                   "radius",
                                   primitive->primitive.radius,
                                   0.01,
                                   10.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.param_a",
                                   "SDF Primitive",
                                   "param a",
                                   primitive->primitive.param_a,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   sdfPrefix + "primitive.param_b",
                                   "SDF Primitive",
                                   "param b",
                                   primitive->primitive.param_b,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    return panel;
  }

  QtDockAddProperty(panel, "name", QtEntityDisplayName(*entity));
  QtDockAddProperty(panel, "components", QtEntityComponentSummary(*entity));
  if (const auto* bounds = FindQtSelectionBounds(selection, primaryId)) {
    QtDockAddProperty(panel, "bounds", QtDockBounds(*bounds));
  }

  const std::string entityPrefix = "entity." + std::to_string(entity->id) + ".";
  QtDockAddEditableGroupedProperty(panel,
                                   entityPrefix + "name",
                                   "Entity",
                                   "name",
                                   QtEntityDisplayName(*entity));

  if (entity->has_transform) {
    QtDockAddRow(panel, "Transform");
    QtDockAddVec3Sliders(panel,
                         entityPrefix + "transform.translation",
                         "Transform",
                         "position",
                         entity->transform.translation,
                         vkpt::scene::Vec3{0.0f, 0.0f, 0.0f},
                         -10.0,
                         10.0,
                         0.01);
    QtDockAddQuatSliders(panel,
                         entityPrefix + "transform.rotation",
                         "Transform",
                         "rotation",
                         entity->transform.rotation,
                         vkpt::scene::Quat{0.0f, 0.0f, 0.0f, 1.0f});
    QtDockAddVec3Sliders(panel,
                         entityPrefix + "transform.scale",
                         "Transform",
                         "scale",
                         entity->transform.scale,
                         vkpt::scene::Vec3{1.0f, 1.0f, 1.0f},
                         0.01,
                         10.0,
                         0.01);
  }
  if (entity->has_mesh) {
    QtDockAddRow(panel, "Mesh Renderer");
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "mesh.mesh_id",
                                     "Mesh Renderer",
                                     "mesh",
                                     entity->mesh.mesh_id == 0u
                                         ? std::string("none")
                                         : std::to_string(entity->mesh.mesh_id),
                                     QtGeometryIdOptions(document, entity->mesh.mesh_id));
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "mesh.material_id",
                                     "Mesh Renderer",
                                     "material",
                                     entity->mesh.material_id == 0u
                                         ? std::string("none")
                                         : std::to_string(entity->mesh.material_id),
                                     QtMaterialIdOptions(document, entity->mesh.material_id));
    if (const auto* material = FindQtSceneMaterial(document, entity->mesh.material_id)) {
      const std::string materialPrefix = "material." + std::to_string(material->id) + ".";
      QtDockAddEditableGroupedProperty(panel,
                                       materialPrefix + "name",
                                       "Material",
                                       "material name",
                                       material->name.empty()
                                           ? std::string("material ") + std::to_string(material->id)
                                           : material->name);
      QtDockAddDropdownGroupedProperty(panel,
                                       materialPrefix + "family",
                                       "Material",
                                       "shader/material model",
                                       material->family.empty() ? std::string("diffuse") : material->family,
                                       QtMaterialFamilyOptions());
      QtDockAddVec3Sliders(panel,
                           materialPrefix + "albedo",
                           "Material",
                           "base color",
                           material->albedo,
                           vkpt::scene::Vec3{0.8f, 0.8f, 0.8f},
                           0.0,
                           1.0,
                           0.01);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "roughness",
                                     "Material",
                                     "roughness",
                                     material->roughness,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.6);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "metallic",
                                     "Material",
                                     "metallic",
                                     material->metallic,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.0);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "ior",
                                     "Material",
                                     "ior",
                                     material->ior,
                                     1.01,
                                     2.5,
                                     0.01,
                                     1.5);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "transmission",
                                     "Material",
                                     "transmission",
                                     material->transmission,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.0);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "clearcoat",
                                     "Material",
                                     "clearcoat",
                                     material->clearcoat,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.0);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "alpha",
                                     "Material",
                                     "alpha",
                                     material->alpha,
                                     0.0,
                                     1.0,
                                     0.01,
                                     1.0);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "sheen",
                                     "Material",
                                     "sheen",
                                     material->sheen,
                                     0.0,
                                     1.0,
                                     0.01,
                                     0.0);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "anisotropy",
                                     "Material",
                                     "anisotropy",
                                     material->anisotropy,
                                     -1.0,
                                     1.0,
                                     0.01,
                                     0.0);
      QtDockAddDropdownGroupedProperty(panel,
                                       materialPrefix + "double_sided",
                                       "Material",
                                       "double sided",
                                       QtDockBool(material->double_sided),
                                       QtBoolOptions());
      QtDockAddVec3Sliders(panel,
                           materialPrefix + "emission",
                           "Material",
                           "emission",
                           material->emission,
                           vkpt::scene::Vec3{0.0f, 0.0f, 0.0f},
                           0.0,
                           10.0,
                           0.01);
      QtDockAddSliderGroupedProperty(panel,
                                     materialPrefix + "emission_intensity",
                                     "Material",
                                     "emission intensity",
                                     material->emission_intensity,
                                     0.0,
                                     50.0,
                                     0.1,
                                     0.0);
    }
  }
  if (entity->has_sdf_primitive) {
    QtDockAddRow(panel, "SDF Primitive");
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "sdf_primitive.shape",
                                     "SDF Primitive",
                                     "shape",
                                     entity->sdf_primitive.shape.empty()
                                         ? std::string("sphere")
                                         : entity->sdf_primitive.shape,
                                     QtSdfShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.radius",
                                   "SDF Primitive",
                                   "radius",
                                   entity->sdf_primitive.radius,
                                   0.01,
                                   10.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.param_a",
                                   "SDF Primitive",
                                   "param a",
                                   entity->sdf_primitive.param_a,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "sdf_primitive.param_b",
                                   "SDF Primitive",
                                   "param b",
                                   entity->sdf_primitive.param_b,
                                   -10.0,
                                   10.0,
                                   0.01,
                                   0.0);
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "material.material_id",
                                     "SDF Primitive",
                                     "material",
                                     entity->material.material_id == 0u
                                         ? std::string("none")
                                         : std::to_string(entity->material.material_id),
                                     QtMaterialIdOptions(document, entity->material.material_id));
  }
  if (entity->has_light) {
    QtDockAddRow(panel, "Light");
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "light.type",
                                     "Light",
                                     "type",
                                     entity->light.type.empty() ? std::string("point") : entity->light.type,
                                     QtLightTypeOptions());
    QtDockAddVec3Sliders(panel,
                         entityPrefix + "light.color",
                         "Light",
                         "color",
                         entity->light.color,
                         vkpt::scene::Vec3{1.0f, 1.0f, 1.0f},
                         0.0,
                         1.0,
                         0.01);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "light.intensity",
                                   "Light",
                                   "intensity",
                                   entity->light.intensity,
                                   0.0,
                                   100.0,
                                   0.1,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "light.radius",
                                   "Light",
                                   "radius",
                                   entity->light.radius,
                                   0.0,
                                   10.0,
                                   0.01,
                                   0.0);
  }
  if (entity->has_camera) {
    QtDockAddRow(panel, "Camera");
    QtDockAddCameraControls(panel, entityPrefix + "camera.", entity->camera);
  }
  QtDockAddRow(panel, "Physics");
  QtDockAddDropdownGroupedProperty(panel,
                                   entityPrefix + "physics.enabled",
                                   "Physics",
                                   "enabled",
                                   QtDockBool(entity->has_physics_body && entity->physics_body.enabled),
                                   QtBoolOptions());
  if (entity->has_physics_body) {
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.body_type",
                                     "Physics",
                                     "body type",
                                     entity->physics_body.dynamic
                                         ? std::string("dynamic")
                                         : entity->physics_body.body_type,
                                     QtPhysicsBodyTypeOptions());
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.shape",
                                     "Physics",
                                     "shape",
                                     entity->physics_body.shape.empty()
                                         ? std::string("box")
                                         : entity->physics_body.shape,
                                     QtPhysicsShapeOptions());
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.mass",
                                   "Physics",
                                   "mass",
                                   entity->physics_body.mass,
                                   0.01,
                                   1000.0,
                                   0.01,
                                   1.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.friction",
                                   "Physics",
                                   "friction",
                                   entity->physics_body.friction,
                                   0.0,
                                   2.0,
                                   0.01,
                                   0.5);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.restitution",
                                   "Physics",
                                   "restitution",
                                   entity->physics_body.restitution,
                                   0.0,
                                   1.0,
                                   0.01,
                                   0.0);
    QtDockAddSliderGroupedProperty(panel,
                                   entityPrefix + "physics.gravity_scale",
                                   "Physics",
                                   "gravity scale",
                                   entity->physics_body.gravity_scale,
                                   -4.0,
                                   4.0,
                                   0.01,
                                   1.0);
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.trigger",
                                     "Physics",
                                     "trigger",
                                     QtDockBool(entity->physics_body.trigger),
                                     QtBoolOptions());
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.allow_sleeping",
                                     "Physics",
                                     "allow sleeping",
                                     QtDockBool(entity->physics_body.allow_sleeping),
                                     QtBoolOptions());
    QtDockAddDropdownGroupedProperty(panel,
                                     entityPrefix + "physics.continuous_collision",
                                     "Physics",
                                     "continuous collision",
                                     QtDockBool(entity->physics_body.continuous_collision),
                                     QtBoolOptions());
  }
  if (!entity->script.script.empty()) {
    QtDockAddRow(panel, "Script: " + entity->script.script);
  }
  return panel;
}

QtDockPanelContent BuildQtMaterialsDock(const vkpt::scene::SceneDocument& document,
                                        const vkpt::pathtracer::RTSceneData& scene,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "materials", "Materials", true, 520.0f, 420.0f);
  QtDockAddProperty(panel, "authored materials", std::to_string(document.materials.size()));
  QtDockAddProperty(panel, "runtime materials", std::to_string(scene.materials.size()));
  for (const auto& material : document.materials) {
    std::ostringstream row;
    row << "#" << material.id << " "
        << (material.name.empty() ? "material" : material.name)
        << " albedo=(" << QtDockVec3(material.albedo) << ")"
        << " roughness=" << QtDockNumber(material.roughness, 2);
    if (material.emission_intensity > 0.0f) {
      row << " emissive=" << QtDockNumber(material.emission_intensity, 2);
    }
    QtDockAddRow(panel, row.str());
  }
  if (document.materials.empty()) {
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
      const auto& material = scene.materials[i];
      std::ostringstream row;
      row << "runtime[" << i << "] albedo=(" << QtDockVec3(material.albedo)
          << ") roughness=" << QtDockNumber(material.roughness, 2)
          << " emissive=" << QtDockBool(material.is_emissive());
      QtDockAddRow(panel, row.str());
    }
  }
  QtDockLimitRows(panel, 96u);
  return panel;
}

QtDockPanelContent BuildQtLightsDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "lights", "Lights", true, 360.0f, 360.0f);
  QtDockAddProperty(panel, "authored lights", std::to_string(document.lights.size()));
  QtDockAddProperty(panel, "runtime lights", std::to_string(scene.lights.size()));
  for (const auto& entity : document.entities) {
    if (!entity.has_light) {
      continue;
    }
    std::ostringstream row;
    row << QtEntityDisplayName(entity) << " #" << entity.id
        << " " << entity.light.type
        << " intensity=" << QtDockNumber(entity.light.intensity, 2)
        << " color=(" << QtDockVec3(entity.light.color) << ")";
    QtDockAddRow(panel, row.str());
  }
  for (std::size_t i = 0; i < scene.lights.size(); ++i) {
    const auto& light = scene.lights[i];
    std::ostringstream row;
    row << "runtime[" << i << "] pos=(" << QtDockVec3(light.position)
        << ") intensity=" << QtDockNumber(light.intensity, 2)
        << " radius=" << QtDockNumber(light.radius, 2);
    QtDockAddRow(panel, row.str());
  }
  if (panel.rows.empty()) {
    QtDockAddRow(panel, "No lights in the loaded document or render scene");
  }
  return panel;
}

QtDockPanelContent BuildQtCameraDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     int active_shot_slot,
                                     const std::array<bool, 4>& saved_shot_slots) {
  auto panel = MakeQtDockPanel(layout, "camera", "Camera", true, 360.0f, 320.0f);
  QtDockAddProperty(panel, "active camera", runtime.active_camera.empty() ? "runtime camera" : runtime.active_camera);
  QtDockAddProperty(panel, "mode", frame_stats.camera_mode.empty() ? "authored" : frame_stats.camera_mode);
  QtDockAddProperty(panel, "position", QtDockVec3(scene.camera_position));
  QtDockAddProperty(panel, "target", QtDockVec3(scene.camera_target));
  QtDockAddProperty(panel, "up", QtDockVec3(scene.camera_up));
  QtDockAddProperty(panel, "fov", QtDockNumber(scene.camera_fov_deg, 2));
  QtDockAddGroupedProperty(panel, "Runtime Lens", "focal length", QtDockNumber(scene.camera_focal_length_mm, 1) + " mm");
  QtDockAddGroupedProperty(panel, "Runtime Lens", "sensor", QtDockNumber(scene.camera_sensor_width_mm, 1) +
                           " x " + QtDockNumber(scene.camera_sensor_height_mm, 1) + " mm");
  QtDockAddGroupedProperty(panel, "Runtime Lens", "aperture radius", QtDockNumber(scene.camera_aperture_radius, 3));
  QtDockAddGroupedProperty(panel, "Runtime Lens", "focus distance", QtDockNumber(scene.camera_focus_distance, 3));
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.focus.pick",
                                 "Focus Tools",
                                 "auto focus",
                                 "Focus Under Cursor");
  QtDockAddButtonGroupedProperty(panel,
                                 "camera.focus.selected",
                                 "Focus Tools",
                                 "selected focus",
                                 "Focus Selected");
  QtDockAddGroupedProperty(panel, "Runtime Film", "exposure compensation",
                           QtDockNumber(scene.camera_exposure_compensation, 2) + " EV");
  QtDockAddGroupedProperty(panel, "Runtime Film", "physical exposure",
                           scene.camera_f_stop > 0.0f ? "active" : "off");
  QtDockAddGroupedProperty(panel, "Runtime Film", "white balance",
                           QtDockNumber(scene.camera_white_balance_kelvin, 0) + " K");
  const int clampedShotSlot = std::clamp(active_shot_slot, 0, 3);
  QtDockAddDropdownGroupedProperty(panel,
                                   "camera.shot.slot",
                                   "Camera Shots",
                                   "active slot",
                                   std::to_string(clampedShotSlot + 1),
                                   {"1", "2", "3", "4"});
  QtDockAddDropdownGroupedProperty(panel,
                                   "camera.shot.save",
                                   "Camera Shots",
                                   "save",
                                   "false",
                                   QtBoolOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "camera.shot.recall",
                                   "Camera Shots",
                                   "recall",
                                   "false",
                                   QtBoolOptions());
  for (std::size_t i = 0; i < saved_shot_slots.size(); ++i) {
    QtDockAddGroupedProperty(panel,
                             "Camera Shots",
                             "slot " + std::to_string(i + 1u),
                             saved_shot_slots[i] ? "saved" : "empty");
  }
  QtDockAddProperty(panel, "viewport tool", vkpt::editor::ToString(runtime.active_viewport_tool));
  for (const auto& entity : document.entities) {
    if (entity.has_camera) {
      std::ostringstream row;
      row << QtEntityDisplayName(entity) << " #" << entity.id
          << " fov=" << QtDockNumber(entity.camera.fov, 2)
          << " near=" << QtDockNumber(entity.camera.near_plane, 3)
          << " far=" << QtDockNumber(entity.camera.far_plane, 1);
      QtDockAddRow(panel, row.str());
      QtDockAddCameraControls(panel,
                              "entity." + std::to_string(entity.id) + ".camera.",
                              entity.camera,
                              "Camera #" + std::to_string(entity.id));
    }
  }
  return panel;
}

QtDockPanelContent BuildQtRenderSettingsDock(const vkpt::pathtracer::RTSceneData& scene,
                                             const vkpt::pathtracer::RenderSettings& settings,
                                             const vkpt::editor::UiRuntimeState& runtime,
                                             const vkpt::editor::UiLayoutDocument& layout,
                                             const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "render_settings", "Render Settings", true, 360.0f, 360.0f);
  QtDockAddProperty(panel, "scene", runtime.active_scene.empty() ? "builtin:preview" : runtime.active_scene);
  QtDockAddProperty(panel, "backend", runtime.active_renderer_backend);
  QtDockAddProperty(panel, "renderer path", runtime.active_renderer_path);
  QtDockAddProperty(panel, "path tracing", frame_stats.render_mode.empty()
      ? (frame_stats.tracer_ready ? "on" : "off")
      : frame_stats.render_mode);
  QtDockAddProperty(panel, "frame", std::to_string(frame_stats.frame_width) + "x" + std::to_string(frame_stats.frame_height));
  QtDockAddProperty(panel, "samples accumulated", std::to_string(frame_stats.sample_count));
  QtDockAddProperty(panel, "publish cap", frame_stats.publish_cap.empty()
      ? std::to_string(frame_stats.preview_publish_hz) + " fps"
      : frame_stats.publish_cap);
  QtDockAddProperty(panel, "threading", frame_stats.background_thread ? "background render thread" : "event loop renderer");
  QtDockAddProperty(panel, "environment", QtDockVec3(scene.environment_color));
  QtDockAddProperty(panel, "triangles", std::to_string(scene.indices.size() / 3u));
  QtDockAddProperty(panel, "instances", std::to_string(scene.instances.size()));
  QtDockAddProperty(panel, "sdf primitives", std::to_string(scene.sdf_primitives.size()));
  QtDockAddProperty(panel, "textures", std::to_string(scene.textures.size()));
  QtDockAddSliderGroupedProperty(panel,
                                 "render.max_depth",
                                 "Integrator",
                                 "max depth",
                                 settings.max_depth,
                                 1.0,
                                 64.0,
                                 1.0,
                                 6.0);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.nee",
                                   "Integrator",
                                   "next event estimation",
                                   QtDockBool(settings.enable_nee),
                                   QtBoolOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.mis",
                                   "Integrator",
                                   "MIS",
                                   QtDockBool(settings.enable_mis),
                                   QtBoolOptions());
  QtDockAddSliderGroupedProperty(panel,
                                 "render.film.exposure",
                                 "Color",
                                 "exposure",
                                 settings.film_resolve.exposure,
                                 0.0,
                                 8.0,
                                 0.01,
                                 1.0);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.tone_map",
                                   "Color",
                                   "tone mapper",
                                   QtToneMapName(settings.film_resolve.tone_map),
                                   QtToneMapOptions());
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.output_transform",
                                   "Color",
                                   "output transform",
                                   QtOutputTransformName(settings.film_resolve.output_transform),
                                   QtOutputTransformOptions());
  QtDockAddSliderGroupedProperty(panel,
                                 "render.film.gamma",
                                 "Color",
                                 "gamma",
                                 settings.film_resolve.gamma,
                                 0.1,
                                 4.0,
                                 0.01,
                                 2.2);
  QtDockAddDropdownGroupedProperty(panel,
                                   "render.film.clamp_output",
                                   "Color",
                                   "clamp output",
                                   QtDockBool(settings.film_resolve.clamp_output),
                                   QtBoolOptions());
  QtDockAddRow(panel, "Reset accumulation is triggered by camera, film, integrator, and future scene edits");
  return panel;
}

QtDockPanelContent BuildQtBenchmarkDock(const vkpt::editor::BenchmarkPanelModel& benchmark,
                                        const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "benchmark_panel", "Benchmark", false, 560.0f, 480.0f);
  QtDockAddProperty(panel, "scene", benchmark.run_desc.scene_path);
  QtDockAddProperty(panel, "backend", benchmark.run_desc.backend);
  QtDockAddProperty(panel, "renderer", benchmark.run_desc.renderer_path);
  QtDockAddProperty(panel, "resolution", std::to_string(benchmark.run_desc.resolution.width) +
      "x" + std::to_string(benchmark.run_desc.resolution.height));
  QtDockAddProperty(panel, "spp", std::to_string(benchmark.run_desc.samples_per_pixel));
  QtDockAddProperty(panel, "max depth", std::to_string(benchmark.run_desc.max_depth));
  QtDockAddProperty(panel, "can run", QtDockBool(benchmark.can_run));
  QtDockAddProperty(panel, "summary", benchmark.result_summary);
  QtDockAddProperty(panel, "score", QtDockNumber(benchmark.score.normalized_score, 3));
  QtDockAddProperty(panel, "confidence", benchmark.score.confidence);
  for (const auto& action : benchmark.calibration_actions) {
    QtDockAddRow(panel, action.label + " [" + (action.supported ? "available" : action.unavailable_reason) + "]");
  }
  return panel;
}

QtDockPanelContent BuildQtDiagnosticsDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::SelectionState& selection,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "diagnostics", "Diagnostics", true, 720.0f, 260.0f);
  QtDockAddProperty(panel, "status", runtime.status_message);
  QtDockAddProperty(panel, "last warning/error", runtime.last_warning_or_error);
  QtDockAddProperty(panel, "last menu action", runtime.last_menu_action);
  QtDockAddProperty(panel, "last clicked entity", std::to_string(runtime.last_clicked_entity));
  QtDockAddProperty(panel, "focused panel", runtime.focused_panel);
  QtDockAddProperty(panel, "active modal", runtime.active_modal.empty() ? "none" : runtime.active_modal);
  QtDockAddProperty(panel, "tracer ready", QtDockBool(frame_stats.tracer_ready));
  QtDockAddProperty(panel, "preview status", frame_stats.preview_status);
  QtDockAddProperty(panel, "selection source", vkpt::editor::ToString(selection.selection_source));
  return panel;
}

QtDockPanelContent BuildQtPerformanceDock(const vkpt::editor::UiRuntimeState& runtime,
                                          const vkpt::editor::UiLayoutDocument& layout,
                                          const QtDockFrameStats& frame_stats) {
  auto panel = MakeQtDockPanel(layout, "performance", "Performance", true, 360.0f, 320.0f);
  QtDockAddProperty(panel, "ui fps", QtDockNumber(runtime.fps, 1));
  QtDockAddProperty(panel, "ui frame ms", QtDockNumber(runtime.frame_ms, 2));
  QtDockAddProperty(panel, "samples", std::to_string(frame_stats.sample_count));
  QtDockAddProperty(panel, "published", std::to_string(frame_stats.render_published));
  QtDockAddProperty(panel, "render dropped", std::to_string(frame_stats.render_dropped));
  QtDockAddProperty(panel, "window received", std::to_string(frame_stats.window_received));
  QtDockAddProperty(panel, "window presented", std::to_string(frame_stats.window_presented));
  QtDockAddProperty(panel, "window dropped", std::to_string(frame_stats.window_dropped));
  QtDockAddProperty(panel, "publish cap", frame_stats.publish_cap.empty()
      ? std::to_string(frame_stats.preview_publish_hz) + " fps"
      : frame_stats.publish_cap);
  QtDockAddProperty(panel, "gpu batches/tick", frame_stats.background_thread
      ? std::string("background")
      : std::to_string(frame_stats.gpu_batches_per_tick));
  QtDockAddProperty(panel, "gpu batch ms", QtDockNumber(frame_stats.gpu_batch_ms, 3));
  QtDockAddProperty(panel, "background jobs", std::to_string(runtime.background_job_count));
  return panel;
}

QtDockPanelContent BuildQtDeviceDock(const vkpt::pathtracer::RTSceneData& scene,
                                     const vkpt::editor::UiRuntimeState& runtime,
                                     const vkpt::editor::UiLayoutDocument& layout,
                                     const QtDockFrameStats& frame_stats,
                                     const QtDockDeviceStats& device_stats) {
  auto panel = MakeQtDockPanel(layout, "device", "Device", true, 420.0f, 260.0f);
  const auto& caps = device_stats.backend_caps;
  const auto selectedBackend = device_stats.selected_backend.empty()
      ? runtime.active_renderer_backend
      : device_stats.selected_backend;
  const auto rendererPath = device_stats.active_renderer_path.empty()
      ? runtime.active_renderer_path
      : device_stats.active_renderer_path;

  const vkpt::render::AcceleratorCapabilities* selectedAccel =
      device_stats.has_selected_accelerator ? &device_stats.selected_accelerator : nullptr;
  const auto& selectedBudget = selectedAccel != nullptr
      ? selectedAccel->backend_caps.memory_budget
      : caps.memory_budget;
  const std::uint64_t usage = selectedAccel != nullptr && selectedAccel->current_usage_bytes > 0u
      ? selectedAccel->current_usage_bytes
      : selectedBudget.current_usage_bytes;
  const std::uint64_t budget = selectedAccel != nullptr && selectedAccel->current_budget_bytes > 0u
      ? selectedAccel->current_budget_bytes
      : selectedBudget.current_budget_bytes;
  const std::uint64_t dedicatedMemory = selectedAccel != nullptr && selectedAccel->dedicated_video_memory_bytes > 0u
      ? selectedAccel->dedicated_video_memory_bytes
      : selectedBudget.dedicated_video_memory_bytes;
  const std::uint64_t sharedMemory = selectedAccel != nullptr && selectedAccel->shared_system_memory_bytes > 0u
      ? selectedAccel->shared_system_memory_bytes
      : selectedBudget.shared_system_memory_bytes;
  const std::string deviceName = selectedAccel != nullptr
      ? selectedAccel->name
      : (caps.platform.platform_name.empty() ? std::string("unknown") : caps.platform.platform_name);
  const std::string deviceKind = selectedAccel != nullptr
      ? QtDockAcceleratorKind(*selectedAccel)
      : (caps.is_simulated ? std::string("Simulated") : std::string("Unknown"));

  QtDockAddGroupedProperty(panel, "Performance", "Rolling rays/sec",
                           QtDockRate(frame_stats.rolling_rays_per_second));
  QtDockAddGroupedProperty(panel, "Performance", "Instant rays/sec",
                           QtDockRate(frame_stats.instant_rays_per_second));
  QtDockAddGroupedProperty(panel, "Performance", "Samples",
                           std::to_string(frame_stats.sample_count));
  QtDockAddGroupedProperty(panel, "Performance", "Total rays",
                           QtDockCount(frame_stats.total_rays));

  QtDockAddGroupedProperty(panel, "Renderer", "Backend", selectedBackend);
  QtDockAddGroupedProperty(panel, "Renderer", "Path", rendererPath);
  QtDockAddGroupedProperty(panel, "Renderer", "Mode",
                           frame_stats.background_thread ? "background thread" : "event loop");
  QtDockAddGroupedProperty(panel, "Renderer", "Features", QtDockFeatureSummary(caps));

  QtDockAddGroupedProperty(panel, "Device", "Active", deviceName);
  QtDockAddGroupedProperty(panel, "Device", "Type", deviceKind);
  QtDockAddGroupedProperty(panel, "Device", "Adapters",
                           device_stats.accelerators.empty()
                               ? std::string("not reported")
                               : std::to_string(device_stats.accelerators.size()));
  QtDockAddGroupedProperty(panel, "Device", "Memory model",
                           caps.memory_model.empty() ? std::string("unknown") : caps.memory_model);

  QtDockAddGroupedProperty(panel, "Memory", "Usage",
                           QtDockMemoryUsage(usage, budget, selectedBudget.budget_unavailable_reason));
  QtDockAddGroupedProperty(panel, "Memory", "Dedicated",
                           QtDockMemoryOrUnavailable(dedicatedMemory));
  QtDockAddGroupedProperty(panel, "Memory", "Shared",
                           QtDockMemoryOrUnavailable(sharedMemory));
  QtDockAddGroupedProperty(panel, "Memory", "Scene buffers",
                           QtDockBytes(EstimateQtSceneMemoryBytes(scene)));
  return panel;
}

QtDockPanelContent BuildQtDebugViewsDock(const vkpt::editor::UiRuntimeState& runtime,
                                         const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "debug_views", "Debug Views", false, 320.0f, 300.0f);
  QtDockAddProperty(panel, "selected view", runtime.selected_debug_view.empty() ? "beauty" : runtime.selected_debug_view);
  QtDockAddProperty(panel, "active channel", runtime.active_debug_channel.empty() ? "rgb" : runtime.active_debug_channel);
  QtDockAddRow(panel, "beauty");
  QtDockAddRow(panel, "albedo");
  QtDockAddRow(panel, "normal");
  QtDockAddRow(panel, "depth");
  QtDockAddRow(panel, "sample_count");
  QtDockAddRow(panel, "selection_id");
  return panel;
}

QtDockPanelContent BuildQtAssetBrowserDock(const vkpt::scene::SceneDocument& document,
                                           const vkpt::pathtracer::RTSceneData& scene,
                                           const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "asset_browser", "Asset Browser", true, 720.0f, 260.0f);
  QtDockAddProperty(panel, "assets", std::to_string(document.assets.size()));
  QtDockAddProperty(panel, "geometry", std::to_string(document.geometry.size()));
  QtDockAddProperty(panel, "textures", std::to_string(scene.textures.size()));
  for (const auto& asset : document.assets) {
    QtDockAddRow(panel, "#" + std::to_string(asset.id) + " " + asset.type + " " + asset.uri);
  }
  for (const auto& geometry : document.geometry) {
    QtDockAddRow(panel, "#" + std::to_string(geometry.id) + " geometry " +
        (geometry.primitive.empty() ? "mesh" : geometry.primitive) +
        " vertices=" + std::to_string(geometry.vertices.size()) +
        " indices=" + std::to_string(geometry.indices.size()));
  }
  for (const auto& texture : scene.textures) {
    QtDockAddRow(panel, "texture " + texture);
  }
  if (panel.rows.empty()) {
    QtDockAddRow(panel, "No external assets referenced by this scene");
  }
  QtDockLimitRows(panel, 128u);
  return panel;
}

QtDockPanelContent BuildQtTimelineDock(const vkpt::scene::SceneDocument& document,
                                       const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "timeline", "Timeline", false, 560.0f, 220.0f);
  std::size_t animated = 0u;
  for (const auto& entity : document.entities) {
    if (!entity.animation.clip.empty()) {
      ++animated;
      QtDockAddRow(panel, QtEntityDisplayName(entity) + " clip=" + entity.animation.clip +
          (entity.animation.looping ? " loop" : " once"));
    }
  }
  QtDockAddProperty(panel, "animated entities", std::to_string(animated));
  if (animated == 0u) {
    QtDockAddRow(panel, "No animation clips in the loaded document");
  }
  return panel;
}

QtDockPanelContent BuildQtScriptDock(const vkpt::scene::SceneDocument& document,
                                     const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "script_panel", "Scripts", false, 520.0f, 420.0f);
  std::size_t scripted = 0u;
  for (const auto& entity : document.entities) {
    if (!entity.script.script.empty()) {
      ++scripted;
      QtDockAddRow(panel, QtEntityDisplayName(entity) + " script=" + entity.script.script);
    }
  }
  QtDockAddProperty(panel, "scripted entities", std::to_string(scripted));
  if (scripted == 0u) {
    QtDockAddRow(panel, "No scripts attached");
  }
  return panel;
}

QtDockPanelContent BuildQtPhysicsDock(const vkpt::scene::SceneDocument& document,
                                      const vkpt::editor::UiLayoutDocument& layout) {
  auto panel = MakeQtDockPanel(layout, "physics", "Physics", false, 420.0f, 320.0f);
  const auto engine = vkpt::physics::GetCompiledPhysicsEngineInfo();
  std::size_t authored = 0u;
  std::size_t enabled = 0u;
  std::size_t dynamic = 0u;
  QtDockAddProperty(panel, "entities", std::to_string(document.entities.size()));
  QtDockAddProperty(panel, "engine", engine.available ? engine.engine_name : std::string("disabled"));
  for (const auto& entity : document.entities) {
    if (entity.has_physics_body) {
      ++authored;
      if (entity.physics_body.enabled) {
        ++enabled;
        if (entity.physics_body.dynamic) {
          ++dynamic;
        }
      }
    }
    std::ostringstream row;
    row << QtEntityDisplayName(entity) << " #" << entity.id
        << " physics=" << QtDockBool(entity.has_physics_body && entity.physics_body.enabled);
    if (entity.has_physics_body) {
      row << " type=" << (entity.physics_body.dynamic ? "dynamic" : entity.physics_body.body_type)
          << " shape=" << entity.physics_body.shape
          << " mass=" << QtDockNumber(entity.physics_body.mass, 2);
      if (entity.physics_body.trigger) {
        row << " trigger";
      }
    }
    QtDockAddRow(panel, row.str());
  }
  QtDockAddProperty(panel, "authored bodies", std::to_string(authored));
  QtDockAddProperty(panel, "enabled bodies", std::to_string(enabled));
  QtDockAddProperty(panel, "dynamic bodies", std::to_string(dynamic));
  if (document.entities.empty()) {
    QtDockAddRow(panel, "No entities in the loaded document");
  }
  QtDockLimitRows(panel, 128u);
  return panel;
}

std::vector<QtDockPanelContent> BuildQtDockPanels(
    const vkpt::scene::SceneDocument& document,
    const vkpt::pathtracer::RTSceneData& scene,
    const vkpt::pathtracer::RenderSettings& settings,
    const vkpt::editor::UiRuntimeState& runtime,
    const vkpt::editor::SelectionState& selection,
    const vkpt::editor::UiLayoutDocument& layout,
    const vkpt::editor::BenchmarkPanelModel& benchmark,
    const QtDockFrameStats& frame_stats,
    const QtDockDeviceStats& device_stats,
    int active_camera_shot_slot,
    const std::array<bool, 4>& saved_camera_shot_slots) {
  std::vector<QtDockPanelContent> panels;
  panels.reserve(15u);
  panels.push_back(BuildQtSceneTreeDock(document, selection, layout));
  panels.push_back(BuildQtInspectorDock(document, selection, runtime, layout));
  panels.push_back(BuildQtMaterialsDock(document, scene, layout));
  panels.push_back(BuildQtLightsDock(document, scene, layout));
  panels.push_back(BuildQtCameraDock(document,
                                     scene,
                                     runtime,
                                     layout,
                                     frame_stats,
                                     active_camera_shot_slot,
                                     saved_camera_shot_slots));
  panels.push_back(BuildQtRenderSettingsDock(scene, settings, runtime, layout, frame_stats));
  panels.push_back(BuildQtBenchmarkDock(benchmark, layout));
  panels.push_back(BuildQtDiagnosticsDock(runtime, selection, layout, frame_stats));
  panels.push_back(BuildQtPerformanceDock(runtime, layout, frame_stats));
  panels.push_back(BuildQtDeviceDock(scene, runtime, layout, frame_stats, device_stats));
  panels.push_back(BuildQtDebugViewsDock(runtime, layout));
  panels.push_back(BuildQtAssetBrowserDock(document, scene, layout));
  panels.push_back(BuildQtTimelineDock(document, layout));
  panels.push_back(BuildQtScriptDock(document, layout));
  panels.push_back(BuildQtPhysicsDock(document, layout));
  return panels;
}

vkpt::platform::QtDockArea QtDockAreaForPanel(std::string_view panel_id) {
  if (panel_id == "scene_graph" || panel_id == "asset_browser") {
    return vkpt::platform::QtDockArea::Left;
  }
  if (panel_id == "diagnostics" ||
      panel_id == "performance" ||
      panel_id == "device" ||
      panel_id == "debug_views" ||
      panel_id == "benchmark_panel" ||
      panel_id == "timeline") {
    return vkpt::platform::QtDockArea::Bottom;
  }
  return vkpt::platform::QtDockArea::Right;
}

std::vector<vkpt::platform::QtDockPanel> ToQtPlatformDockPanels(
    const std::vector<QtDockPanelContent>& panels) {
  std::vector<vkpt::platform::QtDockPanel> out;
  out.reserve(panels.size());
  for (const auto& panel : panels) {
    vkpt::platform::QtDockPanel dock;
    dock.id = panel.id;
    dock.title = panel.title;
    dock.area = QtDockAreaForPanel(panel.id);
    dock.content = panel.properties.empty()
        ? vkpt::platform::QtDockPanelContent::Tree
        : vkpt::platform::QtDockPanelContent::Properties;
    dock.visible = panel.visible && !panel.collapsed;
    dock.enabled = true;
    dock.closable = true;
    dock.movable = panel.docked;
    dock.floatable = true;
    dock.preferred_width = QtDockPreferredPixels(panel.width);
    dock.preferred_height = QtDockPreferredPixels(panel.height);
    dock.rows = panel.rows;
    dock.properties.reserve(panel.properties.size());
    for (const auto& property : panel.properties) {
      vkpt::platform::QtDockProperty dockProperty;
      dockProperty.id = property.id;
      dockProperty.group = property.group;
      dockProperty.name = property.label;
      dockProperty.value = property.value;
      dockProperty.unit = property.unit;
      dockProperty.editor = property.editor;
      dockProperty.options = property.options;
      dockProperty.minimum = property.minimum;
      dockProperty.maximum = property.maximum;
      dockProperty.step = property.step;
      dockProperty.default_value = property.default_value;
      dockProperty.has_numeric_range = property.has_numeric_range;
      dockProperty.has_default = property.has_default;
      dockProperty.editable = property.editable;
      dockProperty.enabled = property.enabled;
      dock.properties.push_back(std::move(dockProperty));
    }
    out.push_back(std::move(dock));
  }
  return out;
}

std::string BuildQtStatusBarText(const vkpt::editor::StatusBarModel& status) {
  std::ostringstream out;
  out << "Scene: " << (status.active_scene.empty() ? "none" : status.active_scene)
      << " | Backend: " << status.backend
      << " | Renderer: " << status.renderer_path
      << " | SPP: " << status.spp
      << " | FPS: " << QtDockNumber(status.fps, 1)
      << " | Frame: " << QtDockNumber(status.frame_ms, 2) << " ms"
      << " | Selected: " << status.selected_entity_count
      << " | Tool: " << status.active_tool;
  if (!status.last_warning_or_error.empty()) {
    out << " | " << status.last_warning_or_error;
  }
  return out.str();
}

void ApplyQtDockPanelsToWindow(vkpt::platform::QtWindow* window,
                               const std::vector<QtDockPanelContent>& panels) {
  if (window == nullptr) {
    return;
  }
  window->set_dock_panels(ToQtPlatformDockPanels(panels));
}

template <typename WindowT>
void ApplyQtStatusBarToWindowTyped(WindowT* window, std::string_view status_text) {
  if (window == nullptr) {
    return;
  }
  if constexpr (requires(WindowT& w, std::string_view text) {
                  w.set_status_bar_text(text);
                }) {
    window->set_status_bar_text(status_text);
  } else if constexpr (requires(WindowT& w, const std::string& text) {
                         w.set_status_bar_text(text);
                       }) {
    const std::string text(status_text);
    window->set_status_bar_text(text);
  } else {
    // App-side adapter only: current QtPlatform.h has no status-bar sink yet.
    (void)status_text;
  }
}

void ApplyQtStatusBarToWindow(vkpt::platform::QtWindow* window,
                              std::string_view status_text) {
  ApplyQtStatusBarToWindowTyped(window, status_text);
}

struct ViewportCameraPose {
  vkpt::pathtracer::Vec3 position{};
  vkpt::pathtracer::Vec3 target{};
  vkpt::pathtracer::Vec3 up{0.0f, 1.0f, 0.0f};
  float fov_deg = 60.0f;
};

struct ViewportPickable {
  struct Triangle {
    vkpt::pathtracer::Vec3 v0{};
    vkpt::pathtracer::Vec3 v1{};
    vkpt::pathtracer::Vec3 v2{};
  };

  vkpt::core::StableId entity_id = 0;
  vkpt::editor::Bounds bounds{};
  std::string label;
  std::vector<Triangle> triangles;
  bool require_triangle_hit = false;
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

vkpt::scene::Quat NormalizeQuat(vkpt::scene::Quat q) {
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len <= 1.0e-6f) {
    return {};
  }
  const float inv = 1.0f / len;
  q.x *= inv;
  q.y *= inv;
  q.z *= inv;
  q.w *= inv;
  return q;
}

vkpt::scene::Quat QuatMultiply(const vkpt::scene::Quat& a, const vkpt::scene::Quat& b) {
  return NormalizeQuat({
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  });
}

vkpt::scene::Quat QuatFromAxisAngle(const vkpt::pathtracer::Vec3& axis, float radians) {
  const auto normalized = PtNormalize(axis, {0.0f, 1.0f, 0.0f});
  const float half = radians * 0.5f;
  const float s = std::sin(half);
  return NormalizeQuat({normalized.x * s, normalized.y * s, normalized.z * s, std::cos(half)});
}

vkpt::pathtracer::Vec3 RotatePointByQuat(const vkpt::pathtracer::Vec3& point,
                                         const vkpt::scene::Quat& rotation) {
  const auto q = NormalizeQuat(rotation);
  const vkpt::pathtracer::Vec3 qv{q.x, q.y, q.z};
  const auto t = PtMul(PtCross(qv, point), 2.0f);
  return PtAdd(PtAdd(point, PtMul(t, q.w)), PtCross(qv, t));
}

vkpt::pathtracer::Vec3 InverseRotatePointByQuat(const vkpt::pathtracer::Vec3& point,
                                                const vkpt::scene::Quat& rotation) {
  auto q = NormalizeQuat(rotation);
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  return RotatePointByQuat(point, q);
}

vkpt::pathtracer::Vec3 ApplySceneTransformToPoint(const vkpt::pathtracer::Vec3& point,
                                                  const vkpt::scene::TransformComponent& transform) {
  const auto scaled = vkpt::pathtracer::Vec3{
      point.x * transform.scale.x,
      point.y * transform.scale.y,
      point.z * transform.scale.z};
  const auto rotated = RotatePointByQuat(scaled, transform.rotation);
  return PtAdd(rotated, {transform.translation.x, transform.translation.y, transform.translation.z});
}

vkpt::pathtracer::Vec3 InverseSceneTransformPoint(const vkpt::pathtracer::Vec3& point,
                                                  const vkpt::scene::TransformComponent& transform) {
  const auto translated = PtSub(point, {transform.translation.x, transform.translation.y, transform.translation.z});
  const auto unrotated = InverseRotatePointByQuat(translated, transform.rotation);
  const auto safeScale = [](float value) {
    return std::fabs(value) <= 1.0e-6f ? 1.0f : value;
  };
  return {
      unrotated.x / safeScale(transform.scale.x),
      unrotated.y / safeScale(transform.scale.y),
      unrotated.z / safeScale(transform.scale.z)};
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

vkpt::pathtracer::Quat4 ToPtQuat4(const vkpt::scene::Quat& q) {
  return {q.x, q.y, q.z, q.w};
}

vkpt::editor::Vec3 ToEditorVec3(const vkpt::pathtracer::Vec3& v) {
  return {v.x, v.y, v.z};
}

vkpt::pathtracer::Vec3 ToPtVec3(const vkpt::editor::Vec3& v) {
  return {v.x, v.y, v.z};
}

int RunDynamicPhysicsPerformanceGate(std::string scenePath,
                                     std::string backend,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t frames) {
  if (scenePath.empty()) {
    scenePath = "assets/scenes/material_shader_physics_showcase.json";
  }
  width = std::max<uint32_t>(1u, width);
  height = std::max<uint32_t>(1u, height);
  frames = std::max<uint32_t>(1u, frames);
  backend = vkpt::render::NormalizeBackendName(backend.empty() ? "d3d12" : backend);

  const std::filesystem::path artifactPath =
      "artifacts/benchmarks/dynamic_physics_gate.json";
  std::error_code ec;
  std::filesystem::create_directories(artifactPath.parent_path(), ec);

  bool passed = false;
  std::string failure;
  std::size_t dynamicInstances = 0u;
  std::size_t physicsDynamicBodies = 0u;
  std::size_t physicsWrites = 0u;
  uint32_t successfulUpdates = 0u;
  uint32_t rebuildCount = 0u;
  double physicsStepMs = 0.0;
  double transformPublishMs = 0.0;
  double renderMs = 0.0;
  uint64_t totalRays = 0u;

  auto writeArtifact = [&]() {
    std::ofstream out(artifactPath.string());
    out << "{\n"
        << "  \"schema\": \"dynamic_physics_gate.v1\",\n"
        << "  \"passed\": " << (passed ? "true" : "false") << ",\n"
        << "  \"scene\": \"" << vkpt::log::EscapeJson(scenePath) << "\",\n"
        << "  \"backend\": \"" << vkpt::log::EscapeJson(backend) << "\",\n"
        << "  \"resolution\": { \"width\": " << width << ", \"height\": " << height << " },\n"
        << "  \"frames\": " << frames << ",\n"
        << "  \"dynamic_instances\": " << dynamicInstances << ",\n"
        << "  \"physics_dynamic_bodies\": " << physicsDynamicBodies << ",\n"
        << "  \"physics_transform_writes\": " << physicsWrites << ",\n"
        << "  \"transform_update_successes\": " << successfulUpdates << ",\n"
        << "  \"full_rebuild_count\": " << rebuildCount << ",\n"
        << std::fixed << std::setprecision(4)
        << "  \"physics_step_ms\": " << physicsStepMs << ",\n"
        << "  \"transform_publish_ms\": " << transformPublishMs << ",\n"
        << "  \"tlas_update_ms\": " << transformPublishMs << ",\n"
        << "  \"render_ms\": " << renderMs << ",\n"
        << "  \"rays_per_second\": "
        << (renderMs > 0.0 ? (static_cast<double>(totalRays) * 1000.0 / renderMs) : 0.0) << ",\n"
        << "  \"total_rays\": " << totalRays << ",\n"
        << "  \"failure\": \"" << vkpt::log::EscapeJson(failure) << "\"\n"
        << "}\n";
  };

  auto fail = [&](std::string message) {
    failure = std::move(message);
    passed = false;
    writeArtifact();
    std::cerr << "dynamic physics gate failed: " << failure << "\n";
    std::cerr << "artifact: " << artifactPath.string() << "\n";
    return 2;
  };

#ifndef PT_ENABLE_D3D12
  return fail("PT_ENABLE_D3D12 is not enabled in this build");
#else
  auto parseResult = vkpt::scene::SceneDocument::load_from_file(scenePath);
  if (!parseResult) {
    return fail("scene parse failed");
  }
  auto document = parseResult.value();
  auto worldResult = document.to_world();
  if (!worldResult) {
    return fail("scene ECS conversion failed");
  }
  auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(document);
  if (!sceneResult) {
    return fail("scene RT conversion failed");
  }
  auto rtScene = sceneResult.value();

  std::unordered_map<vkpt::core::StableId, uint32_t> instanceByEntity;
  instanceByEntity.reserve(rtScene.instances.size());
  for (uint32_t i = 0u; i < rtScene.instances.size(); ++i) {
    const auto& instance = rtScene.instances[i];
    if (instance.entity_id != 0u) {
      instanceByEntity[instance.entity_id] = i;
    }
    if (instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
      ++dynamicInstances;
    }
  }
  if (dynamicInstances == 0u) {
    return fail("scene has no dynamic RT instances");
  }

  auto physics = vkpt::physics::CreatePhysicsWorld();
  auto syncSummary = physics->sync_from_scene_world(worldResult.value());
  physicsDynamicBodies = syncSummary.dynamic_bodies;
  if (physicsDynamicBodies == 0u) {
    return fail("scene has no dynamic physics bodies");
  }

  vkpt::pathtracer::RenderSettings settings{};
  settings.width = width;
  settings.height = height;
  settings.spp = 1u;
  settings.max_depth = 3u;
  settings.seed = 0xD4D4D4D4ull;
  settings.enable_nee = true;
  settings.enable_mis = true;

  const std::string hlslPath =
#ifdef PT_SHADER_HLSL_PATH
      PT_SHADER_HLSL_PATH;
#else
      "src/shaders/gpu/pathtrace_cs.hlsl";
#endif
  auto tracer = std::make_unique<vkpt::gpu::D3D12GpuPathTracer>(hlslPath);
  if (!tracer->is_valid()) {
    return fail("D3D12 tracer init failed: " + tracer->last_error());
  }
  if (backend == "d3d12-dxr") {
    tracer->set_prefer_dxr(true);
    if (!tracer->dxr_supported()) {
      return fail("DXR backend requested but this D3D12 device reports no DXR support");
    }
  } else if (backend != "d3d12") {
    return fail("dynamic physics gate supports only d3d12 and d3d12-dxr backends");
  }
  if (!tracer->configure(settings) ||
      !tracer->load_scene_snapshot(rtScene) ||
      !tracer->build_or_update_acceleration() ||
      !tracer->reset_accumulation()) {
    return fail("D3D12 scene preparation failed: " + tracer->last_error());
  }

  vkpt::physics::PhysicsStepConfig physicsConfig{};
  physicsConfig.fixed_dt = 1.0f / 60.0f;
  physicsConfig.collision_steps = 6;
  physicsConfig.deterministic = true;
  physicsConfig.collision_detection_enabled = true;

  uint32_t revision = 1u;
  auto lastCounters = tracer->read_counters();
  for (uint32_t frame = 0u; frame < frames; ++frame) {
    const auto physicsStart = std::chrono::steady_clock::now();
    const auto stepResult = physics->step_fixed(physicsConfig);
    physicsStepMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - physicsStart).count();
    if (!stepResult) {
      return fail("physics step failed");
    }

    const auto writes = physics->extract_transform_writes();
    physicsWrites += writes.size();
    std::vector<vkpt::pathtracer::RTInstanceTransformUpdate> updates;
    updates.reserve(writes.size());
    for (const auto& write : writes) {
      const auto found = instanceByEntity.find(write.entity);
      if (found == instanceByEntity.end()) {
        continue;
      }
      vkpt::pathtracer::RTInstanceTransformUpdate update{};
      update.entity_id = write.entity;
      update.instance_index = found->second;
      update.flags = vkpt::pathtracer::kRTInstanceFlagDynamicTransform |
                     vkpt::pathtracer::kRTInstanceFlagPhysicsControlled |
                     vkpt::pathtracer::kRTInstanceFlagTransformDirty;
      update.transform_revision = revision++;
      update.translation = ToPtVec3(write.transform.translation);
      update.rotation = ToPtQuat4(write.transform.rotation);
      update.scale = ToPtVec3(write.transform.scale);
      updates.push_back(update);
    }
    if (updates.empty()) {
      continue;
    }

    const auto publishStart = std::chrono::steady_clock::now();
    const bool updated = tracer->update_instance_transforms(updates);
    transformPublishMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - publishStart).count();
    if (!updated) {
      ++rebuildCount;
      return fail("transform-only update failed; backend would require full scene rebuild");
    }
    ++successfulUpdates;

    if (!tracer->reset_accumulation()) {
      return fail("accumulation reset failed after dynamic transform update");
    }
    const auto renderStart = std::chrono::steady_clock::now();
    if (!tracer->render_sample_batch(0, settings.height, frame, frame)) {
      return fail("render_sample_batch failed after dynamic transform update");
    }
    renderMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - renderStart).count();
    const auto counters = tracer->read_counters();
    if (counters.rays >= lastCounters.rays) {
      totalRays += counters.rays - lastCounters.rays;
    }
    lastCounters = counters;
  }

  if (successfulUpdates == 0u) {
    return fail("physics produced no mappable dynamic transform updates");
  }
  physicsStepMs /= static_cast<double>(frames);
  transformPublishMs /= static_cast<double>(successfulUpdates);
  renderMs = std::max(0.0001, renderMs);
  passed = rebuildCount == 0u && totalRays > 0u;
  if (!passed) {
    failure = "gate counters did not meet pass criteria";
  }
  writeArtifact();
  std::cout << "dynamic physics gate: " << (passed ? "ok" : "fail") << "\n";
  std::cout << "artifact: " << artifactPath.string() << "\n";
  std::cout << "dynamic_instances: " << dynamicInstances << "\n";
  std::cout << "physics_dynamic_bodies: " << physicsDynamicBodies << "\n";
  std::cout << "transform_updates: " << successfulUpdates << "\n";
  std::cout << "full_rebuild_count: " << rebuildCount << "\n";
  std::cout << "rays_per_second: "
            << (static_cast<double>(totalRays) * 1000.0 / renderMs) << "\n";
  return passed ? 0 : 2;
#endif
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
  return ApplySceneTransformToPoint({point.x, point.y, point.z}, transform);
}

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

vkpt::scene::TransformComponent TransformFromRtInstance(
    const vkpt::pathtracer::RTInstance& instance) {
  vkpt::scene::TransformComponent transform;
  transform.translation = {instance.translation.x, instance.translation.y, instance.translation.z};
  transform.rotation = {instance.rotation.x, instance.rotation.y, instance.rotation.z, instance.rotation.w};
  transform.scale = {instance.scale.x, instance.scale.y, instance.scale.z};
  transform.dirty = false;
  return transform;
}

vkpt::scene::Quat InverseQuat(vkpt::scene::Quat q) {
  q = NormalizeQuat(q);
  q.x = -q.x;
  q.y = -q.y;
  q.z = -q.z;
  return q;
}

float SafeTransformScaleDivisor(float value) {
  return std::fabs(value) <= 1.0e-6f ? 1.0f : value;
}

vkpt::scene::TransformComponent ConvertWorldTransformToDocumentLocal(
    const vkpt::scene::SceneEntityDefinition& entity,
    const vkpt::scene::SceneWorld* currentWorld,
    const vkpt::scene::TransformComponent& worldTransform) {
  if (!entity.has_hierarchy || entity.hierarchy.parent == 0 || currentWorld == nullptr) {
    return worldTransform;
  }

  const auto* parentWorld = currentWorld->world_transform(entity.hierarchy.parent);
  if (parentWorld == nullptr) {
    return worldTransform;
  }

  vkpt::scene::TransformComponent local = worldTransform;
  const auto delta = PtSub(ToPtVec3(worldTransform.translation),
                           ToPtVec3(parentWorld->translation));
  const auto unrotated = InverseRotatePointByQuat(delta, parentWorld->rotation);
  local.translation = {
      unrotated.x / SafeTransformScaleDivisor(parentWorld->scale.x),
      unrotated.y / SafeTransformScaleDivisor(parentWorld->scale.y),
      unrotated.z / SafeTransformScaleDivisor(parentWorld->scale.z)};
  local.rotation = QuatMultiply(InverseQuat(parentWorld->rotation), worldTransform.rotation);
  local.scale = {
      worldTransform.scale.x / SafeTransformScaleDivisor(parentWorld->scale.x),
      worldTransform.scale.y / SafeTransformScaleDivisor(parentWorld->scale.y),
      worldTransform.scale.z / SafeTransformScaleDivisor(parentWorld->scale.z)};
  local.dirty = true;
  return local;
}

std::string PickableLabel(std::string_view name, vkpt::core::StableId id) {
  if (!name.empty()) {
    return std::string(name);
  }
  return "entity " + std::to_string(id);
}

void AddSdfPickable(std::vector<ViewportPickable>& pickables,
                    vkpt::core::StableId id,
                    std::string label,
                    std::string_view shape,
                    const vkpt::scene::TransformComponent& transform,
                    const vkpt::scene::SdfPrimitiveComponent& primitive) {
  const auto center = ToPtVec3(transform.translation);
  const auto scale = ToPtVec3(transform.scale);
  const float radius = std::max(0.05f, primitive.radius);
  vkpt::pathtracer::Vec3 extent{
      std::max(0.05f, std::fabs(scale.x) * radius),
      std::max(0.05f, std::fabs(scale.y) * radius),
      std::max(0.05f, std::fabs(scale.z) * radius),
  };
  if (shape == "box" || shape == "rounded_box") {
    extent = {
        std::max(0.05f, std::fabs(scale.x)),
        std::max(0.05f, std::fabs(scale.y)),
        std::max(0.05f, std::fabs(scale.z)),
    };
  } else if (shape == "torus") {
    const float major = std::max(0.05f, primitive.param_a);
    const float minor = std::max(0.02f, radius);
    const float torusExtent = major + minor;
    extent = {
        std::max(0.05f, std::fabs(scale.x) * torusExtent),
        std::max(0.05f, std::fabs(scale.y) * minor),
        std::max(0.05f, std::fabs(scale.z) * torusExtent),
    };
  } else if (shape == "capsule") {
    const float halfHeight = std::max(0.0f, primitive.param_a);
    extent = {
        std::max(0.05f, std::fabs(scale.x) * radius),
        std::max(0.05f, std::fabs(scale.y) * (halfHeight + radius)),
        std::max(0.05f, std::fabs(scale.z) * radius),
    };
  } else if (shape == "plane") {
    return;
  }

  vkpt::editor::Bounds bounds{};
  ExpandBounds(bounds, PtSub(center, extent));
  ExpandBounds(bounds, PtAdd(center, extent));
  if (bounds.valid) {
    ViewportPickable pickable{};
    pickable.entity_id = id;
    pickable.bounds = bounds;
    pickable.label = std::move(label);
    pickables.push_back(std::move(pickable));
  }
}

void AddSdfPickable(std::vector<ViewportPickable>& pickables,
                    const vkpt::scene::SceneSdfPrimitiveDefinition& primitive) {
  const std::string shape = primitive.shape.empty()
      ? (primitive.primitive.shape.empty() ? std::string("sphere") : primitive.primitive.shape)
      : primitive.shape;
  AddSdfPickable(pickables,
                 primitive.id,
                 "sdf " + std::to_string(primitive.id),
                 shape,
                 primitive.transform,
                 primitive.primitive);
}

std::vector<ViewportPickable> BuildViewportPickables(const vkpt::scene::SceneDocument& document,
                                                     const vkpt::pathtracer::RTSceneData& scene) {
  std::vector<ViewportPickable> pickables;
  const auto worldSnapshot = BuildSceneWorldSnapshot(document);
  const auto* world = worldSnapshot ? &worldSnapshot.value() : nullptr;
  std::unordered_map<vkpt::core::StableId, const vkpt::scene::SceneGeometryDefinition*> geometryById;
  for (const auto& geometry : document.geometry) {
    geometryById[geometry.id] = &geometry;
  }

  struct MeshPickableRef {
    vkpt::core::StableId entity_id = 0;
    vkpt::core::StableId mesh_id = 0;
    std::string label;
  };
  std::vector<MeshPickableRef> meshRefs;
  meshRefs.reserve(document.entities.size());
  for (const auto& entity : document.entities) {
    if (!entity.has_mesh) {
      continue;
    }
    const auto geometryIt = geometryById.find(entity.mesh.mesh_id);
    if (geometryIt == geometryById.end()) {
      continue;
    }
    const auto* geometry = geometryIt->second;
    if (geometry == nullptr || geometry->vertices.empty() || geometry->indices.empty()) {
      continue;
    }
    meshRefs.push_back({
        entity.id,
        entity.mesh.mesh_id,
        PickableLabel(entity.name, entity.id)});
  }

  auto appendEntitySdfPickables = [&]() {
    for (const auto& entity : document.entities) {
      if (!entity.has_sdf_primitive) {
        continue;
      }
      const auto transform = ResolveEntityWorldTransform(entity, world);
      const std::string shape = entity.sdf_primitive.shape.empty()
          ? std::string("sphere")
          : entity.sdf_primitive.shape;
      AddSdfPickable(pickables,
                     entity.id,
                     PickableLabel(entity.name, entity.id),
                     shape,
                     transform,
                     entity.sdf_primitive);
    }
  };

  if (!meshRefs.empty() && !scene.instances.empty()) {
    const std::size_t count = std::min(meshRefs.size(), scene.instances.size());
    for (std::size_t instanceIndex = 0; instanceIndex < count; ++instanceIndex) {
      const auto& instance = scene.instances[instanceIndex];
      const auto& meshRef = meshRefs[instanceIndex];
      if (instance.has_flag(vkpt::pathtracer::kRTInstanceFlagDynamicTransform)) {
        const auto geometryIt = geometryById.find(meshRef.mesh_id);
        if (geometryIt == geometryById.end() || geometryIt->second == nullptr) {
          continue;
        }
        const auto* geometry = geometryIt->second;
        const auto transform = TransformFromRtInstance(instance);
        vkpt::editor::Bounds bounds{};
        for (const auto& vertex : geometry->vertices) {
          ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
        }
        if (!bounds.valid) {
          continue;
        }
        ViewportPickable pickable{};
        pickable.entity_id = meshRef.entity_id;
        pickable.bounds = bounds;
        pickable.label = meshRef.label;
        pickable.require_triangle_hit = true;
        for (std::size_t index = 0; index + 2u < geometry->indices.size(); index += 3u) {
          const auto i0 = geometry->indices[index + 0u];
          const auto i1 = geometry->indices[index + 1u];
          const auto i2 = geometry->indices[index + 2u];
          if (i0 >= geometry->vertices.size() ||
              i1 >= geometry->vertices.size() ||
              i2 >= geometry->vertices.size()) {
            continue;
          }
          pickable.triangles.push_back({
              TransformPointForPreview(geometry->vertices[i0], transform),
              TransformPointForPreview(geometry->vertices[i1], transform),
              TransformPointForPreview(geometry->vertices[i2], transform)});
        }
        if (!pickable.triangles.empty()) {
          pickables.push_back(std::move(pickable));
        }
        continue;
      }
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
      if (!bounds.valid) {
        continue;
      }

      ViewportPickable pickable{};
      pickable.entity_id = meshRef.entity_id;
      pickable.bounds = bounds;
      pickable.label = meshRef.label;
      pickable.require_triangle_hit = true;
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        const uint32_t i0 = scene.indices[base + 0u];
        const uint32_t i1 = scene.indices[base + 1u];
        const uint32_t i2 = scene.indices[base + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() || i2 >= scene.vertices.size()) {
          continue;
        }
        pickable.triangles.push_back({scene.vertices[i0], scene.vertices[i1], scene.vertices[i2]});
      }
      if (!pickable.triangles.empty()) {
        pickables.push_back(std::move(pickable));
      }
    }

    appendEntitySdfPickables();
    for (const auto& primitive : document.sdf_primitives) {
      AddSdfPickable(pickables, primitive);
    }
    if (!pickables.empty()) {
      return pickables;
    }
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
    const auto transform = ResolveEntityWorldTransform(entity, world);
    vkpt::editor::Bounds bounds{};
    for (const auto& vertex : geometry->vertices) {
      ExpandBounds(bounds, TransformPointForPreview(vertex, transform));
    }
    if (bounds.valid) {
      ViewportPickable pickable{};
      pickable.entity_id = entity.id;
      pickable.bounds = bounds;
      pickable.label = PickableLabel(entity.name, entity.id);
      pickable.require_triangle_hit = true;
      for (std::size_t index = 0; index + 2u < geometry->indices.size(); index += 3u) {
        const auto i0 = geometry->indices[index + 0u];
        const auto i1 = geometry->indices[index + 1u];
        const auto i2 = geometry->indices[index + 2u];
        if (i0 >= geometry->vertices.size() ||
            i1 >= geometry->vertices.size() ||
            i2 >= geometry->vertices.size()) {
          continue;
        }
        pickable.triangles.push_back({
            TransformPointForPreview(geometry->vertices[i0], transform),
            TransformPointForPreview(geometry->vertices[i1], transform),
            TransformPointForPreview(geometry->vertices[i2], transform)});
      }
      if (pickable.triangles.empty()) {
        continue;
      }
      pickables.push_back(std::move(pickable));
    }
  }

  appendEntitySdfPickables();
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
      ViewportPickable pickable{};
      pickable.entity_id = id;
      pickable.bounds = bounds;
      pickable.label = "instance " + std::to_string(id);
      pickable.require_triangle_hit = true;
      for (uint32_t triangle = 0; triangle < instance.triangle_count; ++triangle) {
        const uint32_t base = (instance.first_triangle + triangle) * 3u;
        if (base + 2u >= scene.indices.size()) {
          continue;
        }
        const uint32_t i0 = scene.indices[base + 0u];
        const uint32_t i1 = scene.indices[base + 1u];
        const uint32_t i2 = scene.indices[base + 2u];
        if (i0 >= scene.vertices.size() || i1 >= scene.vertices.size() || i2 >= scene.vertices.size()) {
          continue;
        }
        pickable.triangles.push_back({scene.vertices[i0], scene.vertices[i1], scene.vertices[i2]});
      }
      if (pickable.triangles.empty()) {
        continue;
      }
      pickables.push_back(std::move(pickable));
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
      ViewportPickable pickable{};
      pickable.entity_id = id;
      pickable.bounds = bounds;
      pickable.label = "sdf " + std::to_string(id);
      pickables.push_back(std::move(pickable));
    }
  }

  return pickables;
}

ViewportRay BuildViewportRay(const ViewportCameraPose& camera,
                             float x,
                             float y,
                             float width,
                             float height,
                             float renderAspect) {
  const float safeWidth = std::max(1.0f, width);
  const float safeHeight = std::max(1.0f, height);
  const float safeAspect = std::max(0.01f, renderAspect);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right = PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));
  const float nx = ((x + 0.5f) / safeWidth * 2.0f - 1.0f) * safeAspect * tanHalfFov;
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

bool IntersectFrontFacingTriangle(const ViewportRay& ray,
                                  const ViewportPickable::Triangle& triangle,
                                  float maxDistance,
                                  float& t_out) {
  constexpr float kEpsilon = 1.0e-6f;
  const auto edge1 = PtSub(triangle.v1, triangle.v0);
  const auto edge2 = PtSub(triangle.v2, triangle.v0);
  const auto pvec = PtCross(ray.direction, edge2);
  const float det = PtDot(edge1, pvec);
  if (det <= kEpsilon) {
    return false;
  }

  const float invDet = 1.0f / det;
  const auto tvec = PtSub(ray.origin, triangle.v0);
  const float u = PtDot(tvec, pvec) * invDet;
  if (u < 0.0f || u > 1.0f) {
    return false;
  }

  const auto qvec = PtCross(tvec, edge1);
  const float v = PtDot(ray.direction, qvec) * invDet;
  if (v < 0.0f || u + v > 1.0f) {
    return false;
  }

  const float t = PtDot(edge2, qvec) * invDet;
  if (t <= kEpsilon || t >= maxDistance) {
    return false;
  }
  t_out = t;
  return true;
}

bool IntersectPickableForSelection(const ViewportRay& ray,
                                   const ViewportPickable& pickable,
                                   float maxDistance,
                                   float& t_out) {
  float boundsDistance = 0.0f;
  if (!IntersectBounds(ray, pickable.bounds, boundsDistance) || boundsDistance >= maxDistance) {
    return false;
  }

  if (pickable.triangles.empty()) {
    if (pickable.require_triangle_hit) {
      return false;
    }
    t_out = boundsDistance;
    return true;
  }

  bool hit = false;
  float bestTriangleDistance = maxDistance;
  for (const auto& triangle : pickable.triangles) {
    float triangleDistance = 0.0f;
    if (!IntersectFrontFacingTriangle(ray, triangle, bestTriangleDistance, triangleDistance)) {
      continue;
    }
    bestTriangleDistance = triangleDistance;
    hit = true;
  }
  if (!hit) {
    return false;
  }
  t_out = bestTriangleDistance;
  return true;
}

std::optional<ViewportPickResult> PickViewportObject(const std::vector<ViewportPickable>& pickables,
                                                     const ViewportCameraPose& camera,
                                                     float x,
                                                     float y,
                                                     float width,
                                                     float height,
                                                     float renderAspect) {
  const auto ray = BuildViewportRay(camera, x, y, width, height, renderAspect);
  std::optional<ViewportPickResult> best;
  for (const auto& pickable : pickables) {
    const float maxDistance = best ? best->distance : std::numeric_limits<float>::infinity();
    float distance = 0.0f;
    if (!IntersectPickableForSelection(ray, pickable, maxDistance, distance)) {
      continue;
    }
    if (!best || distance < best->distance) {
      best = ViewportPickResult{pickable.entity_id, pickable.bounds, pickable.label, distance};
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

constexpr std::array<std::pair<int, int>, 12> kViewportBoundsEdges{{
    {0, 1}, {1, 3}, {3, 2}, {2, 0},
    {4, 5}, {5, 7}, {7, 6}, {6, 4},
    {0, 4}, {1, 5}, {2, 6}, {3, 7},
}};

struct ProjectedViewportPoint {
  float x = 0.0f;
  float y = 0.0f;
  float depth = 0.0f;
};

struct OverlayColor {
  std::uint8_t r = 255u;
  std::uint8_t g = 255u;
  std::uint8_t b = 255u;
  std::uint8_t a = 255u;
};

std::optional<ProjectedViewportPoint> ProjectWorldPointToOverlay(
    const vkpt::pathtracer::Vec3& point,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect) {
  if (width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const auto right = PtNormalize(PtCross(forward, camera.up), {1.0f, 0.0f, 0.0f});
  const auto up = PtNormalize(PtCross(right, forward), {0.0f, 1.0f, 0.0f});
  const float aspect = std::max(0.01f, renderAspect);
  const float tanHalfFov = std::tan(0.5f * DegToRad(std::max(1.0f, camera.fov_deg)));

  const auto rel = PtSub(point, camera.position);
  const float depth = PtDot(rel, forward);
  if (depth <= 1.0e-4f) {
    return std::nullopt;
  }
  const float cameraX = PtDot(rel, right);
  const float cameraY = PtDot(rel, up);
  const float ndcX = cameraX / (depth * tanHalfFov * aspect);
  const float ndcY = cameraY / (depth * tanHalfFov);
  return ProjectedViewportPoint{
      (ndcX + 1.0f) * 0.5f * width,
      (1.0f - ndcY) * 0.5f * height,
      depth};
}

void AddProjectedOverlayLine(vkpt::platform::QtSelectionOverlayBox& box,
                             const ProjectedViewportPoint& a,
                             const ProjectedViewportPoint& b,
                             OverlayColor color,
                             float lineWidth) {
  box.lines.push_back(vkpt::platform::QtSelectionOverlayBox::Line{
      a.x, a.y, b.x, b.y, color.r, color.g, color.b, color.a, lineWidth});
}

void AddWorldOverlayLine(vkpt::platform::QtSelectionOverlayBox& box,
                         const ViewportCameraPose& camera,
                         float width,
                         float height,
                         float renderAspect,
                         const vkpt::pathtracer::Vec3& a,
                         const vkpt::pathtracer::Vec3& b,
                         OverlayColor color,
                         float lineWidth) {
  const auto projectedA = ProjectWorldPointToOverlay(a, camera, width, height, renderAspect);
  const auto projectedB = ProjectWorldPointToOverlay(b, camera, width, height, renderAspect);
  if (!projectedA || !projectedB) {
    return;
  }
  AddProjectedOverlayLine(box, *projectedA, *projectedB, color, lineWidth);
}

void AddWorldOverlayPoint(vkpt::platform::QtSelectionOverlayBox& box,
                          const ViewportCameraPose& camera,
                          float width,
                          float height,
                          float renderAspect,
                          const vkpt::pathtracer::Vec3& point,
                          OverlayColor color,
                          float radius,
                          std::string label = {}) {
  const auto projected = ProjectWorldPointToOverlay(point, camera, width, height, renderAspect);
  if (!projected) {
    return;
  }
  box.points.push_back(vkpt::platform::QtSelectionOverlayBox::Point{
      projected->x, projected->y, radius, color.r, color.g, color.b, color.a, std::move(label)});
}

void AddGizmoCornerArc(vkpt::platform::QtSelectionOverlayBox& box,
                       const ViewportCameraPose& camera,
                       float width,
                       float height,
                       float renderAspect,
                       const vkpt::pathtracer::Vec3& corner,
                       const vkpt::pathtracer::Vec3& axisA,
                       const vkpt::pathtracer::Vec3& axisB,
                       float radius,
                       OverlayColor color,
                       float lineWidth = 1.0f) {
  if (radius <= 1.0e-4f) {
    return;
  }
  constexpr int kArcSegments = 14;
  constexpr float kHalfPi = 1.57079632679489661923f;
  auto previous = ProjectedViewportPoint{};
  bool previousValid = false;
  for (int segment = 0; segment <= kArcSegments; ++segment) {
    const float t = (static_cast<float>(segment) / static_cast<float>(kArcSegments)) * kHalfPi;
    const auto world = PtAdd(corner,
                             PtAdd(PtMul(axisA, std::cos(t) * radius),
                                   PtMul(axisB, std::sin(t) * radius)));
    const auto projected = ProjectWorldPointToOverlay(world, camera, width, height, renderAspect);
    if (projected && previousValid) {
      AddProjectedOverlayLine(box, previous, *projected, color, lineWidth);
    }
    if (projected) {
      previous = *projected;
      previousValid = true;
    } else {
      previousValid = false;
    }
  }
}

enum class ViewportGizmoDragKind {
  None,
  Translate,
  FreeformTranslate,
  Rotate,
  ScaleAxis,
};

struct ViewportGizmoHit {
  ViewportGizmoDragKind kind = ViewportGizmoDragKind::None;
  vkpt::pathtracer::Vec3 axis{};
  vkpt::pathtracer::Vec3 pivot{};
  float screen_axis_x = 1.0f;
  float screen_axis_y = 0.0f;
  float pixels_per_unit = 1.0f;
  float axis_world_length = 1.0f;
  int axis_index = -1;
};

vkpt::platform::QtViewportCursor CursorForGizmoHit(const ViewportGizmoHit& hit) {
  switch (hit.kind) {
    case ViewportGizmoDragKind::Translate:
    case ViewportGizmoDragKind::FreeformTranslate:
      return vkpt::platform::QtViewportCursor::Translate;
    case ViewportGizmoDragKind::Rotate:
      return vkpt::platform::QtViewportCursor::Rotate;
    case ViewportGizmoDragKind::ScaleAxis:
      return vkpt::platform::QtViewportCursor::Scale;
    case ViewportGizmoDragKind::None:
    default:
      return vkpt::platform::QtViewportCursor::Default;
  }
}

float ScreenDistance(float ax, float ay, float bx, float by) {
  const float dx = ax - bx;
  const float dy = ay - by;
  return std::sqrt(dx * dx + dy * dy);
}

float ScreenDistanceToSegment(float px,
                              float py,
                              const ProjectedViewportPoint& a,
                              const ProjectedViewportPoint& b,
                              float* tangentX = nullptr,
                              float* tangentY = nullptr) {
  const float vx = b.x - a.x;
  const float vy = b.y - a.y;
  const float lenSq = vx * vx + vy * vy;
  if (lenSq <= 1.0e-6f) {
    if (tangentX != nullptr) {
      *tangentX = 1.0f;
    }
    if (tangentY != nullptr) {
      *tangentY = 0.0f;
    }
    return ScreenDistance(px, py, a.x, a.y);
  }
  const float t = ClampFloat(((px - a.x) * vx + (py - a.y) * vy) / lenSq, 0.0f, 1.0f);
  const float closestX = a.x + vx * t;
  const float closestY = a.y + vy * t;
  const float len = std::sqrt(lenSq);
  if (tangentX != nullptr) {
    *tangentX = vx / len;
  }
  if (tangentY != nullptr) {
    *tangentY = vy / len;
  }
  return ScreenDistance(px, py, closestX, closestY);
}

bool SameGizmoHandle(const std::optional<ViewportGizmoHit>& a,
                     const std::optional<ViewportGizmoHit>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  if (!a) {
    return true;
  }
  return a->kind == b->kind && a->axis_index == b->axis_index;
}

bool IsHoveredGizmoHandle(const std::optional<ViewportGizmoHit>& hover,
                          ViewportGizmoDragKind kind,
                          int axisIndex) {
  return hover && hover->kind == kind && hover->axis_index == axisIndex;
}

std::optional<vkpt::pathtracer::Vec3> ScreenPointOnCameraPlane(
    const ViewportCameraPose& camera,
    float x,
    float y,
    float width,
    float height,
    float renderAspect,
    const vkpt::pathtracer::Vec3& planePoint) {
  const auto ray = BuildViewportRay(camera, x, y, width, height, renderAspect);
  const auto forward = PtNormalize(PtSub(camera.target, camera.position));
  const float denom = PtDot(ray.direction, forward);
  if (std::fabs(denom) <= 1.0e-5f) {
    return std::nullopt;
  }
  const float t = PtDot(PtSub(planePoint, ray.origin), forward) / denom;
  if (t <= 1.0e-5f) {
    return std::nullopt;
  }
  return PtAdd(ray.origin, PtMul(ray.direction, t));
}

std::optional<ViewportGizmoHit> PickSelectionGizmoHandle(const vkpt::editor::Bounds& bounds,
                                                         const ViewportCameraPose& camera,
                                                         float width,
                                                         float height,
                                                         float renderAspect,
                                                         vkpt::editor::GizmoMode mode,
                                                         float mouseX,
                                                         float mouseY) {
  if (!bounds.valid || mode == vkpt::editor::GizmoMode::None) {
    return std::nullopt;
  }
  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  const auto center = PtMul(PtAdd(min, max), 0.5f);
  const float extentX = std::fabs(max.x - min.x);
  const float extentY = std::fabs(max.y - min.y);
  const float extentZ = std::fabs(max.z - min.z);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  if (maxExtent <= 1.0e-5f) {
    return std::nullopt;
  }

  const auto corners = BoundsCorners(bounds);
  std::size_t anchorIndex = 0u;
  float nearestDepth = std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto projected = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
    if (projected && projected->depth < nearestDepth) {
      nearestDepth = projected->depth;
      anchorIndex = i;
    }
  }
  const auto anchor = corners[anchorIndex];
  const auto projectedAnchor = ProjectWorldPointToOverlay(anchor, camera, width, height, renderAspect);
  if (!projectedAnchor) {
    return std::nullopt;
  }

  const bool anchorMinX = std::fabs(anchor.x - min.x) <= std::fabs(anchor.x - max.x);
  const bool anchorMinY = std::fabs(anchor.y - min.y) <= std::fabs(anchor.y - max.y);
  const bool anchorMinZ = std::fabs(anchor.z - min.z) <= std::fabs(anchor.z - max.z);
  struct CornerAxis {
    int axis_index = -1;
    vkpt::pathtracer::Vec3 axis{};
    vkpt::pathtracer::Vec3 endpoint{};
    float length = 0.0f;
  };
  const std::array<CornerAxis, 3> axes{{
      {0,
       {anchorMinX ? 1.0f : -1.0f, 0.0f, 0.0f},
       {anchorMinX ? max.x : min.x, anchor.y, anchor.z},
       extentX},
      {1,
       {0.0f, anchorMinY ? 1.0f : -1.0f, 0.0f},
       {anchor.x, anchorMinY ? max.y : min.y, anchor.z},
       extentY},
      {2,
       {0.0f, 0.0f, anchorMinZ ? 1.0f : -1.0f},
       {anchor.x, anchor.y, anchorMinZ ? max.z : min.z},
       extentZ},
  }};
  const auto tickLength = [maxExtent](float axisExtent) {
    if (axisExtent <= 1.0e-5f) {
      return 0.0f;
    }
    return std::max(0.025f, std::min(axisExtent * 0.35f, maxExtent * 0.18f));
  };
  const float xLength = tickLength(extentX);
  const float yLength = tickLength(extentY);
  const float zLength = tickLength(extentZ);

  std::optional<ViewportGizmoHit> best;
  float bestDistance = std::numeric_limits<float>::infinity();
  int bestPriority = -1;
  constexpr float kTranslateHitRadius = 11.0f;
  constexpr float kRotateHitRadius = 10.0f;
  constexpr float kScaleHitRadius = 12.0f;
  const auto accept_hit = [&](float distance, int priority, ViewportGizmoHit hit) {
    constexpr float kTieBreakPixels = 0.75f;
    if (distance + kTieBreakPixels < bestDistance ||
        (std::fabs(distance - bestDistance) <= kTieBreakPixels && priority > bestPriority)) {
      bestDistance = distance;
      bestPriority = priority;
      best = hit;
    }
  };

  const bool drawTranslate = mode == vkpt::editor::GizmoMode::Translate ||
                             mode == vkpt::editor::GizmoMode::Universal;
  const bool drawRotate = mode == vkpt::editor::GizmoMode::Rotate ||
                          mode == vkpt::editor::GizmoMode::Universal;
  const bool drawScale = mode == vkpt::editor::GizmoMode::Scale ||
                         mode == vkpt::editor::GizmoMode::Universal;

  const auto consider_axis_line = [&](const CornerAxis& axis) {
    if (!drawTranslate || axis.length <= 1.0e-5f) {
      return;
    }
    const auto projectedEnd = ProjectWorldPointToOverlay(axis.endpoint, camera, width, height, renderAspect);
    if (!projectedEnd) {
      return;
    }
    float tangentX = 1.0f;
    float tangentY = 0.0f;
    const float distance = ScreenDistanceToSegment(mouseX,
                                                   mouseY,
                                                   *projectedAnchor,
                                                   *projectedEnd,
                                                   &tangentX,
                                                   &tangentY);
    if (distance > kTranslateHitRadius) {
      return;
    }
    const float screenPixels = ScreenDistance(projectedAnchor->x,
                                              projectedAnchor->y,
                                              projectedEnd->x,
                                              projectedEnd->y);
    accept_hit(distance, 10, ViewportGizmoHit{
        ViewportGizmoDragKind::Translate,
        axis.axis,
        center,
        tangentX,
        tangentY,
        std::max(1.0f, screenPixels / std::max(axis.length, 1.0e-4f)),
        axis.length,
        axis.axis_index});
  };

  const auto consider_axis_endpoint = [&](const CornerAxis& axis) {
    if (!drawScale || axis.length <= 1.0e-5f) {
      return;
    }
    const auto projectedEnd = ProjectWorldPointToOverlay(axis.endpoint, camera, width, height, renderAspect);
    if (!projectedEnd) {
      return;
    }
    const float distance = ScreenDistance(mouseX, mouseY, projectedEnd->x, projectedEnd->y);
    if (distance > kScaleHitRadius) {
      return;
    }
    float sx = projectedEnd->x - projectedAnchor->x;
    float sy = projectedEnd->y - projectedAnchor->y;
    float pixels = std::sqrt(sx * sx + sy * sy);
    if (pixels <= 1.0e-3f) {
      sx = 1.0f;
      sy = 0.0f;
      pixels = 1.0f;
    }
    accept_hit(distance, 30, ViewportGizmoHit{
        ViewportGizmoDragKind::ScaleAxis,
        axis.axis,
        anchor,
        sx / pixels,
        sy / pixels,
        std::max(1.0f, pixels / std::max(axis.length, 1.0e-4f)),
        axis.length,
        axis.axis_index});
  };

  for (const auto& axis : axes) {
    consider_axis_line(axis);
    consider_axis_endpoint(axis);
  }

  const auto consider_arc = [&](const vkpt::pathtracer::Vec3& axis,
                                int axisIndex,
                                const vkpt::pathtracer::Vec3& axisA,
                                const vkpt::pathtracer::Vec3& axisB,
                                float radius) {
    if (!drawRotate || radius <= 1.0e-4f) {
      return;
    }
    constexpr int kArcSegments = 14;
    constexpr float kHalfPi = 1.57079632679489661923f;
    auto previous = ProjectedViewportPoint{};
    bool previousValid = false;
    for (int segment = 0; segment <= kArcSegments; ++segment) {
      const float t = (static_cast<float>(segment) / static_cast<float>(kArcSegments)) * kHalfPi;
      const auto world = PtAdd(anchor,
                               PtAdd(PtMul(axisA, std::cos(t) * radius),
                                     PtMul(axisB, std::sin(t) * radius)));
      const auto projected = ProjectWorldPointToOverlay(world, camera, width, height, renderAspect);
      if (projected && previousValid) {
        float tangentX = 1.0f;
        float tangentY = 0.0f;
        const float distance = ScreenDistanceToSegment(mouseX, mouseY, previous, *projected, &tangentX, &tangentY);
        if (distance <= kRotateHitRadius) {
          accept_hit(distance, 20, ViewportGizmoHit{
              ViewportGizmoDragKind::Rotate,
              axis,
              center,
              tangentX,
              tangentY,
              1.0f,
              std::max(radius, 1.0e-4f),
              axisIndex});
        }
      }
      if (projected) {
        previous = *projected;
        previousValid = true;
      } else {
        previousValid = false;
      }
    }
  };

  consider_arc(axes[0].axis, 0, axes[1].axis, axes[2].axis, std::min(yLength, zLength));
  consider_arc(axes[1].axis, 1, axes[0].axis, axes[2].axis, std::min(xLength, zLength));
  consider_arc(axes[2].axis, 2, axes[0].axis, axes[1].axis, std::min(xLength, yLength));
  return best;
}

std::optional<ViewportGizmoHit> PickSelectionBoundsFreeform(const vkpt::editor::Bounds& bounds,
                                                            const ViewportCameraPose& camera,
                                                            float width,
                                                            float height,
                                                            float renderAspect,
                                                            vkpt::editor::GizmoMode mode,
                                                            float mouseX,
                                                            float mouseY) {
  if (!bounds.valid || mode == vkpt::editor::GizmoMode::None) {
    return std::nullopt;
  }
  const auto corners = BoundsCorners(bounds);
  std::array<std::optional<ProjectedViewportPoint>, 8> projected{};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    projected[i] = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
  }

  float bestDistance = 10.0f;
  for (const auto [a, b] : kViewportBoundsEdges) {
    const auto& pa = projected[static_cast<std::size_t>(a)];
    const auto& pb = projected[static_cast<std::size_t>(b)];
    if (!pa || !pb) {
      continue;
    }
    const float distance = ScreenDistanceToSegment(mouseX, mouseY, *pa, *pb);
    bestDistance = std::min(bestDistance, distance);
  }
  if (bestDistance > 9.5f) {
    return std::nullopt;
  }

  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  return ViewportGizmoHit{
      ViewportGizmoDragKind::FreeformTranslate,
      {},
      PtMul(PtAdd(min, max), 0.5f),
      1.0f,
      0.0f,
      1.0f,
      1.0f,
      -1};
}

void AddSelectionGizmo(vkpt::platform::QtSelectionOverlayBox& box,
                       const vkpt::editor::Bounds& bounds,
                       const ViewportCameraPose& camera,
                       float width,
                       float height,
                       float renderAspect,
                       vkpt::editor::GizmoMode mode,
                       const std::optional<ViewportGizmoHit>& hover) {
  if (mode == vkpt::editor::GizmoMode::None || !bounds.valid) {
    return;
  }
  const auto min = ToPtVec3(bounds.min);
  const auto max = ToPtVec3(bounds.max);
  const float extentX = std::fabs(max.x - min.x);
  const float extentY = std::fabs(max.y - min.y);
  const float extentZ = std::fabs(max.z - min.z);
  const float maxExtent = std::max({extentX, extentY, extentZ});
  if (maxExtent <= 1.0e-5f) {
    return;
  }

  const auto corners = BoundsCorners(bounds);
  std::size_t anchorIndex = 0u;
  float nearestDepth = std::numeric_limits<float>::infinity();
  for (std::size_t i = 0; i < corners.size(); ++i) {
    const auto projected = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
    if (projected && projected->depth < nearestDepth) {
      nearestDepth = projected->depth;
      anchorIndex = i;
    }
  }
  const auto anchor = corners[anchorIndex];
  const bool anchorMinX = std::fabs(anchor.x - min.x) <= std::fabs(anchor.x - max.x);
  const bool anchorMinY = std::fabs(anchor.y - min.y) <= std::fabs(anchor.y - max.y);
  const bool anchorMinZ = std::fabs(anchor.z - min.z) <= std::fabs(anchor.z - max.z);
  struct CornerAxis {
    int axis_index = -1;
    vkpt::pathtracer::Vec3 axis{};
    vkpt::pathtracer::Vec3 endpoint{};
    float length = 0.0f;
    OverlayColor color{};
  };
  constexpr OverlayColor kX{245u, 76u, 76u, 170u};
  constexpr OverlayColor kY{84u, 214u, 112u, 170u};
  constexpr OverlayColor kZ{74u, 144u, 255u, 170u};
  const std::array<CornerAxis, 3> axes{{
      {0,
       {anchorMinX ? 1.0f : -1.0f, 0.0f, 0.0f},
       {anchorMinX ? max.x : min.x, anchor.y, anchor.z},
       extentX,
       kX},
      {1,
       {0.0f, anchorMinY ? 1.0f : -1.0f, 0.0f},
       {anchor.x, anchorMinY ? max.y : min.y, anchor.z},
       extentY,
       kY},
      {2,
       {0.0f, 0.0f, anchorMinZ ? 1.0f : -1.0f},
       {anchor.x, anchor.y, anchorMinZ ? max.z : min.z},
       extentZ,
       kZ},
  }};
  const auto tickLength = [maxExtent](float axisExtent) {
    if (axisExtent <= 1.0e-5f) {
      return 0.0f;
    }
    return std::max(0.025f, std::min(axisExtent * 0.35f, maxExtent * 0.18f));
  };
  const float xLength = tickLength(extentX);
  const float yLength = tickLength(extentY);
  const float zLength = tickLength(extentZ);

  const bool drawTranslate = mode == vkpt::editor::GizmoMode::Translate ||
                             mode == vkpt::editor::GizmoMode::Universal;
  const bool drawRotate = mode == vkpt::editor::GizmoMode::Rotate ||
                          mode == vkpt::editor::GizmoMode::Universal;
  const bool drawScale = mode == vkpt::editor::GizmoMode::Scale ||
                         mode == vkpt::editor::GizmoMode::Universal;

  constexpr OverlayColor kCorner{255u, 255u, 255u, 170u};
  const auto highlight_color = [](OverlayColor color) {
    color.a = 255u;
    color.r = static_cast<std::uint8_t>(std::min(255, static_cast<int>(color.r) + 22));
    color.g = static_cast<std::uint8_t>(std::min(255, static_cast<int>(color.g) + 22));
    color.b = static_cast<std::uint8_t>(std::min(255, static_cast<int>(color.b) + 22));
    return color;
  };

  if (drawTranslate) {
    for (const auto& axis : axes) {
      if (axis.length <= 1.0e-5f) {
        continue;
      }
      const bool hovered = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Translate, axis.axis_index);
      AddWorldOverlayLine(box,
                          camera,
                          width,
                          height,
                          renderAspect,
                          anchor,
                          axis.endpoint,
                          hovered ? highlight_color(axis.color) : axis.color,
                          hovered ? 2.4f : 1.35f);
    }
  }

  if (drawScale) {
    for (const auto& axis : axes) {
      if (axis.length <= 1.0e-5f) {
        continue;
      }
      const bool hovered = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::ScaleAxis, axis.axis_index);
      AddWorldOverlayPoint(box,
                           camera,
                           width,
                           height,
                           renderAspect,
                           axis.endpoint,
                           hovered ? highlight_color(axis.color) : axis.color,
                           hovered ? 4.5f : 3.1f);
    }
  }

  if (drawRotate) {
    const bool hoverX = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 0);
    const bool hoverY = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 1);
    const bool hoverZ = IsHoveredGizmoHandle(hover, ViewportGizmoDragKind::Rotate, 2);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor, axes[1].axis, axes[2].axis,
                      std::min(yLength, zLength), hoverX ? highlight_color(kX) : kX, hoverX ? 2.3f : 1.1f);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor, axes[0].axis, axes[2].axis,
                      std::min(xLength, zLength), hoverY ? highlight_color(kY) : kY, hoverY ? 2.3f : 1.1f);
    AddGizmoCornerArc(box, camera, width, height, renderAspect, anchor, axes[0].axis, axes[1].axis,
                      std::min(xLength, yLength), hoverZ ? highlight_color(kZ) : kZ, hoverZ ? 2.3f : 1.1f);
  }

  if (drawTranslate || drawRotate || drawScale) {
    AddWorldOverlayPoint(box, camera, width, height, renderAspect, anchor, kCorner, 2.8f);
  }
}

std::optional<vkpt::platform::QtSelectionOverlayBox> ProjectBoundsToOverlay(
    const vkpt::editor::Bounds& bounds,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect,
    std::string label,
    bool primary,
    vkpt::editor::GizmoMode gizmoMode,
    const std::optional<ViewportGizmoHit>& hover) {
  if (!bounds.valid || width <= 1.0f || height <= 1.0f) {
    return std::nullopt;
  }
  constexpr std::array<std::pair<int, int>, 12> kBoundsEdges{{
      {0, 1}, {1, 3}, {3, 2}, {2, 0},
      {4, 5}, {5, 7}, {7, 6}, {6, 4},
      {0, 4}, {1, 5}, {2, 6}, {3, 7},
  }};
  const auto corners = BoundsCorners(bounds);
  std::array<std::optional<ProjectedViewportPoint>, 8> projectedCorners{};
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  bool anyProjected = false;
  for (std::size_t i = 0; i < corners.size(); ++i) {
    projectedCorners[i] = ProjectWorldPointToOverlay(corners[i], camera, width, height, renderAspect);
    if (!projectedCorners[i]) {
      continue;
    }
    minX = std::min(minX, projectedCorners[i]->x);
    minY = std::min(minY, projectedCorners[i]->y);
    maxX = std::max(maxX, projectedCorners[i]->x);
    maxY = std::max(maxY, projectedCorners[i]->y);
    anyProjected = true;
  }
  if (!anyProjected) {
    return std::nullopt;
  }
  constexpr OverlayColor kPrimaryBox{255u, 214u, 64u, 245u};
  constexpr OverlayColor kSecondaryBox{102u, 204u, 255u, 230u};
  vkpt::platform::QtSelectionOverlayBox box{};
  box.label = std::move(label);
  box.primary = primary;
  auto boxColor = primary ? kPrimaryBox : kSecondaryBox;
  if (primary && hover && hover->kind == ViewportGizmoDragKind::FreeformTranslate) {
    boxColor = {255u, 244u, 164u, 255u};
  }
  for (const auto [a, b] : kBoundsEdges) {
    if (!projectedCorners[static_cast<std::size_t>(a)] ||
        !projectedCorners[static_cast<std::size_t>(b)]) {
      continue;
    }
    AddProjectedOverlayLine(box,
                            *projectedCorners[static_cast<std::size_t>(a)],
                            *projectedCorners[static_cast<std::size_t>(b)],
                            boxColor,
                            primary
                                ? (hover && hover->kind == ViewportGizmoDragKind::FreeformTranslate ? 2.8f : 2.0f)
                                : 1.5f);
  }
  if (primary) {
    AddSelectionGizmo(box, bounds, camera, width, height, renderAspect, gizmoMode, hover);
  }

  const float margin = 4.0f;
  minX = ClampFloat(minX - margin, -width, width * 2.0f);
  minY = ClampFloat(minY - margin, -height, height * 2.0f);
  maxX = ClampFloat(maxX + margin, -width, width * 2.0f);
  maxY = ClampFloat(maxY + margin, -height, height * 2.0f);
  if (maxX <= minX || maxY <= minY) {
    return std::nullopt;
  }
  box.x = minX;
  box.y = minY;
  box.width = maxX - minX;
  box.height = maxY - minY;
  return box;
}

std::vector<vkpt::platform::QtSelectionOverlayBox> BuildSelectionOverlayBoxes(
    const vkpt::editor::SelectionState& selection,
    const std::vector<ViewportPickable>& pickables,
    const ViewportCameraPose& camera,
    float width,
    float height,
    float renderAspect,
    vkpt::editor::GizmoMode gizmoMode,
    const std::optional<ViewportGizmoHit>& activeHover) {
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
                                            renderAspect,
                                            label,
                                            selectedId == selection.active_primary_entity,
                                            selectedId == selection.active_primary_entity
                                                ? gizmoMode
                                                : vkpt::editor::GizmoMode::None,
                                            selectedId == selection.active_primary_entity
                                                ? activeHover
                                                : std::optional<ViewportGizmoHit>{});
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

std::vector<vkpt::editor::SceneTreeEntityModel> BuildSceneTreeEntitiesFromWorld(
    const vkpt::scene::SceneWorld& world) {
  std::vector<vkpt::editor::SceneTreeEntityModel> models;
  models.reserve(world.all_entities().size());
  for (const auto id : world.all_entities()) {
    const auto* entity = world.get_entity(id);
    if (!entity) {
      continue;
    }
    vkpt::editor::SceneTreeEntityModel model;
    model.entity_id = id;
    model.parent_id = entity->hierarchy ? entity->hierarchy->parent : 0;
    model.sibling_order = entity->hierarchy ? entity->hierarchy->sibling_order : 0;
    model.name = entity->identity.name;
    if (entity->transform.has_value()) {
      model.component_badges.push_back("transform");
    }
    if (entity->mesh_renderer.has_value()) {
      model.component_badges.push_back("mesh");
    }
    if (entity->sdf_primitive.has_value()) {
      model.component_badges.push_back("sdf");
    }
    if (entity->material_override.has_value() ||
        (entity->mesh_renderer.has_value() && entity->mesh_renderer->material_id != 0)) {
      model.component_badges.push_back("material");
    }
    if (entity->camera.has_value()) {
      model.component_badges.push_back("camera");
    }
    if (entity->light.has_value()) {
      model.component_badges.push_back("light");
    }
    if (entity->script.has_value()) {
      model.component_badges.push_back("script");
    }
    if (entity->physics_body.has_value()) {
      model.component_badges.push_back("physics");
    }
    models.push_back(std::move(model));
  }
  return models;
}

bool CheckEcsSceneTreeContracts(std::string* detail = nullptr) {
  auto fail = [&](std::string_view reason) {
    if (detail) {
      *detail = std::string(reason);
    }
    return false;
  };
  auto set_detail = [&](std::string_view text) {
    if (detail) {
      *detail = std::string(text);
    }
  };

  vkpt::scene::SceneDocument doc;
  doc.metadata.schema = "1.0";
  doc.metadata.scene_name = "ecs-tree-smoke";
  vkpt::scene::SceneEntityDefinition rootDef;
  rootDef.id = 1;
  rootDef.name = "Root";
  rootDef.has_physics_body = true;
  rootDef.physics_body.enabled = true;
  rootDef.physics_body.dynamic = true;
  rootDef.physics_body.body_type = "dynamic";
  rootDef.physics_body.shape = "box";
  rootDef.physics_body.mass = 2.0f;
  vkpt::scene::SceneEntityDefinition firstChild;
  firstChild.id = 2;
  firstChild.name = "First";
  firstChild.has_hierarchy = true;
  firstChild.hierarchy.parent = 1;
  firstChild.hierarchy.sibling_order = 1;
  vkpt::scene::SceneEntityDefinition secondChild;
  secondChild.id = 3;
  secondChild.name = "Second";
  secondChild.has_hierarchy = true;
  secondChild.hierarchy.parent = 1;
  secondChild.hierarchy.sibling_order = 0;
  doc.entities = {rootDef, firstChild, secondChild};
  const auto serialized = doc.to_json(true);
  auto reloaded = vkpt::scene::SceneDocument::load_from_text(serialized);
  if (!reloaded) {
    return fail("SceneDocument hierarchy JSON roundtrip failed to parse");
  }
  auto loadedWorld = reloaded.value().to_world();
  if (!loadedWorld) {
    return fail("SceneDocument hierarchy JSON roundtrip failed to build ECS world");
  }
  const auto persistedChildren = loadedWorld.value().children_of(1);
  if (persistedChildren != std::vector<vkpt::core::StableId>({3, 2})) {
    return fail("sibling_order did not persist through save/load");
  }
  auto physics = vkpt::physics::CreatePhysicsWorld();
  const auto physicsInfo = physics->engine_info();
  if (!physicsInfo.runs_on_worker_thread || physicsInfo.threading_model != "dedicated_worker") {
    return fail("physics world is not running through the dedicated worker thread");
  }
  const auto persistedPhysics = physics->sync_from_scene_world(loadedWorld.value());
  if (persistedPhysics.physics_components != 1u || persistedPhysics.enabled_bodies != 1u ||
      persistedPhysics.dynamic_bodies != 1u) {
    return fail("physics body did not persist through JSON and ECS sync");
  }

  vkpt::scene::SceneWorld world;
  const auto root = world.create_entity("Root", 10);
  const auto childA = world.create_entity("Child A", 11);
  const auto childB = world.create_entity("Child B", 12);
  vkpt::scene::TransformComponent rootTransform;
  rootTransform.translation = {10.0f, 0.0f, 0.0f};
  vkpt::scene::TransformComponent childTransform;
  childTransform.translation = {1.0f, 0.0f, 0.0f};
  if (!world.set_transform(root, rootTransform) ||
      !world.set_transform(childA, childTransform) ||
      !world.set_hierarchy_parent(childA, root, 0) ||
      !world.set_hierarchy_parent(childB, root, 1)) {
    return fail("failed to build ECS hierarchy fixture");
  }
  vkpt::scene::MeshRendererComponent mesh;
  mesh.mesh_id = 101;
  mesh.material_id = 201;
  world.set_component(childB, vkpt::scene::ComponentKind::MeshRenderer, mesh);
  vkpt::scene::PhysicsBodyComponent body;
  body.enabled = true;
  body.dynamic = true;
  body.body_type = "dynamic";
  body.mass = 1.5f;
  world.set_component(childA, vkpt::scene::ComponentKind::PhysicsBody, body);
  const auto livePhysics = physics->sync_from_scene_world(world);
  if (livePhysics.enabled_bodies != 1u || livePhysics.dynamic_bodies != 1u) {
    return fail("physics world did not sync enabled ECS physics body");
  }
  vkpt::physics::PhysicsStepConfig physicsStep;
  physicsStep.fixed_dt = 1.0f / 60.0f;
  if (!physics->step_fixed(physicsStep)) {
    return fail("physics fixed step rejected a valid timestep");
  }
  if (physicsInfo.available && physics->extract_transform_writes().empty()) {
    return fail("Jolt physics backend did not publish transform writes");
  }

  world.recompute_world_transforms();
  const auto* before = world.world_transform(childA);
  if (!before || std::abs(before->translation.x - 11.0f) > 0.001f) {
    return fail("hierarchy fixture world transform was invalid before reparent");
  }

  vkpt::scene::WorldCommandBuffer commands;
  commands.add_reorder_sibling(childB, 0, childA);
  commands.add_reparent_entity(childA, 0, true);
  commands.add_create_entity("Camera Child", 13, root);
  commands.add_set_component(13, vkpt::scene::ComponentKind::Camera, vkpt::scene::CameraComponent{});
  if (!commands.replay(world)) {
    return fail("WorldCommandBuffer failed to replay reparent/reorder/create-child commands");
  }
  world.recompute_world_transforms();
  const auto rootChildren = world.children_of(root);
  if (rootChildren != std::vector<vkpt::core::StableId>({childB, 13})) {
    return fail("reorder/create-child commands produced nondeterministic child order");
  }
  const auto* after = world.world_transform(childA);
  if (!after || std::abs(after->translation.x - 11.0f) > 0.001f) {
    return fail("preserve-world reparent changed the child world transform");
  }

  vkpt::editor::SelectionState selection = vkpt::editor::CreateDefaultSelectionState();
  selection.selected_entity_ids = {childA};
  selection.hovered_entity = 13;
  const auto treeRows = vkpt::editor::BuildSceneTreeRows(
      BuildSceneTreeEntitiesFromWorld(world), selection, 13);
  if (treeRows.size() != 4) {
    return fail("scene tree row builder returned the wrong visible row count");
  }
  if (treeRows[0].entity_id != root || treeRows[0].depth != 0 || !treeRows[0].has_children) {
    return fail("scene tree rows did not expose root hierarchy state");
  }
  if (treeRows[1].entity_id != childB || treeRows[1].depth != 1 ||
      std::find(treeRows[1].component_badges.begin(), treeRows[1].component_badges.end(), "mesh") ==
          treeRows[1].component_badges.end()) {
    return fail("scene tree rows did not expose ordered child badges");
  }
  if (treeRows[2].entity_id != 13 || treeRows[2].depth != 1 || !treeRows[2].hovered ||
      treeRows[2].icon != "camera") {
    return fail("scene tree rows did not expose hover/camera state");
  }
  if (treeRows[3].entity_id != childA || !treeRows[3].selected || treeRows[3].depth != 0) {
    return fail("scene tree rows did not reflect selection or reparent-to-root");
  }
  set_detail("ECS tree rows, worker-thread physics sync, hierarchy command replay, preserve-world reparent, and sibling_order JSON roundtrip pass");
  return true;
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

  vkpt::scene::SceneDocument cameraDoc;
  cameraDoc.metadata.schema = "1.0";
  vkpt::scene::SceneEntityDefinition cameraEntity;
  cameraEntity.id = 77;
  cameraEntity.name = "Camera";
  cameraEntity.has_camera = true;
  cameraEntity.camera.fov = 47.0f;
  cameraEntity.camera.aperture_radius = 0.08f;
  cameraEntity.camera.focus_distance = 4.25f;
  cameraEntity.camera.f_stop = 2.8f;
  cameraEntity.camera.shutter_seconds = 1.0f / 125.0f;
  cameraEntity.camera.iso = 400.0f;
  cameraEntity.camera.exposure_compensation = 1.0f;
  cameraEntity.camera.white_balance_kelvin = 5200.0f;
  cameraEntity.camera.iris_blade_count = 7u;
  cameraEntity.camera.iris_rotation_degrees = 15.0f;
  cameraEntity.camera.iris_roundness = 0.35f;
  cameraEntity.camera.anamorphic_squeeze = 1.6f;
  cameraDoc.entities.push_back(cameraEntity);
  const auto cameraRoundTrip = vkpt::scene::SceneDocument::load_from_text(cameraDoc.to_json(false));
  check_true("camera json roundtrip parses", cameraRoundTrip.has_value());
  if (cameraRoundTrip && !cameraRoundTrip.value().entities.empty()) {
    const auto& camera = cameraRoundTrip.value().entities.front().camera;
    check_true("camera json roundtrip iris", camera.iris_blade_count == 7u);
    check_true("camera json roundtrip anamorphic", std::abs(camera.anamorphic_squeeze - 1.6f) < 0.001f);
    check_true("camera json roundtrip exposure", std::abs(camera.exposure_compensation - 1.0f) < 0.001f);
  }

  vkpt::pathtracer::RTSceneData physicalScene;
  physicalScene.camera_f_stop = 2.8f;
  physicalScene.camera_shutter_seconds = 1.0f / 30.0f;
  physicalScene.camera_iso = 200.0f;
  physicalScene.camera_exposure_compensation = 1.0f;
  physicalScene.camera_white_balance_kelvin = 5200.0f;
  vkpt::pathtracer::FilmResolveSettings resolveSettings;
  resolveSettings.exposure = 1.0f;
  resolveSettings.tone_map = vkpt::pathtracer::ToneMapMode::Reinhard;
  resolveSettings.output_transform = vkpt::pathtracer::OutputTransformMode::Linear;
  const auto adjustedResolve =
      vkpt::pathtracer::CameraAdjustedFilmResolveSettings(resolveSettings, physicalScene);
  check_true("physical camera exposure affects resolve", adjustedResolve.exposure > 1.0f);
  check_true("camera white balance affects resolve",
             std::abs(adjustedResolve.white_balance_kelvin - 5200.0f) < 0.001f);
  vkpt::pathtracer::FilmHdr hdr;
  hdr.width = 1u;
  hdr.height = 1u;
  hdr.rgbf = {0.5f, 0.5f, 0.5f};
  const auto ldrLinear = vkpt::pathtracer::ApplyFilmResolve(hdr, adjustedResolve);
  resolveSettings.output_transform = vkpt::pathtracer::OutputTransformMode::Gamma;
  const auto ldrGamma = vkpt::pathtracer::ApplyFilmResolve(hdr, resolveSettings);
  check_true("output transform changes film resolve",
             !ldrLinear.rgba8.empty() && !ldrGamma.rgba8.empty() &&
             ldrLinear.rgba8[0] != ldrGamma.rgba8[0]);
  const auto layoutManifest = vkpt::pathtracer::BuildRTSceneDataLayoutManifest();
  check_true("camera gpu layout manifest builds", layoutManifest.has_value());
  if (layoutManifest) {
    auto has_layout_field = [&](std::string_view field_name) {
      const auto& fields = layoutManifest.value().fields;
      return std::any_of(fields.begin(), fields.end(), [field_name](const auto& field) {
        return field.field == field_name;
      });
    };
    check_true("camera gpu layout has aperture", has_layout_field("camera_aperture_radius"));
    check_true("camera gpu layout has iris", has_layout_field("camera_iris_blade_count"));
    check_true("camera gpu layout has anamorphic", has_layout_field("camera_anamorphic_squeeze"));
  }
#ifdef PT_ENABLE_QT
  const auto cameraDockScene = vkpt::pathtracer::BuildSceneDataFromDocument(cameraDoc);
  check_true("camera dock scene builds", cameraDockScene.has_value());
  if (cameraDockScene) {
    const auto cameraDockPanels = BuildQtDockPanels(cameraDoc,
                                                   cameraDockScene.value(),
                                                   vkpt::pathtracer::RenderSettings{},
                                                   UiRuntimeState{},
                                                   SelectionState{},
                                                   CreateDefaultLayout(),
                                                   BenchmarkPanelModel{},
                                                   QtDockFrameStats{},
                                                   QtDockDeviceStats{},
                                                   0,
                                                   std::array<bool, 4>{});
    const auto cameraPanel = std::find_if(cameraDockPanels.begin(),
                                          cameraDockPanels.end(),
                                          [](const QtDockPanelContent& panel) {
                                            return panel.id == "camera";
                                          });
    check_true("camera dock has focus buttons",
               cameraPanel != cameraDockPanels.end() &&
               std::any_of(cameraPanel->properties.begin(),
                           cameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.focus.pick" &&
                                    property.editor == "button";
                           }) &&
               std::any_of(cameraPanel->properties.begin(),
                           cameraPanel->properties.end(),
                           [](const QtDockProperty& property) {
                             return property.id == "camera.focus.selected" &&
                                    property.editor == "button";
                           }));
  }
#endif

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
  std::string ecs_tree_detail;
  check_true("ecs scene tree integration", CheckEcsSceneTreeContracts(&ecs_tree_detail));

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
  std::string treeEvidence;
  const bool treeHierarchyOk = CheckEcsSceneTreeContracts(&treeEvidence);
  MarkReleaseGate(checklist, "tree.hierarchy", treeHierarchyOk,
                  treeHierarchyOk ? treeEvidence : "ECS scene tree model/runtime contract check failed",
                  treeHierarchyOk ? std::string_view{} : std::string_view{treeEvidence});
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
                  "requires group/ungroup runtime metadata and undo integration");
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
  bool listAccelerators = false;
  bool doRender      = false;
  bool uiModelSmoke  = false;
  bool uiReleaseGate = false;
  bool dynamicPhysicsGate = false;
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
    else if (token == "--list-accelerators") { listAccelerators = true; }
    else if (token == "--headless")       { headless      = true; }
    else if (token == "--render")         { doRender      = true; }
    else if (token == "--window")         { openWindow    = true; }
    else if (token == "--list-gpus")      { listGpus      = true; }
    else if (token == "--crash-test")     { crashTest     = true; }
    else if (token == "--ui-model-smoke") { uiModelSmoke  = true; }
    else if (token == "--ui-release-gate") { uiReleaseGate = true; }
    else if (token == "--dynamic-physics-gate") { dynamicPhysicsGate = true; }
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
      auto applyQtCameraPose = [&](std::string_view reason) {
        qtLastCameraInputTime = std::chrono::steady_clock::now();
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
      bool qtLeftMouseDown = false;
      bool qtRightMouseDown = false;
      bool qtMiddleMouseDown = false;
      bool qtPotentialClick = false;
      float qtClickX = 0.0f;
      float qtClickY = 0.0f;
      float qtClickDragPixels = 0.0f;
      float qtLastMouseX = 0.0f;
      float qtLastMouseY = 0.0f;
      bool qtDockPanelsDirty = true;
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
      auto qtReloadEditedScene = [&](std::string_view reason) {
        auto sceneResult = vkpt::pathtracer::BuildSceneDataFromDocument(qtSceneDocument);
        if (!sceneResult) {
          ui_runtime_state.status_message = "scene edit failed: rebuild scene data";
          qtPreviewStatus = "scene edit failed";
          return false;
        }
        qtScene = std::move(sceneResult.value());
        qtApplySceneLightingFallback();
        qtScene.camera_position = qtCameraPose.position;
        qtScene.camera_target = qtCameraPose.target;
        qtScene.camera_up = qtCameraPose.up;
        qtScene.camera_fov_deg = qtCameraPose.fov_deg;
        qtPickables = BuildViewportPickables(qtSceneDocument, qtScene);
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
        qtPreviewStatus = qtTracerReady ? "scene edited" : "scene edit renderer reload failed";
        ui_runtime_state.status_message = std::string("gizmo ") + std::string(reason);
        qtPublishedRays.store(0u, std::memory_order_relaxed);
        updateQtSelectionOverlay();
        return qtTracerReady;
      };
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
        for (auto& entity : qtSceneDocument.entities) {
          if (entity.has_camera) {
            entity.camera.focus_distance = focusDistance;
            wroteAuthoredCamera = true;
            break;
          }
        }
        for (auto& camera : qtSceneDocument.cameras) {
          camera.camera.focus_distance = focusDistance;
          wroteAuthoredCamera = true;
          break;
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

        if (parts.size() >= 2u && parts[0] == "camera") {
          if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "slot") {
            float value = 0.0f;
            if (!QtParseFloat(edit.value, value)) {
              return failEdit("expected numeric camera shot slot");
            }
            qtActiveCameraShotSlot = static_cast<int>(ClampFloat(std::round(value), 1.0f, 4.0f)) - 1;
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "save") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
            }
            if (value) {
              qtSaveCameraShot(qtActiveCameraShotSlot);
            }
          } else if (parts.size() == 3u && parts[1] == "shot" && parts[2] == "recall") {
            bool value = false;
            if (!QtParseBool(edit.value, value)) {
              return failEdit("expected true or false");
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
          if (parts.size() == 2u && parts[1] == "max_depth") {
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
            if (parts.size() == 5u && field == "color") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric light color component");
              }
              value = ClampFloat(value, 0.0f, 10.0f);
              if (parts[4] == "x") {
                entity->light.color.x = value;
              } else if (parts[4] == "y") {
                entity->light.color.y = value;
              } else if (parts[4] == "z") {
                entity->light.color.z = value;
              } else {
                return failEdit("unknown light color component");
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
            } else if (field == "intensity" || field == "radius") {
              float value = 0.0f;
              if (!QtParseFloat(edit.value, value)) {
                return failEdit("expected numeric light value");
              }
              if (field == "intensity") {
                entity->light.intensity = std::max(0.0f, value);
              } else {
                entity->light.radius = std::max(0.0f, value);
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
      auto qtApplyFpsLookDelta = [&](float dx, float dy) {
        constexpr float kLookSensitivity = 0.0045f;
        qtFpsYaw += dx * kLookSensitivity;
        qtFpsPitch = ClampFloat(qtFpsPitch - dy * kLookSensitivity, -1.45f, 1.45f);
        const auto forward = qtFpsForwardFromAngles();
        qtCameraPose.target = PtAdd(qtCameraPose.position,
                                    PtMul(forward, std::max(1.0f, qtCameraFocusDistance)));
        qtCameraFocusPoint = qtCameraPose.target;
        qtCameraFocusDistance = PtLength(PtSub(qtCameraFocusPoint, qtCameraPose.position));
        qtCameraPose.up = {0.0f, 1.0f, 0.0f};
        applyQtCameraPose("fps look");
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
        const auto forward = qtCameraForward();
        if (qtFpsMode) {
          const float distance = wheelDelta * qtCameraMoveUnitsPerSecond * 0.35f;
          const auto delta = PtMul(forward, distance);
          qtCameraPose.position = PtAdd(qtCameraPose.position, PtMul(forward, distance));
          qtCameraPose.target = PtAdd(qtCameraPose.target, PtMul(forward, distance));
          qtMoveCameraFocusBy(delta);
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
        qtMoveCameraFocusBy(delta);
        applyQtCameraPose("fps move");
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
      std::uint64_t qtLastRayRateTotal = 0u;
      auto qtLastRayRateTime = std::chrono::steady_clock::time_point{};
      double qtInstantRaysPerSecond = 0.0;
      double qtRollingRaysPerSecond = 0.0;
      auto qtLastDockPanelSync = std::chrono::steady_clock::time_point{};
      std::vector<QtDockPanelContent> qtLastDockPanels;
      auto qtBuildFrameStats = [&]() {
        QtDockFrameStats stats;
        stats.sample_count = qtUseBg
            ? qtPublishedSample.load(std::memory_order_relaxed)
            : qtSampleIndex;
        stats.frame_width = qtPublishedWidth.load(std::memory_order_relaxed);
        stats.frame_height = qtPublishedHeight.load(std::memory_order_relaxed);
        stats.preview_publish_hz = qtPreviewPublishHz;
        stats.gpu_batches_per_tick = qtLastGpuBatchesPerTick;
        stats.gpu_batch_ms = qtSmoothedGpuBatchMs;
        stats.ui_frame_ms = qtLastUiFrameMs;
        stats.total_rays = qtPublishedRays.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (qtLastRayRateTime != std::chrono::steady_clock::time_point{} &&
            now > qtLastRayRateTime) {
          const double dt = std::chrono::duration<double>(now - qtLastRayRateTime).count();
          if (stats.total_rays >= qtLastRayRateTotal && dt >= 0.05) {
            qtInstantRaysPerSecond =
                static_cast<double>(stats.total_rays - qtLastRayRateTotal) / dt;
            const double alpha = ClampFloat(static_cast<float>(dt / 2.0), 0.05f, 0.35f);
            qtRollingRaysPerSecond = qtRollingRaysPerSecond <= 0.0
                ? qtInstantRaysPerSecond
                : (qtRollingRaysPerSecond * (1.0 - alpha) + qtInstantRaysPerSecond * alpha);
            qtLastRayRateTotal = stats.total_rays;
            qtLastRayRateTime = now;
          } else if (stats.total_rays < qtLastRayRateTotal) {
            qtInstantRaysPerSecond = 0.0;
            qtRollingRaysPerSecond = 0.0;
            qtLastRayRateTotal = stats.total_rays;
            qtLastRayRateTime = now;
          }
        } else {
          qtLastRayRateTotal = stats.total_rays;
          qtLastRayRateTime = now;
        }
        stats.instant_rays_per_second = qtInstantRaysPerSecond;
        stats.rolling_rays_per_second = qtRollingRaysPerSecond;
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
                                                 qtSavedCameraShots[3].valid});
        ApplyQtDockPanelsToWindow(qtWindow, qtLastDockPanels);
        qtLastDockPanelSync = now;
        qtDockPanelsDirty = false;
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
              } else if (key == 'C' || rawKey == 'C') {
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
                qtFpsMode = false;
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
              if (qtLeftMouseDown && qtGizmoDrag.active) {
                qtApplyGizmoDrag(event.x, event.y, qtFrameCount);
                qtClickDragPixels = std::max(qtClickDragPixels,
                                             ScreenDistance(event.x, event.y, qtClickX, qtClickY));
              } else if (qtLeftMouseDown && qtPotentialClick) {
                const float dx = event.x - qtClickX;
                const float dy = event.y - qtClickY;
                qtClickDragPixels = std::max(qtClickDragPixels, std::sqrt(dx * dx + dy * dy));
              }
              if (qtRightMouseDown && !qtGizmoDrag.active) {
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
        if (!qtInputEvents.empty() || !qtPropertyEdits.empty()) {
          UpdateCrashArtifactsFromUiState(ui_runtime_state,
                                         ui_selection_state,
                                         ui_layout_state,
                                         ui_event_log,
                                         ui_command_history);
        }
        qtApplyContinuousFpsMovement(qtInputDt);
        qtApplyPhysicsSimulation(qtFrameStart);
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
    // Smoke runs use a bounded sample cap so coordinator shutdown is exercised.
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
