-- @editor spawn_lift number default=0.18 min=-2 max=2 step=0.01 label="Spawn Lift"
-- @editor origin_x number default=-1.15 min=-10 max=10 step=0.01 label="Origin X"
-- @editor move_x_amplitude number default=0.22 min=0 max=4 step=0.01 label="X Amplitude"
-- @editor move_x_rate number default=1.75 min=0 max=10 step=0.01 label="X Rate"
-- @editor origin_y number default=0.23 min=-10 max=10 step=0.01 label="Origin Y"
-- @editor move_y_amplitude number default=0.08 min=0 max=4 step=0.01 label="Y Amplitude"
-- @editor move_y_rate number default=1.25 min=0 max=10 step=0.01 label="Y Rate"

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

local function transform_or_default(self)
  local transform = self:get_transform()
  if transform ~= nil then
    return transform
  end

  return {
    translation = { x = 0.0, y = 0.0, z = 0.0 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

function script.on_load(self, ctx)
  self:log("mover loaded at frame " .. tostring(ctx.frame))
end

function script.on_spawn(self, ctx)
  local transform = transform_or_default(self)
  transform.translation.y = transform.translation.y + param_number(ctx, "spawn_lift", 0.18)
  self:set_transform(transform)
  self:set_name("Scripted Mover - spawned")
end

function script.on_enable(self, ctx)
  self:log("mover enabled")
end

function script.on_update(self, ctx)
  local transform = transform_or_default(self)
  local t = ctx.elapsed_seconds or 0.0
  transform.translation.x = param_number(ctx, "origin_x", -1.15) +
      math.sin(t * param_number(ctx, "move_x_rate", 1.75)) *
      param_number(ctx, "move_x_amplitude", 0.22)
  transform.translation.y = param_number(ctx, "origin_y", 0.23) +
      math.cos(t * param_number(ctx, "move_y_rate", 1.25)) *
      param_number(ctx, "move_y_amplitude", 0.08)
  self:set_transform(transform)
end

function script.on_late_update(self, ctx)
  if ctx.frame == 0 then
    self:log("mover completed first late update")
  end
end

function script.on_disable(self, ctx)
  self:log("mover disabled")
end

function script.on_destroy(self, ctx)
  self:log("mover destroyed")
end

function script.on_unload(self, ctx)
  self:log("mover unloaded")
end

return script
