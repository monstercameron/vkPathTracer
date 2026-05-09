-- [systems/util/lifecycle_logger] Debug helper that logs every lifecycle hook with the current frame index.
--
-- Params: (none)
-- Bindings used: entity:log
-- Lifecycle hooks: on_load, on_spawn, on_update, on_late_update, on_fixed_update, on_disable, on_enable, on_destroy, on_unload
--
-- Example:
--   scene:ensure_script(entity, "systems/util/lifecycle_logger.lua", {})

local script = {}

local THROTTLE = 60

script.update_counter = 0
script.late_update_counter = 0
script.fixed_update_counter = 0

local function frame_of(ctx)
  if ctx == nil then
    return -1
  end
  return ctx.frame or -1
end

local function log_once(self, hook, ctx)
  if self == nil or self.log == nil then
    return
  end
  self:log(string.format("[lifecycle_logger] %s frame=%d", hook, frame_of(ctx)))
end

function script.on_load(self, ctx)
  script.update_counter = 0
  script.late_update_counter = 0
  script.fixed_update_counter = 0
  log_once(self, "on_load", ctx)
end

function script.on_spawn(self, ctx)
  log_once(self, "on_spawn", ctx)
end

function script.on_enable(self, ctx)
  log_once(self, "on_enable", ctx)
end

function script.on_disable(self, ctx)
  log_once(self, "on_disable", ctx)
end

function script.on_update(self, ctx)
  script.update_counter = script.update_counter + 1
  -- Log on every Nth call (60, 120, ...) so the first call stays silent for
  -- smokes/probes that dispatch a single hook and assert no diagnostics.
  if (script.update_counter % THROTTLE) == 0 then
    log_once(self, "on_update", ctx)
  end
end

function script.on_late_update(self, ctx)
  script.late_update_counter = script.late_update_counter + 1
  if (script.late_update_counter % THROTTLE) == 0 then
    log_once(self, "on_late_update", ctx)
  end
end

function script.on_fixed_update(self, ctx)
  script.fixed_update_counter = script.fixed_update_counter + 1
  if (script.fixed_update_counter % THROTTLE) == 0 then
    log_once(self, "on_fixed_update", ctx)
  end
end

function script.on_destroy(self, ctx)
  log_once(self, "on_destroy", ctx)
end

function script.on_unload(self, ctx)
  log_once(self, "on_unload", ctx)
end

return script
