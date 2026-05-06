local script = {}

local function transform_or_default(self)
  local transform = self:get_transform()
  if transform ~= nil then
    return transform
  end

  return {
    translation = { x = 0.0, y = 0.0, z = 0.0 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

function script.on_load(self, ctx)
  self:log("mover loaded at frame " .. tostring(ctx.frame))
end

function script.on_spawn(self, ctx)
  local transform = transform_or_default(self)
  transform.translation.y = transform.translation.y + 0.18
  self:set_transform(transform)
  self:set_name("Scripted Mover - spawned")
end

function script.on_enable(self, ctx)
  self:log("mover enabled")
end

function script.on_update(self, ctx)
  local transform = transform_or_default(self)
  local t = ctx.elapsed_seconds or 0.0
  transform.translation.x = -1.15 + math.sin(t * 1.75) * 0.22
  transform.translation.y = 0.23 + math.cos(t * 1.25) * 0.08
  self:set_transform(transform)
end

function script.on_late_update(self, ctx)
  if ctx.frame == 0 then
    self:log("mover completed first late update")
  end
end

function script.on_disable(self, ctx)
  self:log("mover disabled")
end

function script.on_destroy(self, ctx)
  self:log("mover destroyed")
end

function script.on_unload(self, ctx)
  self:log("mover unloaded")
end

return script
