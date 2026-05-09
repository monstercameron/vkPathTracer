-- [user/representative_idle_tick] Trivial cheap-tick script used to populate the representative acceptance scene with many active scripts.
-- representative_idle_tick.lua
--
-- Trivial cheap-tick script used to populate the representative acceptance
-- scene with 49 active scripts (alongside the FPS gameplay script). Each
-- instance increments a per-entity counter and applies a tiny sinusoid to
-- its scale so the script_apply_commands_us / scripts_us paths execute end
-- to end without dominating frame budget.
--
-- @editor amplitude number default=0.02 min=0 max=0.5 step=0.005 label="Pulse Amplitude"
-- @editor rate number default=1.0 min=0 max=10 step=0.1 label="Pulse Rate"

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
  self._tick_count = 0
end

function script.on_update(self, ctx)
  self._tick_count = (self._tick_count or 0) + 1
  local transform = self:get_transform()
  if transform == nil then
    return
  end
  local rate = param_number(ctx, "rate", 1.0)
  local amplitude = param_number(ctx, "amplitude", 0.02)
  local elapsed = (ctx and ctx.elapsed_seconds) or 0.0
  local pulse = 1.0 + math.sin(elapsed * rate) * amplitude
  transform.scale.x = pulse
  transform.scale.y = pulse
  transform.scale.z = pulse
  self:set_transform(transform)
end

return script
