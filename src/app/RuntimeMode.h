#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace vkpt::app {

enum class RuntimeMode : std::uint8_t {
  Edit,
  LiveEdit,
  Play,
};

inline constexpr std::string_view kRuntimeModeEditName = "edit";
inline constexpr std::string_view kRuntimeModeLiveEditName = "live_edit";
inline constexpr std::string_view kRuntimeModePlayName = "play";
inline constexpr std::string_view kRuntimeModeUnknownName = "unknown";

inline constexpr std::array<RuntimeMode, 3> kRuntimeModes = {
  RuntimeMode::Edit,
  RuntimeMode::LiveEdit,
  RuntimeMode::Play,
};

struct RuntimeModeCapabilities {
  RuntimeMode mode = RuntimeMode::Edit;
  bool scripts_running = false;
  bool editor_canvas_enabled = false;
  bool dock_panels_editable = false;
  bool mouse_locked = false;
  bool game_input_enabled = false;
  bool viewport_pick_enabled = false;
  bool gizmo_enabled = false;
  bool lua_input_enabled = false;
};

inline constexpr RuntimeModeCapabilities kRuntimeModeEditCapabilities{
  RuntimeMode::Edit,
  false,  // scripts_running
  true,   // editor_canvas_enabled
  true,   // dock_panels_editable
  false,  // mouse_locked
  false,  // game_input_enabled
  true,   // viewport_pick_enabled
  true,   // gizmo_enabled
  false,  // lua_input_enabled
};

inline constexpr RuntimeModeCapabilities kRuntimeModeLiveEditCapabilities{
  RuntimeMode::LiveEdit,
  true,   // scripts_running
  true,   // editor_canvas_enabled
  true,   // dock_panels_editable
  false,  // mouse_locked
  false,  // game_input_enabled
  true,   // viewport_pick_enabled
  true,   // gizmo_enabled
  false,  // lua_input_enabled
};

inline constexpr RuntimeModeCapabilities kRuntimeModePlayCapabilities{
  RuntimeMode::Play,
  true,   // scripts_running
  false,  // editor_canvas_enabled
  false,  // dock_panels_editable
  true,   // mouse_locked
  true,   // game_input_enabled
  false,  // viewport_pick_enabled
  false,  // gizmo_enabled
  true,   // lua_input_enabled
};

[[nodiscard]] inline constexpr RuntimeModeCapabilities GetRuntimeModeCapabilities(
    RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::Edit:
      return kRuntimeModeEditCapabilities;
    case RuntimeMode::LiveEdit:
      return kRuntimeModeLiveEditCapabilities;
    case RuntimeMode::Play:
      return kRuntimeModePlayCapabilities;
  }
  return kRuntimeModeEditCapabilities;
}

[[nodiscard]] inline constexpr std::string_view RuntimeModeStableName(RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::Edit:
      return kRuntimeModeEditName;
    case RuntimeMode::LiveEdit:
      return kRuntimeModeLiveEditName;
    case RuntimeMode::Play:
      return kRuntimeModePlayName;
  }
  return kRuntimeModeUnknownName;
}

[[nodiscard]] inline constexpr std::string_view RuntimeModeDisplayName(RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::Edit:
      return "Edit";
    case RuntimeMode::LiveEdit:
      return "LiveEdit";
    case RuntimeMode::Play:
      return "Play";
  }
  return "Unknown";
}

[[nodiscard]] inline constexpr bool IsRuntimeModeStableName(std::string_view name) noexcept {
  return name == kRuntimeModeEditName ||
         name == kRuntimeModeLiveEditName ||
         name == kRuntimeModePlayName;
}

[[nodiscard]] inline constexpr RuntimeMode RuntimeModeFromStableName(
    std::string_view name,
    RuntimeMode fallback = RuntimeMode::Edit) noexcept {
  if (name == kRuntimeModeEditName) {
    return RuntimeMode::Edit;
  }
  if (name == kRuntimeModeLiveEditName) {
    return RuntimeMode::LiveEdit;
  }
  if (name == kRuntimeModePlayName) {
    return RuntimeMode::Play;
  }
  return fallback;
}

[[nodiscard]] inline constexpr bool RuntimeModeScriptsRunning(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).scripts_running;
}

[[nodiscard]] inline constexpr bool RuntimeModeEditorCanvasEnabled(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).editor_canvas_enabled;
}

[[nodiscard]] inline constexpr bool RuntimeModeDockPanelsEditable(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).dock_panels_editable;
}

[[nodiscard]] inline constexpr bool RuntimeModeMouseLocked(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).mouse_locked;
}

[[nodiscard]] inline constexpr bool RuntimeModeGameInputEnabled(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).game_input_enabled;
}

[[nodiscard]] inline constexpr bool RuntimeModeViewportPickEnabled(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).viewport_pick_enabled;
}

[[nodiscard]] inline constexpr bool RuntimeModeGizmoEnabled(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).gizmo_enabled;
}

[[nodiscard]] inline constexpr bool RuntimeModeLuaInputEnabled(RuntimeMode mode) noexcept {
  return GetRuntimeModeCapabilities(mode).lua_input_enabled;
}

static_assert(!RuntimeModeScriptsRunning(RuntimeMode::Edit));
static_assert(RuntimeModeScriptsRunning(RuntimeMode::LiveEdit));
static_assert(RuntimeModeScriptsRunning(RuntimeMode::Play));
static_assert(RuntimeModeEditorCanvasEnabled(RuntimeMode::LiveEdit));
static_assert(!RuntimeModeEditorCanvasEnabled(RuntimeMode::Play));
static_assert(RuntimeModeStableName(RuntimeMode::LiveEdit) == kRuntimeModeLiveEditName);

}  // namespace vkpt::app
