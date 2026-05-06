local script = {}

local first_id = 12000
local count = 18

local function entity_exists(ctx, id)
  return ctx.world:find_entity(id) ~= nil
end

local function safe_destroy(ctx, id)
  if entity_exists(ctx, id) then
    ctx.world:destroy_entity(id)
  end
end

local function unspawn_batch(ctx)
  for i = 0, count - 1 do
    safe_destroy(ctx, first_id + i)
  end
end

local function spawn_batch(self, ctx)
  unspawn_batch(ctx)
  for i = 0, count - 1 do
    local col = i % 6
    local row = math.floor(i / 6)
    local id = first_id + i
    ctx.world:spawn_entity({
      id = id,
      name = "Lua Droplet " .. tostring(i),
      transform = {
        translation = {
          x = -1.2 + col * 0.48,
          y = 2.2 + row * 0.22,
          z = -0.45 - row * 0.12
        },
        rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
        scale = { x = 0.55, y = 0.55, z = 0.55 },
      },
      mesh = {
        mesh_id = 50,
        material_id = 2,
      },
    })
  end
end

function script.on_spawn(self, ctx)
  spawn_batch(self, ctx)
  self:set_name("Lua Particle Spawner - active")
end

function script.on_update(self, ctx)
  local phase = ctx.frame % 96
  if phase == 0 then
    spawn_batch(self, ctx)
    self:log("spawned Lua droplet batch")
  elseif phase == 48 then
    unspawn_batch(ctx)
    self:log("unspawned Lua droplet batch")
  end
end

function script.on_destroy(self, ctx)
  unspawn_batch(ctx)
end

return script
