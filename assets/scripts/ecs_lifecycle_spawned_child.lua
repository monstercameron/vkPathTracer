local script = {}

function script.on_spawn(self, ctx)
  self:set_name("Script Spawned Child - on_spawn complete")
  self:log("debug spawn_frame")
end

function script.on_update(self, ctx)
  local transform = self:get_transform()
  if transform == nil then
    return
  end

  transform.translation.y = 0.95 + math.sin((ctx.elapsed_seconds or 0.0) * 3.0) * 0.06
  self:set_transform(transform)
end

function script.on_destroy(self, ctx)
  self:log("spawned child destroyed")
end

return script
