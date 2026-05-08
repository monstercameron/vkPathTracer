# Changelog

## 2026-05-08 (session 40)

### Scene script bootstrap and Lua structure

- Added runtime-only scene script bootstrap worlds so top-level `scene_script` bindings and default FPS fallback scripts execute without mutating source scene JSON.
- Exposed Lua scene bootstrap helpers for main-camera lookup, component queries, idempotent script attachment, built-in system lookup, interactable registration diagnostics, structured diagnostics, and safe engine-owned includes.
- Wired Qt Play/Live Edit script reload and dispatch through the same bootstrap-resolved runtime world, dispatching `on_load` when script-running modes start.
- Added script dock bootstrap diagnostics and scene-init management commands for create/open, default FPS bake, and fallback disable flows.
- Expanded Lua editor annotations with `-- [editor]` alias support, optional metadata-only params, generated Qt controls for missing annotated params, and converted tunable sample scripts to `ctx.params`.
- Tightened Lua editor annotation parsing with real marker boundaries, source-level annotation caching, parser diagnostics, and slider fallback handling for annotation-only numeric params.
- Added `assets/scenes/live_edit_model_lab.json` and `assets/scripts/live_edit_model_lab.lua`, a gameplay-character Live Edit test scene with annotated controls for model scale, color cycling, lights, camera focus, orbiters, UI panels, and animation.
- Prevented script-authored scene changes from reloading script bindings unless scripts actually change, and exposed `entity:get_material_id()` so Live Edit scripts can avoid respawning/reassigning unchanged UI/material state every tick.
- Expanded bootstrap and scripting smoke coverage for runtime overlays, generic FPS fallback, safe include, `ctx.scene` APIs, annotation parser edge cases, and the Live Edit model lab scene.

## 2026-05-07 (session 39)

### Audio, script UI, and runtime cleanup

- Added optional audio build plumbing, audio ECS listener/emitter components, no-op/miniaudio-backed playback, Lua audio event posting, and file-backed footsteps/forest ambience for the Lua audio demo scene.
- Added scriptable ECS UI panels with Lua spawn/update support and Qt canvas rendering for gameplay controls in the third-person and audio demo scenes.
- Added Lua `-- @editor` parameter annotations so scripts can declare panel defaults, types, labels, and numeric ranges while scene `script.params` remain the persisted overrides.
- Hardened Qt editor/playable boundaries so canvas drops and gizmo edits stay in C++ editor mode, playable Lua owns mouse-look input, selection overlays follow current transforms, and the provisional raster polygon preview path was removed.
- Expanded transform-update diagnostics and frame-update handling across Qt, RenderCoordinator, D3D12/DXR/compute paths, and GPU buffer uploads.
- Fixed D3D12 denoiser and temporal AA setting toggles so they update render constants, reset accumulation, and invalidate temporal history without forcing a full scene/acceleration rebuild when resources are already compatible.
- Added meshoptimizer-backed game LOD tooling and generation scripts; generated `game/models/lods/` outputs are ignored as build artifacts instead of tracked source.
- Refined the lowest-LOD showcase generator and scene output with display turntables, tile/warehouse staging, HDRI window lighting, a Lua-controlled sun script, and a generic Lua FPS camera controller attached to the warehouse walkthrough camera.
- Updated setup, Qt, audio-authoring, TODO, and motion/FPS notes for the current audio/UI/render work.

## 2026-05-07 (session 38)

### Static runtime cleanup and transform-update hardening

- Added an additive standard path tracer contract descriptor covering lifecycle expectations, transform-update defaults, stable status names, and GPU buffer packing strides without changing the existing `IPathTracer` virtual interface.
- Routed D3D12/Vulkan GPU packing stride constants and the render coordinator's legacy transform options through the shared path tracer contract helpers.
- Adopted the standard transform-update option helper across Qt viewport, property-edit, physics, and script transform publishing paths, with structured logs for UI posting, coordinator planning, direct applies, policy rejections, and full fallback paths.
- Kept transform-update debug logging cheap by exposing a logger severity gate and skipping structured field construction when debug logs are disabled.
- Removed the provisional animation component/import path from asset classification, glTF import, scene schema load/export, ECS storage, scene conversion, UI menus, timeline rows, and scripting smoke expectations.
- Reframed render-frame and timeline behavior around static frame metadata and particle frame state instead of automatic scene animation playback.
- Disabled live script/FPS playback paths in static mode while keeping manual scene editing, backend switching, and crash/status reporting responsive.
- Improved Qt selection bounds for dynamic local-geometry instances and batched gizmo transform publishing so moved dynamic objects keep viewport overlays and renderer updates in sync.
- Expanded transform-update fallback handling for physics/editor updates and added tiled CPU transform-update planning/results with full-BVH rebuild reporting.
- Aligned D3D12 compute/DXR shader constants so temporal AA fields stay intact while BVH node counts are passed explicitly for shader traversal.

## 2026-05-07 (session 37)

### DXR shadow stability with temporal post-processing

- Fixed DXR hardware shadow rays by raising raytracing pipeline recursion depth only when closest-hit shader shadow `TraceRay` calls are compiled in.
- Kept DXR dynamic transform updates and compute-side guide traversal in sync by uploading instance and dynamic BVH buffers before TLAS refits.
- Invalidated temporal history and cached LDR output when GPU accumulation resets so moved objects do not reuse stale temporal/denoiser state.
- Verified D3D12 `ptbench`/`ptapp` builds and a DXR headless render with hardware shadow rays, temporal AA, and GPU denoising enabled.

## 2026-05-06 (session 36)

### Motion transaction hardening and DXR shadow traversal

- Rewrote the threading notes into a concise current-state contract for UI, render-worker, frame-handoff, physics, and motion-update ownership.
- Hardened Qt render-dialog validation and status handling for still-image output paths, dimensions, ray budgets, and PNG/EXR format selection.
- Tightened physics/render motion publishing so unsupported dynamic transform updates retry cleanly after full reloads instead of leaving stale fast-path state.
- Made scalar CPU and Vulkan instance-transform updates stage candidate scene changes and restore previous committed scenes if rebuild/upload work fails.
- Refined scene conversion dynamic-instance classification so edited dirty transforms do not permanently force dynamic geometry classification.
- Expanded DXR descriptor binding and shader shadow traversal to use static, dynamic, and local BVH buffers instead of broad per-instance triangle loops.
- Added Vulkan soft light sampling and kept D3D12 DXR build defaults biased toward faster rebuild/update iteration.
- Added FPS collision solve and Qt physics slice timing diagnostics, reduced physics collision substeps, and increased FPS movement speed scaling.

## 2026-05-06 (session 35)

### Transactional transforms, Vulkan/DXR parity, and render workflow

- Added transactional instance-transform update planning/results with explicit update reasons and fallback policies so physics, animation, script, and editor motion can avoid silent full-scene fallback.
- Updated RenderCoordinator, scalar CPU, Null, D3D12, and Vulkan paths to use staged transform-update commits before mutating render mirrors.
- Hardened D3D12 dynamic transform updates with staged GPU instance buffers, dynamic BVH/TLAS updates, partial upload support, and DXR descriptor/root-signature changes for SDF buffers.
- Expanded DXR shader behavior with an AnyHit path, default shader-side shadow occlusion, and SDF-aware descriptor binding.
- Expanded Vulkan path tracer support for packed instance transform data, SDF/environment buffers, camera and scene deltas, and transform-update fallback planning.
- Added Qt Start Render dialog/settings plumbing for still PNG/EXR output from the editor and status updates during synchronous preview rendering.
- Added setup documentation, Vulkan SDK cache configuration, launcher improvements, and TODO/threading notes for transform-update atomicity work.

## 2026-05-06 (session 34)

### Runtime input, procedural wood, render controls, and dynamic updates

- Added a layered Qt input router with explicit editor/FPS/script input modes, reusable Escape cancel handling, selection clearing, and mode-aware mouse/wheel behavior.
- Added runtime-generated wood material variety for Louvre floor, bench, and portrait-frame surfaces, including distinct parquet, walnut, and sandalwood-style procedural variants.
- Added Render Settings backend selection controls and validation so supported runtime backends can be selected from the Qt dock.
- Added scene-tree filtering, particle rows, transform-delta publishing for edited entities, and richer Qt dock/property state plumbing.
- Added particle still/sequence render controls, render-time/frame CLI options, and scripting smoke coverage for particle benchmark playback.
- Added D3D12 dynamic instance BVH refit support so transform-only updates can avoid rebuilding the dynamic BVH when the instance set is unchanged.
- Added threading/performance notes and refreshed active TODO planning docs for the current physics/render-update bottleneck work.
- Verified `cmake --build build\default --target ptapp` and `build\default\bin\ptapp.exe --ui-model-smoke`.

## 2026-05-06 (session 33)

### Glass visibility, particles, gallery assets, and editor polish

- Fixed glass-lab regressions by removing stray slab-target quads, keeping the scene at the intended 9 mesh objects and 14 SDF glass samples.
- Improved transmissive visibility across CPU and D3D12 preview paths so glass no longer behaves as an opaque blocker for direct-light visibility, and D3D12 SDF slabs render through shape-aware SDF dispatch.
- Rebalanced glass lab clearcoat values and added forward transmission preview in scalar CPU, D3D12 compute, and legacy GLSL compute paths so slabs and spheres visibly show background geometry through glass instead of reading as dark opaque blockers.
- Added scene particle emitter schema/load/export/script support plus rain and smoke conversion into renderable particle geometry for benchmark/demo scenes.
- Added Louvre/Mona Lisa gallery scene assets, generator tools, and richer wood material family variants for parquet, walnut, sandalwood, pine, teak, and mahogany.
- Preserved Qt dock tree scroll/top-row state across panel rebuilds, added scene edit reload timing diagnostics, and expanded light visibility delta smoke coverage.
- Verified no-Qt and Windows clang-cl D3D12 Qt `ptapp` builds, glass-lab CPU/D3D12 renders, and UI model smoke checks.

## 2026-05-06 (session 32)

### Runtime scene controls, HDRI lighting, and render cleanup

- Added Qt scene graph visibility toggles, entity visibility load/export support, snapshot filtering, and safer transform-only runtime scene updates that preserve scene state.
- Refined third-person camera input with damped mouse look, camera-relative movement, continuous orbit WASD movement, and scripting smoke coverage for controlled aiming.
- Added Radiance RGBE HDRI loading, scene-relative standalone image resolution, RT environment map storage/sampling, and CPU/D3D12 compute/DXR miss shader sampling.
- Added the Lisa HDRI sun/floor scene graph using the Poly Haven Kiara dawn HDRI, with source notes beside the asset.
- Improved CPU/render hot paths and material behavior with faster film resolve/image output, unrolled BVH bounds reductions, lower-cost power helpers, transmissive shadow/refraction handling, SDF box intersections, procedural wood support, and an expanded glass lab scene.
- Verified scene validation, CPU scalar/tiled renders, D3D12 compute HDRI output, Windows clang-cl D3D12 `ptbench`, and the Qt D3D12 `ptapp` target.

## 2026-05-06 (session 31)

### Third-person scene, render deltas, and C++ cache cleanup

- Replaced the block-built third-person hero with the imported low-poly hero asset, kept Lua-driven movement/camera behavior, and added parentable scene model import support plus a third-person script performance gate.
- Added render-scene delta plumbing for camera, dynamic transforms, materials, and lights so interactive edits can avoid full path-tracer scene rebuilds when the geometry layout is unchanged.
- Hardened C++ error handling and lifetime edges across image IO, asset import, JSON/scene loading, physics/Jolt sync, D3D12/DXR dispatch, job waits, and CPU/GPU BVH setup.
- Reduced cache and allocation overhead in scene conversion, CPU BVH construction, frame graph validation, film resolve, PNG writing, D3D12/Vulkan GPU buffer packing, and scripting/physics binding collection.
- Expanded benchmark, doctor, UI validation, and scripting smoke coverage for the imported third-person scene, backend switching, and transform-only update paths.
- Verified default, desktop Clang, and Windows clang-cl D3D12+Qt builds, `ptbench`, `pt_scripting_smoke`, scripting smoke execution, whitespace checks, and CTest discovery.

## 2026-05-06 (session 30)

### Cross-platform shell stubs and Lua playable-mode updates

- Added host platform detection plus runtime platform support metadata so version output, JSON metadata, doctor checks, and runtime logs report built/available/stub state for headless, raw, and Qt shells.
- Added Linux X11/Wayland and macOS Cocoa raw desktop stub reporting, with non-Windows raw initialization returning `Unsupported` cleanly until native implementations land.
- Made non-GUI commands fall back to the headless shell for unavailable GUI platform requests while keeping `--window` failures explicit.
- Updated Qt Lua playable mode to send script transform and camera changes incrementally where possible, route playable mouse-look input into scripts, and avoid per-frame full scene/light reloads for cheap transform-only updates.
- Added the third-person Lua demo scene/scripts and expanded scripting smoke coverage for forward movement, strafing, mouse-look steering, and chase-camera locking.
- Verified desktop Clang `ptapp`, `ptbench`, and `pt_scripting_smoke` targets, platform metadata commands, raw one-frame window smoke, doctor, scene validation, source-size guardrails, and whitespace checks.

## 2026-05-06 (session 29)

### C++ decomposition guardrail completion

- Replaced artificial numbered source chunks with named decomposition modules and runtime fragments for the app runtime, Qt platform, dock panels, benchmark runtime, asset importers, scalar CPU tracer, scripting bindings, D3D12 backend, and DXR pipeline setup.
- Reduced every tracked `.cpp` source below 1000 LOC and verified that no `*.part*.cpp` files remain.
- Consolidated scalar CPU render jobs back into one shared job-system singleton after the render split, and fixed split-related benchmark, Qt dock, scripting, and D3D12 compile issues.
- Verified desktop Clang, desktop Qt, and Windows clang-cl D3D12+Qt builds plus scripting, UI model, source-size, TODO audit, and scene-validation smoke checks.

## 2026-05-06 (session 28)

### Performance fixes and decomposition checkpoint

- Reduced scalar CPU path tracer overhead with worker-local sample counters, contiguous batch rendering helpers, shared tiled CPU BVH setup, and lazy Vulkan CPU film rebuilds.
- Split path tracer film/image/scene conversion helpers, scene JSON/document/world modules, D3D12 path tracer responsibilities, doctor checks, Qt dock builders, viewport interaction, and UI validation out of the largest files.
- Added decomposition guardrails and documentation: a validation checklist, source-size reporting, smoke-script integration, and detailed DECOMP TODO evidence.
- Verified the desktop and Qt debug builds plus UI model smoke/release-gate commands for the current checkpoint.

## 2026-05-06 (session 27)

### FPS-safe animated scene floor and scripting runtime dock

- Enlarged the Animated Cube test scene floor and backdrop so FPS traversal has a much larger walkable surface.
- Added a Qt Scripting dock beside the inspector group with runtime enable/play/pause/step/reload controls, lifecycle hook buttons, binding counts, dispatch stats, and recent script diagnostics.
- Wired the Qt runtime panel to the no-Lua scripting runtime shell so script bindings can be inspected and lifecycle dispatches are reported even when script execution is unavailable.

## 2026-05-05 (session 26)

### glTF animation playback and animated test asset

- Added scene animation playback fields for duration, playback speed, and transform amplitudes, with scene JSON parse/export validation.
- Expanded scene model assets to load external-buffer glTF mesh geometry, material texture references, and basic transform animation clip metadata.
- Marked animated mesh instance paths as dynamic so Qt can update animated transforms without rebuilding the full D3D12 scene each frame, with CPU fallback scene refresh.
- Added the CC0 Khronos AnimatedCube glTF sample and `assets/scenes/animated_cube_test.json` as a focused animation playback test scene.

## 2026-05-05 (session 25)

### Textured asset imports, D3D12 texture pipeline, and docs relocation

- Moved project planning docs, TODOs, and the changelog under `docs/`, and updated README, smoke, todo audit, and prototype links to the new paths.
- Expanded OBJ/MTL scene import support for UVs, texture paths, richer material fields, emissive values, double-sided flags, and spot light direction/cone serialization.
- Added D3D12 texture loading for WIC/TGA assets, texture metadata buffers, base-color and normal texture sampling, and GPU guide, denoise, and temporal accumulation passes.
- Added DXR/runtime tuning for deferred DXR object creation, optional hardware shadow rays, quieter verbose logging, and more representative ray counter estimates.
- Added complete sedan, toy car, and Crytek Sponza scene/model assets, refreshed Sponza material/texture references, and updated showcase scene lighting/collider setup.
- Added Qt dock slider debounce so live slider edits stay responsive without fighting dock value refreshes.
- Moved optimization graph scaffolds from `examples/` to `experiments/`, keeping generated graph result folders ignored.
- Added FPS camera/player collision scaffolding and camera dock controls for entering/exiting FPS mode.
- Expanded `.gitignore` coverage for build outputs, generated diagnostics, local IDE state, and temporary files.

## 2026-05-05 (session 24)

### Qt startup splash progress polish

- Reworked the custom Qt startup splash layout to use font-metric text bounds so title, subtitle, phase, loading, and footer copy no longer clip under Windows font scaling.
- Changed the splash spinner to a visibly rotating bright trail instead of a static radial fade.
- Added a monotonic `Loading 0-100%` counter and determinate progress bar driven by startup phases, with splash completion forcing 100% before fade-out.
- Verified the Windows clang-cl D3D12 Qt build, UI model smoke path, one-frame Qt window launch, and staged whitespace check for the splash changes.

## 2026-05-05 (session 23)

### Qt scene graph selection, quiet GUI launch, and dotenv config

- Added nested Qt scene graph rows with stable hierarchy indentation, selection state, and type-specific icons for models, lights, cameras, SDFs, and other scene items.
- Wired Qt scene graph item activation into the editor selection path so clicking a panel row selects the corresponding scene item in the canvas, including additive and range selection gestures.
- Made the Windows Qt GUI launch without a separate console by default, with `--console`, `--terminal`, and `run.ps1 -Console` restoring terminal output on demand.
- Added `.env` runtime configuration support, a committed `.env.example`, launcher defaults from dotenv/process environment values, and docs for the new flags and environment variables.
- Verified the Windows clang-cl D3D12 Qt build, GUI subsystem header, Qt UI smoke path, dotenv config loading, launcher parse check, and whitespace check for the touched files.

## 2026-05-05 (session 22)

### Physical camera controls, GPU traversal experiments, and profiling docs

- Added scene-level camera lens and film fields for focal length, sensor size, aperture/focus, physical exposure, white balance, iris shape, and anamorphic squeeze.
- Wired the physical camera controls through CPU, Vulkan, D3D12 compute, and DXR camera ray generation and film resolve paths.
- Expanded the Qt camera/render docks with lens, focus, tone-map, exposure, output transform, and saved-shot controls, plus a startup splash path.
- Added D3D12 packed-triangle traversal experiments, PIX autorun/profiling hooks in `ptbench`, and a D3D12/DXR algorithm graph example scaffold.
- Updated the UI plan, TODOs, threading plan, and optimization guide with the new camera, render-threading, and profiling work.

## 2026-05-05 (session 21)

### Render threading coordinator and CPU scheduling stability

- Added a latest-frame handoff and `RenderCoordinator` to own background tiled CPU rendering on a `std::jthread`, coalesce camera/scene/settings updates, and publish immutable frames to the UI.
- Replaced the Qt and raw window tiled CPU background render loops with the coordinator so the UI thread no longer mutates a live tiled tracer.
- Added stop-aware job waits and cancellable tiled CPU sample batches so coordinator shutdown can drain work safely.
- Moved scalar CPU render work onto the shared job system instead of spawning per-render `std::thread` workers.
- Verified the Qt D3D12 build, default build, job-system smoke, and Qt offscreen tiled CPU smoke after the threading changes.

## 2026-05-05 (session 20)

### Physics, multi-accelerator planning, tessellation, and SDF editor support

- Added dynamic physics scene metadata, transform publication, D3D12 dynamic instance update support, and a dynamic physics performance gate for checking transform-only updates without full scene rebuilds.
- Enabled Jolt for the Windows D3D12 and Qt D3D12 presets, pinned the local LLVM clang-cl and Qt prefix for fresh configure runs, and verified the Qt/D3D12 Cornell scene now reports the Jolt backend at runtime.
- Fixed Jolt dynamic-body scene sync so edited/moved bodies are activated, have their sleep timer reset, and no longer require toggling collision detection before simulation resumes.
- Added D3D12 accelerator capability planning with `auto` and `high-performance` preset semantics: `auto` selects one real accelerator by discrete GPU, integrated GPU, then CPU priority, while high-performance enables every eligible real accelerator and keeps WARP opt-in.
- Extended `run.ps1` with `-MaxDepth`, D3D12 throughput knobs, and an in-file reference of current project flags, environment variables, and accelerator preset notes.
- Added optimization graph example packs for CPU path tracing and D3D12/DXR investigation, plus an HDRI sky asset and Lisa studio scene updates for importer/rendering smoke coverage.
- Added cached tessellation request metadata, a compute tessellation shader scaffold, a tessellation sphere-level scene, and CPU analytic sphere SDF intersection to avoid expensive sphere ray marching.
- Wired SDF sphere primitives into the D3D12 compute path with an SDF SRV, `num_sdfs` root constant, analytic sphere hit testing, and SDF shadow occlusion.
- Added D3D12 compute direct-light specular preview terms so mirror, metal, glass, and clearcoat material families remain inspectable under point lights even with a black environment.
- Threaded configurable D3D12 BVH build settings through static and dynamic-instance builders and logged leaf/bucket/split settings for tuning.
- Added `ptbench` diagnostics for the D3D12 shader path and BVH leaf/bucket/split settings, with `PT_D3D12_HLSL_PATH` override support for experiments.
- Added `PT_D3D12_SHADER_TRAVERSAL` experiment selection for D3D12 shader traversal variants, with compile-time shader macros and runtime diagnostics.
- Fixed the DXR closest-hit path to resolve dynamic instance triangle/material offsets and transform hit normals into world space.
- Expanded the Qt inspector with editable dropdowns/sliders/reset controls for materials, mesh IDs, SDF primitives, physics, lights, cameras, and render/device statistics.
- Kept Qt dock panels stable while refreshing live property values in place, avoiding full widget rebuilds for changing inspector/render/device fields.
- Made SDF primitives selectable like meshes in the Qt viewport and scene graph, including editable inspector controls for entity SDF components and standalone SDF definitions.
- Added small imported OBJ scene assets and loader support for model-backed scenes used by the asset/import smoke path.

## 2026-05-05 (session 19)

### Qt shell panels, viewport gizmos, and camera controls

- Added the Qt menu/status/dock shell, editor panel models, dock readiness smoke coverage, and a configurable `run.ps1` Qt launcher.
- Moved CPU preview rendering onto a background path with coalesced Qt framebuffer handoff and throttled UI presentation to keep panels responsive.
- Added viewport picking with front-face triangle filtering, stable 3D selection bounds, and direct scene transform editing for selected mesh/SDF objects.
- Reworked the selected-object gizmo into a compact camera-facing corner control with hover highlights, axis translate, endpoint axis scale, corner-arc rotate, and freeform bounding-box drag.
- Added camera interaction controls for right-drag orbit/FPS look, wheel dolly, keyboard FPS movement, and inverted middle-button pan.
- Fixed Qt menu flicker on selection by preserving the existing `QMenuBar` and skipping identical menu rebuilds.
- Verified Qt/no-Qt builds and the Qt smoke suite after the viewport and camera control changes.

## 2026-05-05 (session 18)

### Material and shader surface implementation

- Added extended scene material fields for material family, metallic, IOR, transmission, clearcoat, sheen, anisotropy, alpha, and double-sided shading.
- Wired material models/effects through CPU, Vulkan, D3D12 compute, and DXR shader paths with procedural surface variation, transmissive, metallic, clearcoat, sheen, toon, and emissive approximations.
- Promoted the material registry entries to implemented status while keeping benchmark approval separate from runtime support.
- Added `assets/scenes/material_shader_physics_showcase.json`, a larger ECS scene with 72 physics-enabled material spheres plus floor and softbox lighting.
- Verified scene schema, shader checks, CPU render smoke, and D3D12 render output for the new showcase scene.

## 2026-05-05 (session 17)

### ECS scene tree TODO completion

- Added stable sibling ordering to ECS hierarchy components and command replay for create-child, preserve-world reparent, subtree delete, and sibling reorder operations.
- Added scene tree row model generation with hierarchy depth, deterministic sibling order, component badges, hover/selection, lock, hidden, and warning state.
- Made scene JSON export include authored scene data and hierarchy `sibling_order`, with a smoke-tested save/load roundtrip.
- Converted bundled scene JSON files to ECS-rooted hierarchies with entity-level camera, light, mesh, and SDF components.
- Expanded the scene schema check to validate every bundled scene, convert to `SceneWorld`, and build path tracer scene data.
- Marked `todos.md` G37-G39 complete and promoted the `tree.hierarchy` UI release gate to passing evidence.

## 2026-05-05 (session 16)

### Qt platform completion, D3D12 preview fixes, and TODO implementation pass

**Qt/window platform:**
- Added the guarded Qt platform path, Qt-enabled presets, runtime/build metadata, and platform selection docs.
- Added Windows `windeployqt` deployment for Qt-enabled executables so Qt DLLs/plugins are copied beside `ptapp.exe`.
- Fixed the Qt live preview path to honor `--width`/`--height` render resolution independently of `--window-width`/`--window-height`.
- Verified `ptapp --window --platform qt --backend d3d12 --scene assets/scenes/cornell_native.json` with the Cornell scene rendering in the Qt GUI.

**D3D12/rendering:**
- Switched the D3D12 film accumulation UAV to a raw buffer/`RWByteAddressBuffer` path so shader read-modify-write accumulation compiles reliably.
- Preserved the inward-wound Cornell-box backface-culling behavior for orbiting interior views.
- Expanded backend contracts, debug views, frame graph metadata, material descriptors, and scene parsing/validation support.

**Diagnostics, benchmarks, and tools:**
- Added `ptdoctor`, richer release gates, Qt GUI smoke scripts, TODO audit tooling, and broader smoke coverage.
- Added benchmark schema extensions, asset/material/lighting inventories, and documentation for platform, Qt lifecycle, native surfaces, threading, and diagnostics.
- Updated `todos.md` to reflect completed work and remaining unchecked follow-up items.

## 2026-05-05 (session 15)

### Tonemap exposure tuning and soft-shadow light radius sampling

**Image/lighting tuning:**
- Reduced GPU tonemap exposure from `1.0` to `0.6` in D3D12 constant setup to correct over-bright preview output.
- Increased Cornell scene point light `radius` to `0.65` for clearly visible penumbra.

**Soft-shadow implementation:**
- Added `radius` to runtime `RTHitLight` and wired light radius through scene conversion from JSON/light entities.
- Updated D3D12 and Vulkan light packing to upload `radius` in the 8th float of each light record (instead of hardcoded zero).
- Updated D3D12 HLSL direct-light sampling to jitter finite-radius lights (radius=0 keeps point-light behavior).
- Updated CPU scalar direct-light sampling to use the same radius-based light position sampling for cross-backend consistency.

**Files touched:**
- `src/pathtracer/PathTracer.h`
- `src/pathtracer/PathTracer.cpp`
- `src/gpu/D3D12GpuPathTracer.cpp`
- `src/gpu/VulkanGpuPathTracer.cpp`
- `src/shaders/gpu/pathtrace_cs.hlsl`
- `assets/scenes/cornell_native.json`

## 2026-05-05 (session 14)

### GPU tonemapping and D3D12 path performance optimizations

**Bottlenecks removed:**
- RGBA32F readback (8 MB/frame at 60fps = ~480 MB/s PCIe): eliminated entirely for the interactive path
- CPU film-averaging loop (~518 K divide+store ops/frame): removed
- CPU `resolve_ldr()` two-pass tonemapping (log-avg luminance + Reinhard): moved to GPU
- 3�3 CPU spatial denoiser pass in `D3D12GpuPathTracer::resolve_ldr()`: removed
- Per-frame temporary descriptor heap allocation in `reset_accumulation()`: replaced with persistent heap

**Changes:**
- `pathtrace_cs.hlsl`: replaced unused `_pad10` with `exposure` constant; added `RWBuffer<uint> LdrBuf : register(u1)`; added `tonemap_main` compute entry point (Reinhard + gamma 2.2, writes packed RGBA8 uint per pixel)
- `D3D12GpuPathTracer.h`: `PathTraceConstants._pad1` ? `exposure`; added `m_tonemapPso`, `m_ldrBuf`, `m_ldrReadbackBuf`, `m_ldrReadbackPtr`, `m_clearHeap`, `m_ldrResolve`; added `create_tonemap_pso()` private method
- `D3D12GpuPathTracer.cpp`:
    - `create_root_sig_and_pso()`: extended UAV descriptor range from 1 to 2 (u0 FilmBuf + u1 LdrBuf); compiles tonemap entry point into `m_tonemapPso`
    - `create_film_buffer()`: creates LDR GPU buffer (4 B/pixel UAV) + CPU-mapped readback buffer; creates persistent `m_clearHeap` for `reset_accumulation()`
    - `destroy_film_buffer()`: cleans up LDR buffers and clear heap
    - `reset_accumulation()`: uses `m_clearHeap` (no per-frame heap allocation)
    - `render_sample_batch()`: descriptor heap expanded to 9 slots; two dispatches per frame (path trace + tonemap) with UAV barriers between them; LDR readback only (2 MB vs 8 MB); CPU film copy removed
    - `resolve_ldr()`: returns `m_ldrResolve` (a `memcpy` from the readback buffer � zero CPU tonemapping work)

## 2026-05-04 (session 13)

### Camera orbit, backface culling, and Cornell box face winding fixes

- Added camera orbit animation: camera rotates 7.5�/sec around the scene bounding-box centroid at a fixed radius. Orbit state is computed from elapsed time each frame; orbit center derived from the vertex bounding box of the loaded scene.
- Added `update_camera()` fast path to `IPathTracer` and `D3D12GpuPathTracer`: updates camera position/target without invalidating uploaded geometry, avoiding a full scene reload on every orbit step.
- Added backface culling to `pathtrace_cs.hlsl` `IntersectTri()`: changed `if (abs(a) < 1e-5)` to `if (a < 1e-5)` so back-facing triangles are skipped.
- Fixed face winding order for all 6 Cornell box room surfaces in `cornell_native.json` (floor, ceiling, back_wall, left_wall, right_wall, ceiling_light_mesh): indices changed from `[0,1,2, 0,2,3]` to `[0,2,1, 0,3,2]` so normals face inward toward the room interior.
- Fixed face winding order for the bottom and z-min faces of all 3 cuboid objects (diffuse, mirror, glossy): bottom indices changed from `[0,4,5, 0,5,1]` to `[0,5,4, 0,1,5]`; z-min from `[0,1,2, 0,2,3]` to `[0,2,1, 0,3,2]`.
- Added 60fps frame pacing (`sleep_for(16ms - frame_work_time)`) to the window render loop.
- Hardened CPU fallback guard: if an explicit non-CPU backend is requested and unavailable, the application exits with an error rather than silently falling back to CPU.

## 2026-05-04 (session 12)

### D3D12 reliability, backend selection hardening, and material-color parsing fix

- Hardened backend routing so explicit GPU backend requests do not silently fall back to CPU. Backend aliases now normalize `dxr` and `d3d12dxr` to `d3d12-dxr`, and availability checks fail fast when requested backends are not compiled in.
- Fixed scene material vector parsing in scene import for `albedo` and `emission` by correctly reading vectors from the parent material object key path.
- Updated D3D12 and shader constant layouts to include `rays_per_pixel`, added BVH/triangle-material bindings in the compute shader path, and integrated DXR pipeline plumbing with improved diagnostics and resource lifetime handling.
- Corrected Vulkan sample/ray counter accumulation to increment rays by per-sample work rather than cumulative sample count.

## 2026-05-04 (session 11)

### Cornell box with diffuse / mirror / glossy cuboids; infinite convergence; GPU confirmed on Arc B580

**Session goals:** Add material variety (mirror, glossy, diffuse cuboids) to the Cornell box; ensure the GPU path tracer runs on the discrete Arc B580; make the window accumulate samples indefinitely for continuous convergence.

---

**`assets/scenes/cornell_native.json` � scene rebuild**
- Proper Cornell box: saturated red left wall `[0.65, 0.05, 0.05]`, green right wall `[0.12, 0.45, 0.12]`, white floor/ceiling/back wall, area light on ceiling `emission_intensity=12`
- SDF sphere removed (GPU tracer does not support SDF intersection)
- Three axis-aligned cuboids with distinct materials and sizes:

| Object | x | y (height) | z | Material |
|---|---|---|---|---|
| Diffuse (tall, gray) | -0.70 ? -0.20 | 0 ? 1.0 | -0.60 ? -0.10 | albedo=0.76, roughness=1.0 |
| Mirror (medium, silver) | 0.05 ? 0.55 | 0 ? 0.80 | -0.50 ? 0.00 | albedo=0.95, roughness=0.0 |
| Glossy (short, warm gold) | 0.30 ? 0.85 | 0 ? 0.45 | 0.10 ? 0.55 | albedo=[0.85,0.55,0.15], roughness=0.3 |

Each cuboid is 6 faces � 2 triangles = 12 triangles, indices generated with a consistent CCW-outer winding (normal flip handled by the path tracer).

---

**BSDF � all three path tracers updated consistently**

`src/pathtracer/PathTracer.cpp`, `src/cpu/Avx2PathTracer.cpp`, `src/shaders/gpu/pathtrace.comp`:

| `roughness` | BSDF | Implementation |
|---|---|---|
| = 0.001 | Perfect mirror | `dir = rd - 2�(n�rd)�n`; NEE skipped (delta BSDF) |
| = 0.999 | Lambertian diffuse | cosine-weighted hemisphere (unchanged) |
| 0.001�0.999 | Glossy Phong lobe | sample around mirror direction; `exponent = max(0, 2/a4 - 2)` where `a = roughness�` |

Throughput weight = `albedo` in all cases (energy-conserving approximation: `f_bsdf�cos? / pdf = albedo`). `roughness=0.3` ? `exponent � 245` (tight highlight, clearly glossy).

New `sample_phong_lobe()` helper added to `ScalarCpuPathTracer` (header + impl) and as an anonymous-namespace `inline` function in `Avx2PathTracer.cpp` and a GLSL function in `pathtrace.comp`.

---

**`src/cpu/CpuFeatures.cpp/.h` � AVX2 dispatch fix**
- `BuildSimdDispatchInfo` now correctly sets `preferred = X86Avx2` when AVX2 is detected (previously collapsed all x86 AVX variants to `X86Avx`)
- `X86Avx512` added to the preference chain above `X86Avx2`
- `ToString(SimdBackend)` updated for `X86Avx2` and `X86Avx512`

**`src/cpu/TiledCpuPathTracer.h/.cpp` � AVX2 tile dispatch**
- Constructor detects CPU features via `QueryCpuFeatures()` + `BuildSimdDispatchInfo()`
- `init_tile_tracers()` creates `Avx2CpuPathTracer` per tile when `preferred == X86Avx2`, else `ScalarCpuPathTracer`
- `film()` accessor added to the interface
- `TileState::tracer` promoted from `unique_ptr<ScalarCpuPathTracer>` to `unique_ptr<IPathTracer>`

---

**`src/app/main.cpp` � infinite accumulation + GPU name in overlay**
- `previewSettings.spp = UINT32_MAX` � removes the 64-sample cap so the window accumulates indefinitely while the camera is stationary
- Background render thread loops `while (!bgRenderStop)` (no spp limit)
- Overlay shows `samples=N` (no denominator) and `backend: vulkan  [Intel(R) Arc(TM) B580 Graphics]`
- `--list-gpus` flag enumerates Vulkan physical devices and prints the selected GPU

**`src/gpu/VulkanGpuPathTracer.h/.cpp` � device enumeration + diagnostics**
- `init_device()` logs ALL physical devices with name, type (`DISCRETE`/`INTEGRATED`), API version, and VRAM
- New accessors: `gpu_name()`, `vram_mb()`, `gpu_type()`, `vulkan_api()`
- On this machine: GPU0 = Intel UHD 770 (INTEGRATED, 16 GB), GPU1 = Intel Arc B580 (DISCRETE, 12 GB) ? B580 selected

**Vulkan push-constant layout fix**
- `vec3 env_color` in GLSL `layout(push_constant)` has 16-byte alignment (std430), creating an 8-byte gap vs the C++ struct at offset 88. `pc.max_depth_f` was reading garbage ? `uint(0)` ? path loop never ran ? all-black
- Fixed by replacing the trailing `vec3 env_color; float max_depth_f` with four individual `float` scalars (`env_r`, `env_g`, `env_b`, `max_depth_f`); scalars are 4-byte aligned, no gap

---

## 2026-05-04 (session 10)

### Real Vulkan GPU compute path tracer � Intel Arc B580, Windows + Linux plumbing

**Goal:** Replace the fully simulated `VulkanComputeBackend` with a real Vulkan compute path tracer running on the GPU.

**`src/shaders/gpu/pathtrace.comp` � GLSL compute shader (new)**
One invocation per pixel. Brute-force M�ller�Trumbore triangle intersection across all scene instances. PCG hash RNG seeded per-pixel-per-sample. Cosine-weighted hemisphere sampling identical to `ScalarCpuPathTracer`. NEE with point lights + shadow rays. Emissive geometry visible at depth 0 and when no point lights present. Accumulates into a persistent RGBA32F film buffer (`r_sum, g_sum, b_sum, sample_count` per pixel). Push constants carry all per-dispatch parameters (camera, scene counts, sample index, seed). Local workgroup size 8�8.

**`src/gpu/VulkanGpuPathTracer.h/.cpp` � real `IPathTracer` over Vulkan (new)**
Implements `IPathTracer` so it integrates cleanly into all existing render paths.
- `init_device()`: `vkCreateInstance` (Vulkan 1.2, no extensions) ? enumerate physical devices, select discrete GPU (Intel Arc B580) ? create `VkDevice` with compute queue ? `vkGetDeviceQueue`
- `create_cmd_pool()`: `VkCommandPool` (resettable) + single persistent `VkCommandBuffer` + `VkFence`
- `create_pipeline()`: loads SPIR-V from `<exe>/shaders/pathtrace.spv` ? `VkShaderModule` ? 6-binding `VkDescriptorSetLayout` (all `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`) ? `VkPipelineLayout` (push constants 104 bytes) ? `VkComputePipeline`
- `build_or_update_acceleration()`: packs `RTSceneData` into flat GPU arrays (3f/vertex, 3f+3f+1f+1f/material, 4u/instance, 8f/light), creates `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | HOST_COHERENT_BIT` storage buffers and maps+copies
- `render_sample_batch()`: writes push constants (camera basis using same `cross(forward,up)` convention as scalar tracer, per-sample seed), records `vkCmdBindPipeline` + `vkCmdBindDescriptorSets` + `vkCmdPushConstants` + `vkCmdDispatch(ceil(w/8), ceil(h/8), 1)`, submits and waits on fence (10s timeout), reads back host-coherent film buffer into CPU `FilmBuffer` for `resolve_ldr()`
- All buffers `HOST_VISIBLE | HOST_COHERENT` so no staging copies are needed; film buffer directly readable by CPU after fence

**`CMakeLists.txt` � Vulkan SDK integration**
- `find_package(Vulkan REQUIRED)` when `PT_ENABLE_VULKAN=ON`; respects `VULKAN_SDK` env variable for SDK path
- `find_program(GLSLC)` located from SDK Bin dir
- `add_custom_command` compiles `pathtrace.comp` ? `${binary}/bin/shaders/pathtrace.spv` at build time
- `add_custom_target(pt_shaders)` as dependency of `ptapp`
- `target_include_directories` and `target_link_libraries` wire `Vulkan_INCLUDE_DIRS` and `Vulkan_LIBRARIES` into `ptapp` and `ptbench`
- `PT_SHADER_SPV_PATH` compile definition passes the SPIR-V path as a string literal so the runtime can locate it without hardcoding

**`CMakePresets.json` � `windows-clang-vulkan-debug` preset (new)**
- `PT_ENABLE_VULKAN=ON`, `PT_ENABLE_AVX2=ON`, `PT_ENABLE_CPU_RAYTRACER=ON`
- `VULKAN_SDK=C:/VulkanSDK/1.4.341.1`
- Clang++ Debug, Ninja, Windows

**`src/app/main.cpp` � backend routing**
- `--backend vulkan` or `--backend vulkan-compute` in `--window` and `--render` modes creates a `VulkanGpuPathTracer(spv_path)`; falls back to CPU tiled if the GPU tracer fails to initialise
- Background render thread and all existing window-mode logic unchanged

**Runtime confirmation**
```
[info] [vulkan] GPU: Intel(R) Arc(TM) B580 Graphics
[info] [vulkan] Compute pipeline created from .../shaders/pathtrace.spv
[info] [app]    Using Vulkan GPU path tracer
```

**Linux plumbing notes**
- `find_package(Vulkan)` and `find_program(GLSLC)` work on Linux with `apt install vulkan-sdk` or system Vulkan packages
- The `linux-clang-vulkan-debug` preset (`PT_ENABLE_VULKAN=ON`) already existed and now has real build rules behind it
- No platform-specific code in `VulkanGpuPathTracer.cpp` � pure Vulkan API, compiles on Linux unchanged

---

## 2026-05-04 (session 9)

### Multithreaded AVX2 path tracer wired into GUI window � functional Cornell box render

**Goal:** Get the `TiledCpuPathTracer` + `Avx2CpuPathTracer` pipeline producing a visible image in the `--window` GUI.

**`src/app/main.cpp` � backend selection + non-blocking window render**
- `"auto"` and empty backend now select `TiledCpuPathTracer` (previously fell through to `ScalarCpuPathTracer`), matching the `--render` path.
- Added `<atomic>` and `<mutex>` includes.
- For `TiledCpuPathTracer` in `--window` mode: rendering runs on a dedicated `std::thread` (`bgRenderThread`). Each completed sample is posted to a mutex-protected `BgFrame` struct. The main Win32 loop reads the latest frame without blocking, keeping the message pump responsive. `bgRenderStop` + `bgRenderThread.join()` on window close prevents use-after-free.
- `ScalarCpuPathTracer` retains the original incremental per-pixel-batch path (unchanged).

**`src/cpu/Avx2PathTracer.cpp` + `Avx2PathTracer.h` � full rewrite against scalar reference**

Four bugs in the original implementation produced an all-black image:

| Bug | Root cause | Fix |
|-----|-----------|-----|
| Wrong camera basis | `cam_right = cross({0,1,0}, forward)` gives opposite sign to scalar tracer | `cam_right = cross(forward, camera_up)` � exact match to `ScalarCpuPathTracer::build_or_update_acceleration()` |
| All pixels same RNG | `rng_state[i] = i ^ seed` � every lane-0 pixel across the whole image used an identical random sequence; all samples degenerate | Per-pixel/per-sample seed via `make_rng(x, y, width, sample_index, ...)` matching scalar's `SampleKey` construction |
| Closest hit not tracked | `best_t` passed by value to `intersect8` � never updated, so every subsequent triangle tested against `infinity`; `hit_t` ended up holding the *last* hit, not the closest | `best_t = hit_t` after each triangle test; `intersect8` also takes `current_best_t` by value and only accepts `t < current_best_t` |
| No light contribution | `!enable_nee \|\| depth==0` guard suppressed emissive at bounce depth > 0 with no NEE implementation | Full NEE + MIS-weighted emissive mirroring `ScalarCpuPathTracer::trace` exactly: direct point-light sampling with shadow ray, MIS weight, throughput accumulation |

New structure of `Avx2PathTracer.cpp`:
- `intersect8()` � 8-wide AVX2+FMA M�ller�Trumbore; accepts `current_best_t` to cull farther hits; returns lane bitmask and updates `out_t/u/v` only for closer hits.
- `intersect_scene8()` � loops all instances/triangles, maintains `best_t` per-lane in an `__m256`, collects closest hit position/normal/material into `Hit8`.
- `trace_one()` � scalar path-tracing loop identical to `ScalarCpuPathTracer::trace`: environment miss, emissive (NEE off or depth 0), MIS-weighted emissive hit, NEE direct shadow ray, Lambertian BSDF sample, Russian roulette. Uses `intersect_scene8` for each bounce.
- `render_sample_batch()` � generates camera rays per-pixel (mirroring scalar `camera_rays()`), calls `trace_one` per pixel with correct per-pixel RNG.

**`src/platform/DesktopPlatform.cpp` � eliminate repaint flash**
- `WM_PAINT` no longer calls `FillRect` when a valid framebuffer is present. `StretchBlt` covers the entire client area, making the preceding black fill redundant. Fill is preserved only when no framebuffer exists (startup/errors). Removes the visible black flash that occurred on every `set_overlay_text`-triggered repaint.

**Result:** Cornell box renders in the GUI window with ceiling light, correct NEE illumination, and progressive sample accumulation. `log_avg_lum�4.9`, `max_channel�213` confirmed by offline render test.

---

## 2026-05-03 (session 8)

### CPU path tracer bring-up � window preview, multithreading, SIMD dispatch

**Commits**
| Commit | Scope | Summary |
|--------|-------|---------|
| `8d9f0f5` | scene | Cornell box � sphere, area light mesh, five materials, camera at (0,1,3) |
| `68c925b` | cpu | SIMD dispatch scaffold � `SimdBackend` enum, ARM NEON/VCE/SME, x86 SSE/AVX/AMX detection |
| `9967c76` | pathtracer | Multithreaded CPU tracer � stripe-interleaved thread pool, atomic counters, SIMD dispatch wiring |
| `308326b` | app/platform | Win32 preview window � GDI blit, Fisher-Yates pixel queue, 16ms tracing budget, overlay HUD |

**Cornell box scene** (`assets/scenes/cornell_native.json`)
Fully specified Cornell box: floor, ceiling, back/left/right walls, area light mesh on ceiling, SDF sphere in center. Five materials (white/red/green/light/sphere). Camera at (0,1,3) targeting (0,1,2). Point light at (0,1.85,0).

**SIMD dispatch scaffold** (`src/cpu/CpuFeatures.h/.cpp`)
`SimdBackend` enum (`Scalar`, `ArmNeon`, `ArmVce`, `ArmSme`, `X86Sse`, `X86Avx`, `X86Amx`). `SimdDispatchInfo` struct with `preferred` and `available` backends. `BuildSimdDispatchInfo()` selects best backend at runtime: ARM prefers SME > VCE > NEON; x86 prefers AVX > SSE; AMX is placeholder pending kernel support. VCE mapped as SVE?SVE2; SME detected via `__ARM_FEATURE_SME`. `vce` and `sme` fields added to `CpuFeatureSet`, serialized to JSON.

**Multithreaded CPU path tracer** (`src/pathtracer/PathTracer.h/.cpp`)
`render_sample_pixels` now runs a `std::thread` worker pool. Thread count is `hardware_concurrency()` (12 on this machine). Work is stripe-interleaved: thread `t` processes pixel indices `t, t+N, t+2N, �`. Each thread accumulates into a `LocalAccum` struct (no mutex on hot path); results are merged after all joins. All `SampleCounters` increments use `std::atomic_ref<uint64_t>` to eliminate data races. `read_counters()` moved out-of-line with atomic loads. `configure()` initializes `m_worker_count` and `m_simd_dispatch` and logs both.

**Win32 preview window** (`src/app/main.cpp`, `src/platform/DesktopPlatform.h/.cpp`)
`--window` mode creates a Win32 window via `DesktopPlatform`. Each frame: up to 16ms of CPU path tracing via `render_sample_pixels`, then film resolve and GDI `StretchBlt` DIBSection upload to client area. `WM_ERASEBKGND` returns 1 and `InvalidateRect(FALSE)` suppress flicker. Pixel processing order is Fisher-Yates shuffled per-sample so convergence is stochastic across the whole canvas rather than top-down. Overlay HUD (white text, black shadow) shows frame/sample/non-black stats. Tracer initialised with scene loaded from `--scene` arg; falls back to checkerboard diagnostic texture on load failure.

---

## 2026-05-03 (session 7, Gate 10)

### Gate 10 complete � Release candidate: reproducible benchmark artifacts

**Gate 10 acceptance:** *"Release candidate benchmark scene pack runs with reproducible artifacts."*

**F17 � Startup self-test extended** (`src/app/main.cpp`)
Added `CheckJobSystem()` (creates `JobSystem(1)`, submits a job, waits, verifies completion, shuts down), `CheckSceneSchema()` (loads and validates `cornell_native.json` using `SceneDocument::load_from_file` if present), and `CheckBenchmarkArtifactWrite()` (writes a probe JSON to `artifacts/self_test/` and verifies). `RunDoctor` and the `--doctor` flag now cover all 8 subsystems: build, cpu, backends, assets, shaders, job_system, scene_schema, benchmark_artifact_write. Added `--check-job-system`, `--check-scene-schema`, `--check-bench-write` flags.

**F18 � Profiler event schema** (`src/benchmark/BenchmarkSchema.h/.cpp`)
`ProfilerEventKind` enum (8 kinds: CpuZone, GpuZone, JobTiming, FrameStage, AssetImport, BvhBuild, ShaderCompile, RenderPass). `ProfilerEvent` struct (kind, name, category, thread_id, start_ms, duration_ms). `ProfilerEventKindName()`, `SerializeProfilerEvent()`, `SerializeProfilerTrace()`. `ptbench run` now writes `profiler_trace.json` alongside `results.json` with scene_build, render_samples, resolve_and_write, and total events.

**F15/F16/F19/F20** � Already implemented in previous sessions (GPU memory pressure experiment, shader variant compile matrix, CI smoke plan, release gate check scripts).

---

## 2026-05-03 (session 6, Gates 8�9)

### Commits
| Commit | Scope | Summary |
|--------|-------|---------|
| `98b3c7f` | build/platform | Gate 8 � multi-backend capability flags and platform extensions (C11-C17) |
| `7834502` | pathtracer | Gate 9 � NEE, MIS, film resolve pipeline (D22/D23/D26) |
| `84ebd4a` | materials/shaders | Gate 9 � material evaluation interface, pack registries, shader/SDF manifests (D24/E07-E11) |
| `eceae0d` | render/editor | Gate 9 � debug view registry and editor-lite control model (E18-E21) |
| `3d7ac6c` | diagnostics | Gate 9 � crash recorder with minidump metadata and structured log |
| `9bdbf35` | app/bench | Gate 9 � app command wiring, benchmark schema extensions |
| `e43808b` | docs/scripts | CI smoke plan and release gate check scripts |


### Gate 9 complete � Material/shader library, asset import, debug views, and editor-lite controls

**Gate 9 acceptance:** *"Material/shader library, asset import, debug views, and editor-lite controls exist."*

**D22 � Next-event estimation** (`src/pathtracer/PathTracer.h/.cpp`)
Direct light sampling added to `ScalarCpuPathTracer`. `RenderSettings::enable_nee` flag gates the feature. `sample_direct_light()` selects a random `RTHitLight`, casts a shadow ray, and accumulates Lambert direct contribution. Cornell scene converges faster with `--nee`.

**D23 � MIS** (`src/pathtracer/PathTracer.h/.cpp`)
`MisWeight()` power heuristic (beta=2). `RenderSettings::enable_mis` flag. When both NEE and MIS are enabled, BSDF and light PDFs are balanced via the power heuristic, reducing fireflies without bias.

**D24 � Material evaluation interface** (`src/materials/MaterialInterface.h/.cpp`)
`IMaterial` abstract interface: `evaluate`, `sample`, `pdf`, `is_delta`, `is_emissive`, `energy_check`. Concrete implementations: `DiffuseMaterial` (Lambertian), `MirrorMaterial` (delta reflection), `GlassMaterial` (Schlick Fresnel refraction), `EmissiveMaterial`.

**D26 � Film resolve pipeline** (`src/pathtracer/PathTracer.h/.cpp`)
`ToneMapMode` enum (Linear, Reinhard, FilmicApprox, AcesApprox), `FilmResolveSettings` (exposure, white_balance placeholder, tone_map_mode, gamma, clamp_output), `ApplyFilmResolve()` free function.

**E07�E09 � Material pack registries** (`src/materials/MaterialDescriptors.h/.cpp`)
`MaterialFamily` enum, `ImplementationStatus` enum, `MaterialDescriptor` struct. Full registry: Pack 1 (13 benchmark-core materials, implemented), Pack 2 (19 experimental), Pack 3 (15 backlog), Advanced (25 deferred). `SerializeMaterialRegistry()` exports JSON.

**E10 � Shader family manifest** (`src/shaders/ShaderFamilyManifest.h/.cpp`)
`ShaderFamily` enum (14 families), `ShaderFamilyDescriptor`, `GetShaderFamilyManifest()`, `SerializeShaderFamilyManifest()`. JSON export. CPU-path families marked implemented.

**E11 � SDF feature inventory** (`src/shaders/SdfShaderInventory.h/.cpp`)
`SdfFeature` enum (21 features), `SdfFeatureDescriptor`, `GetSdfFeatureInventory()`, `SerializeSdfFeatureInventory()`. JSON export. Sphere, Box, RoundedBox, Capsule, Torus, Plane marked implemented.

**E18 � Debug view declarations** (`src/render/DebugViews.h/.cpp`)
`DebugViewId` enum (20 views), `DebugViewDescriptor`, `GetDebugViewRegistry()`, `IsDebugViewAvailable()`, `FindDebugView()`, `SerializeDebugViewRegistry()`. UI/CLI can list and query views.

**E19 � Editor-lite control model** (already in `src/editor/UiModels.h`)
Confirmed complete: SelectionState, UiRuntimeState (debug view selector, inspector/material/light/camera/benchmark panel interfaces).

**E20 � Editor command descriptors** (already in `src/editor/UiModels.h`)
Confirmed complete: EditorCommand, 21 command kinds, all payloads, EditorCommandHistory.

**E21 � Demo camera controls** (`src/editor/UiModels.h/.cpp`)
`CameraControllerMode` (Orbit, Fps, Turntable, ScriptedBenchmarkPath), `CameraOrbitState`, `CameraFpsState`, `CameraWaypoint`, `CameraControllerState`, `CameraController` class. Dirty-flag protocol triggers accumulation reset on camera change.

### Gate 8 complete � D3D12, Metal, and WebGPU adapters compile behind capability flags

**Gate 8 acceptance:** *"D3D12, Metal, and WebGPU adapters compile behind capability flags."*

**C11 � Vulkan hardware RT capability probe** (`src/render/backends/VulkanBackend.h/.cpp`, `src/render/interface/RenderContracts.h/.cpp`)
Extended `RenderBackendCapabilities` with `ray_query_supported`, `acceleration_structure_supported`, `shader_group_handle_size`, and `max_as_size`. `VulkanComputeBackend::capabilities()` reports Vulkan RT availability; `ptbench dump-capabilities` includes all RT fields.

**C12�C13 � D3D12 backend skeleton and DXR capability probe**
`PT_ENABLE_D3D12` CMake option guards D3D12 compilation. `BackendKind::D3d12` registered in `RenderContracts.h`; `BackendKindToString()` and `SerializeBackendCapabilities()` cover D3D12 and DXR tier fields.

**C14�C15 � Metal backend skeleton and ray tracing capability probe**
`PT_ENABLE_METAL` CMake option added. `BackendKind::Metal` registered; Metal RT availability included in capability serialization.

**C16�C17 � WebGPU backend skeleton and capability probe**
`PT_ENABLE_WEBGPU` CMake option added. `BackendKind::WebGpu` registered; WebGPU compute, storage, and presentation fields in `RenderBackendCapabilities`.

**RenderBackendCapabilities extensions**
Added: `supports_present`, `supports_multiqueue`, `max_workgroup_size_{x,y,z}`, `max_buffer_alignment`, `memory_model`. All new fields serialized in `SerializeBackendCapabilities()`.

**CMakeLists.txt**
New options: `PT_ENABLE_D3D12`, `PT_ENABLE_METAL`, `PT_ENABLE_WEBGPU`, `PT_ENABLE_OPENGL_EXPERIMENTAL`, `PT_ENABLE_EDITOR`, `PT_ENABLE_PROFILING`, `PT_ENABLE_SANITIZERS`, `PT_STRICT_DETERMINISM`. Build prints active/disabled feature flag summary at configure time.

**E01 � Editor UI model layer** (`src/editor/UiModels.h/.cpp`)
Complete data-model layer for the editor UI. Data types: `UiPanelState`, `UiLayoutDocument` (8 presets), `UiRuntimeState`, `SelectionState`, `SceneEntityBounds`, `MenuBar`/`MenuItem`, `UiShortcut`. Command layer: `EditorCommand` variant over 21 kinds (entity selection/CRUD, transform, material, light/camera property, script attach/detach, asset import/assign, benchmark run, unsupported-action passthrough). `EditorCommandHistory` (capped vector) and `UiEventLog` (capped deque). Interfaces: `IUiSystem`, `IEditorCommandSink`, `ISelectionService`, `IUiPlatformBridge`, `IInspectorModelProvider`, `ISceneTreeModelProvider`, `IAssetBrowserModelProvider`, `IBenchmarkPanelModelProvider`, `IUiLogger`. Layout factories, JSON/JSONL serializers for runtime state, selection, layout document, menu bar, and command log. Asset drop validation and default keyboard shortcut map with conflict detection.

**CMakeLists.txt**
Added `src/editor/UiModels.cpp` to both `ptapp` and `ptbench` source lists.

---

## 2026-05-03 (session 5, Gate 7)

### Gate 7 complete � SIMD CPU backends and backend performance experiments

**Gate 7 acceptance:** *"SIMD CPU backends and backend performance experiments are available."*

**D13 � SIMD abstraction layer** (`src/cpu/SimdKernel.h`)
`SimdMode` enum (`Scalar`, `NEON`, `SVE`, `AVX`, `AVX2`, `AVX512`), `SimdModeName()`, and `SelectBestSimdMode()`. On AArch64 prefers SVE > NEON > Scalar; on x86 prefers AVX512 > AVX2 > AVX > Scalar. All selection logic is compile-time guarded so non-participating ISAs do not appear in binaries.

**D14 � x86 CPU feature detection** (`src/cpu/CpuFeatures.h/.cpp`)
`CpuFeatureSet` struct with fields for SSE2, SSE4.1/4.2, AVX, AVX2, AVX-512F/DQ/BW/VL, and FMA. `QueryCpuFeatures()` uses `__get_cpuid` / `__get_cpuid_count` (GCC/Clang) or `__cpuid` / `__cpuidex` (MSVC) guarded by `VKPT_ARCH_X86`. `SerializeCpuFeatures()` returns a JSON object string for embedding in `dump-capabilities` output.

**D15 � ARM CPU feature detection** (`src/cpu/CpuFeatures.h/.cpp`)
Same `CpuFeatureSet` and `QueryCpuFeatures()` for AArch64: NEON always true (implied by AArch64 ABI), SVE/SVE2/FP16/dot-product detected from compiler predefined macros (`__ARM_FEATURE_SVE` etc.) guarded by `VKPT_ARCH_ARM64`.

**D16 � Packet ray interface** (`src/cpu/PacketRay.h`)
`RayPacket` (SoA layout, up to 16 lanes), `HitPacket` (hit mask + t/u/v/material per lane), `TriangleSOA` (v0, e1, e2, material_index). Maximum packet width `kMaxPacketWidth = 16`.

**D17 � AVX2 8-wide intersection kernel** (`src/cpu/SimdKernelAvx2.h`)
`intersect_triangle_packet_avx2()` � 8-lane M�ller�Trumbore using `__m256` with FMA (`_mm256_fmadd_ps`, `_mm256_fmsub_ps`). `intersect_triangle_packet_avx2_full()` for arbitrary count with scalar tail. Guarded by `#if defined(__AVX2__)`.

**D18 � AVX-512 16-wide intersection kernel** (`src/cpu/SimdKernelAvx512.h`)
`intersect_triangle_packet_avx512()` � 16-lane M�ller�Trumbore using `__m512` and `__mmask16` predication with `_mm512_mask_cmp_ps_mask`. Guarded by `#if defined(__AVX512F__)`. Notes warn about potential frequency throttling.

**D19 � NEON 4-wide intersection kernel** (`src/cpu/SimdKernelNeon.h`)
`intersect_triangle_packet_neon_4()` � 4-lane M�ller�Trumbore using `float32x4_t`. Uses Newton�Raphson refinement (`vrecpsq_f32`) for `vrecpeq_f32` precision. `intersect_triangle_packet_neon()` for arbitrary count. Guarded by `#if defined(__ARM_NEON)`.

**D20 � SVE variable-width intersection kernel** (`src/cpu/SimdKernelSve.h`)
`intersect_triangle_packet_sve()` � variable-length M�ller�Trumbore using `svfloat32_t`, `svbool_t`, and `svcntw()` for runtime lane width. `svwhilelt_b32` predication for arbitrary packet count. Guarded by `#if defined(__ARM_FEATURE_SVE)`.

**F09 � SIMD sweep experiment** (`src/benchmark/ptbench.cpp` � `SimdSweepCommand`)
`ptbench simd-sweep [--rays N] [--triangles N] [--output dir]` � generates random rays and triangles, benchmarks all compiled/available kernels (scalar, NEON, SVE, AVX2, AVX-512) in Mrays/s, identifies best, writes `simd_sweep.json`. CPU features and best SIMD mode reported.

**F10 � Tile-size sweep experiment** (`src/benchmark/ptbench.cpp` � `TileSweepCommand`)
`ptbench tile-sweep --scene <path> [--workers N] [--spp N] [--resolution WxH] [--output dir]` � runs cpu-tiled with tile heights 8, 16, 32, 64, measures Msamples/s per configuration, identifies best, writes `tile_sweep.json`.

**dump-capabilities enhancement**
JSON output now includes `"cpu"` section (architecture + all feature flags) and `"simd_mode"` before the backends array.

**CMakeLists.txt**
Added `src/cpu/CpuFeatures.cpp` to both `ptapp` and `ptbench` source lists.



**Gate 6 acceptance:** *"Multithreaded CPU renderer, parallel BVH, and job system are validated."*

**B08 � Deterministic job mode** (`src/jobs/JobSystem.h/.cpp`)
Added `set_deterministic(bool)` support. In deterministic mode, workers acquire a `std::mutex m_serialMutex` before executing each job, ensuring sequential one-at-a-time execution with stable ordering. Added missing standard library headers (`<deque>`, `<thread>`, `<condition_variable>`, `<memory>`) to the header. Fixed `m_jobs` from `std::vector<JobEntry>` to `std::vector<std::unique_ptr<JobEntry>>` to allow non-moveable members (`std::mutex`, `std::atomic`, `std::condition_variable`) inside `JobEntry`.

**D11 � Parallel BVH builder** (`src/cpu/ParallelBvhBuilder.h/.cpp`)
`ParallelBvhBuilder::build()` partitions AABBs by midpoint split on longest axis. When primitive count = 256 and a `IJobSystem` is provided, left/right subtrees are built as parallel jobs via `jobs->submit_job()` + `wait_group`. Uses `std::stable_partition` in deterministic mode. Reports `BvhBuildStats` including `node_count`, `leaf_count`, `build_ms`, `worker_count`.

**D12 � Tile-based CPU renderer** (`src/cpu/TiledCpuPathTracer.h/.cpp`)
`TiledCpuPathTracer` implements `IPathTracer` and partitions the image into horizontal tiles. Each tile owns a `ScalarCpuPathTracer` instance. `render_sample_batch()` dispatches tile jobs in parallel via the internal `JobSystem`, then merges tile films into the master `FilmBuffer` via `FilmBuffer::import_tile()`. Added `FilmBuffer::import_tile()` and `ScalarCpuPathTracer::film()` accessor to `PathTracer.h/.cpp`.

**F08 � Multithreaded CPU benchmark** (`src/benchmark/ptbench.cpp`)
`ptbench run --renderer-path cpu-tiled` selects `TiledCpuPathTracer`. New CLI options: `--workers N`, `--tile-size H`, `--deterministic`. Diagnostics in `results.json`: `renderer=cpu-tiled`, `worker_count`, `tile_height_rows`, `bvh_nodes`, `bvh_build_ms`, `deterministic`, `speedup_estimate_vs_scalar`.

**CMakeLists.txt** � Added `src/jobs/JobSystem.cpp`, `src/cpu/ParallelBvhBuilder.cpp`, `src/cpu/TiledCpuPathTracer.cpp` to both `ptapp` and `ptbench` targets.

**Verification:** `ptbench run --backend cpu --renderer-path cpu-tiled --workers 2` produces `results.json` with all expected diagnostics. Clean full rebuild (36/36 objects) with only pre-existing warnings.

## 2026-05-03 (session 4)

### Gate 5 complete � Benchmark CLI, artifact contract, scene validation, image compare

**Gate 5 acceptance:** *"Benchmark CLI runs CPU and Vulkan paths and writes results.json."*

All Gate 5 dependencies were already implemented in `ptbench`. Verified and marked complete:

**F01 � Benchmark result schema** (`src/benchmark/BenchmarkSchema.h/.cpp`)
`BenchmarkResult` with fields: run_id, scene, backend, renderer_path, resolution, spp, seed, timing, throughput, memory, image_hash, reference_error, diagnostics. All numeric metrics carry units.

**F02 � Benchmark run descriptor** (`src/benchmark/BenchmarkSchema.h/.cpp`)
`BenchmarkRunDesc` with scene path, backend, renderer-path, resolution, spp, duration, warmup-frames, seed, output dir, reference image, tolerance policy. Serializes to/from JSON; replayable.

**F03 � Benchmark CLI shell** (`src/benchmark/ptbench.cpp`)
`ptbench` with 8 commands: `run`, `list-scenes`, `list-backends`, `list-renderer-paths`, `validate-scene`, `compare`, `dump-capabilities`, `run-experiments`.

**F04 � Benchmark artifact contract** (`src/benchmark/ptbench.cpp`)
`ptbench run` writes all required artifacts: `results.json`, `results.csv`, `metadata.json`, `scene_snapshot.json`, `shader_manifest.json`, `asset_manifest.json`, `beauty.png`, `beauty.exr`, `logs.jsonl`.

**F05 � Scene validation command** (`ptbench validate-scene`)
Validates schema version, asset refs, materials, lights, camera, benchmark settings, backend compatibility. Outputs human text or `--json`.

**F06 � Image comparison pipeline** (`ptbench compare`)
Computes mean absolute error, max error, RMSE, NaN/Inf count; writes diff heatmap PNG. Tolerance policy is explicit.

**F07 � CPU scalar benchmark** (`ptbench run --backend cpu --renderer-path cpu-scalar`)
Correctness baseline � renders scene and writes full artifact set.

Verified run:
```
ptbench run --scene assets/scenes/cornell_native.json --backend cpu \
            --renderer-path cpu-scalar --resolution 64x64 --spp 4 \
            --output artifacts/gate5_test
-> run complete; artifacts: beauty.png, beauty.exr, results.json, ...
```

**todos.md:** F01�F07 marked `[x]`; F08+ retain `[ ]`; Gate 5 annotated `(completed)`.

---

## 2026-05-03 (session 3)

### Gate 4 complete � Vulkan compute render path wired

**Gate 4 acceptance:** *"Vulkan compute backend renders the same tiny scene."*

`--render --backend vulkan` now routes through `RunVulkanBVHPass` instead of `ScalarCpuPathTracer`:
- Uploads vertex/index buffers to the simulated Vulkan allocator
- Runs 4-pass frame graph: `bvh_upload ? bvh_build ? pathtracer ? film_resolve`
- Writes PNG output from the simulated film buffer
- Reports vertex/index/instance/bvh-node counts on stdout

CPU scalar path is unchanged and remains the default when `--backend` is unset.

Verified output:
```
ptapp --render --backend vulkan --width 32 --height 32
render complete (vulkan-compute): artifacts/renders/cornell.png
vertices: 8  indices: 36  instances: 1  bvh_nodes: 23
```

**todos.md:** Gates 2, 3, 4 marked `(completed)` in the gate index.

---

## 2026-05-03 (session 2)

### A07�A13, A16, B04�B07, C10

Implemented all remaining Gate-5 infrastructure and wired it into the main binary.

**A12 � PT_ASSERT / PT_VERIFY / PT_FATAL macros** (`src/core/Assert.h/.cpp`)
Assertion layer with three tiers: `PT_ASSERT` (debug-only), `PT_VERIFY` (always-on), `PT_FATAL` (always aborts). All three write a crash artifact JSON before terminating so failures are recoverable in headless/CI runs.

**A10 � Runtime config system** (`src/core/Config.h/.cpp`)
Key-value config file parser (`key = value`, `#` comments), `PTAPP_*` env-var overrides, CLI flag overlay. Each value carries its `ConfigSource` (Default / ConfigFile / EnvVar / CliFlag). Exposed via `--config <path>` and `--dump-config` in `ptapp`.

**A07 � Crash flight recorder** (`src/diagnostics/CrashRecorder.h/.cpp`)
Singleton that tracks active backend, frame stage, pass name, shader, scene, and last 1 024 log events. On `flush()` it writes `artifacts/crashes/crash_<timestamp>/crash_state.json` + `last_log_events.jsonl` + `build_info.json`.

**A08 � Platform crash hooks** (`src/diagnostics/CrashHooks.h/.cpp`)
`install_crash_hooks()` wires `SetUnhandledExceptionFilter` (Windows) / `sigaction` (POSIX) to call `CrashRecorder::flush()` before the process dies.

**A13 � Status file writer** (`src/diagnostics/StatusFile.h/.cpp`)
Writes `artifacts/status/latest_status.json` on every run with build status, last run result, selected backend/scene/renderer-path, last error, crash artifact path, and a performance summary.

**A09 � ptdoctor** (`src/app/main.cpp`)
Full `--doctor` / `--check-build` / `--check-cpu` / `--check-backends` / `--check-assets` / `--check-shaders` implementation. Each check prints `[ok ]` or `[FAIL]`. Also added `--crash-test`, `--config`, `--dump-config`.

**A16 � Integration smoke CI script** (`tools/smoke.ps1`)
9-step PowerShell script: configure ? build ? binary-exists ? version ? doctor ? render ? status-file ? exr-output ? dump-config. Returns non-zero exit code on any failure for CI consumption.

**B04 � Desktop window stub** (`src/platform/DesktopPlatform.h/.cpp`)
`DesktopWindow` and `DesktopFileSystem` behind `IWindow` / `IFileSystem`. Tracks open/focus/resize state and drains input events.

**B05 � Input event normalization** (`src/platform/DesktopPlatform.h/.cpp`)
`DesktopInput` queues and emits key, mouse-move, mouse-button, mouse-wheel, focus-change, close-requested events as normalized `InputEvent` structs.

**B06 � Job system foundation** (`src/jobs/JobSystem.h/.cpp`)
Thread-pool `JobSystem` implementing `IJobSystem`: `submit_job`, `submit_range_job`, `wait`, `wait_group`, `worker_count`, `pump_main_thread`, `shutdown`, and `deterministic` mode toggle.

**B07 � Task graph scheduling** (`src/jobs/TaskGraph.h`)
Dependency-aware `TaskGraph` with topological sort, cycle detection, per-task timing via `TaskExecutionSample`, and optional `IJobSystem` dispatch for parallel execution.

**C10 � Vulkan software-BVH compute pass** (`src/render/backends/VulkanBackend.h/.cpp`)
`RunVulkanBVHPass()` uploads vertex/index buffers, initialises a BVH node buffer (`2N-1` nodes � 32 bytes), allocates film texture, and drives a 4-pass frame graph (`bvh_upload ? bvh_build ? pathtracer ? film_resolve`). Returns a `VulkanBVHPassResult` with upload stats.

**Resource registry** (`src/render/interface/ResourceRegistry.h`)
`ResourceLifetimeRegistry` tracks per-handle lease info (kind, label, size, frame acquired/last-accessed, ref-count) for render resource lifetime diagnostics.

**Build** (`CMakeLists.txt`)
Added `Assert.cpp`, `Config.cpp`, `CrashRecorder.cpp`, `CrashHooks.cpp`, `StatusFile.cpp` to both `ptapp` and `ptbench` targets.

---

## 2026-05-03

### Commit notes

- 9d7b71a � feat(app): add minimal ptapp entrypoint and --version output
- 7ae1d90 � build: add CMake presets and build metadata wiring
- e47db3e � chore: add initial project scaffolding layout
- b7b04e6 � feat(core): add primitive types and headless platform foundation
- 43f8d28 � feat(app): implement headless app shell and diagnostic logging
- 2d48124 � docs: mark Gate 1 tasks complete for implemented milestones
- 22252de � fix(platform): resolve HeadlessFileSystem stream declaration parse issues
- 1056e65 - feat(scene): add gate 2 scene ECS schema and snapshot system
- 278f505 - feat(app): wire scene loading into startup path
- 810141a � feat(benchmark): add gate3 benchmark schemas, scene manifests, and ptbench CLI
- 74d7157 � feat(app): implement scalar CPU path-tracing render path and Gate 3 render CLI flags
- e98b4e0 � feat(render): finish Gate 4 backend scaffold (interfaces, factory, null/vulkan backends, frame graph, layout manifest API), add backend-capability diagnostics in ptapp/ptbench, and wire manifest serialization
- 66c7ff5 � fix(benchmark): correct PNG scanline indexing in ptbench image compare loader
