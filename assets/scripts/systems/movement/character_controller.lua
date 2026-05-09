-- [systems/movement/character_controller] Kinematic WASD character controller with gravity, jump, and a flat ground plane.
--
-- Params:
--   walk_speed (number, default 4.0): horizontal speed when not running
--   run_speed (number, default 7.0): horizontal speed while shift is held
--   gravity (number, default -9.8): vertical acceleration applied each tick
--   jump_velocity (number, default 5.0): upward velocity applied on space when grounded
--   ground_y (number, default 0.0): y level of the simple ground plane (no raycast)
-- Bindings used: entity:get_transform, entity:set_transform, ctx.input:key_down, ctx.dt
-- Lifecycle hooks: on_load, on_update
--
-- Example:
--   scene:ensure_script(entity, "systems/movement/character_controller.lua", {walk_speed="3.5", jump_velocity="6"})

local script = {}

local DEFAULT_WALK_SPEED = 4.0
local DEFAULT_RUN_SPEED = 7.0
local DEFAULT_GRAVITY = -9.8
local DEFAULT_JUMP_VELOCITY = 5.0
local DEFAULT_GROUND_Y = 0.0
local MAX_DT = 0.1

-- Velocity persists across ticks so the integrator sees momentum; reset on load.
script.velocity_x = 0.0
script.velocity_y = 0.0
script.velocity_z = 0.0
script.grounded = true

local function clamp(v, lo, hi)
  return math.max(lo, math.min(hi, v))
end

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

local function transform_or_default(entity)
  local transform = entity ~= nil and entity:get_transform() or nil
  if transform ~= nil then
    return transform
  end
  return {
    translation = { x = 0.0, y = 0.0, z = 0.0 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

local function key(input, name)
  return input ~= nil and input:key_down(name)
end

local function yaw_from_quat(q)
  if q == nil then
    return 0.0
  end
  return math.atan(2.0 * (q.w * q.y + q.x * q.z),
      1.0 - 2.0 * (q.y * q.y + q.x * q.x))
end

function script.on_load(self, ctx)
  script.velocity_x = 0.0
  script.velocity_y = 0.0
  script.velocity_z = 0.0
  script.grounded = true
end

function script.on_update(self, ctx)
  local walk_speed = param_number(ctx, "walk_speed", DEFAULT_WALK_SPEED)
  local run_speed = param_number(ctx, "run_speed", DEFAULT_RUN_SPEED)
  local gravity = param_number(ctx, "gravity", DEFAULT_GRAVITY)
  local jump_velocity = param_number(ctx, "jump_velocity", DEFAULT_JUMP_VELOCITY)
  local ground_y = param_number(ctx, "ground_y", DEFAULT_GROUND_Y)

  local dt = clamp(ctx.dt or ctx.delta_seconds or (1.0 / 60.0), 0.0, MAX_DT)
  local input = ctx.input
  local transform = transform_or_default(self)

  -- Movement axes are taken in entity-yaw space so the controller follows the body's facing.
  local yaw = yaw_from_quat(transform.rotation)
  local forward_x = math.sin(yaw)
  local forward_z = -math.cos(yaw)
  local right_x = math.cos(yaw)
  local right_z = math.sin(yaw)

  local move_x = 0.0
  local move_z = 0.0
  if key(input, "W") or key(input, "up") then
    move_x = move_x + forward_x
    move_z = move_z + forward_z
  end
  if key(input, "S") or key(input, "down") then
    move_x = move_x - forward_x
    move_z = move_z - forward_z
  end
  if key(input, "D") or key(input, "right") then
    move_x = move_x + right_x
    move_z = move_z + right_z
  end
  if key(input, "A") or key(input, "left") then
    move_x = move_x - right_x
    move_z = move_z - right_z
  end

  local move_len = math.sqrt(move_x * move_x + move_z * move_z)
  if move_len > 0.000001 then
    move_x = move_x / move_len
    move_z = move_z / move_len
  end

  local running = key(input, "shift")
  local horizontal_speed = running and run_speed or walk_speed
  script.velocity_x = move_x * horizontal_speed
  script.velocity_z = move_z * horizontal_speed

  -- Vertical integration: gravity always pulls; jump latches once when grounded.
  script.velocity_y = script.velocity_y + gravity * dt
  if script.grounded and key(input, "space") then
    script.velocity_y = jump_velocity
    script.grounded = false
  end

  transform.translation.x = transform.translation.x + script.velocity_x * dt
  transform.translation.y = transform.translation.y + script.velocity_y * dt
  transform.translation.z = transform.translation.z + script.velocity_z * dt

  -- Simple ground plane test; reset vertical velocity on landing.
  if transform.translation.y <= ground_y then
    transform.translation.y = ground_y
    if script.velocity_y < 0.0 then
      script.velocity_y = 0.0
    end
    script.grounded = true
  else
    script.grounded = false
  end

  self:set_transform(transform)
end

return script
