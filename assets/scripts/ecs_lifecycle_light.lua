local script = {}

function script.on_spawn(self, ctx)
  self:set_name("Lifecycle Key Light - scripted")
end

function script.on_update(self, ctx)
  local light = self:get_light()
  if light == nil then
    return
  end

  light.intensity = 5.0 + math.sin((ctx.elapsed_seconds or 0.0) * 2.0) * 1.25
  light.radius = 0.55 + math.cos((ctx.elapsed_seconds or 0.0) * 1.4) * 0.08
  self:set_light(light)
end

function script.on_disable(self, ctx)
  local light = self:get_light()
  if light ~= nil then
    light.intensity = 5.0
    light.radius = 0.55
    self:set_light(light)
  end
end

return script
