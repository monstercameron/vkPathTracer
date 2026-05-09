-- [user/ecs_lifecycle_spawned_child] Per-child bob animation for entities the lifecycle spawner produces at runtime.
-- @editor base_y number default=0.95 min=-4 max=4 step=0.01 label="Base Y"
-- @editor bob_amplitude number default=0.06 min=0 max=2 step=0.01 label="Bob Amplitude"
-- @editor bob_rate number default=3.0 min=0 max=12 step=0.1 label="Bob Rate"

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
  self:set_name("Script Spawned Child - on_spawn complete")
  self:log("debug spawn_frame")
end

function script.on_update(self, ctx)
  local transform = self:get_transform()
  if transform == nil then
    return
  end

  transform.translation.y = param_number(ctx, "base_y", 0.95) +
      math.sin((ctx.elapsed_seconds or 0.0) * param_number(ctx, "bob_rate", 3.0)) *
      param_number(ctx, "bob_amplitude", 0.06)
  self:set_transform(transform)
end

function script.on_destroy(self, ctx)
  self:log("spawned child destroyed")
end

return script
