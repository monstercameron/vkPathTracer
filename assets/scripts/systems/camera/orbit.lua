-- [systems/camera/orbit] Reusable third-person orbit camera; attach to a camera entity, point at any target.
--
-- Params:
--   target_entity_name (string, default ""): name of the entity the camera orbits around (required)
--   radius (number, default 5.0): distance from target along the orbit
--   min_pitch_deg (number, default -30.0): lower pitch clamp in degrees
--   max_pitch_deg (number, default 60.0): upper pitch clamp in degrees
--   mouse_sensitivity (number, default 0.005): radians per pixel of mouse motion
-- Bindings used: ctx.world:find_entity, entity:id, entity:get_transform, entity:set_transform, entity:get_camera, entity:set_camera, ctx.input.mouse_delta_x, ctx.input.mouse_delta_y
-- Lifecycle hooks: on_load, on_update
--
-- Example:
--   scene:ensure_script(entity, "systems/camera/orbit.lua", {target_entity_name="Hero", radius="6", mouse_sensitivity="0.004"})

local script = {}

local DEFAULT_RADIUS = 5.0
local DEFAULT_MIN_PITCH_DEG = -30.0
local DEFAULT_MAX_PITCH_DEG = 60.0
local DEFAULT_MOUSE_SENSITIVITY = 0.005

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

local function param_string(ctx, name, fallback)
  if ctx == nil or ctx.params == nil then
    return fallback
  end
  local value = ctx.params[name]
  if value == nil or value == "" then
    return fallback
  end
  return tostring(value)
end

local function transform_or_default(entity)
  if entity == nil then
    return nil
  end
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

local function normalize_vec3(x, y, z, fx, fy, fz)
  local len = math.sqrt(x * x + y * y + z * z)
  if len <= 0.000001 then
    return fx, fy, fz
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

local function camera_look_quat(fx, fy, fz)
  fx, fy, fz = normalize_vec3(fx, fy, fz, 0.0, 0.0, -1.0)
  local rx, ry, rz = cross(fx, fy, fz, 0.0, 1.0, 0.0)
  rx, ry, rz = normalize_vec3(rx, ry, rz, 1.0, 0.0, 0.0)
  local ux, uy, uz = cross(rx, ry, rz, fx, fy, fz)
  ux, uy, uz = normalize_vec3(ux, uy, uz, 0.0, 1.0, 0.0)

  local m00, m01, m02 = rx, ux, -fx
  local m10, m11, m12 = ry, uy, -fy
  local m20, m21, m22 = rz, uz, -fz
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

-- Cached target lookup. Re-resolves only if the cached entity has gone missing.
script.cached_target_id = 0
script.cached_target = nil
script.yaw = 0.0
script.pitch = 0.2

local function resolve_target(ctx, target_name)
  if ctx == nil or ctx.world == nil or target_name == nil or target_name == "" then
    return nil
  end
  if script.cached_target ~= nil then
    -- Cheap re-check via name lookup; the binding returns a fresh handle anyway.
    local current = ctx.world:find_entity(target_name)
    if current ~= nil then
      script.cached_target = current
      return current
    end
    script.cached_target = nil
    script.cached_target_id = 0
  end
  local found = ctx.world:find_entity(target_name)
  if found ~= nil then
    script.cached_target = found
    if found.id ~= nil then
      script.cached_target_id = found:id() or 0
    end
  end
  return found
end

function script.on_load(self, ctx)
  script.yaw = 0.0
  script.pitch = 0.2
  script.cached_target = nil
  script.cached_target_id = 0
end

function script.on_update(self, ctx)
  local target_name = param_string(ctx, "target_entity_name", "")
  if target_name == "" then
    return
  end
  local radius = param_number(ctx, "radius", DEFAULT_RADIUS)
  local min_pitch = math.rad(param_number(ctx, "min_pitch_deg", DEFAULT_MIN_PITCH_DEG))
  local max_pitch = math.rad(param_number(ctx, "max_pitch_deg", DEFAULT_MAX_PITCH_DEG))
  local sensitivity = param_number(ctx, "mouse_sensitivity", DEFAULT_MOUSE_SENSITIVITY)

  local target = resolve_target(ctx, target_name)
  if target == nil then
    return
  end

  local input = ctx.input
  local mouse_dx = input ~= nil and (input.mouse_delta_x or 0.0) or 0.0
  local mouse_dy = input ~= nil and (input.mouse_delta_y or 0.0) or 0.0
  if math.abs(mouse_dx) < 0.01 then mouse_dx = 0.0 end
  if math.abs(mouse_dy) < 0.01 then mouse_dy = 0.0 end

  script.yaw = script.yaw + mouse_dx * sensitivity
  script.pitch = clamp(script.pitch - mouse_dy * sensitivity, min_pitch, max_pitch)

  local target_transform = transform_or_default(target)
  local cos_pitch = math.cos(script.pitch)
  local offset_x = math.sin(script.yaw) * cos_pitch * radius
  local offset_y = math.sin(script.pitch) * radius
  local offset_z = math.cos(script.yaw) * cos_pitch * radius

  local cam_transform = transform_or_default(self)
  cam_transform.translation.x = target_transform.translation.x - offset_x
  cam_transform.translation.y = target_transform.translation.y + offset_y
  cam_transform.translation.z = target_transform.translation.z - offset_z

  local fwd_x = target_transform.translation.x - cam_transform.translation.x
  local fwd_y = target_transform.translation.y - cam_transform.translation.y
  local fwd_z = target_transform.translation.z - cam_transform.translation.z
  fwd_x, fwd_y, fwd_z = normalize_vec3(fwd_x, fwd_y, fwd_z, 0.0, 0.0, -1.0)
  cam_transform.rotation = camera_look_quat(fwd_x, fwd_y, fwd_z)
  self:set_transform(cam_transform)

  local camera = self:get_camera()
  if camera ~= nil then
    if math.abs((camera.focus_distance or 0.0) - radius) > 0.01 then
      camera.focus_distance = radius
      self:set_camera(camera)
    end
  end
end

return script
