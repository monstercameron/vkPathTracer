local script = {}

local CONTROLS_PANEL_ID = 9790
local CONTROLS_PANEL_NAME = "Audio Controls Panel"
local CAMERA_NAME = "Audio Demo Camera Listener"
local CAMERA_TARGET_Y = 0.65
local CAMERA_DISTANCE = 5.8
local CAMERA_RUN_DISTANCE = 6.4
local MIN_CAMERA_PITCH = 0.12
local MAX_CAMERA_PITCH = 0.72
local MOUSE_YAW_SENSITIVITY = 0.0018
local MOUSE_PITCH_SENSITIVITY = 0.0014
local MAX_MOUSE_DELTA = 90.0

local function clamp(value, lo, hi)
  return math.max(lo, math.min(hi, value))
end

local function filtered_mouse_delta(value)
  if math.abs(value) < 0.01 then
    return 0.0
  end
  return clamp(value, -MAX_MOUSE_DELTA, MAX_MOUSE_DELTA)
end

local function key(input, name)
  return input ~= nil and input:key_down(name)
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

local function yaw_quat(yaw)
  local half = yaw * 0.5
  return { x = 0.0, y = math.sin(half), z = 0.0, w = math.cos(half) }
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

local function yaw_from_quat(q)
  return math.atan(2.0 * (q.w * q.y + q.x * q.z), 1.0 - 2.0 * (q.y * q.y + q.x * q.x))
end

local function cross(ax, ay, az, bx, by, bz)
  return ay * bz - az * by,
      az * bx - ax * bz,
      ax * by - ay * bx
end

local function camera_look_quat(forward_x, forward_y, forward_z)
  local right_x, right_y, right_z = cross(forward_x, forward_y, forward_z, 0.0, 1.0, 0.0)
  right_x, right_y, right_z = normalize_vec3(right_x, right_y, right_z, { x = 1.0, y = 0.0, z = 0.0 })
  local up_x, up_y, up_z = cross(right_x, right_y, right_z, forward_x, forward_y, forward_z)

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

local function movement_axis(input, camera_yaw)
  local x = 0.0
  local z = 0.0
  if key(input, "A") or key(input, "left") then x = x - 1.0 end
  if key(input, "D") or key(input, "right") then x = x + 1.0 end
  if key(input, "W") or key(input, "up") then z = z + 1.0 end
  if key(input, "S") or key(input, "down") then z = z - 1.0 end
  local len = math.sqrt(x * x + z * z)
  if len > 0.0 then
    x = x / len
    z = z / len
  end
  local forward_x = math.sin(camera_yaw)
  local forward_z = -math.cos(camera_yaw)
  local right_x = math.cos(camera_yaw)
  local right_z = math.sin(camera_yaw)
  return right_x * x + forward_x * z,
      right_z * x + forward_z * z,
      len > 0.0
end

local function camera_yaw_from_transform(camera_transform, player_transform, fallback_yaw)
  local forward_x = player_transform.translation.x - camera_transform.translation.x
  local forward_z = player_transform.translation.z - camera_transform.translation.z
  if math.sqrt(forward_x * forward_x + forward_z * forward_z) <= 0.0001 then
    return fallback_yaw
  end
  return math.atan(forward_x, -forward_z)
end

local function current_camera_pitch(camera_transform, player_transform)
  local target_y = player_transform.translation.y + CAMERA_TARGET_Y
  local dx = camera_transform.translation.x - player_transform.translation.x
  local dz = camera_transform.translation.z - player_transform.translation.z
  local horizontal = math.max(0.0001, math.sqrt(dx * dx + dz * dz))
  return clamp(math.atan(camera_transform.translation.y - target_y, horizontal),
      MIN_CAMERA_PITCH, MAX_CAMERA_PITCH)
end

local function update_camera(ctx, player_transform, camera_yaw, pitch, running)
  local camera = ctx.world:find_entity(CAMERA_NAME)
  if camera == nil then
    return
  end

  local camera_transform = transform_or_default(camera)
  local distance = running and CAMERA_RUN_DISTANCE or CAMERA_DISTANCE
  local target_x = player_transform.translation.x
  local target_y = player_transform.translation.y + CAMERA_TARGET_Y
  local target_z = player_transform.translation.z
  local forward_x = math.sin(camera_yaw)
  local forward_z = -math.cos(camera_yaw)
  local horizontal_distance = math.cos(pitch) * distance
  local vertical_distance = math.sin(pitch) * distance

  camera_transform.translation.x = target_x - forward_x * horizontal_distance
  camera_transform.translation.y = target_y + vertical_distance
  camera_transform.translation.z = target_z - forward_z * horizontal_distance

  local dx = target_x - camera_transform.translation.x
  local dy = target_y - camera_transform.translation.y
  local dz = target_z - camera_transform.translation.z
  dx, dy, dz = normalize_vec3(dx, dy, dz, { x = 0.0, y = 0.0, z = -1.0 })
  camera_transform.rotation = camera_look_quat(dx, dy, dz)
  camera:set_transform(camera_transform)

  local camera_component = camera:get_camera()
  if camera_component ~= nil then
    camera_component.focus_distance = distance
    camera:set_camera(camera_component)
  end
end

local function play(ctx, event_name, options)
  if ctx ~= nil and ctx.audio ~= nil then
    ctx.audio:post_event(event_name, options or {})
  end
end

local function ensure_controls_panel(ctx)
  if ctx == nil or ctx.world == nil then
    return
  end
  if ctx.world:find_entity(CONTROLS_PANEL_NAME) ~= nil then
    return
  end

  ctx.world:spawn_entity({
    id = CONTROLS_PANEL_ID,
    name = CONTROLS_PANEL_NAME,
    parent = 9730,
    ui_panel = {
      id = "audio_demo.controls",
      title = "Audio Demo Controls",
      anchor = "top_left",
      x = 16.0,
      y = 16.0,
      width = 304.0,
      opacity = 0.88,
      font_size = 13.0,
      background = { x = 0.035, y = 0.045, z = 0.055 },
      foreground = { x = 0.92, y = 0.96, z = 1.0 },
      accent = { x = 0.12, y = 0.72, z = 0.95 },
      lines = {
        "WASD / Arrows   Move player",
        "Shift           Run footsteps",
        "Mouse           Look",
        "Space           Fire",
        "E               Pickup sound",
        "G               Terminal sound",
        "F1              Toggle game mode",
      },
    },
  })
end

function script.on_load(self, ctx)
  play(ctx, "ui.radar.ping", { volume = 0.55, spatial = false })
end

function script.on_update(self, ctx)
  ensure_controls_panel(ctx)
  if ctx == nil or ctx.world == nil then
    return
  end

  local transform = transform_or_default(self)
  local camera = ctx.world:find_entity(CAMERA_NAME)
  local camera_transform = camera ~= nil and transform_or_default(camera) or nil
  local facing_yaw = yaw_from_quat(transform.rotation)
  local camera_yaw = camera_transform ~= nil and
      camera_yaw_from_transform(camera_transform, transform, facing_yaw) or
      facing_yaw
  local pitch = camera_transform ~= nil and
      current_camera_pitch(camera_transform, transform) or
      MIN_CAMERA_PITCH

  local input = ctx.input
  local mouse_dx = filtered_mouse_delta(input.mouse_delta_x or 0.0)
  local mouse_dy = filtered_mouse_delta(input.mouse_delta_y or 0.0)
  local mouse_look = math.abs(mouse_dx) > 0.001 or math.abs(mouse_dy) > 0.001
  if mouse_look then
    camera_yaw = camera_yaw + mouse_dx * MOUSE_YAW_SENSITIVITY
    pitch = clamp(pitch + mouse_dy * MOUSE_PITCH_SENSITIVITY, MIN_CAMERA_PITCH, MAX_CAMERA_PITCH)
  end

  local dx, dz, moving = movement_axis(input, camera_yaw)
  local dt = ctx.dt or ctx.delta_seconds or 0.016
  local running = moving and key(input, "shift")
  local speed = running and 3.8 or 2.2

  if moving then
    transform.translation.x = transform.translation.x + dx * speed * dt
    transform.translation.z = transform.translation.z + dz * speed * dt

    local cadence = running and 11 or 18
    if (ctx.frame % cadence) == 0 then
      play(ctx, "player.footstep.dirt", {
        position = transform.translation,
        volume = running and 0.72 or 0.52,
        pitch = running and 1.12 or 0.96,
        spatial = true
      })
    end
  end
  if moving or mouse_look then
    transform.rotation = yaw_quat(camera_yaw)
    self:set_transform(transform)
  end
  update_camera(ctx, transform, camera_yaw, pitch, running)

  if key(input, "space") and (ctx.frame % 10) == 0 then
    play(ctx, "weapon.rifle.fire", {
      position = transform.translation,
      volume = 0.85,
      pitch = 1.0,
      spatial = true
    })
  end

  if key(input, "E") and (ctx.frame % 30) == 0 then
    play(ctx, "pickup.collect", {
      position = transform.translation,
      volume = 0.68,
      spatial = true
    })
  end

  if key(input, "G") and (ctx.frame % 45) == 0 then
    play(ctx, "objective.terminal.disabled", {
      position = transform.translation,
      volume = 0.75,
      spatial = true
    })
  end

  if (ctx.frame % 180) == 0 then
    play(ctx, "ui.radar.ping", { volume = 0.35, spatial = false })
  end
end

return script
