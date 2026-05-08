# Lua Scripting

Lua scripts attach to scene entities through `script` components. Runtime mode controls hook dispatch: Edit keeps scripts stopped, F1 toggles full Play mode with mouse lock and gameplay input, and F2 enters Live Edit. While Live Edit is active, F2 toggles the Live Edit mouse lock and Lua viewport input without stopping scripts.

## Component

```json
"script": {
  "source": "assets/scripts/my_controller.lua",
  "language": "lua",
  "entry": "default",
  "module_id": "player_controller",
  "enabled": true,
  "reload_on_save": true,
  "params": {
    "speed": "3.5",
    "target": "42",
    "mode": "patrol"
  }
}
```

`source` is the script path. Legacy `path` is accepted on load and normalized back to `source` on export. `params` currently preserve scalar values as strings for compatibility with existing scenes; the Qt script panel infers text, number, and boolean editors from those values and writes changes back through scene commands. Scripts should convert with `tonumber(...)` or string comparisons when they need typed values.

Scenes may also declare a top-level `scene_script` block with the same fields. Scene init scripts are injected into the runtime world as transient bindings for Play and Live Edit; the injection does not mutate source JSON.

Scripts can declare editor metadata and defaults with Lua comments. These annotations do not replace scene `params`; `default=...` values populate missing defaults in `ctx.params`, and every annotation tells the Qt scripting panel which editor control to use. Scene-authored params still win and remain the persisted source of truth.

```lua
-- @editor walk_speed number default=4.2 min=0 max=20 step=0.1 label="Walk Speed"
-- @editor show_controls bool default=true
-- [editor] focus_distance number min=0 max=200 step=0.25 label="Focus Distance"
local script = {}

function script.on_update(self, ctx)
  local speed = tonumber(ctx.params.walk_speed or "4.2") or 4.2
end

return script
```

Supported annotation fields are `type`, `default`, `label`, `min`, `max`, and `step`. The shorthand form above treats the first token after the name as the type. `-- @editor` and `-- [editor]` are both accepted; omit `default` for optional params that should appear in the panel without changing runtime behavior until the user authors a value.

## Lifecycle

Scripts return a table. Any of these hooks may be defined:

```lua
local script = {}

function script.on_load(self, ctx) end
function script.on_spawn(self, ctx) end
function script.on_enable(self, ctx) end
function script.on_update(self, ctx) end
function script.on_fixed_update(self, ctx) end
function script.on_late_update(self, ctx) end
function script.on_disable(self, ctx) end
function script.on_destroy(self, ctx) end
function script.on_unload(self, ctx) end

return script
```

The runtime dispatches bindings in stable entity order. Commands are written to `WorldCommandBuffer` and replayed by the host; scripts do not receive mutable ECS or renderer objects.

## Context API

`ctx` includes:

- `entity_id`, `frame`, `elapsed_seconds`, `delta_seconds`, `dt`, `fixed_delta_seconds`
- `deterministic`, `params`
- `input:key_down(key)`, `input:mouse_delta()`, raw mouse delta fields
- `world:find_entity(id_or_name)`, `world:entity(id_or_name)`, `world:children_of(entity_or_id)`, `world:spawn_entity(def)`, `world:destroy_entity(entity_or_id)`
- `world:has_component(entity_or_id, name)`, `world:reparent_entity(child, parent)`, `world:reorder_entity(moved, before, after)`, `world:remove_component(entity, name)`, `world:assign_material(entity, material_id)`
- `scene:main_camera()`, `scene:find_entity(id_or_name)`, `scene:entities_with_component(name)`, `scene:ensure_script(entity, source, params)`, `scene:use_system(module_name)`, `scene:register_interactable(entity, config)`
- `diagnostic(level, message)`, `include(source)`
- `audio:post_event(event, options)`, `audio:stop(handle)`

Entity handles expose methods: `id()`, `get_name()`, `set_name(name)`, `get_transform()`, `set_transform(transform)`, `get_light()`, `set_light(light)`, `get_camera()`, `set_camera(camera)`, `get_physics()`, `get_ui_panel()`, `set_ui_panel(panel)`, `log(message)`, and `set_debug_value(name, value)`.

`world:spawn_entity(def)` accepts a table with these fields:

```lua
ctx.world:spawn_entity({
  id = 9200,                 -- optional; runtime allocates when omitted
  name = "Spawned Actor",
  parent = self:id(),        -- optional entity id or handle
  transform = {
    translation = { x = 0, y = 1, z = 0 },
    rotation = { x = 0, y = 0, z = 0, w = 1 },
    scale = { x = 1, y = 1, z = 1 },
  },
  mesh = { geometry_id = 7202, material_id = 7102 },
  sdf = { shape = "sphere", material_id = 7102 },
  material = { material_id = 7102 },
  script = { source = "assets/scripts/child.lua", enabled = true },
  ui_panel = {
    title = "Controls",
    anchor = "top_left",
    lines = { "WASD  Move", "Mouse  Camera" },
  },
  light = { type = "point", intensity = 4.0 },
  camera = { fov = 55.0 },
})
```

## Minimal Script

```lua
local script = {}

function script.on_update(self, ctx)
  local transform = self:get_transform()
  if transform == nil then return end
  transform.translation.x = transform.translation.x + (tonumber(ctx.params.speed or "1") * ctx.dt)
  self:set_transform(transform)
end

return script
```

## Generic FPS Camera

`assets/scripts/systems/generic_fps_camera.lua` is the canonical reusable game-mode controller for any active camera entity. `assets/scripts/generic_fps_camera.lua` remains as a compatibility wrapper for older scenes. Attach either path as that camera's `script` component, then tune behavior through params:

```json
"script": {
  "source": "assets/scripts/systems/generic_fps_camera.lua",
  "language": "lua",
  "entry": "default",
  "enabled": true,
  "reload_on_save": true,
  "params": {
    "movement_mode": "walk",
    "walk_speed": "4.2",
    "run_multiplier": "1.75",
    "mouse_yaw_sensitivity": "0.002",
    "mouse_pitch_sensitivity": "0.0016",
    "fixed_y": "1.72",
    "min_x": "-20",
    "max_x": "20",
    "min_z": "-14.8",
    "max_z": "14.4",
    "show_controls": "true"
  }
}
```

`movement_mode` can be `walk` for flat FPS movement or `fly`/`noclip` for full 3D movement. Bounds and `fixed_y` are optional, so the same script can be dropped onto different cameras and scenes without code edits.

## Live Edit Workflow

Live Edit splits the old game-mode behavior into three authoring states:

| Mode | Scripts | Editor canvas | Input |
| --- | --- | --- | --- |
| Edit | Stopped | Selection, gizmos, inspector, and asset drops enabled | Editor shortcuts only |
| Live Edit | Running | Selection, gizmos, inspector, script params, and reload stay enabled | Gameplay input is off unless viewport input is armed |
| Play | Running | Editor selection, gizmos, drops, and inspector scene edits blocked | Gameplay input active; mouse may lock |

Two-way sync uses the scene command path in both directions. Lua writes such as `set_transform`, `set_light`, or `set_camera` must update the runtime world, scene document, inspector values, selection bounds, gizmos, scene tree, and render state in the same frame. Manual editor writes from gizmos, inspector fields, reparenting, or script params must update the canonical scene/world before the next script tick.

Conflict policy: while an editor drag or inspector edit is active, the editor value wins for the touched `(entity, component)`. Script writes to that same component are suppressed or deferred, logged with entity id, component, script source, frame, and editor action, then resume from the committed editor value after release.

Manual QA scene: open `assets/scenes/live_edit_model_lab.json`. It uses the tracked low-poly hero gameplay model and `assets/scripts/live_edit_model_lab.lua`, which exposes annotated controls for color cycling, model scale, bob/spin/wobble animation, orbiters, camera focus, lights, and an in-scene UI panel.

```powershell
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe `
  --window --platform qt --backend d3d12 `
  --scene assets\scenes\live_edit_model_lab.json
```

Manual pass:

1. Enter Live Edit from the script panel or mode controls. Confirm scripts tick, the mouse stays unlocked, and editor selection/gizmos still work.
2. Select `Live Edit Scripted Model Rig`, move or scale it with the gizmo, and confirm the viewport, inspector, and selection bounds update immediately.
3. In the scripting panel, edit params such as `model_scale`, `target_material_id`, `hue_degrees`, `orbiter_enabled`, `key_intensity`, and `camera_focus_distance`; confirm the model, props, lighting, and camera react on the next tick.
4. Toggle `animation_enabled` and `color_cycle`, reload scripts, and confirm the scripted defaults and authored params remain stable.
5. Arm Live Edit viewport input, focus the viewport, and use WASD/mouse to drive `Live Edit Camera`; disarm input and confirm scripts continue without consuming gameplay input.
6. Switch to Play, confirm gameplay input and optional mouse lock are active while editor scene edits are blocked, then exit to Edit and confirm scripts stop.

## Sandbox And Budgets

Scripts get math, table, string, and safe base functions. `io`, `os`, `package`, `debug`, `require`, `dofile`, `loadfile`, `load`, `print`, and `collectgarbage` are unavailable. Each hook has an instruction budget and Lua memory budget; budget failures are reported as script diagnostics and disable the binding until scripts are reloaded.

## Qt Scripting Panel

The Qt scripting dock shows runtime mode state, Lua availability, binding counts, available Lua files, attach/detach/new/open/reload controls, lifecycle hook dispatch buttons, editable script fields, inferred text/number/boolean params, recent diagnostics, runtime per-binding state, and captured variables/upvalues after hook execution. Use the dock reload button after editing scripts or params.
