-- @editor time_of_day_hour number default=15.5 min=0 max=24 step=0.25 label="Time Of Day"
-- @editor sunrise_hour number default=6.0 min=0 max=12 step=0.25 label="Sunrise Hour"
-- @editor sunset_hour number default=18.5 min=12 max=24 step=0.25 label="Sunset Hour"
-- @editor max_elevation_degrees number default=62.0 min=5 max=90 step=1 label="Max Elevation"
-- @editor azimuth_start_degrees number default=-98.0 min=-180 max=180 step=1 label="Azimuth Start"
-- @editor azimuth_end_degrees number default=84.0 min=-180 max=180 step=1 label="Azimuth End"
-- @editor sun_distance_meters number default=34.0 min=1 max=120 step=1 label="Sun Distance"
-- @editor sun_intensity number default=900.0 min=0 max=5000 step=25 label="Sun Intensity"
local script = {}

local DEFAULTS = {
  time_of_day_hour = 15.5,
  sunrise_hour = 6.0,
  sunset_hour = 18.5,
  max_elevation_degrees = 62.0,
  azimuth_start_degrees = -98.0,
  azimuth_end_degrees = 84.0,
  sun_distance_meters = 34.0,
  sun_intensity = 900.0,
}

script.last_elevation_degrees = 0.0
script.last_azimuth_degrees = 0.0
script.last_direction = "not evaluated"

local function clamp(v, lo, hi)
  if v < lo then
    return lo
  end
  if v > hi then
    return hi
  end
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
    translation = { x = -17.0, y = 18.0, z = 20.0 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

local function light_or_default(self, sun_intensity)
  local light = self:get_light()
  if light ~= nil then
    return light
  end

  return {
    type = "spot",
    color = { x = 1.0, y = 0.86, z = 0.62 },
    intensity = sun_intensity,
    radius = 0.12,
    direction = { x = 0.535, y = -0.566, z = -0.629 },
    beam_angle = 72.0,
    blend = 0.48,
  }
end

local function apply_time_of_day(self, ctx)
  local time_of_day_hour = param_number(ctx, "time_of_day_hour")
  local sunrise_hour = param_number(ctx, "sunrise_hour")
  local sunset_hour = param_number(ctx, "sunset_hour")
  local max_elevation_degrees = param_number(ctx, "max_elevation_degrees")
  local azimuth_start_degrees = param_number(ctx, "azimuth_start_degrees")
  local azimuth_end_degrees = param_number(ctx, "azimuth_end_degrees")
  local sun_distance_meters = param_number(ctx, "sun_distance_meters")
  local sun_intensity = param_number(ctx, "sun_intensity")

  local day_span = math.max(0.1, sunset_hour - sunrise_hour)
  local t = clamp((time_of_day_hour - sunrise_hour) / day_span, 0.0, 1.0)
  local elevation = math.sin(t * math.pi) * max_elevation_degrees
  local azimuth = azimuth_start_degrees + (azimuth_end_degrees - azimuth_start_degrees) * t
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
  transform.translation.x = from_origin_to_sun.x * sun_distance_meters
  transform.translation.y = 1.8 + math.max(6.0, from_origin_to_sun.y * sun_distance_meters)
  transform.translation.z = from_origin_to_sun.z * sun_distance_meters
  self:set_transform(transform)

  local warm = 1.0 - clamp(elevation / 45.0, 0.0, 1.0)
  local brightness = math.max(0.22, math.sin(elevation_rad))
  local light = light_or_default(self, sun_intensity)
  light.type = "spot"
  light.color = {
    x = 1.0,
    y = 0.78 + (1.0 - warm) * 0.16,
    z = 0.52 + (1.0 - warm) * 0.34,
  }
  light.intensity = sun_intensity * brightness
  light.radius = 0.12
  light.direction = from_sun_to_origin
  light.beam_angle = 72.0
  light.blend = 0.48
  self:set_light(light)

  script.last_elevation_degrees = elevation
  script.last_azimuth_degrees = azimuth
  script.last_direction = string.format("%.3f, %.3f, %.3f", from_sun_to_origin.x, from_sun_to_origin.y, from_sun_to_origin.z)
end

function script.on_load(self, ctx)
  apply_time_of_day(self, ctx)
end

function script.on_update(self, ctx)
  apply_time_of_day(self, ctx)
end

return script
