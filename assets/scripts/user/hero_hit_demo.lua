-- [user/hero_hit_demo] Hero plays idle animation; at t=2s, a scripted hit
-- triggers ragdoll mode with a backward+upward impulse to the chest.
-- The script is the user-visible authority arbitration story: animation
-- drives joints up to t=2s, then the ragdoll component takes over and
-- physics drives them.

local script = {}
local self_state = { hit_at_t = 2.0, fired = false, elapsed = 0.0 }

function script.on_load(self, ctx)
  -- Set the idle clip. The asset loader names this clip "idle"; if the
  -- runtime can't find it the binding silently no-ops.
  if self.set_animation_clip ~= nil then
    self:set_animation_clip("idle")
  end
  if self.set_animation_speed ~= nil then
    self:set_animation_speed(1.0)
  end
  if self.resume_animation ~= nil then
    self:resume_animation()
  end
end

function script.on_update(self, ctx)
  local dt = 0.0
  if ctx ~= nil and ctx.dt ~= nil then
    dt = ctx.dt
  end
  self_state.elapsed = self_state.elapsed + dt
  if not self_state.fired and self_state.elapsed >= self_state.hit_at_t then
    self_state.fired = true
    if self.enable_ragdoll ~= nil then
      self:enable_ragdoll({
        seed_from_animation = true,
        impulse = { x = 0.0, y = 5.0, z = -10.0 },  -- backward + slightly up
        impulse_joint = "body",
      })
    end
    if self.log ~= nil then
      self:log("hero_hit_demo: ragdoll triggered at t=" .. tostring(self_state.elapsed))
    end
  end
end

return script
