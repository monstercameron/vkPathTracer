local script = {}

function script.on_spawn(self, ctx)
  local child = ctx.world:spawn_entity({
    name = "Script Spawned Child",
    parent = self:id(),
    transform = {
      translation = { x = 0.0, y = 0.95, z = -0.01 },
      rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
      scale = { x = 0.45, y = 0.45, z = 1.0 },
    },
    mesh = {
      mesh_id = 7202,
      material_id = 7103,
    },
    script = {
      source = "assets/scripts/ecs_lifecycle_spawned_child.lua",
      language = "lua",
      entry = "default",
      enabled = true,
    },
  })

  self:set_name("Scripted Spawner - child requested")
  if child ~= nil then
    self:log("spawned child " .. tostring(child:id()))
  end
end

function script.on_update(self, ctx)
  if ctx.frame == 120 then
    self:log("spawner reached frame 120")
  end
end

function script.on_destroy(self, ctx)
  self:log("spawner destroyed after issuing spawn request")
end

return script
