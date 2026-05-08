local script = {}

local CONFIG = {
  player = "Lua Audio Player",
  player_script = "assets/scripts/audio_interaction_demo.lua",
  interactables = {
    pickup = {
      entity = "Pickup Audio Marker",
      sound = "pickup.collect",
      prompt = "E  Pickup",
      radius = 1.8,
    },
    terminal = {
      entity = "Terminal Audio Marker",
      sound = "objective.terminal.disabled",
      prompt = "E  Terminal",
      radius = 2.0,
    },
    ambience = {
      entity = "Ambient Forest Emitter",
      sound = "ambience.forest",
      prompt = "E  Listen",
      radius = 3.0,
    },
  },
}

local function load_config(ctx)
  if ctx ~= nil and type(ctx.include) == "function" then
    local ok, loaded = pcall(function()
      return ctx:include("assets/scripts/scenes/audio_lua_interaction_demo/config.lua")
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
  local player = scene:find_entity(config.player)
  if player ~= nil then
    scene:ensure_script(player, config.player_script, {
      scene_init = "audio_lua_interaction_demo",
    })
  end
  for _, item in pairs(config.interactables or {}) do
    local entity = scene:find_entity(item.entity)
    if entity ~= nil then
      scene:register_interactable(entity, item)
    end
  end
  if ctx ~= nil and type(ctx.diagnostic) == "function" then
    ctx:diagnostic("info", "audio interaction scene init applied")
  end
end

return script
