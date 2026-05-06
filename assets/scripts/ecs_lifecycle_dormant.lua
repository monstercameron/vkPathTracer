local script = {}

function script.on_enable(self, ctx)
  self:set_name("Dormant Script - enabled")
end

function script.on_update(self, ctx)
  local transform = self:get_transform()
  if transform == nil then
    return
  end

  transform.scale.x = 1.0 + math.sin((ctx.elapsed_seconds or 0.0) * 4.0) * 0.15
  transform.scale.y = 1.0 + math.cos((ctx.elapsed_seconds or 0.0) * 4.0) * 0.15
  self:set_transform(transform)
end

function script.on_disable(self, ctx)
  self:set_name("Dormant Script - disabled binding")
end

return script
