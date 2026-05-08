-- @editor first_id number default=12000 min=1 max=100000 step=1 label="First Entity ID"
-- @editor count number default=18 min=1 max=128 step=1 label="Spawn Count"
-- @editor respawn_period number default=96 min=1 max=600 step=1 label="Respawn Period"
-- @editor cleanup_phase number default=48 min=0 max=600 step=1 label="Cleanup Phase"

local script = {}

local DEFAULT_FIRST_ID = 12000
local DEFAULT_COUNT = 18
local DEFAULT_RESPAWN_PERIOD = 96
local DEFAULT_CLEANUP_PHASE = 48

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

local function read_settings(ctx)
  return {
    first_id = math.max(1, math.floor(param_number(ctx, "first_id", DEFAULT_FIRST_ID))),
    count = math.max(1, math.floor(param_number(ctx, "count", DEFAULT_COUNT))),
    respawn_period = math.max(1, math.floor(param_number(ctx, "respawn_period", DEFAULT_RESPAWN_PERIOD))),
    cleanup_phase = math.max(0, math.floor(param_number(ctx, "cleanup_phase", DEFAULT_CLEANUP_PHASE))),
  }
end

local function entity_exists(ctx, id)
  return ctx.world:find_entity(id) ~= nil
end

local function safe_destroy(ctx, id)
  if entity_exists(ctx, id) then
    ctx.world:destroy_entity(id)
  end
end

local function unspawn_batch(ctx, settings)
  for i = 0, settings.count - 1 do
    safe_destroy(ctx, settings.first_id + i)
  end
end

local function spawn_batch(self, ctx, settings)
  unspawn_batch(ctx, settings)
  for i = 0, settings.count - 1 do
    local col = i % 6
    local row = math.floor(i / 6)
    local id = settings.first_id + i
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
  local settings = read_settings(ctx)
  spawn_batch(self, ctx, settings)
  self:set_name("Lua Particle Spawner - active")
end

function script.on_update(self, ctx)
  local settings = read_settings(ctx)
  local phase = ctx.frame % settings.respawn_period
  local cleanup_phase = settings.cleanup_phase % settings.respawn_period
  if phase == 0 then
    spawn_batch(self, ctx, settings)
    self:log("spawned Lua droplet batch")
  elseif phase == cleanup_phase then
    unspawn_batch(ctx, settings)
    self:log("unspawned Lua droplet batch")
  end
end

function script.on_destroy(self, ctx)
  local settings = read_settings(ctx)
  unspawn_batch(ctx, settings)
end

return script
