# Game Engine TODOs

Path-traced game engine for small demos. Items are ordered **low-level → high-level**: lower-numbered sections are dependencies that higher-numbered sections build on. Within a section, tasks are granular and ordered the same way.

Conventions:
- `[ ]` open · `[x]` done
- `Files:` is the expected touch list (not exhaustive)
- `Depends on:` references other task IDs
- `Done when:` is the acceptance signal — usually a smoke target, gate run, or visible behaviour

This is the live punch list. Anything not in it is out of scope. Networking, store integration, and multi-language localization are explicitly deferred.

---

## §1 Foundations — engine primitives nothing else can ship without

These are the bedrock: until they exist, every higher-level system has to fake or stub them.

### 1.1 Input

#### `INP01` — Gamepad input layer (XInput on Windows, SDL_GameController fallback)
- [ ] **What**: poll connected controllers each frame, surface buttons, sticks, triggers, and connect/disconnect events through the existing `IInputSource` contract. Currently only keyboard + mouse are wired in `src/platform/`.
- **Files**: `src/platform/Interfaces.h` (extend `InputEvent` shape — additive variants), new `src/platform/qt/QtGamepadInput.cpp` or `src/input/GamepadXInput.cpp`, `src/scripting/ScriptRuntimeBindings.cpp` (Lua `ctx.input.button_down`, `ctx.input.axis`).
- **Depends on**: nothing.
- **Done when**: a Lua script can read `ctx.input:button_down("face_a")` and move an entity with `ctx.input:axis("left_x")`. New smoke target `pt_gamepad_input_smoke` constructs a synthetic gamepad source, posts events, and validates downstream consumption.

#### `INP02` — Input rebinding + persisted bindings
- [ ] **What**: data structure `InputBindings { action_name → [physical_input...] }`, default profile loaded at startup, override profile written to disk per user.
- **Files**: `src/input/InputBindings.h/cpp` (new), `src/core/Config.h` (point at bindings file), `assets/input/default_bindings.json` (default).
- **Depends on**: `INP01`, `SAV03` (file location convention).
- **Done when**: rebinding a key in a settings menu (placeholder for now) writes `<savedir>/bindings.json` and survives restart. Smoke validates round-trip.

#### `INP03` — Dead zones + axis curves
- [ ] **What**: configurable dead zone (radial + axial), response curve (linear / quadratic / custom). Per-axis, per-binding.
- **Files**: `src/input/InputBindings.h/cpp`, `src/input/AxisCurve.h/cpp` (new).
- **Depends on**: `INP01`, `INP02`.
- **Done when**: stick drift below dead zone reports zero; smoke test feeds known curve points and validates output.

#### `INP04` — Vibration / rumble API
- [ ] **What**: `IInputSource::set_vibration(controller_id, low_freq, high_freq, duration_ms)`. Lua binding.
- **Files**: `src/platform/Interfaces.h` (additive), gamepad backend impl, bindings.
- **Depends on**: `INP01`.
- **Done when**: Lua call triggers measurable vibration (visual on debug panel even without hardware).

---

### 1.2 Skeletal animation runtime

The biggest critical missing system. Every demo with a character needs at minimum a skeleton, animation clips, and skinning. README mentioned "animation timelines" but nothing in `src/` implements joint-based animation.

#### `ANI01` — Skeleton data type
- [ ] **What**: `Skeleton { vector<Joint> joints; }` where `Joint { string name; int parent_index; mat4 inverse_bind; }`. Skeleton lives on the `Mesh` asset (or alongside it). Hierarchy stored as parallel array (parent index `-1` = root).
- **Files**: new `src/animation/Skeleton.h/cpp`, scene document additions in `src/scene/SceneDocument.h` for `SkeletonComponent`.
- **Depends on**: nothing.
- **Done when**: glTF importer extracts skeletons (skin nodes); a `pt_skeleton_smoke` round-trips a skeleton through scene serialize/deserialize and validates joint count + parent links.

#### `ANI02` — Animation clip data
- [ ] **What**: `AnimationClip { float duration; vector<JointTrack> tracks; }`, `JointTrack { int joint_index; vector<TranslationKey/RotationKey/ScaleKey> ... }`. Each key has `time`, `value`, optional `tangent` for cubic interpolation.
- **Files**: `src/animation/AnimationClip.h/cpp`, asset import extensions in `src/assets/`.
- **Depends on**: `ANI01`.
- **Done when**: glTF animation tracks load into `AnimationClip`; smoke compares deserialized clip duration + key counts against the source file.

#### `ANI03` — Sampler (evaluate clip → joint pose at time t)
- [ ] **What**: given clip + time + interpolation mode (step/linear/cubic), produce `vector<mat4> joint_local_matrices`. Compose into `vector<mat4> joint_world_matrices` by walking parent indices.
- **Files**: `src/animation/AnimationSampler.h/cpp`.
- **Depends on**: `ANI01`, `ANI02`.
- **Done when**: smoke test loads a known clip, samples at fixed times, asserts joint world matrices match expected fixtures.

#### `ANI04` — Skinning matrix upload (CPU side)
- [ ] **What**: per-frame compute `final_skinning_matrix[i] = joint_world_matrices[i] * inverse_bind[i]`. Upload to a per-mesh GPU buffer.
- **Files**: `src/animation/AnimationSkinning.h/cpp`, GPU buffer allocation in `src/gpu/D3D12GpuPathTracer.cpp` and `src/gpu/VulkanGpuPathTracer.cpp`.
- **Depends on**: `ANI03`.
- **Done when**: GPU receives per-mesh `mat4[joint_count]` matching CPU compute; visible bind-pose verification.

#### `ANI05` — Vertex skinning in the path tracer
- [ ] **What**: when a mesh has a skeleton, deform vertices in the BVH leaves before ray hit-shading. Two viable approaches:
  - **(A)** rebuild the BVH each frame from skinned positions (correct, expensive). The existing `dynamic_bvh` channel can carry skinned geometry as an instance with full rebuild.
  - **(B)** keep static BVH, displace at hit time via per-vertex weights + bone matrices in the closest-hit shader. Cheaper but needs shader work.
- **Files**: `src/shaders/gpu/pathtrace_cs.hlsl` (skinning in hit shader for B), `src/pathtracer/SceneConversion.cpp` (mesh tagging), `src/scene/SceneSnapshot.cpp` (skeleton baked into `RenderSceneSnapshot`).
- **Depends on**: `ANI04`.
- **Done when**: a glTF rigged character animates correctly under D3D12+DXR with no frame stalls beyond the BVH cost; smoke renders one frame at a known animation time and validates pixel hash against fixture.

#### `ANI06` — Animation lifecycle component
- [ ] **What**: `AnimationComponent { clip_id, time_seconds, speed, loop, paused }` per entity. Sim loop advances `time_seconds += dt * speed`, wraps if loop.
- **Files**: `src/scene/SceneDocument.h`, `src/scene/SceneWorld.cpp` (component CRUD), Lua bindings `entity:set_animation(clip, opts)` / `entity:animation_time()`.
- **Depends on**: `ANI02`, `ANI05`.
- **Done when**: Lua script can play/pause/scrub a clip on a character; visible in editor.

#### `ANI07` — Animation events (callback at keyframe)
- [ ] **What**: clips carry optional `vector<AnimationEvent { float time; string name; }>`. Sampler crossing an event time emits a Lua hook `on_animation_event(self, ctx, event_name)`.
- **Files**: `src/animation/AnimationClip.h`, `src/scripting/ScriptRuntime.cpp` (new lifecycle hook).
- **Depends on**: `ANI06`.
- **Done when**: a script defines `on_animation_event(self, ctx, name)`, plays a clip with a "footstep" event, and the script logs the event at the right frame.

#### `ANI08` — Two-clip blend (additive playback)
- [ ] **What**: blend two clips with weight `w ∈ [0,1]`. Output joint pose = lerp(joint_a, joint_b, w) per joint. Useful for walk-to-run blends.
- **Files**: `src/animation/AnimationBlend.h/cpp`, extend `AnimationComponent` to hold two clip slots + weight.
- **Depends on**: `ANI03`.
- **Done when**: scene with one rigged character blends idle + walk smoothly via Lua-controlled weight.

#### `ANI09` — Animation graph (state machine + transitions)
- [ ] **What**: declarative state machine with conditions, transition durations, optional cross-fade. Built on `ANI08`. Loaded from JSON.
- **Files**: `src/animation/AnimationGraph.h/cpp`, asset format under `assets/animations/*.graph.json`.
- **Depends on**: `ANI08`.
- **Done when**: a 3-state graph (idle → walk → run with conditions on velocity) plays through correctly when the character moves.

#### `ANI10` — Inverse Kinematics (foot IK, look-at)
- [ ] **What**: two-bone IK solver (foot placement on uneven terrain), look-at constraint (head/eye toward target).
- **Files**: `src/animation/AnimationIK.h/cpp`.
- **Depends on**: `ANI09`.
- **Done when**: character feet stay on ground when terrain dips; head tracks the player camera.

---

### 1.3 Particle / VFX runtime

Scene document already has `SceneParticleEmitterDefinition` but no simulation. Path tracing makes some VFX hard — emissive billboards and short-lived emissive meshes work well, full volume rendering does not.

#### `VFX01` — Particle emitter component + simulation update
- [ ] **What**: `ParticleEmitterComponent { spawn_rate, lifetime, initial_velocity_range, gravity, color_over_life, size_over_life }`. Simulation runs in sim worker each tick: spawn N particles per dt, advance position/velocity, kill by lifetime.
- **Files**: new `src/vfx/ParticleSystem.h/cpp`, integration into sim loop, scene component glue.
- **Depends on**: nothing structural; runs CPU-side.
- **Done when**: an emitter spawns particles visible in editor as transform gizmos; smoke validates spawn count over time.

#### `VFX02` — GPU upload of particle instance buffer
- [ ] **What**: each frame write live particle positions/sizes/colors into a GPU buffer. The path tracer treats them as small emissive instances (one tri or quad per particle, single material with emission color).
- **Files**: `src/vfx/ParticleGpuBuffer.h/cpp`, `src/gpu/D3D12GpuPathTracer.cpp` and `VulkanGpuPathTracer.cpp` (particle instance buffer binding), shader updates in `src/shaders/`.
- **Depends on**: `VFX01`.
- **Done when**: emitter spawns visible particles in the path-traced viewport; particles light nearby surfaces faintly because they're emissive.

#### `VFX03` — Particle pool with frame-stable budget
- [ ] **What**: per-emitter cap (e.g. 256 particles), global cap (e.g. 4096). Excess spawns dropped (with diagnostic counter).
- **Files**: `src/vfx/ParticleSystem.cpp`.
- **Depends on**: `VFX01`.
- **Done when**: at sustained spawn rate, particle count plateaus at cap; a `vkp.vfx.particles_dropped_total` counter increments.

#### `VFX04` — Particle module: noise / curl
- [ ] **What**: optional per-emitter modifier — perlin noise displacement on velocity each tick. Cheap visual polish for fire/smoke.
- **Files**: `src/vfx/ParticleModules.h/cpp`.
- **Depends on**: `VFX01`.
- **Done when**: smoke emitter visibly curls.

#### `VFX05` — Lua API (spawn / stop / burst / set_param)
- [ ] **What**: bindings: `entity:emitter_spawn(template_id, position, params)`, `entity:emitter_stop()`, `entity:emitter_burst(count)`. Templates loaded from JSON.
- **Files**: `src/scripting/ScriptRuntime.cpp` (binding definitions), `src/vfx/ParticleTemplates.h/cpp`.
- **Depends on**: `VFX01`-`VFX04`.
- **Done when**: a Lua script triggers a one-shot burst on impact event.

#### `VFX06` — Decal primitive
- [ ] **What**: bullet hole, scorch mark, blood splat. Decal = oriented box that projects a texture onto whatever surfaces it intersects. Implementation: extra hit-shader pass that, if hit point is inside any decal box, samples the decal texture and replaces base albedo.
- **Files**: `src/vfx/DecalSystem.h/cpp`, `src/scene/SceneDocument.h` (new component), shader updates.
- **Depends on**: nothing structural.
- **Done when**: scripted `entity:spawn_decal(...)` leaves a visible bullet hole that survives camera motion.

---

### 1.4 Path-traced render quality

These are about making the path tracer good enough that game scenes look right at low SPP. Currently 3 SPP cornell looks noisy.

#### `RND01` — Robust denoiser for moving cameras
- [ ] **What**: `--denoiser` flag exists for D3D12. Confirm it works under camera motion (current implementation may streak/ghost). Investigate whether existing temporal filter + spatial filter can be reused; alternative: integrate Intel Open Image Denoise or NVIDIA OptiX denoiser.
- **Files**: `src/gpu/D3D12GpuPathTracer.Compute.cpp` (denoise dispatch), `src/shaders/gpu/denoise_*.hlsl`.
- **Depends on**: nothing.
- **Done when**: gate run on representative_acceptance_scene with continuous camera_pan shows no smear / reset artifact across phase boundaries.

#### `RND02` — Vulkan denoiser parity
- [ ] **What**: Vulkan currently has no denoiser. Match the D3D12 denoiser path with SPIR-V equivalents.
- **Files**: `src/gpu/VulkanGpuPathTracer.cpp`, `src/shaders/gpu/*.glsl` or compiled SPIR-V.
- **Depends on**: `RND01`.
- **Done when**: Vulkan + denoiser produces equivalent quality to D3D12 + denoiser on cornell at 4 SPP.

#### `RND03` — ReSTIR temporal reservoir for direct lighting
- [ ] **What**: instead of resampling NEE light per pixel each frame, maintain a per-pixel reservoir of "good" light samples that gets temporally reused + spatially shared. Massive variance reduction for scenes with many lights.
- **Files**: new `src/shaders/gpu/restir_*.hlsl`, reservoir buffer in `D3D12GpuPathTracer`. Roughly mirror NVIDIA ReSTIR DI papers.
- **Depends on**: nothing structural; replaces light sampling code path.
- **Done when**: a 50-light scene at 1 SPP looks as clean as the current 16 SPP NEE+MIS path; gate measures SSIM vs ground truth.

#### `RND04` — Light cluster / culling for many small lights
- [ ] **What**: split the screen into a 16×16×16 voxel grid of view-aligned clusters, classify each cluster against scene lights (which lights overlap this cluster). Per-pixel light sampling only iterates cluster's light list. Required for >10 small lights to not collapse perf.
- **Files**: `src/render/LightClusterBuilder.h/cpp`, shader updates for cluster lookup at hit time.
- **Depends on**: nothing.
- **Done when**: scene with 100 small lights stays at >50% the FPS of the same scene with 2 lights (vs current ~5%).

#### `RND05` — Single-scattering volumetrics (fog / god rays)
- [ ] **What**: add a volume primitive (axis-aligned box with density field), march rays through it, accumulate single-scattering toward lights. Restrict to "homogeneous + analytic phase function" first.
- **Files**: `src/scene/SceneDocument.h` (volume component), shader updates for volume integration.
- **Depends on**: `RND04`.
- **Done when**: a scene with a single fog box + sun lamp shows visible god rays through windows; perf remains within 20% of fog-disabled.

#### `RND06` — Material variant system
- [ ] **What**: per-entity `material_variant_id` selects among material LUT entries — useful for damage states, hover-glow, palette swaps. Per-frame swap with no scene rebuild (currently each material change rebuilds the BVH because `assign_material` triggers it).
- **Files**: `src/scene/SceneSnapshot.cpp` (instance variant index), shader sample-material lookup.
- **Depends on**: `RND04`.
- **Done when**: 100 entities each with 4 variants can swap variant per frame at no perf cost beyond an instance-buffer update.

---

## §2 World — physics, collision, AI scaffolding

Builds on §1 foundations. These are "things you can hit, walk on, or get hit by."

### 2.1 Trigger volumes & collision events

#### `WRL01` — Trigger volume component
- [ ] **What**: `TriggerVolumeComponent { shape (box/sphere/capsule), event_tag }` per entity. Marked non-solid (no physics push), but participates in overlap queries.
- **Files**: `src/scene/SceneDocument.h`, `src/physics/PhysicsWorld.cpp` (Jolt sensor/trigger registration).
- **Depends on**: nothing.
- **Done when**: a scene tagged with two trigger volumes registers correctly in Jolt as sensors.

#### `WRL02` — Overlap event dispatch
- [ ] **What**: per-frame Jolt overlap queries between every active body and every trigger volume. On state transition (was-not-overlapping → now-overlapping or vice versa), fire `on_trigger_enter(self, ctx, other_entity)` / `on_trigger_exit(...)` Lua hooks.
- **Files**: `src/physics/PhysicsWorld.cpp` (overlap pairs), `src/scripting/ScriptRuntime.cpp` (new lifecycle hooks).
- **Depends on**: `WRL01`.
- **Done when**: a character walking through a marker entity logs enter / exit events exactly once per crossing.

#### `WRL03` — Collision event hooks (`on_collision`, `on_hit`)
- [ ] **What**: Jolt's contact listener delivers persistent contact info each step. Surface as `on_collision_enter / on_collision_stay / on_collision_exit` Lua hooks for entities that opt in.
- **Files**: `src/physics/PhysicsWorld.cpp` (contact listener wiring), `src/scripting/ScriptRuntime.cpp`.
- **Depends on**: nothing structural.
- **Done when**: a Lua script attached to a bullet entity reacts to first contact with a wall (decal + sfx).

---

### 2.2 Physics character controller

The Lua kinematic CC at `assets/scripts/systems/movement/character_controller.lua` is a placeholder. Replace with a real physics-driven one.

#### `WRL04` — Jolt CharacterVirtual integration
- [ ] **What**: wrap Jolt's `CharacterVirtual` (capsule sweep, slope handling, step-up, sliding) as a `CharacterControllerComponent`. Per-tick: take input direction → call `CharacterVirtual::Update(input_velocity, dt)` → write back transform.
- **Files**: new `src/physics/CharacterController.h/cpp`, scene component, Lua bindings.
- **Depends on**: nothing.
- **Done when**: a character walks up gentle slopes, slides off steep ones, climbs stairs ≤ step-height, doesn't fall through floors.

#### `WRL05` — Ground/jump/crouch state machine on the controller
- [ ] **What**: maintain `is_grounded`, `was_grounded_recently` (coyote time), `jump_buffer` (jump pre-buffer when player presses just before landing). Capsule shrinks on crouch.
- **Files**: `src/physics/CharacterController.cpp`, Lua bindings.
- **Depends on**: `WRL04`.
- **Done when**: jump feels good (coyote time + buffer), crouch capsule resizes correctly.

#### `WRL06` — CC vs trigger volume interaction
- [ ] **What**: trigger volumes work the same way for character controller as for dynamic bodies — overlap events fire.
- **Files**: minor wiring in `WRL04` to also feed into the trigger system.
- **Depends on**: `WRL02`, `WRL04`.
- **Done when**: walking the CC through a trigger fires enter/exit events.

---

### 2.3 AI / NPC behavior

#### `WRL07` — NavMesh import (Recast / Detour)
- [ ] **What**: integrate Recast as a vendor library; bake NavMesh from collision geometry; serialize as a binary blob alongside the scene.
- **Files**: `vendor/recast/` (new submodule), `src/ai/NavMeshBuilder.h/cpp`, `tools/bake_navmesh.cpp`.
- **Depends on**: nothing.
- **Done when**: a scene-bake step produces a NavMesh that visually covers walkable surfaces in the editor (debug view).

#### `WRL08` — A* pathfinding query
- [ ] **What**: `find_path(navmesh, start, end) → vector<vec3> waypoints`. Smoothed with funnel algorithm. Per-frame cap on queries.
- **Files**: `src/ai/Pathfinding.h/cpp`.
- **Depends on**: `WRL07`.
- **Done when**: NPC can traverse the NavMesh from one corner of cornell to another.

#### `WRL09` — Behavior tree primitives
- [ ] **What**: small set of nodes: `Sequence`, `Selector`, `Parallel`, `Decorator`, leaf `Action` and `Condition`. Author trees in JSON or Lua.
- **Files**: `src/ai/BehaviorTree.h/cpp`.
- **Depends on**: nothing.
- **Done when**: a 5-node tree (idle → see player → chase → attack → idle) drives an NPC.

#### `WRL10` — Sensor model (line-of-sight via existing ray traversal)
- [ ] **What**: AI "can I see entity X" query. Implementation: cast one ray from sensor origin to target, use existing path-tracer BVH. Cheap, matches the renderer's model of the world.
- **Files**: `src/ai/Sensors.h/cpp`.
- **Depends on**: nothing structural; reuses BVH.
- **Done when**: NPC reacts only when the player is in line-of-sight and within range.

---

## §3 Presentation — what the player actually sees and hears

Built on §1 + §2.

### 3.1 HUD / in-game UI

The Qt editor docks are not visible in shipped builds. Game UI is rendered as a separate overlay layer on top of the path-traced framebuffer.

#### `HUD01` — In-canvas overlay surface
- [ ] **What**: separate compositor pass that, after path tracing produces the LDR frame, draws an overlay (rgba8) on top before final present. Two backends: D3D12 (raster pipeline state with blend) and CPU (memcpy alpha-blend for the headless path).
- **Files**: new `src/hud/HudCompositor.h/cpp`, `src/gpu/D3D12GpuPathTracer.cpp` (additional pass), shader pair in `src/shaders/hud/`.
- **Depends on**: nothing structural.
- **Done when**: a solid red `100×100` rectangle drawn at `(50, 50)` shows on top of the rendered scene, semi-transparent.

#### `HUD02` — Bitmap font rendering
- [ ] **What**: simple bitmap font (atlas + glyph table) for HUD text. Pre-baked atlas; no dynamic font loading. `draw_text(x, y, text, color)`.
- **Files**: `src/hud/Font.h/cpp`, asset under `assets/fonts/default.png` + `assets/fonts/default.json`.
- **Depends on**: `HUD01`.
- **Done when**: scoreboard "Score: 1234" renders cleanly in the canvas overlay.

#### `HUD03` — Sprite / texture rendering
- [ ] **What**: `draw_sprite(x, y, w, h, texture, uv0, uv1, color)`. Supports 9-slice for stretchy frames.
- **Files**: `src/hud/SpriteBatch.h/cpp`.
- **Depends on**: `HUD01`.
- **Done when**: health-bar with a 3-slice frame (left cap, middle stretch, right cap) animates.

#### `HUD04` — Layout primitives (anchors, padding)
- [ ] **What**: `HudLayout { anchor (top-left, top-right, ...), offset, size }`. Resolves at draw time given current canvas size.
- **Files**: `src/hud/HudLayout.h/cpp`.
- **Depends on**: `HUD01`-`HUD03`.
- **Done when**: HUD elements stay anchored when the window is resized.

#### `HUD05` — Lua HUD API
- [ ] **What**: `ctx.hud:text(...)`, `ctx.hud:sprite(...)`, `ctx.hud:rect(...)`. Drawn during a new lifecycle hook `on_hud(self, ctx)` after `on_update`.
- **Files**: `src/scripting/ScriptRuntime.cpp`, new lifecycle hook.
- **Depends on**: `HUD01`-`HUD04`.
- **Done when**: a Lua script draws "FPS: 60" in the upper-left without C++ changes.

#### `HUD06` — Input capture priority for HUD
- [ ] **What**: when HUD is active (a menu is up), input goes to HUD first, gameplay second. Avoid the player firing weapons while clicking menu buttons.
- **Files**: `src/hud/HudInputRouter.h/cpp`, integration with `src/app/AppRuntimeQtRenderLoop.inc` input dispatch.
- **Depends on**: `HUD01`, `INP01`.
- **Done when**: clicking on a menu button does not also trigger gameplay click.

---

### 3.2 Cinematic / camera director

Builds on `systems/camera/*.lua` foundations.

#### `CAM01` — Cinematic camera component
- [ ] **What**: `CinematicCameraComponent { tracks: vector<CameraKey { time, position, target, fov, lens } >, blend_mode }`. Sampler interpolates between keys.
- **Files**: `src/scene/SceneDocument.h`, new `src/cinematic/CinematicCamera.h/cpp`.
- **Depends on**: `ANI03` (sampler reuse).
- **Done when**: a 3-key 5-second cutscene plays smoothly when triggered.

#### `CAM02` — Camera blend / handoff between cameras
- [ ] **What**: switch the active rendering camera over a `blend_seconds` period. Both cameras evaluated, output mixed at viewport level.
- **Files**: `src/cinematic/CameraBlend.h/cpp`, integration in `src/render/RenderCoordinator.cpp` (which camera the next frame uses).
- **Depends on**: `CAM01`.
- **Done when**: triggering a cinematic blends from gameplay camera to cutscene camera over 1 second.

#### `CAM03` — Cutscene timeline (camera + animation events + dialogue triggers)
- [ ] **What**: timeline asset that composes camera tracks, animation triggers on actors, dialogue events, all with a single play head.
- **Files**: `src/cinematic/Cutscene.h/cpp`, asset format.
- **Depends on**: `CAM01`, `ANI06`, `AUD05` (dialogue events).
- **Done when**: 30-second cutscene plays end-to-end with camera + character animation + voice line.

#### `CAM04` — Letterbox bars during cinematic
- [ ] **What**: HUD overlay draws top + bottom black bars while cinematic is active. Animated in/out.
- **Files**: `src/hud/Letterbox.h/cpp`.
- **Depends on**: `HUD01`, `CAM01`.
- **Done when**: starting a cinematic eases letterbox in over 0.5 s; ending eases out.

---

### 3.3 LOD streaming

The relay yard pack already has on-disk LODs. Add runtime selection.

#### `LOD01` — LOD switching by camera distance
- [ ] **What**: each mesh has a `vector<MeshLod { distance_threshold, mesh_id }>`. Per frame, classify each instance by camera distance, swap mesh_id when threshold crossed (with hysteresis to avoid popping).
- **Files**: `src/scene/SceneSnapshot.cpp` (LOD selection during snapshot build), `src/scene/SceneDocument.h` (LOD data).
- **Depends on**: nothing.
- **Done when**: instance at far distance uses the LOD3 mesh; close-up uses LOD0; visible "popping" stays subtle.

#### `LOD02` — Async geometry upload
- [ ] **What**: when a new LOD is requested, upload it to the GPU on a worker thread. Until upload completes, keep using the previous LOD. Don't block the render thread.
- **Files**: `src/gpu/D3D12GpuPathTracer.cpp` (async copy queue), `src/render/AssetUploader.h/cpp` (new).
- **Depends on**: `LOD01`.
- **Done when**: switching LODs during a camera flight produces no per-frame stall > 5 ms.

#### `LOD03` — Per-instance LOD bias (cinematic priority)
- [ ] **What**: a "hero" entity flag forces always-LOD0 regardless of distance. For cutscene actors.
- **Files**: scene component, `src/scene/SceneSnapshot.cpp`.
- **Depends on**: `LOD01`.
- **Done when**: a hero entity uses LOD0 even when far from camera during a cutscene.

---

## §4 Audio — beyond one-shots

Currently we have `systems/audio/spatial_emitter.lua` and `audio_post_event` plumbing. Game audio needs music + ducking + mix snapshots.

#### `AUD01` — Streamed music track
- [ ] **What**: `audio:play_music(path, fade_in_seconds)` plays a streamed (not fully buffered) audio file. Single music slot.
- **Files**: `src/audio/AudioSystem.cpp` (music streaming via miniaudio), Lua bindings.
- **Depends on**: nothing.
- **Done when**: 5-minute OGG plays without loading the whole file into memory; volume fades in over the requested duration.

#### `AUD02` — Music crossfade between tracks
- [ ] **What**: `audio:crossfade_music(new_path, fade_seconds)` fades out old track + fades in new track simultaneously.
- **Files**: extend `AUD01` impl.
- **Depends on**: `AUD01`.
- **Done when**: switching tracks is smooth, no pop or overlap artifact.

#### `AUD03` — Audio bus mix snapshots
- [ ] **What**: per-bus volume (master, music, sfx, voice). `audio:apply_mix_snapshot(name, blend_seconds)` morphs to a named snapshot. Settings menu hooks here.
- **Files**: `src/audio/AudioMix.h/cpp`, scene-level mix config.
- **Depends on**: nothing structural; existing bus controls already in `AudioSystem`.
- **Done when**: pause-menu snapshot reduces music volume, applies low-pass to sfx, dialog stays full.

#### `AUD04` — Reverb zones (trigger volumes that change reverb)
- [ ] **What**: tag a trigger volume with a reverb preset. While listener is inside, the master reverb send is set to that preset.
- **Files**: `src/audio/AudioReverb.h/cpp`.
- **Depends on**: `WRL01`, `WRL02`.
- **Done when**: walking from outside into a "cave" trigger swaps to a long-tail reverb.

#### `AUD05` — Voice line / dialogue dispatch
- [ ] **What**: `audio:play_dialogue(line_id)` — single non-overlapping voice channel; auto-ducks music. Triggered by cutscenes or gameplay events.
- **Files**: extend `AudioSystem`, dialogue table under `assets/dialogue/lines.json`.
- **Depends on**: `AUD03`.
- **Done when**: triggering dialogue ducks music, plays the line, restores music when complete.

---

## §5 Persistence — saving and loading state

#### `SAV01` — Save game serialization
- [ ] **What**: serialize world state to a compact binary or JSON blob: entity transforms, active scene name, player state (script-defined fields), inventory. Versioned.
- **Files**: new `src/save/SaveGame.h/cpp`, `src/scene/SceneWorld.cpp` (export hooks).
- **Depends on**: nothing structural.
- **Done when**: `save("slot1.sav")` writes a file, `load("slot1.sav")` restores entities and player state to identical visual output.

#### `SAV02` — Save slot management
- [ ] **What**: enumerate save slots, capture screenshot thumbnail, capture timestamp + level name. Slots stored under `<savedir>/slots/`.
- **Files**: extend `SaveGame.h/cpp`.
- **Depends on**: `SAV01`.
- **Done when**: load menu shows a list of saves with thumbnails and times.

#### `SAV03` — Settings persistence
- [ ] **What**: per-user settings (graphics, audio, controls) written to `<savedir>/settings.json`. Loaded at startup, applied before window opens.
- **Files**: `src/save/Settings.h/cpp`, `src/core/Config.h` extension.
- **Depends on**: nothing.
- **Done when**: changing a setting and restarting persists the change.

#### `SAV04` — Autosave hooks (`on_save_request` Lua)
- [ ] **What**: trigger an autosave from gameplay (level transition, checkpoint). Lua: `engine.save_now("auto")`.
- **Files**: extend `SaveGame.h/cpp`, `ScriptRuntime` bindings.
- **Depends on**: `SAV01`.
- **Done when**: crossing a checkpoint trigger writes an autosave file, with a 300 ms cap on stall to the main thread.

#### `SAV05` — Save data migration / versioning
- [ ] **What**: each save carries a schema version. Loading an older save runs registered migrators.
- **Files**: `src/save/SaveMigration.h/cpp`.
- **Depends on**: `SAV01`.
- **Done when**: an old fixture save loads and pixel-equivalent state re-renders.

---

## §6 Game state machine — the orchestrator

Tying everything together. Highest-level layer.

#### `GSM01` — Mode definitions
- [ ] **What**: enum `GameMode { Boot, MainMenu, Loading, InGame, Paused, GameOver, Credits }`. Today the runtime has `RuntimeMode { Edit, Play, LiveEdit }` — that's the *editor* mode. Game mode is the *gameplay* state.
- **Files**: new `src/game/GameStateMachine.h/cpp`.
- **Depends on**: nothing.
- **Done when**: entering game mode `MainMenu` enables the right HUD and disables the right gameplay scripts.

#### `GSM02` — Mode transitions with optional load screen
- [ ] **What**: `transition_to(target_mode, options)` runs a fade-out, optionally shows a "Loading..." screen, then fades back in. Asset loads can hook in.
- **Files**: extend `GSM01`.
- **Depends on**: `GSM01`, `HUD01`, `LOD02` (async upload during load).
- **Done when**: transitioning from MainMenu to InGame fades out, swaps scenes, fades in cleanly.

#### `GSM03` — Pause menu wiring
- [ ] **What**: `Paused` mode freezes physics + script `on_update` hooks; HUD draws pause menu; resuming re-thaws.
- **Files**: `src/game/GameStateMachine.cpp`, sim worker check on game mode before stepping.
- **Depends on**: `GSM01`, `HUD01`-`HUD05`.
- **Done when**: Esc opens pause menu, gameplay stops, Esc again resumes.

#### `GSM04` — Title screen + main menu
- [ ] **What**: a startup scene that shows the game title, plays main music, lets the user select "New Game / Continue / Settings / Quit". Built from `HUD01`-`HUD05`.
- **Files**: `assets/scenes/title_screen.json`, `assets/scripts/user/title_screen.lua`.
- **Depends on**: `GSM01`-`GSM02`, `HUD05`, `AUD01`.
- **Done when**: launching `ptapp` lands at title screen and "New Game" transitions into a demo level.

#### `GSM05` — Game-over flow
- [ ] **What**: when the player loses, transition to GameOver mode, show overlay, options to restart or return to menu.
- **Files**: extend `GSM01`.
- **Depends on**: `GSM01`, `HUD05`.
- **Done when**: scripted player death triggers the flow correctly.

#### `GSM06` — Credits scene
- [ ] **What**: scrolling credits, music swap, return to title at end.
- **Files**: a scene + scripts; minimal new C++ if HUD scrolling primitive exists.
- **Depends on**: `GSM01`, `HUD05`.
- **Done when**: credits scroll cleanly and return to title.

---

## §7 Settings menu — final polish before demo-ready

#### `STG01` — Settings data model
- [ ] **What**: typed struct: graphics (SPP, max_depth, denoise on/off, resolution scale, vsync), audio (4 bus volumes, music on/off), controls (rebinding). Wired to runtime config.
- **Files**: extend `src/save/Settings.h/cpp`.
- **Depends on**: `SAV03`.
- **Done when**: model round-trips through serialize/deserialize.

#### `STG02` — Settings menu HUD
- [ ] **What**: tabbed menu (Graphics / Audio / Controls / Back). Built from `HUD01`-`HUD05`.
- **Files**: `assets/scripts/user/settings_menu.lua` + supporting C++ if needed.
- **Depends on**: `STG01`, `HUD01`-`HUD05`.
- **Done when**: changing a setting in the menu applies live and persists.

#### `STG03` — Resolution scale slider (live)
- [ ] **What**: rendering at `0.5×`, `0.75×`, `1.0×`, `1.5×` of window resolution. Path tracer accepts this without restart.
- **Files**: `src/render/RenderCoordinator.cpp` (resolution change), `src/gpu/D3D12GpuPathTracer.cpp` (recreate textures).
- **Depends on**: `STG02`.
- **Done when**: dragging slider re-targets render resolution without crash; takes effect within 2 frames.

#### `STG04` — SPP / max-depth sliders (live)
- [ ] **What**: per-frame quality knobs adjustable from settings menu without scene reload.
- **Files**: render settings hot-reload path (already partially there).
- **Depends on**: `STG02`.
- **Done when**: sliders affect noise level live.

#### `STG05` — Control rebinding UI
- [ ] **What**: list of game actions, click-to-rebind UX, save to bindings file.
- **Files**: `assets/scripts/user/settings_rebind.lua`, integration with `INP02`.
- **Depends on**: `INP02`, `STG02`.
- **Done when**: rebinding "jump" to space-bar persists and works in next session.

---

## §8 Optional polish (deferred unless a demo demands them)

These are tagged but not currently scheduled.

- **OPT01** — Localization / string tables (defer until non-English demo)
- **OPT02** — Crash recovery / autosave restore on relaunch (depends on `SAV04`)
- **OPT03** — Achievement / progress tracking (defer)
- **OPT04** — Replay recording (input-deterministic, possible because we have determinism mode)
- **OPT05** — In-engine screenshot export with EXIF metadata
- **OPT06** — Photo mode (free camera + DOF + filter LUTs during gameplay pause)

---

## Out of scope (by design)
- Networking / multiplayer
- Steam / store integration
- Modding API beyond Lua
- VR
- Console platforms

---

## Cross-cutting acceptance for the first demo

A demo is "shippable" when:
- [ ] Can launch from title screen, navigate menu, start a level, play, pause, save, load, return to title, quit.
- [ ] No black bars, no torn frames, no per-frame stall > 50 ms in normal play.
- [ ] Crash artifacts capture enough to debug a reported issue without the developer machine.
- [ ] Settings persist across launches.
- [ ] Save/load round-trips visually identical state.
- [ ] At least one rigged character animates correctly.
- [ ] At least one HUD overlay (health/score/objective) is visible during gameplay.
- [ ] Music fades correctly between menu and gameplay; SFX duck for dialogue.
- [ ] Gamepad fully playable end-to-end.
