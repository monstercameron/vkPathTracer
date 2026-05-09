-- [user/ecs_lifecycle_dormant] Dormant entity script that pulses scale; used by the ECS lifecycle scripting demo.
-- @editor scale_amplitude number default=0.15 min=0 max=2 step=0.01 label="Scale Amplitude"
-- @editor scale_rate number default=4.0 min=0 max=12 step=0.1 label="Scale Rate"

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

function script.on_enable(self, ctx)
  self:set_name("Dormant Script - enabled")
end

function script.on_update(self, ctx)
  local transform = self:get_transform()
  if transform == nil then
    return
  end

  local rate = param_number(ctx, "scale_rate", 4.0)
  local amplitude = param_number(ctx, "scale_amplitude", 0.15)
  transform.scale.x = 1.0 + math.sin((ctx.elapsed_seconds or 0.0) * rate) * amplitude
  transform.scale.y = 1.0 + math.cos((ctx.elapsed_seconds or 0.0) * rate) * amplitude
  self:set_transform(transform)
end

function script.on_disable(self, ctx)
  self:set_name("Dormant Script - disabled binding")
end

return script
