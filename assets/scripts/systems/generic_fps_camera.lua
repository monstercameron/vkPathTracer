-- Canonical reusable FPS camera system. Keep the legacy compatibility path at
-- assets/scripts/generic_fps_camera.lua runnable until authored scenes migrate.
local script = {}

local DEFAULT_WALK_SPEED = 4.2
local DEFAULT_RUN_MULTIPLIER = 1.75
local DEFAULT_MOUSE_YAW_SENSITIVITY = 0.0020
local DEFAULT_MOUSE_PITCH_SENSITIVITY = 0.0016
local DEFAULT_MAX_MOUSE_DELTA = 120.0
local DEFAULT_MIN_PITCH_DEGREES = -82.0
local DEFAULT_MAX_PITCH_DEGREES = 82.0
local DEFAULT_MAX_DT = 0.08

local function clamp(value, lo, hi)
  return math.max(lo, math.min(hi, value))
end

local function param_string(params, name, fallback)
  local value = params ~= nil and params[name] or nil
  if value == nil or value == "" then
    return fallback
  end
  return tostring(value)
end

local function param_number(params, name, fallback)
  local value = params ~= nil and tonumber(params[name] or "") or nil
  if value == nil then
    return fallback
  end
  return value
end

local function optional_number(params, name)
  if params == nil then
    return nil
  end
  return tonumber(params[name] or "")
end

local function param_bool(params, name, fallback)
  local value = params ~= nil and params[name] or nil
  if value == nil or value == "" then
    return fallback
  end
  value = tostring(value)
  return value == "1" or value == "true" or value == "yes" or value == "on"
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

local function normalize_vec3(x, y, z, fallback)
  local len = math.sqrt(x * x + y * y + z * z)
  if len <= 0.000001 then
    return fallback.x, fallback.y, fallback.z
  end
  return x / len, y / len, z / len
end

local function normalize_quat(q)
  local len = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w)
  if len <= 0.000001 then
    return { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }
  end
  return { x = q.x / len, y = q.y / len, z = q.z / len, w = q.w / len }
end

local function cross(ax, ay, az, bx, by, bz)
  return ay * bz - az * by,
      az * bx - ax * bz,
      ax * by - ay * bx
end

local function rotate_by_quat(q, x, y, z)
  q = normalize_quat(q)
  local tx = 2.0 * (q.y * z - q.z * y)
  local ty = 2.0 * (q.z * x - q.x * z)
  local tz = 2.0 * (q.x * y - q.y * x)
  return x + q.w * tx + (q.y * tz - q.z * ty),
      y + q.w * ty + (q.z * tx - q.x * tz),
      z + q.w * tz + (q.x * ty - q.y * tx)
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

local function yaw_pitch_from_transform(transform)
  local forward_x, forward_y, forward_z = rotate_by_quat(
      transform.rotation or { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
      0.0,
      0.0,
      -1.0)
  forward_x, forward_y, forward_z = normalize_vec3(
      forward_x, forward_y, forward_z, { x = 0.0, y = 0.0, z = -1.0 })
  local horizontal = math.sqrt(forward_x * forward_x + forward_z * forward_z)
  return math.atan(forward_x, -forward_z), math.atan(forward_y, horizontal)
end

local function key(input, name)
  return input ~= nil and input:key_down(name)
end

local function update_controls_panel(self, ctx, params)
  if not param_bool(params, "show_controls", false) then
    return
  end
  local name = param_string(params, "controls_panel_name", "FPS Camera Controls Panel")
  if ctx.world:find_entity(name) ~= nil then
    return
  end
  local panel_id = optional_number(params, "controls_panel_id")
  ctx.world:spawn_entity({
    id = panel_id,
    name = name,
    parent = self:id(),
    ui_panel = {
      title = param_string(params, "controls_title", "FPS Camera Controls"),
      anchor = param_string(params, "controls_anchor", "top_left"),
      lines = {
        "WASD / Arrows   Move camera",
        "Mouse           Look",
        "Shift           Run",
        "Space / E       Move up",
        "Ctrl / Q        Move down",
      },
    },
  })
end

function script.on_update(self, ctx)
  local params = ctx.params or {}
  local transform = transform_or_default(self)
  local original_translation = {
    x = transform.translation.x,
    y = transform.translation.y,
    z = transform.translation.z,
  }
  local original_rotation = {
    x = transform.rotation.x,
    y = transform.rotation.y,
    z = transform.rotation.z,
    w = transform.rotation.w,
  }
  local dt = clamp(ctx.dt or ctx.delta_seconds or (1.0 / 60.0),
                   0.0,
                   param_number(params, "max_dt", DEFAULT_MAX_DT))
  local input = ctx.input
  local max_mouse = param_number(params, "max_mouse_delta", DEFAULT_MAX_MOUSE_DELTA)
  local mouse_dx = clamp(input ~= nil and (input.mouse_delta_x or 0.0) or 0.0, -max_mouse, max_mouse)
  local mouse_dy = clamp(input ~= nil and (input.mouse_delta_y or 0.0) or 0.0, -max_mouse, max_mouse)
  if math.abs(mouse_dx) < 0.01 then mouse_dx = 0.0 end
  if math.abs(mouse_dy) < 0.01 then mouse_dy = 0.0 end

  local yaw, pitch = yaw_pitch_from_transform(transform)
  yaw = yaw + mouse_dx * param_number(params, "mouse_yaw_sensitivity", DEFAULT_MOUSE_YAW_SENSITIVITY)
  local pitch_sign = param_bool(params, "invert_y", false) and 1.0 or -1.0
  pitch = pitch + pitch_sign * mouse_dy *
      param_number(params, "mouse_pitch_sensitivity", DEFAULT_MOUSE_PITCH_SENSITIVITY)
  local min_pitch = math.rad(param_number(params, "min_pitch_degrees", DEFAULT_MIN_PITCH_DEGREES))
  local max_pitch = math.rad(param_number(params, "max_pitch_degrees", DEFAULT_MAX_PITCH_DEGREES))
  pitch = clamp(pitch, min_pitch, max_pitch)

  local cos_pitch = math.cos(pitch)
  local forward_x = math.sin(yaw) * cos_pitch
  local forward_y = math.sin(pitch)
  local forward_z = -math.cos(yaw) * cos_pitch
  local flat_forward_x, _, flat_forward_z = normalize_vec3(
      math.sin(yaw), 0.0, -math.cos(yaw), { x = 0.0, y = 0.0, z = -1.0 })
  local right_x = math.cos(yaw)
  local right_z = math.sin(yaw)

  local movement_mode = param_string(params, "movement_mode", "walk")
  local fly = movement_mode == "fly" or movement_mode == "noclip"
  local move_x = 0.0
  local move_y = 0.0
  local move_z = 0.0

  if key(input, "W") or key(input, "up") then
    move_x = move_x + (fly and forward_x or flat_forward_x)
    move_y = move_y + (fly and forward_y or 0.0)
    move_z = move_z + (fly and forward_z or flat_forward_z)
  end
  if key(input, "S") or key(input, "down") then
    move_x = move_x - (fly and forward_x or flat_forward_x)
    move_y = move_y - (fly and forward_y or 0.0)
    move_z = move_z - (fly and forward_z or flat_forward_z)
  end
  if key(input, "D") or key(input, "right") then
    move_x = move_x + right_x
    move_z = move_z + right_z
  end
  if key(input, "A") or key(input, "left") then
    move_x = move_x - right_x
    move_z = move_z - right_z
  end

  local vertical_enabled = param_bool(params, "enable_vertical", fly)
  if vertical_enabled then
    if key(input, "space") or key(input, "E") then
      move_y = move_y + 1.0
    end
    if key(input, "control") or key(input, "Q") then
      move_y = move_y - 1.0
    end
  end

  local move_len = math.sqrt(move_x * move_x + move_y * move_y + move_z * move_z)
  if move_len > 0.000001 then
    move_x = move_x / move_len
    move_y = move_y / move_len
    move_z = move_z / move_len
  end

  local walk_speed = param_number(params, "walk_speed", DEFAULT_WALK_SPEED)
  local run_multiplier = param_number(params, "run_multiplier", DEFAULT_RUN_MULTIPLIER)
  local speed = key(input, "shift") and walk_speed * run_multiplier or walk_speed
  transform.translation.x = transform.translation.x + move_x * speed * dt
  transform.translation.y = transform.translation.y + move_y * speed * dt
  transform.translation.z = transform.translation.z + move_z * speed * dt

  local fixed_y = optional_number(params, "fixed_y")
  if fixed_y ~= nil then
    transform.translation.y = fixed_y
  end

  local min_x = optional_number(params, "min_x")
  local max_x = optional_number(params, "max_x")
  local min_y = optional_number(params, "min_y")
  local max_y = optional_number(params, "max_y")
  local min_z = optional_number(params, "min_z")
  local max_z = optional_number(params, "max_z")
  if min_x ~= nil then transform.translation.x = math.max(min_x, transform.translation.x) end
  if max_x ~= nil then transform.translation.x = math.min(max_x, transform.translation.x) end
  if min_y ~= nil then transform.translation.y = math.max(min_y, transform.translation.y) end
  if max_y ~= nil then transform.translation.y = math.min(max_y, transform.translation.y) end
  if min_z ~= nil then transform.translation.z = math.max(min_z, transform.translation.z) end
  if max_z ~= nil then transform.translation.z = math.min(max_z, transform.translation.z) end

  transform.rotation = camera_look_quat(forward_x, forward_y, forward_z)
  local moved = math.abs(transform.translation.x - original_translation.x) > 0.00001 or
      math.abs(transform.translation.y - original_translation.y) > 0.00001 or
      math.abs(transform.translation.z - original_translation.z) > 0.00001
  local rotation_direct_delta =
      math.abs(transform.rotation.x - original_rotation.x) +
      math.abs(transform.rotation.y - original_rotation.y) +
      math.abs(transform.rotation.z - original_rotation.z) +
      math.abs(transform.rotation.w - original_rotation.w)
  local rotation_negated_delta =
      math.abs(transform.rotation.x + original_rotation.x) +
      math.abs(transform.rotation.y + original_rotation.y) +
      math.abs(transform.rotation.z + original_rotation.z) +
      math.abs(transform.rotation.w + original_rotation.w)
  if moved or math.min(rotation_direct_delta, rotation_negated_delta) > 0.00001 then
    self:set_transform(transform)
  end

  local camera = self:get_camera()
  if camera ~= nil then
    local camera_dirty = false
    local fov = optional_number(params, "fov")
    if fov ~= nil and math.abs((camera.fov or 0.0) - fov) > 0.001 then
      camera.fov = fov
      camera_dirty = true
    end
    local focus_distance = optional_number(params, "focus_distance")
    if focus_distance ~= nil and math.abs((camera.focus_distance or 0.0) - focus_distance) > 0.001 then
      camera.focus_distance = focus_distance
      camera_dirty = true
    end
    if camera_dirty then
      self:set_camera(camera)
    end
  end

  update_controls_panel(self, ctx, params)
end

return script
