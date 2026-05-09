-- [test/script_counter_probe] Test fixture: increments script-local state every on_update
-- and surfaces the counter via self:log() so tests can read it through dispatch diagnostics.
-- on_load and on_disable exist as no-op markers so play->edit->play tests can drive the
-- full mode-toggle sequence; on_load also touches ctx bindings so any host-pointer
-- regression manifests as a binding fault rather than a silent no-op.
local script = {}
script._tick = 0

function script.on_load(self, ctx)
  -- Touch a binding so a stale host pointer would surface here.
  if ctx ~= nil and ctx.world ~= nil then
    local _ = ctx.world:find_entity(self:id())
  end
  self:log("load")
end

function script.on_update(self, ctx)
  script._tick = script._tick + 1
  self:log("tick=" .. tostring(script._tick))
end

function script.on_disable(self, ctx)
  self:log("disable")
end

return script
