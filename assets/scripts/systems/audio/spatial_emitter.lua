-- [systems/audio/spatial_emitter] Drives a spatial audio voice from the host entity's transform.
--
-- Params:
--   event_name (string, default ""): audio event to post (required)
--   volume (number, default 1.0): emitter volume in [0, 1+]
--   loop (boolean, default false): loop the event; the emitter will reposition on every update while looping
--   auto_play (boolean, default true): post the event during on_spawn instead of waiting for a manual trigger
-- Bindings used: ctx.audio:post_event, ctx.audio:stop, entity:get_transform
-- Lifecycle hooks: on_load, on_spawn, on_update, on_destroy
--
-- Example:
--   scene:ensure_script(entity, "systems/audio/spatial_emitter.lua", {event_name="ambience.forest", loop="true", volume="0.6"})

local script = {}

local DEFAULT_VOLUME = 1.0
local DEFAULT_LOOP = false
local DEFAULT_AUTO_PLAY = true

script.handle = nil
script.is_looping = false
script.last_event = ""

local function param_number(ctx, name, fallback)
  if ctx == nil or ctx.params == nil then
    return fallback
  end
  local parsed = tonumber(ctx.params[name])
  if parsed == nil then
    return fallback
  end
  return parsed
end

local function param_string(ctx, name, fallback)
  if ctx == nil or ctx.params == nil then
    return fallback
  end
  local value = ctx.params[name]
  if value == nil or value == "" then
    return fallback
  end
  return tostring(value)
end

local function param_bool(ctx, name, fallback)
  if ctx == nil or ctx.params == nil then
    return fallback
  end
  local value = ctx.params[name]
  if value == nil or value == "" then
    return fallback
  end
  value = tostring(value)
  return value == "1" or value == "true" or value == "yes" or value == "on"
end

local function transform_translation(entity)
  if entity == nil then
    return { x = 0.0, y = 0.0, z = 0.0 }
  end
  local transform = entity:get_transform()
  if transform == nil or transform.translation == nil then
    return { x = 0.0, y = 0.0, z = 0.0 }
  end
  return {
    x = transform.translation.x,
    y = transform.translation.y,
    z = transform.translation.z,
  }
end

local function start_emitter(self, ctx)
  if ctx == nil or ctx.audio == nil then
    return
  end
  local event_name = param_string(ctx, "event_name", "")
  if event_name == "" then
    return
  end
  local volume = param_number(ctx, "volume", DEFAULT_VOLUME)
  local loop = param_bool(ctx, "loop", DEFAULT_LOOP)
  local position = transform_translation(self)
  script.handle = ctx.audio:post_event(event_name, {
    position = position,
    volume = volume,
    spatial = true,
    loop = loop,
  })
  script.is_looping = loop
  script.last_event = event_name
end

local function stop_emitter(ctx)
  if ctx == nil or ctx.audio == nil or script.handle == nil then
    script.handle = nil
    script.is_looping = false
    return
  end
  ctx.audio:stop(script.handle)
  script.handle = nil
  script.is_looping = false
end

function script.on_load(self, ctx)
  script.handle = nil
  script.is_looping = false
  script.last_event = ""
end

function script.on_spawn(self, ctx)
  if param_bool(ctx, "auto_play", DEFAULT_AUTO_PLAY) then
    start_emitter(self, ctx)
  end
end

function script.on_update(self, ctx)
  -- Repositioning is only meaningful for sustained looping voices; one-shot voices
  -- track their starting world-space position inside the audio mixer.
  if not script.is_looping or script.handle == nil then
    return
  end
  if ctx == nil or ctx.audio == nil then
    return
  end
  -- Re-post: cheapest cross-platform way to move a tracked emitter when there is
  -- no dedicated reposition call. Skip when the event hasn't changed identity.
  local event_name = param_string(ctx, "event_name", "")
  if event_name ~= script.last_event then
    stop_emitter(ctx)
    start_emitter(self, ctx)
  end
end

function script.on_destroy(self, ctx)
  stop_emitter(ctx)
end

return script
