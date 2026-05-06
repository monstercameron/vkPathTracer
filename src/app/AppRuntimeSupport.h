#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "core/Logging.h"
#include "editor/UiModels.h"
#include "platform/PlatformFactory.h"

#if defined(PT_ENABLE_RAW_DESKTOP)
#include "platform/DesktopPlatform.h"
#endif

#ifdef PT_ENABLE_QT
#include "platform/qt/QtPlatform.h"
#endif

namespace vkpt::app {

using NativeMenuId = unsigned int;

std::uint32_t InteractiveCpuWorkerCount();

#ifdef _WIN32
HMENU BuildNativeMenuBarFromModel(
    const vkpt::editor::MenuBar& menuModel,
    std::unordered_map<NativeMenuId, std::string>& menuCommands);
#endif

#if defined(_WIN32) && defined(PT_ENABLE_QT)
std::vector<vkpt::platform::QtMenuItem> BuildQtMenuBarFromModel(
    const vkpt::editor::MenuBar& menuModel,
    std::unordered_map<NativeMenuId, std::string>& menuCommands);
#endif

std::string BuildWindowTitleBase(std::string_view shell_name);
std::string BuildWindowRuntimeTitle(std::string_view title_base,
                                    const vkpt::editor::UiRuntimeState& runtime_state,
                                    const vkpt::editor::UiLayoutDocument& layout_state);

#if defined(PT_ENABLE_RAW_DESKTOP)
void SetWindowFrameStatus(vkpt::platform::DesktopWindow* window,
                          std::string_view title_base,
                          const vkpt::editor::UiRuntimeState& runtime_state,
                          const vkpt::editor::UiLayoutDocument& layout_state,
                          vkpt::core::FrameIndex frame_index,
                          std::string_view perf_text);
std::string FormatThroughputKmb(double value_per_second);
void LogDesktopWindowState(const char* stage,
                           const vkpt::platform::DesktopWindow& window);
#endif

void LogUiWindowStartup(std::uint32_t width,
                        std::uint32_t height,
                        const vkpt::editor::UiLayoutDocument& layout_state,
                        const vkpt::editor::UiRuntimeState& runtime_state);

std::string UiWindowErrorString();
void EnableOptionalConsole(bool requested);
void InitializeLogging();
void InitializeCrashRecorder();
void BootStep(std::string_view message);

#ifdef PT_ENABLE_QT
void DrainQtQueuedWork(int maxMilliseconds = 16);
#endif

void LogRuntimeMetadata(vkpt::log::Logger& logger,
                        std::string_view phase,
                        vkpt::platform::RuntimePlatformKind requestedPlatform,
                        vkpt::platform::RuntimePlatformKind selectedPlatform,
                        vkpt::platform::RuntimePlatformKind effectivePlatform,
                        bool openWindow,
                        bool doRender,
                        bool autoExit,
                        std::uint32_t frameLimit,
                        std::uint32_t uiPresentHz);
bool ValidateRuntimePlatformSelection(
    vkpt::platform::RuntimePlatformKind requestedPlatform,
    vkpt::platform::RuntimePlatformKind effectivePlatform,
    bool openWindow,
    bool headless);
void PrintNonGuiPlatformShellNotice(
    std::string_view command,
    vkpt::platform::RuntimePlatformKind selectedPlatform,
    vkpt::platform::RuntimePlatformKind effectivePlatform);

}  // namespace vkpt::app
