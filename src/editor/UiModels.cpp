#include "editor/UiModelsInternal.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace vkpt::editor {

const char* ToString(UiPanelId id) {
  switch (id) {
    case UiPanelId::SceneTree:
      return "scene_tree";
    case UiPanelId::Inspector:
      return "inspector";
    case UiPanelId::AssetBrowser:
      return "asset_browser";
    case UiPanelId::BenchmarkPanel:
      return "benchmark_panel";
    case UiPanelId::BenchmarkHistory:
      return "benchmark_history";
    case UiPanelId::ScriptPanel:
      return "script_panel";
    case UiPanelId::MaterialEditor:
      return "material_editor";
    case UiPanelId::LightsPanel:
      return "lights_panel";
    case UiPanelId::CameraPanel:
      return "camera_panel";
    case UiPanelId::RenderSettings:
      return "render_settings";
    case UiPanelId::Diagnostics:
      return "diagnostics";
    case UiPanelId::Performance:
      return "performance";
    case UiPanelId::DebugViews:
      return "debug_views";
    case UiPanelId::Timeline:
      return "timeline";
    case UiPanelId::Physics:
      return "physics";
    case UiPanelId::Console:
      return "console";
    case UiPanelId::StatusBar:
      return "status_bar";
    case UiPanelId::Viewport:
      return "viewport";
    case UiPanelId::MenuBar:
      return "menu_bar";
    case UiPanelId::Toolbar:
      return "toolbar";
    case UiPanelId::Unknown:
      return "unknown";
    default:
      return "unknown";
  }
}

const char* ToString(SelectionSource source) {
  switch (source) {
    case SelectionSource::Viewport:
      return "viewport";
    case SelectionSource::SceneTree:
      return "scene_tree";
    case SelectionSource::Inspector:
      return "inspector";
    case SelectionSource::AssetBrowser:
      return "asset_browser";
    case SelectionSource::ScriptPanel:
      return "script_panel";
    case SelectionSource::Unknown:
    default:
      return "unknown";
  }
}

const char* ToString(GizmoMode mode) {
  switch (mode) {
    case GizmoMode::Translate:
      return "translate";
    case GizmoMode::Rotate:
      return "rotate";
    case GizmoMode::Scale:
      return "scale";
    case GizmoMode::Universal:
      return "universal";
    case GizmoMode::None:
    default:
      return "none";
  }
}

const char* ToString(ViewportTool tool) {
  switch (tool) {
    case ViewportTool::Select:
      return "select";
    case ViewportTool::Orbit:
      return "orbit";
    case ViewportTool::Fps:
      return "fps";
    case ViewportTool::Turntable:
      return "turntable";
    case ViewportTool::Walk:
      return "walk";
    case ViewportTool::None:
    default:
      return "none";
  }
}

const char* ToString(UiDockArea area) {
  switch (area) {
    case UiDockArea::Top:
      return "top";
    case UiDockArea::Left:
      return "left";
    case UiDockArea::Right:
      return "right";
    case UiDockArea::Bottom:
      return "bottom";
    case UiDockArea::Center:
      return "center";
    case UiDockArea::Status:
      return "status";
    case UiDockArea::Floating:
      return "floating";
    default:
      return "left";
  }
}

std::string_view ToString(LayoutPreset preset) {
  return LayoutName(preset);
}

std::string_view ParseLayoutName(LayoutPreset preset) {
  return LayoutName(preset);
}

const char* ToString(EditorCommandKind kind) {
  switch (kind) {
    case EditorCommandKind::kSelectEntity:
      return "SelectEntity";
    case EditorCommandKind::kToggleSelectEntity:
      return "ToggleSelectEntity";
    case EditorCommandKind::kClearSelection:
      return "ClearSelection";
    case EditorCommandKind::kSetTransform:
      return "SetTransform";
    case EditorCommandKind::kSetMaterial:
      return "SetMaterial";
    case EditorCommandKind::kSetLightProperty:
      return "SetLightProperty";
    case EditorCommandKind::kSetCameraProperty:
      return "SetCameraProperty";
    case EditorCommandKind::kSetComponentProperty:
      return "SetComponentProperty";
    case EditorCommandKind::kCreateEntity:
      return "CreateEntity";
    case EditorCommandKind::kDeleteEntity:
      return "DeleteEntity";
    case EditorCommandKind::kDuplicateEntity:
      return "DuplicateEntity";
    case EditorCommandKind::kGroupEntities:
      return "GroupEntities";
    case EditorCommandKind::kUngroupEntities:
      return "UngroupEntities";
    case EditorCommandKind::kMergeEntities:
      return "MergeEntities";
    case EditorCommandKind::kReparentEntity:
      return "ReparentEntity";
    case EditorCommandKind::kReorderSibling:
      return "ReorderSibling";
    case EditorCommandKind::kAttachScript:
      return "AttachScript";
    case EditorCommandKind::kDetachScript:
      return "DetachScript";
    case EditorCommandKind::kImportAsset:
      return "ImportAsset";
    case EditorCommandKind::kAssignAsset:
      return "AssignAsset";
    case EditorCommandKind::kRunBenchmark:
      return "RunBenchmark";
    case EditorCommandKind::kUnsupportedUiAction:
      return "UnsupportedUiAction";
    default:
      return "Unknown";
  }
}

// ----- CameraController implementation ------------------------------------

const char* ToString(CameraControllerMode mode) {
  switch (mode) {
    case CameraControllerMode::Orbit:                return "Orbit";
    case CameraControllerMode::Fps:                  return "Fps";
    case CameraControllerMode::Turntable:            return "Turntable";
    case CameraControllerMode::ScriptedBenchmarkPath:return "ScriptedBenchmarkPath";
    default:                                         return "Unknown";
  }
}

CameraController::CameraController(CameraControllerMode mode) {
  m_state.mode = mode;
}

void CameraController::set_mode(CameraControllerMode mode) {
  if (m_state.mode != mode) {
    m_state.mode = mode;
    mark_dirty();
  }
}

CameraControllerMode CameraController::mode() const {
  return m_state.mode;
}

void CameraController::apply_orbit_delta(float d_azimuth, float d_elevation, float d_zoom) {
  m_state.orbit.azimuth += d_azimuth;
  m_state.orbit.elevation += d_elevation;
  m_state.orbit.radius = std::max(0.01f, m_state.orbit.radius + d_zoom);
  mark_dirty();
}

void CameraController::apply_fps_delta(float d_yaw, float d_pitch, float d_forward, float d_strafe) {
  m_state.fps.yaw += d_yaw;
  m_state.fps.pitch += d_pitch;
  const float cy = std::cos(m_state.fps.yaw);
  const float sy = std::sin(m_state.fps.yaw);
  m_state.fps.position_x += cy * d_forward + (-sy) * d_strafe;
  m_state.fps.position_z += sy * d_forward + cy * d_strafe;
  mark_dirty();
}

void CameraController::apply_turntable_delta(float d_azimuth, float d_zoom) {
  constexpr float kPiOver4 = 0.7853981633974483f;
  constexpr float kTwoPi   = 6.2831853071795865f;
  m_state.orbit.azimuth = std::fmod(m_state.orbit.azimuth + d_azimuth, kTwoPi);
  m_state.orbit.elevation = std::max(-kPiOver4, std::min(kPiOver4, m_state.orbit.elevation));
  m_state.orbit.radius = std::max(0.01f, m_state.orbit.radius + d_zoom);
  mark_dirty();
}

void CameraController::set_benchmark_path(std::vector<CameraWaypoint> waypoints) {
  m_state.benchmark_waypoints = std::move(waypoints);
  m_benchmark_waypoint_index = 0;
  m_benchmark_waypoint_elapsed = 0.0f;
  mark_dirty();
}

void CameraController::advance_benchmark_path(float delta_seconds) {
  if (m_state.benchmark_waypoints.empty()) return;
  m_benchmark_waypoint_elapsed += delta_seconds;
  const auto& wp = m_state.benchmark_waypoints[m_benchmark_waypoint_index];
  if (m_benchmark_waypoint_elapsed >= wp.dwell_seconds) {
    m_benchmark_waypoint_elapsed -= wp.dwell_seconds;
    m_benchmark_waypoint_index = (m_benchmark_waypoint_index + 1) % m_state.benchmark_waypoints.size();
    mark_dirty();
  }
}

bool CameraController::is_dirty(vkpt::core::FrameIndex /*current_frame*/) const {
  return m_dirty;
}

void CameraController::acknowledge_dirty(vkpt::core::FrameIndex current_frame) {
  m_state.last_dirty_frame = current_frame;
  m_dirty = false;
}

const CameraControllerState& CameraController::state() const {
  return m_state;
}

void CameraController::mark_dirty(vkpt::core::FrameIndex frame) {
  m_state.last_dirty_frame = frame;
  m_dirty = true;
}

}  // namespace vkpt::editor
