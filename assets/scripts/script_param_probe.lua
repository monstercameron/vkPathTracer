-- @editor offset_x number default=1.25 min=-10 max=10 step=0.25 label="Offset X"
-- [editor] offset_y number default=0.5 min=-10 max=10 step=0.25 label="Offset Y"
local script = {}

function script.on_update(self, ctx)
  script.live_frame = ctx.frame or 0
  script.live_dt = ctx.dt or ctx.delta_seconds or 0.0
  local transform = self:get_transform()
  if transform == nil then
    return
  end
  transform.translation.x = tonumber(ctx.params.offset_x or "0") or 0
  transform.translation.y = ctx.entity_id == self:id() and 1.0 or -1.0
  self:set_transform(transform)
  if ctx.world:has_component(self:id(), "script") then
    self:log("param probe executed")
  end
end

return script
