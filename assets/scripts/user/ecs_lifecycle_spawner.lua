-- [user/ecs_lifecycle_spawner] Spawns a scripted child entity once at on_spawn for the ECS lifecycle scripting demo.
-- @editor child_name text default="Script Spawned Child" label="Child Name"
-- @editor child_source text default="assets/scripts/user/ecs_lifecycle_spawned_child.lua" label="Child Script"
-- @editor child_offset_y number default=0.95 min=-4 max=4 step=0.01 label="Child Offset Y"
-- @editor child_offset_z number default=-0.01 min=-4 max=4 step=0.01 label="Child Offset Z"
-- @editor child_scale_x number default=0.45 min=0.01 max=4 step=0.01 label="Child Scale X"
-- @editor child_scale_y number default=0.45 min=0.01 max=4 step=0.01 label="Child Scale Y"
-- @editor child_scale_z number default=1.0 min=0.01 max=4 step=0.01 label="Child Scale Z"
-- @editor child_mesh_id number default=7202 min=0 max=100000 step=1 label="Child Mesh ID"
-- @editor child_material_id number default=7103 min=0 max=100000 step=1 label="Child Material ID"

local script = {}

local function param_number(ctx, name, fallback)
  if ctx == nil or ctx.params == nil then
    return fallback
  end
  local parsed = tonumber(ctx.params[name])
  if parsed == nil then
    return fallback
  end
  return parsed
end

local function param_string(ctx, name, fallback)
  if ctx == nil or ctx.params == nil or ctx.params[name] == nil or ctx.params[name] == "" then
    return fallback
  end
  return ctx.params[name]
end

function script.on_spawn(self, ctx)
  local child = ctx.world:spawn_entity({
    name = param_string(ctx, "child_name", "Script Spawned Child"),
    parent = self:id(),
    transform = {
      translation = {
        x = 0.0,
        y = param_number(ctx, "child_offset_y", 0.95),
        z = param_number(ctx, "child_offset_z", -0.01),
      },
      rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
      scale = {
        x = param_number(ctx, "child_scale_x", 0.45),
        y = param_number(ctx, "child_scale_y", 0.45),
        z = param_number(ctx, "child_scale_z", 1.0),
      },
    },
    mesh = {
      mesh_id = math.floor(param_number(ctx, "child_mesh_id", 7202)),
      material_id = math.floor(param_number(ctx, "child_material_id", 7103)),
    },
    script = {
      source = param_string(ctx, "child_source", "assets/scripts/user/ecs_lifecycle_spawned_child.lua"),
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
