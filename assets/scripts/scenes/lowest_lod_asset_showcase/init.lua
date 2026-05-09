local script = {}

local CONFIG = {
  camera_system = "systems.generic_fps_camera",
  camera_script = "assets/scripts/systems/generic_fps_camera.lua",
  camera_name = "FPS warehouse walkthrough camera",
  sun_name = "Lua time of day sun",
  sun_script = "assets/scripts/user/warehouse_time_of_day_sun.lua",
  camera_params = {
    movement_mode = "walk",
    walk_speed = "4.2",
    run_multiplier = "1.75",
    mouse_yaw_sensitivity = "0.0020",
    mouse_pitch_sensitivity = "0.0016",
    show_controls = "true",
    controls_title = "Warehouse FPS Controls",
  },
}

local function load_config(ctx)
  if ctx ~= nil and type(ctx.include) == "function" then
    local ok, loaded = pcall(function()
      return ctx:include("assets/scripts/scenes/lowest_lod_asset_showcase/config.lua")
    end)
    if ok and type(loaded) == "table" then
      return loaded
    end
  end
  return CONFIG
end

function script.on_load(self, ctx)
  local scene = ctx ~= nil and ctx.scene or nil
  if scene == nil then
    return
  end
  local config = load_config(ctx)
  local system = scene:use_system(config.camera_system)
  local camera_script = system ~= nil and system.source or config.camera_script
  local camera = scene:find_entity(config.camera_name) or scene:main_camera()
  if camera ~= nil then
    scene:ensure_script(camera, camera_script, config.camera_params)
  end
  local sun = scene:find_entity(config.sun_name)
  if sun ~= nil then
    scene:ensure_script(sun, config.sun_script, {
      scene_init = "lowest_lod_asset_showcase",
    })
  end
  if ctx ~= nil and type(ctx.diagnostic) == "function" then
    ctx:diagnostic("info", "lowest LOD showcase scene init applied")
  end
end

return script
