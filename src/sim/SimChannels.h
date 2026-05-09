#pragma once

// Channels exchanged between the Qt UI thread and the simulation worker
// thread. See doc comment on SimWorker for the data-flow contract.
//
// The two structures here are POD-ish: they own only stdlib containers and
// trivially-copyable scalars, so they can be moved/copied across threads with
// no synchronization beyond the carrier (MpscRing / LatestSlot).
//
// SimInputFrame: UI -> Sim. One per Qt main-loop tick. Carries the input
//   snapshot the sim needs to advance one simulation step. The sequence
//   field is monotonically increasing on the UI side; consumers may use it
//   to detect dropped frames.
//
// UiSimMirror:  Sim -> UI. Latest-wins. UI reads this every repaint to
//   populate dock panels, status overlays, and the rendered framebuffer's
//   per-frame metadata. The UI must NOT take a reference into live sim
//   state — only this mirror.

#include <cstdint>
#include <string>
#include <vector>

namespace vkpt::sim {

struct SimInputFrame {
  std::uint64_t sequence = 0u;
  float dt = 0.0f;
  std::uint64_t frame_index = 0u;

  // Input snapshot.
  std::vector<int> keys_down;
  float mouse_delta_x = 0.0f;
  float mouse_delta_y = 0.0f;
  float mouse_wheel = 0.0f;
  std::uint32_t mouse_buttons_mask = 0u;
  bool viewport_focused = false;
  bool gizmo_drag_active = false;

  // Intent flags.
  bool play_mode = false;
  bool fps_mode = false;
  bool live_edit_armed = false;
};

struct CameraPose {
  float position_x = 0.0f;
  float position_y = 0.0f;
  float position_z = 0.0f;
  float forward_x = 0.0f;
  float forward_y = 0.0f;
  float forward_z = -1.0f;
  float up_x = 0.0f;
  float up_y = 1.0f;
  float up_z = 0.0f;
};

struct UiSimMirror {
  std::uint64_t sim_frame = 0u;
  CameraPose camera_pose{};
  std::string preview_status;
  std::uint32_t sample_index = 0u;
  std::uint32_t image_width = 0u;
  std::uint32_t image_height = 0u;
  std::uint32_t bindings_runnable = 0u;
  std::uint32_t bindings_disabled = 0u;
  std::uint64_t physics_steps_total = 0u;
  std::uint64_t script_dispatches_total = 0u;
};

}  // namespace vkpt::sim
