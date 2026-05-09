-- [user/fps_weapon_demo] FPS weapon gameplay controller (movement, ADS, tracers, audio dispatch) for the shooting range demo scene.
-- @editor walk_speed number default=3.9 min=0 max=12 step=0.1 label="Walk Speed"
-- @editor run_multiplier number default=1.7 min=1 max=4 step=0.05 label="Run Multiplier"
-- @editor crouch_multiplier number default=0.45 min=0.1 max=1 step=0.05 label="Crouch Multiplier"
-- @editor ads_move_multiplier number default=0.72 min=0.1 max=1 step=0.05 label="ADS Move Multiplier"
-- @editor acceleration number default=18.0 min=1 max=80 step=1 label="Acceleration"
-- @editor deceleration number default=24.0 min=1 max=80 step=1 label="Deceleration"
-- @editor mouse_yaw_sensitivity number default=0.0020 min=0 max=0.02 step=0.0001 label="Yaw Sensitivity"
-- @editor mouse_pitch_sensitivity number default=0.0016 min=0 max=0.02 step=0.0001 label="Pitch Sensitivity"
-- @editor ads_sensitivity_multiplier number default=0.58 min=0.1 max=1 step=0.01 label="ADS Sensitivity"
-- @editor max_mouse_delta number default=120.0 min=1 max=500 step=1 label="Max Mouse Delta"
-- @editor hip_fov number default=68.0 min=35 max=110 step=1 label="Hip FOV"
-- @editor ads_fov number default=38.0 min=10 max=80 step=1 label="ADS FOV"
-- @editor hip_focus_distance number default=9.0 min=0.5 max=80 step=0.5 label="Hip Focus"
-- @editor ads_focus_distance number default=12.0 min=0.5 max=120 step=0.5 label="ADS Focus"
-- @editor ads_focus_wheel_step number default=1.5 min=0.1 max=8 step=0.1 label="ADS Focus Step"
-- @editor hip_f_stop number default=5.6 min=0.7 max=22 step=0.1 label="Hip F-Stop"
-- @editor ads_f_stop number default=0.85 min=0.35 max=8 step=0.05 label="ADS F-Stop"
-- @editor hip_aperture_radius number default=0.0 min=0 max=0.25 step=0.005 label="Hip Aperture"
-- @editor ads_aperture_radius number default=0.075 min=0 max=0.3 step=0.005 label="ADS Aperture"
-- @editor aim_lock_target text default="cursor" label="Aim Lock Target"
-- @editor aim_lock_strength number default=10.0 min=0 max=24 step=0.5 label="Aim Assist"
-- @editor range_root_name text default="FPS Demo Root" label="Range Root"
-- @editor range_target_prefix text default="FPS Range Sphere Target" label="Range Targets"
-- @editor range_target_impulse number default=0.10 min=0 max=1 step=0.01 label="Target Impulse"
-- @editor range_target_gravity number default=9.8 min=0 max=30 step=0.1 label="Target Gravity"
-- @editor range_target_bounce number default=0.46 min=0 max=1 step=0.01 label="Target Bounce"
-- @editor fire_rate number default=7.5 min=0.5 max=18 step=0.1 label="Fire Rate"
-- @editor bullet_speed number default=140.0 min=5 max=260 step=1 label="Bullet Speed"
-- @editor bullet_lifetime number default=2.4 min=0.1 max=5 step=0.1 label="Bullet Lifetime"
-- @editor bullet_hit_radius number default=0.36 min=0.05 max=3 step=0.05 label="Hit Radius"
-- @editor bullet_visual_radius number default=0.022 min=0.005 max=0.12 step=0.001 label="Bullet Radius"
-- @editor bullet_visual_length number default=0.16 min=0.02 max=0.8 step=0.01 label="Bullet Length"
-- @editor flashlight_name text default="FPS Player Flashlight" label="Flashlight"
-- @editor flashlight_intensity number default=38.0 min=0 max=120 step=1 label="Flashlight Intensity"
-- @editor flashlight_radius number default=14.0 min=1 max=60 step=0.5 label="Flashlight Throw"
-- @editor flashlight_beam_degrees number default=8.0 min=4 max=45 step=0.5 label="Flashlight Cone"
-- @editor flashlight_blend number default=0.10 min=0 max=1 step=0.01 label="Flashlight Edge"
-- @editor flashlight_forward_offset number default=0.18 min=-0.5 max=1.0 step=0.01 label="Flashlight Forward"
-- @editor flashlight_drop number default=0.06 min=0 max=0.6 step=0.01 label="Flashlight Drop"
-- @editor target_name text default="FPS Hero Target Model" label="Hero Target"
-- @editor weapon_name text default="FPS Player Rifle Model" label="Weapon Model"
-- @editor controls_panel_name text default="FPS Weapon Controls Panel" label="Controls Panel"
-- @editor show_controls bool default=true label="Show Controls"

local script = {}
local runtime_state = nil

local DEFAULTS = {
  standing_height = 1.72,
  crouch_height = 1.08,
  min_pitch = math.rad(-82.0),
  max_pitch = math.rad(82.0),
  max_dt = 0.08,
  bullet_pool_start_id = 780000,
  bullet_pool_size = 8,
  muzzle_flash_pool_start_id = 780100,
  muzzle_flash_pool_size = 4,
  impact_flash_pool_start_id = 780120,
  impact_flash_pool_size = 4,
  inactive_translation = { x = 0.0, y = -20.0, z = 0.0 },
}

local function clamp(value, lo, hi)
  return math.max(lo, math.min(hi, value))
end

local function lerp(a, b, t)
  return a + (b - a) * clamp(t, 0.0, 1.0)
end

local function approach(current, target, rate, dt)
  local step = math.max(0.0, rate * dt)
  if current < target then
    return math.min(target, current + step)
  end
  return math.max(target, current - step)
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
  if ctx == nil or ctx.params == nil or ctx.params[name] == nil or ctx.params[name] == "" then
    return fallback
  end
  return tostring(ctx.params[name])
end

local function param_bool(ctx, name, fallback)
  if ctx == nil or ctx.params == nil or ctx.params[name] == nil then
    return fallback
  end
  local value = tostring(ctx.params[name]):lower()
  if value == "true" or value == "1" or value == "yes" or value == "on" then
    return true
  end
  if value == "false" or value == "0" or value == "no" or value == "off" then
    return false
  end
  return fallback
end

local function key(input, name)
  return input ~= nil and input.enabled ~= false and type(input.key_down) == "function" and input:key_down(name)
end

local function mouse(input, name)
  return input ~= nil and input.enabled ~= false and type(input.mouse_down) == "function" and input:mouse_down(name)
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

local function entity_name(entity)
  if entity == nil or type(entity.get_name) ~= "function" then
    return ""
  end
  return entity:get_name() or ""
end

local function starts_with(text, prefix)
  if text == nil or prefix == nil or prefix == "" then
    return false
  end
  return string.sub(text, 1, string.len(prefix)) == prefix
end

local function max3(a, b, c)
  return math.max(a, math.max(b, c))
end

local function absmax3(a, b, c)
  return max3(math.abs(a), math.abs(b), math.abs(c))
end

local function vec_len(x, y, z)
  return math.sqrt(x * x + y * y + z * z)
end

local function normalize_vec3(x, y, z, fallback)
  local len = vec_len(x, y, z)
  if len <= 0.000001 then
    return fallback.x, fallback.y, fallback.z
  end
  return x / len, y / len, z / len
end

local function cross(ax, ay, az, bx, by, bz)
  return ay * bz - az * by,
      az * bx - ax * bz,
      ax * by - ay * bx
end

local function normalize_quat(q)
  local len = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w)
  if len <= 0.000001 then
    return { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }
  end
  return { x = q.x / len, y = q.y / len, z = q.z / len, w = q.w / len }
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
  local fx, fy, fz = rotate_by_quat(
      transform.rotation or { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
      0.0,
      0.0,
      -1.0)
  fx, fy, fz = normalize_vec3(fx, fy, fz, { x = 0.0, y = 0.0, z = -1.0 })
  local horizontal = math.sqrt(fx * fx + fz * fz)
  return math.atan(fx, -fz), math.atan(fy, horizontal)
end

local function read_settings(ctx)
  return {
    walk_speed = param_number(ctx, "walk_speed", 3.9),
    run_multiplier = param_number(ctx, "run_multiplier", 1.7),
    crouch_multiplier = param_number(ctx, "crouch_multiplier", 0.45),
    ads_move_multiplier = param_number(ctx, "ads_move_multiplier", 0.72),
    acceleration = param_number(ctx, "acceleration", 18.0),
    deceleration = param_number(ctx, "deceleration", 24.0),
    yaw_sensitivity = param_number(ctx, "mouse_yaw_sensitivity", 0.0020),
    pitch_sensitivity = param_number(ctx, "mouse_pitch_sensitivity", 0.0016),
    ads_sensitivity = param_number(ctx, "ads_sensitivity_multiplier", 0.58),
    max_mouse_delta = param_number(ctx, "max_mouse_delta", 120.0),
    hip_fov = param_number(ctx, "hip_fov", 68.0),
    ads_fov = param_number(ctx, "ads_fov", 38.0),
    hip_focus_distance = param_number(ctx, "hip_focus_distance", 9.0),
    ads_focus_distance = param_number(ctx, "ads_focus_distance", 12.0),
    ads_focus_wheel_step = param_number(ctx, "ads_focus_wheel_step", 1.5),
    hip_f_stop = param_number(ctx, "hip_f_stop", 5.6),
    ads_f_stop = param_number(ctx, "ads_f_stop", 0.85),
    hip_aperture_radius = param_number(ctx, "hip_aperture_radius", 0.0),
    ads_aperture_radius = param_number(ctx, "ads_aperture_radius", 0.075),
    aim_lock_target = param_string(ctx, "aim_lock_target", "cursor"),
    aim_lock_strength = param_number(ctx, "aim_lock_strength", 10.0),
    range_root_name = param_string(ctx, "range_root_name", "FPS Demo Root"),
    range_target_prefix = param_string(ctx, "range_target_prefix", "FPS Range Sphere Target"),
    range_target_impulse = param_number(ctx, "range_target_impulse", 0.10),
    range_target_gravity = param_number(ctx, "range_target_gravity", 9.8),
    range_target_bounce = param_number(ctx, "range_target_bounce", 0.46),
    fire_rate = param_number(ctx, "fire_rate", 7.5),
    bullet_speed = param_number(ctx, "bullet_speed", 140.0),
    bullet_lifetime = param_number(ctx, "bullet_lifetime", 2.4),
    bullet_hit_radius = param_number(ctx, "bullet_hit_radius", 0.36),
    bullet_visual_radius = param_number(ctx, "bullet_visual_radius", 0.022),
    bullet_visual_length = param_number(ctx, "bullet_visual_length", 0.16),
    flashlight_name = param_string(ctx, "flashlight_name", "FPS Player Flashlight"),
    flashlight_intensity = param_number(ctx, "flashlight_intensity", 38.0),
    flashlight_radius = param_number(ctx, "flashlight_radius", 14.0),
    flashlight_beam_degrees = param_number(ctx, "flashlight_beam_degrees", 8.0),
    flashlight_blend = param_number(ctx, "flashlight_blend", 0.10),
    flashlight_forward_offset = param_number(ctx, "flashlight_forward_offset", 0.18),
    flashlight_drop = param_number(ctx, "flashlight_drop", 0.06),
    target_name = param_string(ctx, "target_name", "FPS Hero Target Model"),
    weapon_name = param_string(ctx, "weapon_name", "FPS Player Rifle Model"),
    controls_panel_name = param_string(ctx, "controls_panel_name", "FPS Weapon Controls Panel"),
    show_controls = param_bool(ctx, "show_controls", true),
  }
end

local function ensure_state()
  if runtime_state ~= nil then
    return runtime_state
  end
  runtime_state = {
    movement = "idle",
    weapon = "idle",
    yaw = nil,
    pitch = nil,
    velocity_x = 0.0,
    velocity_z = 0.0,
    camera_height = DEFAULTS.standing_height,
    ads_blend = 0.0,
    ads_focus_offset = 0.0,
    aim_bias = 0.0,
    aim_zone = "center",
    fire_cooldown = 0.0,
    recoil_timer = 0.0,
    step_timer = 0.0,
    stride_phase = 0.0,
    bullet_slot = 0,
    muzzle_flash_slot = 0,
    impact_flash_slot = 0,
    bullets = {},
    flashes = {},
    hits = 0,
    target_hits = 0,
    target_velocities = {},
    initial_target_transforms = {},
    reset_key_down = false,
    flashlight_key_down = false,
    flashlight_on = false,
    flashlight_dirty = true,
    controls_signature = "",
    controls_refresh_timer = 0.0,
  }
  return runtime_state
end

local function copy_vec3(value)
  return {
    x = value ~= nil and value.x or 0.0,
    y = value ~= nil and value.y or 0.0,
    z = value ~= nil and value.z or 0.0,
  }
end

local function copy_quat(value)
  return {
    x = value ~= nil and value.x or 0.0,
    y = value ~= nil and value.y or 0.0,
    z = value ~= nil and value.z or 0.0,
    w = value ~= nil and value.w or 1.0,
  }
end

local function copy_transform(value)
  return {
    translation = copy_vec3(value ~= nil and value.translation or nil),
    rotation = copy_quat(value ~= nil and value.rotation or nil),
    scale = copy_vec3(value ~= nil and value.scale or { x = 1.0, y = 1.0, z = 1.0 }),
  }
end

local function nearly_equal(a, b, eps)
  return math.abs((a or 0.0) - (b or 0.0)) <= (eps or 0.0001)
end

local function vec3_nearly_equal(a, b, eps)
  a = a or { x = 0.0, y = 0.0, z = 0.0 }
  b = b or { x = 0.0, y = 0.0, z = 0.0 }
  return nearly_equal(a.x, b.x, eps) and nearly_equal(a.y, b.y, eps) and nearly_equal(a.z, b.z, eps)
end

local function quat_nearly_equal(a, b, eps)
  a = a or { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }
  b = b or { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }
  return nearly_equal(a.x, b.x, eps) and nearly_equal(a.y, b.y, eps) and
      nearly_equal(a.z, b.z, eps) and nearly_equal(a.w, b.w, eps)
end

local function transform_nearly_equal(a, b, eps)
  return vec3_nearly_equal(a ~= nil and a.translation or nil, b ~= nil and b.translation or nil, eps) and
      quat_nearly_equal(a ~= nil and a.rotation or nil, b ~= nil and b.rotation or nil, eps) and
      vec3_nearly_equal(a ~= nil and a.scale or nil, b ~= nil and b.scale or nil, eps)
end

local function lerp_vec3(a, b, t)
  return {
    x = lerp(a.x, b.x, t),
    y = lerp(a.y, b.y, t),
    z = lerp(a.z, b.z, t),
  }
end

local function default_player_transform()
  return {
    translation = { x = 0.0, y = DEFAULTS.standing_height, z = 6.4 },
    rotation = { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
    scale = { x = 1.0, y = 1.0, z = 1.0 },
  }
end

local function next_pooled_id(state, slot_name, start_id, pool_size)
  state[slot_name] = (state[slot_name] or 0) + 1
  return start_id + ((state[slot_name] - 1) % pool_size)
end

local function set_entity_transform(entity, position, rotation, scale)
  if entity == nil then
    return
  end
  local next_transform = {
    translation = copy_vec3(position),
    rotation = copy_quat(rotation),
    scale = copy_vec3(scale),
  }
  local current = entity:get_transform()
  if current ~= nil and transform_nearly_equal(current, next_transform, 0.00005) then
    return
  end
  entity:set_transform(next_transform)
end

local function hide_pooled_entity(ctx, id, hide_light)
  if ctx == nil or ctx.world == nil then
    return
  end
  local entity = ctx.world:find_entity(id)
  if entity == nil then
    return
  end
  set_entity_transform(entity,
                       DEFAULTS.inactive_translation,
                       { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
                       { x = 0.001, y = 0.001, z = 0.001 })
  if hide_light then
    local light = entity:get_light()
    if light ~= nil then
      light.intensity = 0.0
      light.radius = 0.01
      entity:set_light(light)
    end
  end
end

local function play(ctx, event_name, options)
  if ctx ~= nil and ctx.audio ~= nil then
    ctx.audio:post_event(event_name, options or {})
  end
end

local function spawn_flash(ctx,
                           state,
                           slot_name,
                           pool_start_id,
                           pool_size,
                           position,
                           scale,
                           duration,
                           light_intensity,
                           radius,
                           color,
                           scale_end)
  local id = next_pooled_id(state, slot_name, pool_start_id, pool_size)
  local flash = ctx.world:find_entity(id)
  if flash == nil then
    return
  end

  set_entity_transform(flash,
                       position,
                       { x = 0.0, y = 0.0, z = 0.0, w = 1.0 },
                       scale)
  local light = flash:get_light()
  if light ~= nil then
    light.color = copy_vec3(color)
    light.intensity = light_intensity
    light.radius = radius
    flash:set_light(light)
  end
  state.flashes[id] = {
    time_left = duration,
    duration = duration,
    scale = copy_vec3(scale),
    scale_end = scale_end ~= nil and copy_vec3(scale_end) or nil,
    growth_done = false,
    intensity = light_intensity,
    radius = radius,
  }
end

local function update_flashes(ctx, state, dt)
  for id, flash_state in pairs(state.flashes) do
    flash_state.time_left = flash_state.time_left - dt
    if flash_state.time_left <= 0.0 then
      hide_pooled_entity(ctx, id, true)
      state.flashes[id] = nil
    else
      state.flashes[id] = flash_state
      local flash = ctx.world:find_entity(id)
      if flash ~= nil then
        local transform = flash:get_transform()
        if transform ~= nil then
          local t = clamp(flash_state.time_left / math.max(0.001, flash_state.duration), 0.0, 1.0)
          local transform_dirty = false
          if flash_state.scale_end ~= nil then
            if not flash_state.growth_done and t < 0.92 then
              transform.scale = copy_vec3(flash_state.scale_end)
              flash_state.growth_done = true
              transform_dirty = true
            end
          else
            local s = math.max(0.001, t)
            transform.scale.x = flash_state.scale.x * s
            transform.scale.y = flash_state.scale.y * s
            transform.scale.z = flash_state.scale.z * s
            transform_dirty = true
          end
          if transform_dirty then
            flash:set_transform(transform)
          end
        end
        local light = flash:get_light()
        if light ~= nil then
          local t = clamp(flash_state.time_left / math.max(0.001, flash_state.duration), 0.0, 1.0)
          local target_intensity = flash_state.intensity * t * t
          local current_intensity = light.intensity or 0.0
          -- Only push assign_light when intensity steps materially. The
          -- snapshot bus race fixed by 9894cce gets worse when every active
          -- flash emits a light command per frame, so we coalesce small
          -- decay steps into the next visible threshold change.
          local intensity_delta = math.abs(current_intensity - target_intensity)
          local minimum_step = math.max(0.5, flash_state.intensity * 0.08)
          if intensity_delta >= minimum_step or t < 0.05 then
            light.intensity = target_intensity
            light.radius = flash_state.radius
            flash:set_light(light)
          end
        end
      end
    end
  end
end

local function segment_distance_to_point(ax, ay, az, bx, by, bz, px, py, pz)
  local abx = bx - ax
  local aby = by - ay
  local abz = bz - az
  local apx = px - ax
  local apy = py - ay
  local apz = pz - az
  local ab_len_sq = abx * abx + aby * aby + abz * abz
  local t = 0.0
  if ab_len_sq > 0.000001 then
    t = clamp((apx * abx + apy * aby + apz * abz) / ab_len_sq, 0.0, 1.0)
  end
  local cx = ax + abx * t
  local cy = ay + aby * t
  local cz = az + abz * t
  local dx = px - cx
  local dy = py - cy
  local dz = pz - cz
  return math.sqrt(dx * dx + dy * dy + dz * dz), { x = cx, y = cy, z = cz }
end

local function hero_target_info(ctx, settings)
  if ctx == nil or ctx.world == nil then
    return nil
  end
  local target = ctx.world:find_entity(settings.target_name)
  if target == nil then
    return nil
  end
  local transform = target:get_transform()
  if transform == nil then
    return nil
  end
  local scale = math.max(transform.scale ~= nil and transform.scale.y or 1.0, 0.25)
  local base_radius = settings.bullet_hit_radius + 0.30 * scale
  local head = {
    x = transform.translation.x,
    y = transform.translation.y + 3.05 * scale,
    z = transform.translation.z - 0.25 * scale,
  }
  local center = {
    x = transform.translation.x,
    y = transform.translation.y + 1.65 * scale,
    z = transform.translation.z - 0.20 * scale,
  }
  local feet = {
    x = transform.translation.x,
    y = transform.translation.y + 0.35 * scale,
    z = transform.translation.z - 0.08 * scale,
  }
  return {
    entity = target,
    kind = "hero",
    transform = transform,
    center = center,
    head = head,
    feet = feet,
    radius = base_radius,
    hit_points = {
      { name = "head", point = head, radius = base_radius * 0.72 },
      { name = "body", point = center, radius = base_radius },
      { name = "feet", point = feet, radius = base_radius * 0.82 },
    },
  }
end

local function sphere_target_info(entity, transform, settings)
  if entity == nil or transform == nil then
    return nil
  end
  local scale = transform.scale or { x = 0.3, y = 0.3, z = 0.3 }
  local radius = math.max(0.08, absmax3(scale.x or 0.3, scale.y or 0.3, scale.z or 0.3))
  local center = {
    x = transform.translation.x,
    y = transform.translation.y,
    z = transform.translation.z,
  }
  return {
    entity = entity,
    kind = "range_sphere",
    transform = transform,
    center = center,
    head = center,
    feet = center,
    radius = radius + settings.bullet_hit_radius * 0.18,
    hit_points = {
      { name = "sphere", point = center, radius = radius + settings.bullet_hit_radius * 0.18 },
    },
  }
end

local function collect_targets(ctx, settings)
  local targets = {}
  local hero = hero_target_info(ctx, settings)
  if hero ~= nil then
    targets[#targets + 1] = hero
  end
  if ctx == nil or ctx.world == nil then
    return targets
  end
  local root = ctx.world:find_entity(settings.range_root_name)
  if root == nil or type(ctx.world.children_of) ~= "function" then
    return targets
  end
  for _, entity in ipairs(ctx.world:children_of(root)) do
    if starts_with(entity_name(entity), settings.range_target_prefix) then
      local transform = entity:get_transform()
      local target = sphere_target_info(entity, transform, settings)
      if target ~= nil then
        targets[#targets + 1] = target
      end
    end
  end
  return targets
end

local function remember_range_target_defaults(ctx, state, settings)
  if state.initial_target_transforms == nil then
    state.initial_target_transforms = {}
  end
  for _, target in ipairs(collect_targets(ctx, settings)) do
    if target.kind == "range_sphere" and target.entity ~= nil then
      local id = target.entity:id()
      if id ~= nil and state.initial_target_transforms[id] == nil then
        state.initial_target_transforms[id] = copy_transform(target.transform)
      end
    end
  end
end

local function aim_point_for_target(target, settings, state)
  if target.kind == "range_sphere" then
    return target.center, "sphere"
  end
  local requested = string.lower(settings.aim_lock_target or "cursor")
  if requested == "head" then
    return target.head, "head"
  end
  if requested == "feet" or requested == "foot" then
    return target.feet, "feet"
  end
  if requested == "body" or requested == "torso" or requested == "center" then
    return target.center, "body"
  end

  local bias = state.aim_bias or 0.0
  if bias > 0.36 then
    return target.head, "head"
  end
  if bias < -0.36 then
    return target.feet, "feet"
  end
  return target.center, "body"
end

local function aim_target_adjustment(ctx, settings, state, transform, forward_x, forward_y, forward_z, dt)
  if state.ads_blend <= 0.01 then
    return state.yaw, state.pitch, settings.ads_focus_distance + state.ads_focus_offset
  end
  local best = nil
  for _, target in ipairs(collect_targets(ctx, settings)) do
    local aim_point, zone = aim_point_for_target(target, settings, state)
    local tx = aim_point.x - transform.translation.x
    local ty = aim_point.y - transform.translation.y
    local tz = aim_point.z - transform.translation.z
    local distance = math.max(0.5, vec_len(tx, ty, tz))
    local nx, ny, nz = normalize_vec3(tx, ty, tz, { x = forward_x, y = forward_y, z = forward_z })
    local dot = forward_x * nx + forward_y * ny + forward_z * nz
    if dot > 0.92 and (best == nil or dot > best.dot + 0.002 or
        (math.abs(dot - best.dot) <= 0.002 and distance < best.distance)) then
      best = {
        target = target,
        zone = zone,
        x = nx,
        y = ny,
        z = nz,
        dot = dot,
        distance = distance,
      }
    end
  end
  if best == nil then
    return state.yaw, state.pitch, settings.ads_focus_distance + state.ads_focus_offset
  end

  state.aim_zone = best.zone
  if best.dot > 0.94 then
    local target_yaw = math.atan(best.x, -best.z)
    local horizontal = math.sqrt(best.x * best.x + best.z * best.z)
    local target_pitch = math.atan(best.y, horizontal)
    local assist = 1.0 - math.exp(-math.max(0.0, settings.aim_lock_strength) * dt * state.ads_blend)
    state.yaw = lerp(state.yaw, target_yaw, assist)
    state.pitch = lerp(state.pitch, target_pitch, assist)
  end
  return state.yaw, state.pitch, best.distance + state.ads_focus_offset
end

local function update_controls_panel(ctx, state, settings, dt)
  if not settings.show_controls or ctx == nil or ctx.world == nil then
    return
  end
  state.controls_refresh_timer = math.max(0.0, (state.controls_refresh_timer or 0.0) - dt)
  local lines = {
    "WASD move   Shift run   Ctrl crouch",
    "Right mouse ADS   Left mouse fire   F flashlight   R reset",
    "move " .. state.movement .. "   weapon " .. state.weapon,
    "ADS aim " .. (state.aim_zone or "body") .. "   light " .. (state.flashlight_on and "on" or "off"),
    "rounds " .. tostring(state.hits) .. " hit(s)   spheres " .. tostring(state.target_hits or 0),
  }
  local signature = table.concat(lines, "\n")
  if state.controls_signature == signature or state.controls_refresh_timer > 0.0 then
    return
  end
  local panel = ctx.world:find_entity(settings.controls_panel_name)
  if panel == nil then
    return
  end
  local ui = panel:get_ui_panel()
  if ui == nil then
    return
  end
  ui.lines = lines
  panel:set_ui_panel(ui)
  state.controls_signature = signature
  state.controls_refresh_timer = 0.20
end

local function update_movement_state(input, state, settings, dt)
  local move_x = 0.0
  local move_z = 0.0
  if key(input, "W") or key(input, "up") then move_z = move_z + 1.0 end
  if key(input, "S") or key(input, "down") then move_z = move_z - 1.0 end
  if key(input, "D") or key(input, "right") then move_x = move_x + 1.0 end
  if key(input, "A") or key(input, "left") then move_x = move_x - 1.0 end
  local move_len = math.sqrt(move_x * move_x + move_z * move_z)
  if move_len > 0.000001 then
    move_x = move_x / move_len
    move_z = move_z / move_len
  end

  local moving = move_len > 0.000001
  local crouching = key(input, "control") or key(input, "ctrl")
  local running = moving and not crouching and key(input, "shift")
  if crouching then
    state.movement = moving and "crouch_move" or "crouch"
  elseif running then
    state.movement = "run"
  elseif moving then
    state.movement = "walk"
  else
    state.movement = "idle"
  end

  local speed = settings.walk_speed
  if running then speed = speed * settings.run_multiplier end
  if crouching then speed = speed * settings.crouch_multiplier end
  if state.ads_blend > 0.01 then
    speed = speed * lerp(1.0, settings.ads_move_multiplier, state.ads_blend)
  end

  local target_vx = move_x * speed
  local target_vz = move_z * speed
  local rate = moving and settings.acceleration or settings.deceleration
  state.velocity_x = approach(state.velocity_x, target_vx, rate, dt)
  state.velocity_z = approach(state.velocity_z, target_vz, rate, dt)
  state.camera_height = lerp(state.camera_height,
                             crouching and DEFAULTS.crouch_height or DEFAULTS.standing_height,
                             1.0 - math.exp(-12.0 * dt))
  return moving, running, crouching
end

local function update_weapon_model(ctx, state, settings, moving, running, crouching)
  local weapon = ctx.world:find_entity(settings.weapon_name)
  if weapon == nil then
    return
  end
  local current_transform = transform_or_default(weapon)
  local transform = copy_transform(current_transform)
  local ads = state.ads_blend
  local bob_amount = moving and (running and 0.055 or 0.032) or 0.0
  if crouching then bob_amount = bob_amount * 0.45 end
  bob_amount = bob_amount * (1.0 - ads * 0.78)
  local recoil = state.recoil_timer > 0.0 and state.recoil_timer * 0.10 or 0.0
  local bob_x = math.sin(state.stride_phase) * bob_amount
  local bob_y = math.abs(math.cos(state.stride_phase)) * bob_amount

  transform.translation.x = lerp(0.40, 0.055, ads) + bob_x
  transform.translation.y = lerp(-0.34, -0.235, ads) + bob_y - recoil
  transform.translation.z = lerp(-0.72, -0.86, ads) + recoil
  transform.scale = { x = 0.34, y = 0.34, z = 0.34 }
  transform.rotation = { x = 0.0, y = -0.7071068, z = 0.0, w = 0.7071068 }
  if not transform_nearly_equal(current_transform, transform, 0.00005) then
    weapon:set_transform(transform)
  end
end

local function muzzle_position(transform, state)
  local forward_x, forward_y, forward_z = rotate_by_quat(transform.rotation, 0.0, 0.0, -1.0)
  local right_x, right_y, right_z = rotate_by_quat(transform.rotation, 1.0, 0.0, 0.0)
  local up_x, up_y, up_z = rotate_by_quat(transform.rotation, 0.0, 1.0, 0.0)
  local ads = state.ads_blend
  local side = lerp(0.37, 0.055, ads)
  local down = lerp(-0.25, -0.16, ads)
  local out = lerp(0.92, 1.02, ads)
  return {
    x = transform.translation.x + right_x * side + up_x * down + forward_x * out,
    y = transform.translation.y + right_y * side + up_y * down + forward_y * out,
    z = transform.translation.z + right_z * side + up_z * down + forward_z * out,
  }, forward_x, forward_y, forward_z
end

local function spawn_bullet(ctx, state, settings, transform)
  local muzzle, fx, fy, fz = muzzle_position(transform, state)
  -- Re-aim from the muzzle through a point on the crosshair line at the
  -- current focus distance so bullets converge with the reticle even when
  -- the muzzle sits ~37 cm to the right of the camera at hip-fire. Without
  -- this, hip-fire bullets fly parallel to the camera forward and miss
  -- centred targets to the side every shot.
  local aim_distance = math.max(2.5, settings.ads_focus_distance + (state.ads_focus_offset or 0.0))
  local aim_x = transform.translation.x + fx * aim_distance
  local aim_y = transform.translation.y + fy * aim_distance
  local aim_z = transform.translation.z + fz * aim_distance
  local dir_x, dir_y, dir_z = normalize_vec3(aim_x - muzzle.x,
                                             aim_y - muzzle.y,
                                             aim_z - muzzle.z,
                                             { x = fx, y = fy, z = fz })
  local id = next_pooled_id(state,
                            "bullet_slot",
                            DEFAULTS.bullet_pool_start_id,
                            DEFAULTS.bullet_pool_size)
  local bullet_entity = ctx.world:find_entity(id)
  if bullet_entity == nil then
    return
  end
  local bullet_rotation = camera_look_quat(dir_x, dir_y, dir_z)
  set_entity_transform(bullet_entity,
                       muzzle,
                       bullet_rotation,
                       {
                         x = settings.bullet_visual_radius,
                         y = settings.bullet_visual_radius,
                         z = settings.bullet_visual_length,
                       })
  state.bullets[id] = {
    life = settings.bullet_lifetime,
    x = muzzle.x,
    y = muzzle.y,
    z = muzzle.z,
    vx = dir_x * settings.bullet_speed,
    vy = dir_y * settings.bullet_speed,
    vz = dir_z * settings.bullet_speed,
    rotation = bullet_rotation,
  }
  spawn_flash(ctx,
              state,
              "muzzle_flash_slot",
              DEFAULTS.muzzle_flash_pool_start_id,
              DEFAULTS.muzzle_flash_pool_size,
              muzzle,
              { x = 0.028, y = 0.028, z = 0.028 },
              0.082,
              18.0,
              0.52,
              { x = 1.0, y = 0.58, z = 0.18 },
              { x = 0.36, y = 0.36, z = 0.36 })
  play(ctx, "weapon.rifle.fire", {
    position = muzzle,
    volume = 0.88,
    pitch = 1.0,
    spatial = true,
  })
  state.recoil_timer = 1.0
end

local function update_weapon_state(ctx, state, settings, input, transform, dt)
  local ads_down = mouse(input, "right") or mouse(input, "ads") or mouse(input, "aim")
  local fire_down = mouse(input, "left") or mouse(input, "fire")
  state.ads_blend = lerp(state.ads_blend, ads_down and 1.0 or 0.0, 1.0 - math.exp(-16.0 * dt))
  if ads_down and input ~= nil then
    local wheel = input.mouse_wheel_delta or 0.0
    if math.abs(wheel) > 0.001 then
      state.ads_focus_offset = clamp(
          state.ads_focus_offset + wheel * settings.ads_focus_wheel_step,
          -settings.ads_focus_distance + 1.0,
          48.0)
    end
  end

  state.fire_cooldown = math.max(0.0, state.fire_cooldown - dt)
  state.recoil_timer = math.max(0.0, state.recoil_timer - dt * 8.0)
  if fire_down and state.fire_cooldown <= 0.0 then
    spawn_bullet(ctx, state, settings, transform)
    state.fire_cooldown = 1.0 / math.max(0.1, settings.fire_rate)
    state.weapon = "firing"
  elseif state.recoil_timer > 0.0 then
    state.weapon = ads_down and "ads_fire_recover" or "fire_recover"
  elseif state.fire_cooldown > 0.0 then
    state.weapon = ads_down and "ads_cooldown" or "cooldown"
  elseif ads_down then
    state.weapon = "ads"
  else
    state.weapon = "idle"
  end
end

local function apply_range_target_impulse(state, settings, target, bullet)
  if target == nil or target.kind ~= "range_sphere" or target.entity == nil then
    return
  end
  local id = target.entity:id()
  if id == nil then
    return
  end
  local current = state.target_velocities[id] or { vx = 0.0, vy = 0.0, vz = 0.0 }
  local impulse = math.max(0.0, settings.range_target_impulse)
  current.vx = clamp(current.vx + bullet.vx * impulse, -9.0, 9.0)
  current.vy = clamp(current.vy + math.max(1.3, math.abs(bullet.vy) * 0.05 + 1.4), -4.0, 7.0)
  current.vz = clamp(current.vz + bullet.vz * impulse, -12.0, 7.0)
  state.target_velocities[id] = current
  state.target_hits = (state.target_hits or 0) + 1
end

local function update_bullets(ctx, state, settings, dt)
  local targets = collect_targets(ctx, settings)
  for id, bullet in pairs(state.bullets) do
    local prev_x = bullet.x
    local prev_y = bullet.y
    local prev_z = bullet.z
    bullet.x = bullet.x + bullet.vx * dt
    bullet.y = bullet.y + bullet.vy * dt
    bullet.z = bullet.z + bullet.vz * dt
    bullet.life = bullet.life - dt

    local hit = false
    local hit_pos = { x = bullet.x, y = bullet.y, z = bullet.z }
    local hit_zone = nil
    local hit_target = nil
    local best_travel = nil
    for _, target in ipairs(targets) do
      if target.hit_points ~= nil then
        for _, sample in ipairs(target.hit_points) do
          local distance, closest = segment_distance_to_point(
              prev_x, prev_y, prev_z,
              bullet.x, bullet.y, bullet.z,
              sample.point.x, sample.point.y, sample.point.z)
          if distance <= sample.radius then
            local travel = vec_len(closest.x - prev_x, closest.y - prev_y, closest.z - prev_z)
            if best_travel == nil or travel < best_travel then
              hit = true
              hit_pos = closest
              hit_zone = sample.name
              hit_target = target
              best_travel = travel
            end
          end
        end
      end
    end

    local entity = ctx.world:find_entity(id)
    if entity ~= nil and not hit then
      entity:set_transform({
        translation = { x = bullet.x, y = bullet.y, z = bullet.z },
        rotation = bullet.rotation,
        scale = {
          x = settings.bullet_visual_radius,
          y = settings.bullet_visual_radius,
          z = settings.bullet_visual_length,
        },
      })
    end

    if hit then
      state.hits = state.hits + 1
      if hit_target ~= nil and hit_target.entity ~= nil then
        hit_target.entity:set_debug_value("hit " .. tostring(state.hits) .. " " .. (hit_zone or hit_target.kind))
      end
      apply_range_target_impulse(state, settings, hit_target, bullet)
      play(ctx, "weapon.bullet.impact", {
        position = hit_pos,
        volume = 1.10,
        pitch = 1.0 + (state.hits % 4) * 0.02,
        spatial = true,
      })
      spawn_flash(ctx,
                  state,
                  "impact_flash_slot",
                  DEFAULTS.impact_flash_pool_start_id,
                  DEFAULTS.impact_flash_pool_size,
                  hit_pos,
                  { x = 0.11, y = 0.11, z = 0.11 },
                  0.10,
                  4.5,
                  0.24,
                  { x = 1.0, y = 0.32, z = 0.10 })
      hide_pooled_entity(ctx, id, false)
      state.bullets[id] = nil
    elseif bullet.life <= 0.0 then
      hide_pooled_entity(ctx, id, false)
      state.bullets[id] = nil
    else
      state.bullets[id] = bullet
    end
  end
end

local function update_range_target_motion(ctx, state, settings, dt)
  if ctx == nil or ctx.world == nil or state.target_velocities == nil then
    return
  end
  for id, velocity in pairs(state.target_velocities) do
    local entity = ctx.world:find_entity(id)
    if entity == nil then
      state.target_velocities[id] = nil
    else
      local transform = entity:get_transform()
      if transform == nil then
        state.target_velocities[id] = nil
      else
        local radius = absmax3(transform.scale.x or 0.3, transform.scale.y or 0.3, transform.scale.z or 0.3)
        velocity.vy = velocity.vy - settings.range_target_gravity * dt
        transform.translation.x = transform.translation.x + velocity.vx * dt
        transform.translation.y = transform.translation.y + velocity.vy * dt
        transform.translation.z = transform.translation.z + velocity.vz * dt

        local floor_y = radius + 0.02
        if transform.translation.y < floor_y then
          transform.translation.y = floor_y
          if velocity.vy < 0.0 then
            velocity.vy = -velocity.vy * settings.range_target_bounce * 0.5
          end
          velocity.vx = velocity.vx * 0.55
          velocity.vz = velocity.vz * 0.55
        end

        entity:set_transform(transform)
        local speed = math.abs(velocity.vx) + math.abs(velocity.vy) + math.abs(velocity.vz)
        -- Aggressive settle threshold: a bouncing sphere is the dominant
        -- per-frame transform mutator after a hit, and the snapshot publish
        -- path that crashes is hit hardest while the target is moving. Stop
        -- pushing transforms as soon as motion is small rather than waiting
        -- for sub-cm/s speeds.
        if speed < 0.18 and transform.translation.y <= floor_y + 0.01 then
          state.target_velocities[id] = nil
        else
          state.target_velocities[id] = velocity
        end
      end
    end
  end
end

local function update_flashlight(ctx, state, settings, player_transform)
  if ctx == nil or ctx.world == nil then
    return
  end
  -- Skip every-frame flashlight work when the light is off. The flashlight
  -- entity has a light component, so set_entity_transform() on it during
  -- camera turns triggers markLightTouched in qtApplyScriptCommandsToDocument
  -- and re-publishes a light delta every tick. With the player's kinematic
  -- targets also moving after a hit, that compounds the snapshot-bus pressure
  -- that takes down the latest crash. When the flashlight is off we have no
  -- visible effect to maintain, so we bail out unless flashlight_dirty is set
  -- (toggle just happened — push the off-state once and clear the flag).
  local flashlight_active = state.flashlight_on or state.flashlight_dirty
  if not flashlight_active then
    return
  end
  local flashlight = ctx.world:find_entity(settings.flashlight_name)
  if flashlight == nil then
    return
  end

  -- Anchor the flashlight to the camera every frame so it actually behaves like
  -- a flashlight: a fixed-world spot is a stage light, not a hand torch.
  local cam_pos = player_transform ~= nil and player_transform.translation
      or { x = 0.0, y = DEFAULTS.standing_height, z = 6.4 }
  local cam_rot = player_transform ~= nil and player_transform.rotation
      or { x = 0.0, y = 0.0, z = 0.0, w = 1.0 }
  local fwd_x, fwd_y, fwd_z = rotate_by_quat(cam_rot, 0.0, 0.0, -1.0)
  fwd_x, fwd_y, fwd_z = normalize_vec3(fwd_x, fwd_y, fwd_z, { x = 0.0, y = 0.0, z = -1.0 })

  local fwd_offset = settings.flashlight_forward_offset or 0.18
  local drop = settings.flashlight_drop or 0.10
  local position = {
    x = cam_pos.x + fwd_x * fwd_offset,
    y = cam_pos.y - drop + fwd_y * fwd_offset,
    z = cam_pos.z + fwd_z * fwd_offset,
  }
  set_entity_transform(flashlight,
                       position,
                       cam_rot,
                       { x = 0.10, y = 0.10, z = 0.10 })

  -- The renderer derives spot direction from the entity rotation when
  -- light.direction is zero (see light_direction_from_component). The entity
  -- transform above is updated every frame, so we can leave the direction at
  -- zero here and avoid pushing assign_light commands every frame — those would
  -- spam the snapshot bus and amplify the lazy path-tracer-scene init race
  -- that fix(scene) 9894cce repaired.
  local current = flashlight:get_light()
  local target_intensity = state.flashlight_on and settings.flashlight_intensity or 0.0
  local needs_update = state.flashlight_dirty
  if not needs_update and current ~= nil then
    needs_update = current.type ~= "spot" or
        not nearly_equal(current.intensity or 0.0, target_intensity, 0.005) or
        not nearly_equal(current.radius or 0.0, settings.flashlight_radius, 0.01) or
        not nearly_equal(current.beam_angle or 0.0, settings.flashlight_beam_degrees, 0.05) or
        not nearly_equal(current.blend or 0.0, settings.flashlight_blend, 0.01)
  elseif current == nil then
    needs_update = true
  end
  if needs_update then
    local light = current ~= nil and current or {}
    light.type = "spot"
    light.color = { x = 1.0, y = 0.95, z = 0.82 }
    light.intensity = target_intensity
    light.radius = settings.flashlight_radius
    light.direction = { x = 0.0, y = 0.0, z = 0.0 }
    light.beam_angle = settings.flashlight_beam_degrees
    light.blend = settings.flashlight_blend
    flashlight:set_light(light)
    state.flashlight_dirty = false
  end
end

local function reset_gameplay_scene(ctx, self, state, settings)
  state.movement = "idle"
  state.weapon = "idle"
  state.yaw = 0.0
  state.pitch = 0.0
  state.velocity_x = 0.0
  state.velocity_z = 0.0
  state.camera_height = DEFAULTS.standing_height
  state.ads_blend = 0.0
  state.ads_focus_offset = 0.0
  state.aim_bias = 0.0
  state.aim_zone = "body"
  state.fire_cooldown = 0.0
  state.recoil_timer = 0.0
  state.step_timer = 0.0
  state.stride_phase = 0.0
  state.bullets = {}
  state.flashes = {}
  state.hits = 0
  state.target_hits = 0
  state.target_velocities = {}
  state.flashlight_on = false
  state.flashlight_dirty = true
  state.controls_signature = ""
  state.controls_refresh_timer = 0.0

  for i = 0, DEFAULTS.bullet_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.bullet_pool_start_id + i, false)
  end
  for i = 0, DEFAULTS.muzzle_flash_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.muzzle_flash_pool_start_id + i, true)
  end
  for i = 0, DEFAULTS.impact_flash_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.impact_flash_pool_start_id + i, true)
  end

  remember_range_target_defaults(ctx, state, settings)
  if ctx ~= nil and ctx.world ~= nil and state.initial_target_transforms ~= nil then
    for id, saved in pairs(state.initial_target_transforms) do
      local target = ctx.world:find_entity(id)
      if target ~= nil then
        target:set_transform(copy_transform(saved))
        target:set_debug_value("")
      end
    end
  end

  local transform = default_player_transform()
  self:set_transform(transform)
  update_flashlight(ctx, state, settings, transform)
  return transform
end

function script.on_update(self, ctx)
  if ctx == nil or ctx.world == nil then
    return
  end
  local state = ensure_state()
  local settings = read_settings(ctx)
  local input = ctx.input or {}
  local dt = clamp(ctx.dt or ctx.delta_seconds or (1.0 / 60.0), 0.0, DEFAULTS.max_dt)
  local transform = transform_or_default(self)
  local original_transform = copy_transform(transform)

  if state.yaw == nil or state.pitch == nil then
    state.yaw, state.pitch = yaw_pitch_from_transform(transform)
  end
  remember_range_target_defaults(ctx, state, settings)

  local reset_down = key(input, "R") or key(input, "r")
  if reset_down and not state.reset_key_down then
    transform = reset_gameplay_scene(ctx, self, state, settings)
    original_transform = copy_transform(transform)
  end
  state.reset_key_down = reset_down

  local flashlight_down = key(input, "F") or key(input, "f")
  if flashlight_down and not state.flashlight_key_down then
    state.flashlight_on = not state.flashlight_on
    state.flashlight_dirty = true
  end
  state.flashlight_key_down = flashlight_down

  local mouse_scale = lerp(1.0, settings.ads_sensitivity, state.ads_blend)
  local mouse_dx = clamp(input.mouse_delta_x or 0.0, -settings.max_mouse_delta, settings.max_mouse_delta)
  local mouse_dy = clamp(input.mouse_delta_y or 0.0, -settings.max_mouse_delta, settings.max_mouse_delta)
  if math.abs(mouse_dx) < 0.01 then mouse_dx = 0.0 end
  if math.abs(mouse_dy) < 0.01 then mouse_dy = 0.0 end
  state.yaw = state.yaw + mouse_dx * settings.yaw_sensitivity * mouse_scale
  state.pitch = clamp(state.pitch - mouse_dy * settings.pitch_sensitivity * mouse_scale,
                      DEFAULTS.min_pitch,
                      DEFAULTS.max_pitch)
  if state.ads_blend > 0.01 then
    state.aim_bias = clamp((state.aim_bias or 0.0) - mouse_dy * 0.025, -1.0, 1.0)
    if mouse_dy == 0.0 then
      state.aim_bias = approach(state.aim_bias, 0.0, 0.22, dt)
    end
  else
    state.aim_bias = approach(state.aim_bias or 0.0, 0.0, 2.8, dt)
    state.aim_zone = "body"
  end

  local moving, running, crouching = update_movement_state(input, state, settings, dt)
  local cos_pitch = math.cos(state.pitch)
  local forward_x = math.sin(state.yaw) * cos_pitch
  local forward_y = math.sin(state.pitch)
  local forward_z = -math.cos(state.yaw) * cos_pitch
  local focus_target
  state.yaw, state.pitch, focus_target =
      aim_target_adjustment(ctx, settings, state, transform, forward_x, forward_y, forward_z, dt)
  cos_pitch = math.cos(state.pitch)
  forward_x = math.sin(state.yaw) * cos_pitch
  forward_y = math.sin(state.pitch)
  forward_z = -math.cos(state.yaw) * cos_pitch

  local flat_forward_x, _, flat_forward_z = normalize_vec3(
      math.sin(state.yaw), 0.0, -math.cos(state.yaw), { x = 0.0, y = 0.0, z = -1.0 })
  local right_x = math.cos(state.yaw)
  local right_z = math.sin(state.yaw)
  transform.translation.x = transform.translation.x +
      (right_x * state.velocity_x + flat_forward_x * state.velocity_z) * dt
  transform.translation.z = transform.translation.z +
      (right_z * state.velocity_x + flat_forward_z * state.velocity_z) * dt
  transform.translation.y = state.camera_height
  transform.rotation = camera_look_quat(forward_x, forward_y, forward_z)
  if not transform_nearly_equal(original_transform, transform, 0.00005) then
    self:set_transform(transform)
  end
  update_flashlight(ctx, state, settings, transform)

  local speed = math.sqrt(state.velocity_x * state.velocity_x + state.velocity_z * state.velocity_z)
  if moving and speed > 0.15 then
    state.stride_phase = state.stride_phase + dt * (running and 15.0 or 10.5) * (crouching and 0.55 or 1.0)
    state.step_timer = state.step_timer - dt
    if state.step_timer <= 0.0 then
      state.step_timer = running and 0.30 or (crouching and 0.62 or 0.45)
      play(ctx, "player.footstep.dirt", {
        position = transform.translation,
        volume = running and 0.68 or (crouching and 0.28 or 0.48),
        pitch = running and 1.10 or (crouching and 0.82 or 0.96),
        spatial = true,
      })
    end
  else
    state.step_timer = 0.0
    state.stride_phase = state.stride_phase + dt * 2.0
  end

  update_weapon_state(ctx, state, settings, input, transform, dt)
  update_weapon_model(ctx, state, settings, moving, running, crouching)
  update_bullets(ctx, state, settings, dt)
  update_range_target_motion(ctx, state, settings, dt)
  update_flashes(ctx, state, dt)

  local camera = self:get_camera()
  if camera ~= nil then
    local next_fov = lerp(settings.hip_fov, settings.ads_fov, state.ads_blend)
    local next_focus = lerp(settings.hip_focus_distance, clamp(focus_target, 0.5, 120.0), state.ads_blend)
    local next_f_stop = lerp(settings.hip_f_stop, settings.ads_f_stop, state.ads_blend)
    local next_aperture = lerp(settings.hip_aperture_radius, settings.ads_aperture_radius, state.ads_blend)
    if not nearly_equal(camera.fov, next_fov, 0.0005) or
        not nearly_equal(camera.focus_distance, next_focus, 0.0005) or
        not nearly_equal(camera.f_stop, next_f_stop, 0.0005) or
        not nearly_equal(camera.aperture_radius, next_aperture, 0.0005) then
      camera.fov = next_fov
      camera.focus_distance = next_focus
      camera.f_stop = next_f_stop
      camera.aperture_radius = next_aperture
      self:set_camera(camera)
    end
  end

  update_controls_panel(ctx, state, settings, dt)
end

local function clean_pooled_entities(ctx)
  -- Tucks every pool entity back to the inactive translation and zeroes pool
  -- lights. Called from on_load so that re-entering Play mode does not leave
  -- bullets or muzzle/impact flashes scattered across the range from the
  -- previous session.
  for i = 0, DEFAULTS.bullet_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.bullet_pool_start_id + i, false)
  end
  for i = 0, DEFAULTS.muzzle_flash_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.muzzle_flash_pool_start_id + i, true)
  end
  for i = 0, DEFAULTS.impact_flash_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.impact_flash_pool_start_id + i, true)
  end
end

function script.on_load(self, ctx)
  if ctx == nil or ctx.world == nil then
    return
  end
  -- Lua VM resets across binding reload, so runtime_state starts at nil. Build
  -- a fresh state and clean any scene leftover from the previous Play session.
  runtime_state = nil
  local state = ensure_state()
  local settings = read_settings(ctx)
  state.flashlight_on = false
  state.flashlight_dirty = true
  clean_pooled_entities(ctx)

  local flashlight = ctx.world:find_entity(settings.flashlight_name)
  if flashlight ~= nil then
    local light = flashlight:get_light() or {}
    light.type = "spot"
    light.intensity = 0.0
    light.radius = settings.flashlight_radius
    light.beam_angle = settings.flashlight_beam_degrees
    light.blend = settings.flashlight_blend
    light.direction = { x = 0.0, y = 0.0, z = 0.0 }
    flashlight:set_light(light)
  end

  local hero = ctx.world:find_entity(settings.target_name)
  if hero ~= nil and type(hero.set_debug_value) == "function" then
    hero:set_debug_value("")
  end
end

function script.on_disable(self, ctx)
  if ctx == nil or ctx.world == nil then
    return
  end
  -- Leave the scene in a clean editor state: flashlight off (with a zeroed
  -- direction so the renderer falls back to the entity rotation rather than
  -- holding a stale direction baked from the previous play session), every
  -- pooled bullet/flash hidden so the editor view does not see leftover
  -- tracer streaks or muzzle/impact flashes from the last session.
  local settings = read_settings(ctx)
  local flashlight = ctx.world:find_entity(settings.flashlight_name)
  if flashlight ~= nil then
    local light = flashlight:get_light() or {}
    light.type = "spot"
    light.intensity = 0.0
    light.radius = settings.flashlight_radius
    light.beam_angle = settings.flashlight_beam_degrees
    light.blend = settings.flashlight_blend
    light.direction = { x = 0.0, y = 0.0, z = 0.0 }
    flashlight:set_light(light)
  end
  for i = 0, DEFAULTS.bullet_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.bullet_pool_start_id + i, false)
  end
  for i = 0, DEFAULTS.muzzle_flash_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.muzzle_flash_pool_start_id + i, true)
  end
  for i = 0, DEFAULTS.impact_flash_pool_size - 1 do
    hide_pooled_entity(ctx, DEFAULTS.impact_flash_pool_start_id + i, true)
  end
  if runtime_state ~= nil then
    runtime_state.bullets = {}
    runtime_state.flashes = {}
    runtime_state.target_velocities = {}
  end
end

return script
