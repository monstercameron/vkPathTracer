#include "app/ViewportInteraction.h"

#ifdef PT_ENABLE_QT

#include <algorithm>
#include <cmath>

namespace vkpt::app {

namespace {

ViewportMouseClickOwner ClickOwnerFor(ViewportMouseInputMode mode, int button) {
  if (button != 0) {
    return ViewportMouseClickOwner::NonPrimaryButton;
  }
  switch (mode) {
    case ViewportMouseInputMode::Editor:
      return ViewportMouseClickOwner::EditorSelection;
    case ViewportMouseInputMode::FpsCamera:
      return ViewportMouseClickOwner::FpsCamera;
    case ViewportMouseInputMode::GameModeScript:
      return ViewportMouseClickOwner::GameModeScript;
    default:
      return ViewportMouseClickOwner::None;
  }
}

}  // namespace

void ResetViewportMouseClick(ViewportMouseClickState& state) {
  state = {};
}

void BeginViewportMouseClick(ViewportMouseClickState& state,
                             ViewportMouseInputMode mode,
                             int button,
                             float x,
                             float y) {
  state.phase = ViewportMouseClickPhase::Pressed;
  state.owner = ClickOwnerFor(mode, button);
  state.button = button;
  state.press_x = x;
  state.press_y = y;
  state.drag_pixels = 0.0f;
}

float UpdateViewportMouseClickDrag(ViewportMouseClickState& state, float x, float y) {
  if (state.phase == ViewportMouseClickPhase::Idle) {
    return 0.0f;
  }
  const float dx = x - state.press_x;
  const float dy = y - state.press_y;
  state.drag_pixels = std::max(state.drag_pixels, std::sqrt(dx * dx + dy * dy));
  if (state.drag_pixels > 0.0f) {
    state.phase = ViewportMouseClickPhase::Dragging;
  }
  return state.drag_pixels;
}

void MarkViewportMouseClickDrag(ViewportMouseClickState& state) {
  if (state.phase != ViewportMouseClickPhase::Idle) {
    state.phase = ViewportMouseClickPhase::Dragging;
  }
}

ViewportMouseClickResult EndViewportMouseClick(ViewportMouseClickState& state,
                                               ViewportMouseInputMode currentMode,
                                               int button,
                                               float x,
                                               float y,
                                               float clickThresholdPixels) {
  ViewportMouseClickResult result{};
  result.owner = state.owner;
  result.drag_pixels = UpdateViewportMouseClickDrag(state, x, y);

  const bool matchingButton = state.phase != ViewportMouseClickPhase::Idle &&
                              button == state.button;
  const bool withinClickThreshold = result.drag_pixels <= clickThresholdPixels;
  result.editor_pick_allowed =
      matchingButton &&
      withinClickThreshold &&
      state.owner == ViewportMouseClickOwner::EditorSelection &&
      currentMode == ViewportMouseInputMode::Editor;
  result.game_mode_click =
      matchingButton &&
      withinClickThreshold &&
      state.owner == ViewportMouseClickOwner::GameModeScript;
  result.suppressed_editor_pick =
      matchingButton &&
      withinClickThreshold &&
      !result.editor_pick_allowed &&
      (state.owner == ViewportMouseClickOwner::GameModeScript ||
       state.owner == ViewportMouseClickOwner::FpsCamera);

  ResetViewportMouseClick(state);
  return result;
}

}  // namespace vkpt::app

#endif
