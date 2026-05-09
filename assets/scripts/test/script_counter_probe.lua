-- [test/script_counter_probe] Test fixture: increments script-local state every on_update
-- and surfaces the counter via self:log() so tests can read it through dispatch diagnostics.
local script = {}
script._tick = 0

function script.on_update(self, ctx)
  script._tick = script._tick + 1
  self:log("tick=" .. tostring(script._tick))
end

return script
