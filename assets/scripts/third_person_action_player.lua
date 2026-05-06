local script = {}

local MOUSE_YAW_SENSITIVITY = 0.0045
local MOUSE_PITCH_SENSITIVITY = 0.0035
local MIN_CAMERA_PITCH = 0.08
local MAX_CAMERA_PITCH = 0.62
local CAMERA_TARGET_Y = 1.35
local PLAYER_COLLISION_RADIUS = 0.42
local CONTACT_SLOP = 0.015
local CHARACTER_MODEL_NAMES = {
  "Hero Character Model",
}
local CHARACTER_MODEL_SCALE = 0.31

local function clamp(value, lo, hi)
  return math.max(lo, math.min(hi, value))
end

local function length2(x, z)
  return math.sqrt(x * x + z * z)
end

local function transform_or_default(entity)
  local transform = entity:get_transform()
  if transform ~= nil then
    return transform
  end

  return {
    translation = { x = 0.0, y = 0.0, z = 0.0 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

local function yaw_quat(yaw)
  local half = yaw * 0.5
  return { x = 0.0, y = math.sin(half), z = 0.0, w = math.cos(half) }
end

local function yaw_from_quat(q)
  return math.atan(2.0 * (q.w * q.y + q.x * q.z), 1.0 - 2.0 * (q.y * q.y + q.x * q.x))
end

local function pitch_quat(pitch)
  local half = pitch * 0.5
  return { x = math.sin(half), y = 0.0, z = 0.0, w = math.cos(half) }
end

local function roll_quat(roll)
  local half = roll * 0.5
  return { x = 0.0, y = 0.0, z = math.sin(half), w = math.cos(half) }
end

local function quat_mul(a, b)
  return {
    x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  }
end

local function normalize_quat(q)
  local len = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w)
  if len <= 0.000001 then
    return { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }
  end
  return { x = q.x / len, y = q.y / len, z = q.z / len, w = q.w / len }
end

local function vec_length(x, y, z)
  return math.sqrt(x * x + y * y + z * z)
end

local function normalize_vec3(x, y, z, fallback)
  local len = vec_length(x, y, z)
  if len <= 0.000001 then
    return fallback.x, fallback.y, fallback.z
  end
  return x / len, y / len, z / len
end

local function entity_name(entity)
  if entity == nil or entity.get_name == nil then
    return ""
  end
  return entity:get_name() or ""
end

local function entity_physics(entity)
  if entity == nil or entity.get_physics == nil then
    return nil
  end
  return entity:get_physics()
end

local function max3(a, b, c)
  return math.max(a, math.max(b, c))
end

local function is_floor_collider(entity, transform)
  return entity_name(entity) == "Training Floor" or
      (transform.scale.y <= 0.16 and transform.translation.y <= 0.05)
end

local function collect_contact_colliders(ctx, self_id)
  local boxes = {}
  local balls = {}
  local root = ctx.world:find_entity("Third Person Demo Root")
  if root == nil then
    return boxes, balls
  end

  for _, entity in ipairs(ctx.world:children_of(root)) do
    if entity:id() ~= self_id then
      local body = entity_physics(entity)
      local transform = transform_or_default(entity)
      if body ~= nil and body.enabled and not body.trigger then
        if body.dynamic and body.shape == "sphere" then
          balls[#balls + 1] = {
            entity = entity,
            transform = transform,
            radius = max3(transform.scale.x, transform.scale.y, transform.scale.z),
          }
        elseif not body.dynamic and body.body_type == "static" and body.shape == "box" and
            not is_floor_collider(entity, transform) then
          boxes[#boxes + 1] = {
            x = transform.translation.x,
            z = transform.translation.z,
            half_x = math.abs(transform.scale.x) * 0.5,
            half_z = math.abs(transform.scale.z) * 0.5,
          }
        end
      end
    end
  end
  return boxes, balls
end

local function resolve_circle_box(x, z, box)
  local closest_x = clamp(x, box.x - box.half_x, box.x + box.half_x)
  local closest_z = clamp(z, box.z - box.half_z, box.z + box.half_z)
  local dx = x - closest_x
  local dz = z - closest_z
  local dist_sq = dx * dx + dz * dz
  local radius = PLAYER_COLLISION_RADIUS + CONTACT_SLOP
  if dist_sq >= radius * radius then
    return x, z, false
  end

  if dist_sq > 0.000001 then
    local dist = math.sqrt(dist_sq)
    local push = radius - dist
    return x + dx / dist * push, z + dz / dist * push, true
  end

  local left = math.abs(x - (box.x - box.half_x))
  local right = math.abs((box.x + box.half_x) - x)
  local back = math.abs(z - (box.z - box.half_z))
  local front = math.abs((box.z + box.half_z) - z)
  local min_axis = math.min(left, right, back, front)
  if min_axis == left then
    return box.x - box.half_x - radius, z, true
  elseif min_axis == right then
    return box.x + box.half_x + radius, z, true
  elseif min_axis == back then
    return x, box.z - box.half_z - radius, true
  end
  return x, box.z + box.half_z + radius, true
end

local function resolve_static_contacts(x, z, boxes)
  for _ = 1, 3 do
    local changed = false
    for _, box in ipairs(boxes) do
      local next_x, next_z, hit = resolve_circle_box(x, z, box)
      x = next_x
      z = next_z
      changed = changed or hit
    end
    if not changed then
      break
    end
  end
  return x, z
end

local function resolve_dynamic_ball_contacts(x, z, balls, move_x, move_z, speed, dt)
  for _, ball in ipairs(balls) do
    local ball_transform = ball.transform
    local dx = ball_transform.translation.x - x
    local dz = ball_transform.translation.z - z
    local min_dist = PLAYER_COLLISION_RADIUS + ball.radius + CONTACT_SLOP
    local dist_sq = dx * dx + dz * dz
    if dist_sq < min_dist * min_dist then
      local nx = move_x
      local nz = move_z
      local dist = math.sqrt(math.max(0.0, dist_sq))
      if dist > 0.000001 then
        nx = dx / dist
        nz = dz / dist
      elseif length2(nx, nz) <= 0.000001 then
        nx = 1.0
        nz = 0.0
      end

      local penetration = min_dist - dist
      local impulse = penetration + speed * dt * 0.85
      ball_transform.translation.x = ball_transform.translation.x + nx * impulse
      ball_transform.translation.z = ball_transform.translation.z + nz * impulse
      ball.entity:set_transform(ball_transform)

      local hero_push = math.min(penetration * 0.25, 0.08)
      x = x - nx * hero_push
      z = z - nz * hero_push
    end
  end
  return x, z
end

local function cross(ax, ay, az, bx, by, bz)
  return ay * bz - az * by,
      az * bx - ax * bz,
      ax * by - ay * bx
end

local function camera_look_quat(forward_x, forward_y, forward_z)
  forward_x, forward_y, forward_z = normalize_vec3(
      forward_x, forward_y, forward_z, { x = 0.0, y = 0.0, z = -1.0 })
  local right_x, right_y, right_z = cross(forward_x, forward_y, forward_z, 0.0, 1.0, 0.0)
  right_x, right_y, right_z = normalize_vec3(
      right_x, right_y, right_z, { x = 1.0, y = 0.0, z = 0.0 })
  local up_x, up_y, up_z = cross(right_x, right_y, right_z, forward_x, forward_y, forward_z)
  up_x, up_y, up_z = normalize_vec3(up_x, up_y, up_z, { x = 0.0, y = 1.0, z = 0.0 })

  local m00 = right_x
  local m01 = up_x
  local m02 = -forward_x
  local m10 = right_y
  local m11 = up_y
  local m12 = -forward_y
  local m20 = right_z
  local m21 = up_z
  local m22 = -forward_z
  local trace = m00 + m11 + m22
  local q = {}
  if trace > 0.0 then
    local s = math.sqrt(trace + 1.0) * 2.0
    q.w = 0.25 * s
    q.x = (m21 - m12) / s
    q.y = (m02 - m20) / s
    q.z = (m10 - m01) / s
  elseif m00 > m11 and m00 > m22 then
    local s = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
    q.w = (m21 - m12) / s
    q.x = 0.25 * s
    q.y = (m01 + m10) / s
    q.z = (m02 + m20) / s
  elseif m11 > m22 then
    local s = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
    q.w = (m02 - m20) / s
    q.x = (m01 + m10) / s
    q.y = 0.25 * s
    q.z = (m12 + m21) / s
  else
    local s = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
    q.w = (m10 - m01) / s
    q.x = (m02 + m20) / s
    q.y = (m12 + m21) / s
    q.z = 0.25 * s
  end
  return normalize_quat(q)
end

local function limb_quat(angle)
  return pitch_quat(angle)
end

local function set_limb(ctx, name, angle, lift)
  local entity = ctx.world:find_entity(name)
  if entity == nil then
    return
  end
  local transform = transform_or_default(entity)
  transform.rotation = limb_quat(angle)
  if lift ~= nil then
    transform.translation.y = lift
  end
  entity:set_transform(transform)
end

local function update_walk_pose(ctx, moving, running, strafe_input)
  local t = ctx.elapsed_seconds or 0.0
  local stride_rate = running and 9.5 or 6.6
  local stride = math.sin(t * stride_rate)
  local counter = math.sin(t * stride_rate + math.pi)
  local amount = moving and (running and 0.78 or 0.54) or 0.08
  local body_bob = moving and math.abs(math.sin(t * stride_rate)) * 0.06 or math.sin(t * 2.0) * 0.015

  local posed_model = false
  local stride_lean = moving and stride * (running and 0.055 or 0.035) or 0.0
  local strafe_lean = moving and -strafe_input * (running and 0.075 or 0.052) or 0.0
  local pose_rotation = quat_mul(roll_quat(strafe_lean), pitch_quat(stride_lean))
  for _, name in ipairs(CHARACTER_MODEL_NAMES) do
    local model = ctx.world:find_entity(name)
    if model ~= nil then
      local transform = transform_or_default(model)
      transform.translation.y = body_bob
      transform.rotation = pose_rotation
      transform.scale = {
        x = CHARACTER_MODEL_SCALE,
        y = CHARACTER_MODEL_SCALE,
        z = CHARACTER_MODEL_SCALE,
      }
      model:set_transform(transform)
      posed_model = true
    end
  end
  if posed_model then
    return
  end

  set_limb(ctx, "Hero Left Arm", counter * amount * 0.7)
  set_limb(ctx, "Hero Right Arm", stride * amount * 0.7)
  set_limb(ctx, "Hero Left Leg", stride * amount, 0.35 + body_bob)
  set_limb(ctx, "Hero Right Leg", counter * amount, 0.35 + body_bob)

  local torso = ctx.world:find_entity("Hero Torso")
  if torso ~= nil then
    local transform = transform_or_default(torso)
    transform.translation.y = 1.24 + body_bob
    torso:set_transform(transform)
  end
end

local function current_camera_pitch(camera_transform, player_transform)
  local target_y = player_transform.translation.y + CAMERA_TARGET_Y
  local dx = camera_transform.translation.x - player_transform.translation.x
  local dz = camera_transform.translation.z - player_transform.translation.z
  local horizontal = math.max(0.0001, math.sqrt(dx * dx + dz * dz))
  return clamp(math.atan(camera_transform.translation.y - target_y, horizontal),
      MIN_CAMERA_PITCH, MAX_CAMERA_PITCH)
end

local function update_camera(ctx, player_transform, yaw, pitch, running)
  local camera = ctx.world:find_entity("Action Camera")
  if camera == nil then
    return
  end

  local px = player_transform.translation.x
  local pz = player_transform.translation.z
  local target_x = px
  local target_y = player_transform.translation.y + CAMERA_TARGET_Y
  local target_z = pz
  local forward_x = math.sin(yaw)
  local forward_z = -math.cos(yaw)
  local distance = running and 5.6 or 5.0
  local horizontal_distance = math.cos(pitch) * distance
  local vertical_distance = math.sin(pitch) * distance

  local transform = transform_or_default(camera)
  transform.translation.x = target_x - forward_x * horizontal_distance
  transform.translation.y = target_y + vertical_distance
  transform.translation.z = target_z - forward_z * horizontal_distance

  local dx = target_x - transform.translation.x
  local dy = target_y - transform.translation.y
  local dz = target_z - transform.translation.z
  local len = math.max(0.0001, math.sqrt(dx * dx + dy * dy + dz * dz))
  dx = dx / len
  dy = dy / len
  dz = dz / len

  transform.rotation = camera_look_quat(dx, dy, dz)
  camera:set_transform(transform)

  local camera_component = camera:get_camera()
  if camera_component ~= nil then
    local target_fov = running and 56.0 or 50.0
    if math.abs((camera_component.focus_distance or 0.0) - distance) > 0.01 or
        math.abs((camera_component.fov or 0.0) - target_fov) > 0.01 then
      camera_component.focus_distance = distance
      camera_component.fov = target_fov
      camera:set_camera(camera_component)
    end
  end
end

function script.on_load(self, ctx)
  self:log("third-person action controller loaded")
end

function script.on_update(self, ctx)
  local input = ctx.input
  local mouse_dx = input.mouse_delta_x or 0.0
  local mouse_dy = input.mouse_delta_y or 0.0
  local mouse_look = math.abs(mouse_dx) > 0.001 or math.abs(mouse_dy) > 0.001
  local forward_input = 0.0
  local strafe_input = 0.0
  if input:key_down("W") or input:key_down("up") then
    forward_input = forward_input + 1.0
  end
  if input:key_down("S") or input:key_down("down") then
    forward_input = forward_input - 1.0
  end
  if input:key_down("A") or input:key_down("left") then
    strafe_input = strafe_input - 1.0
  end
  if input:key_down("D") or input:key_down("right") then
    strafe_input = strafe_input + 1.0
  end

  local move_x = 0.0
  local move_z = 0.0
  local move_len = length2(strafe_input, forward_input)
  local moving = move_len > 0.001
  local running = moving and input:key_down("shift")
  local dt = math.min(ctx.dt or ctx.delta_seconds or 0.016, 0.05)
  local transform = transform_or_default(self)
  local yaw = yaw_from_quat(transform.rotation)
  local camera = ctx.world:find_entity("Action Camera")
  local camera_transform = camera ~= nil and transform_or_default(camera) or nil
  local pitch = camera_transform ~= nil and current_camera_pitch(camera_transform, transform) or 0.20

  if mouse_look then
    yaw = yaw - mouse_dx * MOUSE_YAW_SENSITIVITY
    pitch = clamp(pitch + mouse_dy * MOUSE_PITCH_SENSITIVITY, MIN_CAMERA_PITCH, MAX_CAMERA_PITCH)
  end

  if moving then
    local forward_x = math.sin(yaw)
    local forward_z = -math.cos(yaw)
    local right_x = math.cos(yaw)
    local right_z = math.sin(yaw)
    move_x = right_x * strafe_input + forward_x * forward_input
    move_z = right_z * strafe_input + forward_z * forward_input
    move_len = length2(move_x, move_z)
    move_x = move_x / move_len
    move_z = move_z / move_len
    local speed = running and 2.65 or 1.55
    local next_x = clamp(transform.translation.x + move_x * speed * dt, -4.8, 4.8)
    local next_z = clamp(transform.translation.z + move_z * speed * dt, -4.8, 4.8)
    local boxes, balls = collect_contact_colliders(ctx, self:id())
    next_x, next_z = resolve_static_contacts(next_x, next_z, boxes)
    next_x, next_z = resolve_dynamic_ball_contacts(next_x, next_z, balls, move_x, move_z, speed, dt)
    next_x, next_z = resolve_static_contacts(next_x, next_z, boxes)
    transform.translation.x = clamp(next_x, -4.8, 4.8)
    transform.translation.z = clamp(next_z, -4.8, 4.8)
  elseif not mouse_look then
    return
  end

  transform.rotation = yaw_quat(yaw)
  self:set_transform(transform)
  if moving then
    update_walk_pose(ctx, moving, running, strafe_input)
  end
  update_camera(ctx, transform, yaw, pitch, running)
end

return script
