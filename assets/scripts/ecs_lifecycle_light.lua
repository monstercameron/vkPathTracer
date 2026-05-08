-- @editor base_intensity number default=5.0 min=0 max=25 step=0.1 label="Base Intensity"
-- @editor intensity_amplitude number default=1.25 min=0 max=10 step=0.1 label="Intensity Amplitude"
-- @editor intensity_rate number default=2.0 min=0 max=10 step=0.1 label="Intensity Rate"
-- @editor base_radius number default=0.55 min=0 max=8 step=0.01 label="Base Radius"
-- @editor radius_amplitude number default=0.08 min=0 max=4 step=0.01 label="Radius Amplitude"
-- @editor radius_rate number default=1.4 min=0 max=10 step=0.1 label="Radius Rate"

local script = {}

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

function script.on_spawn(self, ctx)
  self:set_name("Lifecycle Key Light - scripted")
end

function script.on_update(self, ctx)
  local light = self:get_light()
  if light == nil then
    return
  end

  light.intensity = param_number(ctx, "base_intensity", 5.0) +
      math.sin((ctx.elapsed_seconds or 0.0) * param_number(ctx, "intensity_rate", 2.0)) *
      param_number(ctx, "intensity_amplitude", 1.25)
  light.radius = param_number(ctx, "base_radius", 0.55) +
      math.cos((ctx.elapsed_seconds or 0.0) * param_number(ctx, "radius_rate", 1.4)) *
      param_number(ctx, "radius_amplitude", 0.08)
  self:set_light(light)
end

function script.on_disable(self, ctx)
  local light = self:get_light()
  if light ~= nil then
    light.intensity = param_number(ctx, "base_intensity", 5.0)
    light.radius = param_number(ctx, "base_radius", 0.55)
    self:set_light(light)
  end
end

return script
