local script = {}

-- Mirrored from config.lua because the current sandbox intentionally blocks
-- require/dofile/loadfile.
local CONFIG = {
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

local function copy_params(params)
  local out = {}
  for key, value in pairs(params or {}) do
    out[key] = value
  end
  return out
end

local function emit(ctx, self, level, message)
  if ctx ~= nil and type(ctx.diagnostic) == "function" then
    ctx:diagnostic(level, message, {
      source = "assets/scripts/scenes/default_fps/init.lua",
      system = CONFIG.system_module,
    })
    return
  end
  if self ~= nil and type(self.log) == "function" then
    self:log(level .. ": " .. message)
  end
end

local function find_main_camera(scene)
  if scene == nil then
    return nil, "scene bootstrap API unavailable"
  end
  if type(scene.main_camera) == "function" then
    local camera = scene:main_camera()
    if camera ~= nil then
      return camera, "scene:main_camera"
    end
  end
  if CONFIG.camera_name ~= nil and type(scene.find_entity) == "function" then
    local camera = scene:find_entity(CONFIG.camera_name)
    if camera ~= nil then
      return camera, "configured camera name"
    end
  end
  return nil, "no main camera found"
end

local function bootstrap(self, ctx)
  local scene = ctx ~= nil and ctx.scene or nil
  local camera, reason = find_main_camera(scene)
  if camera == nil then
    emit(ctx, self, "warning", "default FPS bootstrap skipped: " .. reason)
    return false
  end
  if type(scene.ensure_script) ~= "function" then
    emit(ctx, self, "warning",
        "default FPS bootstrap found a camera but scene:ensure_script is unavailable")
    return false
  end

  local params = copy_params(CONFIG.params)
  params.bootstrap_source = "assets/scripts/scenes/default_fps/init.lua"
  params.input_capture = CONFIG.input_capture

  local ok = scene:ensure_script(camera, CONFIG.system_source, params)
  if ok == false then
    emit(ctx, self, "error", "default FPS bootstrap could not attach " .. CONFIG.system_source)
    return false
  end

  emit(ctx, self, "info", "default FPS bootstrap attached via " .. reason)
  return true
end

function script.on_load(self, ctx)
  bootstrap(self, ctx)
end

function script.on_spawn(self, ctx)
  bootstrap(self, ctx)
end

script.config = CONFIG
script.bootstrap = bootstrap

return script
