#include "app/AppRuntimeSupport.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "build_info.generated.h"
#include "app/AppOptions.h"
#include "core/Logging.h"
#include "diagnostics/CrashRecorder.h"

#ifdef PT_ENABLE_QT
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>
#endif

namespace vkpt::app {

uint32_t InteractiveCpuWorkerCount() {
  const uint32_t hardware =
      std::max<uint32_t>(1u, static_cast<uint32_t>(std::thread::hardware_concurrency()));
  if (hardware <= 2u) {
    return 1u;
  }
  if (hardware <= 4u) {
    return 2u;
  }
  const uint32_t reserved =
      std::clamp<uint32_t>((hardware + 1u) / 2u, 3u, hardware - 1u);
  return std::max<uint32_t>(1u, hardware - reserved);
}

constexpr NativeMenuId kFirstMenuCommandId = 40000u;

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
#endif

#if defined(_WIN32) && defined(PT_ENABLE_QT)
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

void DrainQtQueuedWork(int maxMilliseconds) {
  if (QCoreApplication::instance() != nullptr) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, maxMilliseconds);
  }
}
#endif

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
  const auto rawSupport = vkpt::platform::DescribeRuntimePlatform(
      vkpt::platform::RuntimePlatformKind::Raw);
  logger.log(vkpt::log::Severity::Info, "app", "runtime metadata", {
    {"phase", std::string(phase)},
    {"host_platform", vkpt::platform::HostPlatformName(vkpt::platform::HostPlatform())},
    {"requested_platform", vkpt::platform::RuntimePlatformKindName(requestedPlatform)},
    {"selected_platform", vkpt::platform::RuntimePlatformKindName(selectedPlatform)},
    {"effective_platform", vkpt::platform::RuntimePlatformKindName(effectivePlatform)},
    {"window_system", std::string(WindowSystemName(effectivePlatform))},
    {"raw_built", YesNo(IsRawPlatformBuilt())},
    {"raw_available", rawSupport.available ? "yes" : "no"},
    {"raw_stub", rawSupport.stub ? "yes" : "no"},
    {"raw_implementation", rawSupport.implementation},
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

bool ValidateRuntimePlatformSelection(
    vkpt::platform::RuntimePlatformKind requestedPlatform,
    vkpt::platform::RuntimePlatformKind effectivePlatform,
    bool openWindow,
    bool headless) {
  if (!vkpt::platform::IsPlatformBuilt(effectivePlatform)) {
    const auto support = vkpt::platform::DescribeRuntimePlatform(effectivePlatform);
    std::cerr << "selected platform is not built: "
              << vkpt::platform::RuntimePlatformKindName(effectivePlatform);
    if (support.unavailable_reason != nullptr && support.unavailable_reason[0] != '\0') {
      std::cerr << " (" << support.unavailable_reason << ")";
    }
    std::cerr << "\n";
    return false;
  }
  if (openWindow && !headless &&
      requestedPlatform == vkpt::platform::RuntimePlatformKind::Auto &&
      effectivePlatform == vkpt::platform::RuntimePlatformKind::Headless) {
    std::cerr << "--window requires a window platform; build Qt or a host native raw platform\n";
    return false;
  }
  if (!vkpt::platform::IsPlatformAvailable(effectivePlatform)) {
    const auto support = vkpt::platform::DescribeRuntimePlatform(effectivePlatform);
    std::cerr << "selected platform is not available on "
              << vkpt::platform::HostPlatformName(vkpt::platform::HostPlatform())
              << ": " << vkpt::platform::RuntimePlatformKindName(effectivePlatform);
    if (support.stub) {
      std::cerr << " (" << support.implementation << ")";
    }
    if (support.unavailable_reason != nullptr && support.unavailable_reason[0] != '\0') {
      std::cerr << ": " << support.unavailable_reason;
    }
    std::cerr << "\n";
    return false;
  }
  return true;
}

void PrintNonGuiPlatformShellNotice(std::string_view command,
                                    vkpt::platform::RuntimePlatformKind selectedPlatform,
                                    vkpt::platform::RuntimePlatformKind effectivePlatform) {
  if (selectedPlatform != effectivePlatform &&
      effectivePlatform == vkpt::platform::RuntimePlatformKind::Headless) {
    std::cout << "ui shell: headless ("
              << vkpt::platform::RuntimePlatformKindName(selectedPlatform)
              << " requested; GUI not initialized for "
              << command << ")\n";
  }
}


}  // namespace vkpt::app
