-- @editor animation_enabled bool default=true label="Animation"
-- @editor model_scale number default=1.05 min=0.2 max=3.0 step=0.01 label="Model Scale"
-- @editor spin_speed_degrees number default=24.0 min=-180 max=180 step=1 label="Spin Speed"
-- @editor bob_height number default=0.16 min=0 max=1.5 step=0.01 label="Bob Height"
-- @editor bob_speed number default=1.4 min=0 max=8 step=0.1 label="Bob Speed"
-- @editor breathe_amount number default=0.08 min=0 max=0.5 step=0.01 label="Scale Pulse"
-- @editor breathe_speed number default=2.2 min=0 max=10 step=0.1 label="Pulse Speed"
-- @editor wobble_degrees number default=5.0 min=0 max=35 step=0.5 label="Wobble"
-- @editor wobble_speed number default=1.7 min=0 max=10 step=0.1 label="Wobble Speed"
-- @editor color_cycle bool default=true label="Cycle Materials"
-- @editor target_material_id number default=9812 min=9810 max=9815 step=1 label="Model Material"
-- @editor palette_start number default=9810 min=9810 max=9815 step=1 label="Palette Start"
-- @editor palette_count number default=6 min=1 max=6 step=1 label="Palette Count"
-- @editor color_cycle_speed number default=0.45 min=0 max=4 step=0.05 label="Material Cycle Speed"
-- @editor hue_degrees number default=205 min=0 max=360 step=1 label="Light Hue"
-- @editor hue_cycle_speed number default=18 min=-180 max=180 step=1 label="Hue Cycle Speed"
-- @editor key_intensity number default=34 min=0 max=120 step=1 label="Key Intensity"
-- @editor rim_intensity number default=16 min=0 max=80 step=1 label="Rim Intensity"
-- @editor orbiter_enabled bool default=true label="Orbiters"
-- @editor orbiter_radius number default=1.75 min=0 max=5 step=0.05 label="Orbiter Radius"
-- @editor orbiter_speed number default=1.1 min=-6 max=6 step=0.05 label="Orbiter Speed"
-- @editor orbiter_scale number default=0.16 min=0.02 max=0.7 step=0.01 label="Orbiter Scale"
-- @editor accent_material_id number default=9814 min=9810 max=9815 step=1 label="Orbiter Material"
-- @editor camera_fov number default=42 min=20 max=90 step=1 label="Camera FOV"
-- @editor camera_focus_distance number default=5.8 min=0.5 max=30 step=0.1 label="Focus Distance"
-- @editor show_panel bool default=true label="Show In-Scene Panel"

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

local function clamp(value, lo, hi)
  return math.max(lo, math.min(hi, value))
end

local function yaw_quat(yaw)
  local half = yaw * 0.5
  return { x = 0.0, y = math.sin(half), z = 0.0, w = math.cos(half) }
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

local function hsv_to_rgb(h, s, v)
  h = h - math.floor(h)
  local i = math.floor(h * 6.0)
  local f = h * 6.0 - i
  local p = v * (1.0 - s)
  local q = v * (1.0 - f * s)
  local t = v * (1.0 - (1.0 - f) * s)
  local m = i % 6
  if m == 0 then return v, t, p end
  if m == 1 then return q, v, p end
  if m == 2 then return p, v, t end
  if m == 3 then return p, q, v end
  if m == 4 then return t, p, v end
  return v, p, q
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

local function read_settings(ctx)
  return {
    animation_enabled = param_bool(ctx, "animation_enabled", true),
    model_scale = param_number(ctx, "model_scale", 1.05),
    spin_speed_degrees = param_number(ctx, "spin_speed_degrees", 24.0),
    bob_height = param_number(ctx, "bob_height", 0.16),
    bob_speed = param_number(ctx, "bob_speed", 1.4),
    breathe_amount = param_number(ctx, "breathe_amount", 0.08),
    breathe_speed = param_number(ctx, "breathe_speed", 2.2),
    wobble_degrees = param_number(ctx, "wobble_degrees", 5.0),
    wobble_speed = param_number(ctx, "wobble_speed", 1.7),
    color_cycle = param_bool(ctx, "color_cycle", true),
    target_material_id = math.floor(param_number(ctx, "target_material_id", 9812) + 0.5),
    palette_start = math.floor(param_number(ctx, "palette_start", 9810) + 0.5),
    palette_count = math.max(1, math.floor(param_number(ctx, "palette_count", 6) + 0.5)),
    color_cycle_speed = param_number(ctx, "color_cycle_speed", 0.45),
    hue_degrees = param_number(ctx, "hue_degrees", 205.0),
    hue_cycle_speed = param_number(ctx, "hue_cycle_speed", 18.0),
    key_intensity = param_number(ctx, "key_intensity", 34.0),
    rim_intensity = param_number(ctx, "rim_intensity", 16.0),
    orbiter_enabled = param_bool(ctx, "orbiter_enabled", true),
    orbiter_radius = param_number(ctx, "orbiter_radius", 1.75),
    orbiter_speed = param_number(ctx, "orbiter_speed", 1.1),
    orbiter_scale = param_number(ctx, "orbiter_scale", 0.16),
    accent_material_id = math.floor(param_number(ctx, "accent_material_id", 9814) + 0.5),
    camera_fov = param_number(ctx, "camera_fov", 42.0),
    camera_focus_distance = param_number(ctx, "camera_focus_distance", 5.8),
    show_panel = param_bool(ctx, "show_panel", true),
  }
end

local function selected_material(settings, t)
  if settings.color_cycle then
    local count = clamp(settings.palette_count, 1, 6)
    local offset = math.floor(math.max(0.0, t * settings.color_cycle_speed)) % count
    return settings.palette_start + offset
  end
  return settings.target_material_id
end

local function assign_material_tree(ctx, entity, material_id, depth)
  if ctx == nil or ctx.world == nil or entity == nil or material_id <= 0 then
    return
  end
  ctx.world:assign_material(entity, material_id)
  if depth <= 0 then
    return
  end
  for _, child in ipairs(ctx.world:children_of(entity) or {}) do
    assign_material_tree(ctx, child, material_id, depth - 1)
  end
end

local function update_light(ctx, name, intensity, hue, saturation, value)
  local entity = ctx.world:find_entity(name)
  if entity == nil then
    return
  end
  local light = entity:get_light()
  if light == nil then
    return
  end
  local r, g, b = hsv_to_rgb(hue, saturation, value)
  light.color = { x = r, y = g, z = b }
  light.intensity = intensity
  entity:set_light(light)
end

local function update_orbiter(ctx, name, phase, settings, t, material_id)
  local entity = ctx.world:find_entity(name)
  if entity == nil then
    return
  end
  local transform = transform_or_default(entity)
  local angle = t * settings.orbiter_speed + phase
  transform.translation.x = math.cos(angle) * settings.orbiter_radius
  transform.translation.y = 1.05 + math.sin(angle * 2.0) * 0.22
  transform.translation.z = math.sin(angle) * settings.orbiter_radius
  transform.scale = {
    x = settings.orbiter_scale,
    y = settings.orbiter_scale,
    z = settings.orbiter_scale,
  }
  transform.rotation = yaw_quat(angle)
  entity:set_transform(transform)
  ctx.world:assign_material(entity, material_id)
end

local function update_camera(ctx, settings)
  local camera = ctx.world:find_entity("Live Edit Camera")
  if camera == nil then
    return
  end
  local camera_component = camera:get_camera()
  if camera_component == nil then
    return
  end
  camera_component.fov = settings.camera_fov
  camera_component.focus_distance = settings.camera_focus_distance
  camera:set_camera(camera_component)
end

local function update_panel(ctx, settings, material_id, hue)
  local panel_entity = ctx.world:find_entity("Live Edit Control Panel")
  if panel_entity == nil then
    return
  end
  local panel = panel_entity:get_ui_panel()
  if panel == nil then
    return
  end
  panel.visible = settings.show_panel
  panel.lines = {
    "Live Edit Model Lab",
    "material " .. tostring(material_id) .. "  hue " .. string.format("%.0f", hue * 360.0),
    "scale " .. string.format("%.2f", settings.model_scale) ..
        "  spin " .. string.format("%.0f", settings.spin_speed_degrees),
    "bob " .. string.format("%.2f", settings.bob_height) ..
        "  pulse " .. string.format("%.2f", settings.breathe_amount),
    "orbit " .. tostring(settings.orbiter_enabled) ..
        "  radius " .. string.format("%.2f", settings.orbiter_radius),
  }
  panel_entity:set_ui_panel(panel)
end

function script.on_load(self, ctx)
  self:log("live edit model lab loaded")
end

function script.on_update(self, ctx)
  if ctx == nil or ctx.world == nil then
    return
  end
  local settings = read_settings(ctx)
  local t = ctx.elapsed_seconds or 0.0
  local material_id = selected_material(settings, t)
  local hue = ((settings.hue_degrees + t * settings.hue_cycle_speed) % 360.0) / 360.0

  local transform = transform_or_default(self)
  if script.base_y == nil then
    script.base_y = transform.translation.y
  end
  if settings.animation_enabled then
    local pulse = 1.0 + math.sin(t * settings.breathe_speed) * settings.breathe_amount
    local scale = math.max(0.01, settings.model_scale * pulse)
    local yaw = math.rad(t * settings.spin_speed_degrees)
    local roll = math.rad(math.sin(t * settings.wobble_speed) * settings.wobble_degrees)
    transform.translation.y = script.base_y + math.sin(t * settings.bob_speed) * settings.bob_height
    transform.scale = { x = scale, y = scale, z = scale }
    transform.rotation = quat_mul(yaw_quat(yaw), roll_quat(roll))
  else
    transform.scale = {
      x = settings.model_scale,
      y = settings.model_scale,
      z = settings.model_scale,
    }
  end
  self:set_transform(transform)

  assign_material_tree(ctx, self, material_id, 4)
  update_light(ctx, "Live Edit Key Light", settings.key_intensity, 0.095, 0.34, 1.0)
  update_light(ctx, "Live Edit Rim Light", settings.rim_intensity, hue, 0.78, 1.0)
  update_light(ctx, "Live Edit Floor Glow", settings.rim_intensity * 0.32, hue + 0.12, 0.66, 0.85)
  update_camera(ctx, settings)

  if settings.orbiter_enabled then
    update_orbiter(ctx, "Live Edit Orbiter A", 0.0, settings, t, settings.accent_material_id)
    update_orbiter(ctx, "Live Edit Orbiter B", math.pi, settings, t, material_id)
  end
  update_panel(ctx, settings, material_id, hue)
end

return script
