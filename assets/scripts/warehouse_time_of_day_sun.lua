local script = {}

script.time_of_day_hour = 15.5
script.sunrise_hour = 6.0
script.sunset_hour = 18.5
script.max_elevation_degrees = 62.0
script.azimuth_start_degrees = -98.0
script.azimuth_end_degrees = 84.0
script.sun_distance_meters = 34.0
script.sun_intensity = 900.0
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

local function light_or_default(self)
  local light = self:get_light()
  if light ~= nil then
    return light
  end

  return {
    type = "spot",
    color = { x = 1.0, y = 0.86, z = 0.62 },
    intensity = script.sun_intensity,
    radius = 0.12,
    direction = { x = 0.535, y = -0.566, z = -0.629 },
    beam_angle = 72.0,
    blend = 0.48,
  }
end

local function apply_time_of_day(self)
  local day_span = math.max(0.1, script.sunset_hour - script.sunrise_hour)
  local t = clamp((script.time_of_day_hour - script.sunrise_hour) / day_span, 0.0, 1.0)
  local elevation = math.sin(t * math.pi) * script.max_elevation_degrees
  local azimuth = script.azimuth_start_degrees + (script.azimuth_end_degrees - script.azimuth_start_degrees) * t
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
  transform.translation.x = from_origin_to_sun.x * script.sun_distance_meters
  transform.translation.y = 1.8 + math.max(6.0, from_origin_to_sun.y * script.sun_distance_meters)
  transform.translation.z = from_origin_to_sun.z * script.sun_distance_meters
  self:set_transform(transform)

  local warm = 1.0 - clamp(elevation / 45.0, 0.0, 1.0)
  local brightness = math.max(0.22, math.sin(elevation_rad))
  local light = light_or_default(self)
  light.type = "spot"
  light.color = {
    x = 1.0,
    y = 0.78 + (1.0 - warm) * 0.16,
    z = 0.52 + (1.0 - warm) * 0.34,
  }
  light.intensity = script.sun_intensity * brightness
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
  apply_time_of_day(self)
end

function script.on_update(self, ctx)
  apply_time_of_day(self)
end

return script
