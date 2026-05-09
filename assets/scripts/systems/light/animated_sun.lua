-- [systems/light/animated_sun] Time-of-day driver for a directional sun light; animates direction, position, and color.
--
-- Params:
--   cycle_seconds (number, default 120.0): seconds for a full sunrise-to-sunset cycle
--   azimuth_offset_deg (number, default 0.0): yaw rotation applied to the sun arc, in degrees
--   peak_intensity (number, default 1.5): light intensity at solar noon
--   pause_at_noon_seconds (number, default 0.0): optional dwell at peak elevation
-- Bindings used: entity:get_light, entity:set_light, entity:get_transform, entity:set_transform, ctx.elapsed_seconds
-- Lifecycle hooks: on_load, on_update
--
-- Example:
--   scene:ensure_script(entity, "systems/light/animated_sun.lua", {cycle_seconds="60", peak_intensity="2.0"})

local script = {}

local DEFAULTS = {
  cycle_seconds = 120.0,
  azimuth_offset_deg = 0.0,
  peak_intensity = 1.5,
  pause_at_noon_seconds = 0.0,
}

local MAX_ELEVATION_DEGREES = 70.0
local SUN_DISTANCE_METERS = 30.0

script.last_elevation_degrees = 0.0
script.last_azimuth_degrees = 0.0

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function radians(degrees)
  return degrees * math.pi / 180.0
end

local function normalize(v)
  local len = math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
  if len <= 0.000001 then
    return { x = 0.0, y = -1.0, z = 0.0 }
  end
  return { x = v.x / len, y = v.y / len, z = v.z / len }
end

local function param_number(ctx, name)
  local fallback = DEFAULTS[name]
  if ctx == nil or ctx.params == nil then
    return fallback
  end
  local value = tonumber(ctx.params[name])
  if value == nil then
    return fallback
  end
  return value
end

local function transform_or_default(self)
  local transform = self:get_transform()
  if transform ~= nil then
    return transform
  end
  return {
    translation = { x = 0.0, y = SUN_DISTANCE_METERS, z = 0.0 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

local function light_or_default(self, peak_intensity)
  local light = self:get_light()
  if light ~= nil then
    return light
  end
  return {
    type = "directional",
    color = { x = 1.0, y = 0.9, z = 0.75 },
    intensity = peak_intensity,
    direction = { x = 0.0, y = -1.0, z = 0.0 },
  }
end

-- Phase math: derive a normalized day-progress t in [0, 1] from elapsed time and
-- the cycle length. The dwell window stretches t around 0.5 so the sun pauses
-- visually at peak without freezing the entire script tick.
local function compute_phase(elapsed_seconds, cycle_seconds, pause_seconds)
  cycle_seconds = math.max(0.5, cycle_seconds)
  pause_seconds = math.max(0.0, pause_seconds)
  local total_seconds = cycle_seconds + pause_seconds
  local cursor = elapsed_seconds % total_seconds
  if pause_seconds > 0.0 then
    local before_noon = cycle_seconds * 0.5
    local after_noon_start = before_noon + pause_seconds
    if cursor < before_noon then
      return cursor / cycle_seconds
    elseif cursor < after_noon_start then
      return 0.5
    else
      return 0.5 + (cursor - after_noon_start) / cycle_seconds
    end
  end
  return cursor / cycle_seconds
end

local function apply_time_of_day(self, ctx)
  local cycle_seconds = param_number(ctx, "cycle_seconds")
  local azimuth_offset_deg = param_number(ctx, "azimuth_offset_deg")
  local peak_intensity = param_number(ctx, "peak_intensity")
  local pause_at_noon_seconds = param_number(ctx, "pause_at_noon_seconds")

  local elapsed = ctx ~= nil and (ctx.elapsed_seconds or 0.0) or 0.0
  local t = clamp(compute_phase(elapsed, cycle_seconds, pause_at_noon_seconds), 0.0, 1.0)

  -- Same elevation/azimuth construction as the warehouse demo: elevation peaks
  -- at noon (sin curve), azimuth sweeps linearly across the sky.
  local elevation = math.sin(t * math.pi) * MAX_ELEVATION_DEGREES
  local azimuth = azimuth_offset_deg - 90.0 + 180.0 * t
  local elevation_rad = radians(elevation)
  local azimuth_rad = radians(azimuth)
  local cos_elevation = math.cos(elevation_rad)

  local from_origin_to_sun = normalize({
    x = math.sin(azimuth_rad) * cos_elevation,
    y = math.sin(elevation_rad),
    z = math.cos(azimuth_rad) * cos_elevation,
  })
  local from_sun_to_origin = normalize({
    x = -from_origin_to_sun.x,
    y = -from_origin_to_sun.y,
    z = -from_origin_to_sun.z,
  })

  local transform = transform_or_default(self)
  transform.translation.x = from_origin_to_sun.x * SUN_DISTANCE_METERS
  transform.translation.y = math.max(2.0, from_origin_to_sun.y * SUN_DISTANCE_METERS)
  transform.translation.z = from_origin_to_sun.z * SUN_DISTANCE_METERS
  self:set_transform(transform)

  local warm = 1.0 - clamp(elevation / 45.0, 0.0, 1.0)
  local brightness = math.max(0.15, math.sin(elevation_rad))
  local light = light_or_default(self, peak_intensity)
  light.color = {
    x = 1.0,
    y = 0.78 + (1.0 - warm) * 0.16,
    z = 0.52 + (1.0 - warm) * 0.34,
  }
  light.intensity = peak_intensity * brightness
  light.direction = from_sun_to_origin
  self:set_light(light)

  script.last_elevation_degrees = elevation
  script.last_azimuth_degrees = azimuth
end

function script.on_load(self, ctx)
  apply_time_of_day(self, ctx)
end

function script.on_update(self, ctx)
  apply_time_of_day(self, ctx)
end

return script
