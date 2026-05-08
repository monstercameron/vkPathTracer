-- Authoring defaults for the default FPS bootstrap. init.lua mirrors these
-- values until the sandbox exposes safe script include support.
return {
  system_source = "assets/scripts/systems/generic_fps_camera.lua",
  system_module = "systems.generic_fps_camera",
  camera_name = "main_camera",
  input_capture = "game_mode",
  params = {
    movement_mode = "walk",
    walk_speed = "4.2",
    run_multiplier = "1.75",
    mouse_yaw_sensitivity = "0.0020",
    mouse_pitch_sensitivity = "0.0016",
    max_mouse_delta = "120.0",
    min_pitch_degrees = "-82.0",
    max_pitch_degrees = "82.0",
    enable_vertical = "false",
    show_controls = "true",
    controls_panel_name = "FPS Camera Controls Panel",
    controls_title = "FPS Camera Controls",
    controls_anchor = "top_left",
  },
}
