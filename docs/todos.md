# Active third-person action scene TODOs

* [ ] Add real skeletal/skinned glTF animation playback so the hero's authored walk clip can drive bones instead of static mesh pose.

Below is the **six-agent parallel workstream plan** I would give to Codex agents. It preserves the uploaded plan’s core constraints: **C++23, Clang/LLVM, shared architecture, backend adapters, deterministic scheduling, renderer capability gates, ECS, scene schema, path tracing, benchmark mode, Jolt/Lua as staged systems, and multi-backend graphics support**. 

The execution model is:

```text
6 agents work in parallel.
Each workstream has chronological todos.
Each todo is singular in focus.
Each todo has deliverable, implementation hints, and acceptance criteria.
All agents integrate through shared interfaces, not direct cross-module hacks.
```

Use this as the master plan for the Codex swarm.

---

# 0. Non-negotiable project rules

## Language and compiler

```text
Primary language:
  C++23.

Primary compiler family:
  Clang/LLVM.

Windows:
  clang-cl.

Linux/macOS:
  clang++.

Web:
  Emscripten / em++ / WebAssembly.

Core runtime:
  never JavaScript.

Web target:
  C++/WASM + WebGPU/WGSL.
```

Clang supports `-std=c++23`, but individual C++23 features still need compiler/STL feature checks, so the build must detect features instead of assuming all C++23 library pieces are available everywhere. ([Clang][1])

## Build and presets

Use `CMakePresets.json` as the canonical build contract. CMake officially supports shared `CMakePresets.json` and local `CMakeUserPresets.json`, which is ideal for repeatable agent builds. ([CMake][2])

## Web target

Emscripten is an LLVM-based toolchain for compiling C/C++ to WebAssembly, so the web path should remain C++-first. ([Emscripten][3])

## WebGPU shader target

WebGPU uses WGSL for shaders. The WebGPU backend should therefore have a first-class WGSL path, not a JS shader toy path. ([W3C][4])

## Graphics API strategy

Primary graphics APIs:

```text
Vulkan
D3D12
Metal
WebGPU
CPU reference / CPU path tracer
```

Optional, only if explicitly useful:

```text
OpenGL 4.3+ compute fallback
```

But OpenGL should **not** become the main renderer. The plan’s serious paths are Vulkan/D3D12/Metal/WebGPU plus CPU.

## Hardware ray tracing strategy

Use two renderer families:

```text
Compute path:
  software BVH traversal in compute shaders.
  required baseline for Vulkan, D3D12, Metal, WebGPU.

Hardware RT path:
  optional fast path where supported.
  Vulkan RT, D3D12 DXR, Metal ray tracing.
```

Vulkan exposes ray tracing through extensions such as `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, and `VK_KHR_ray_query`; support must be checked at runtime. ([Vulkan Documentation][5])

D3D12 ray tracing is DXR, described by Microsoft as first-class ray tracing support in D3D12. ([Microsoft][6])

Metal also has ray tracing acceleration structure support. ([Apple Developer][7])

## CPU performance strategy

CPU path tracing is not just a reference path. It should have:

```text
scalar backend
SSE/SSE4 optional backend
AVX backend
AVX2 backend
AVX-512 backend
ARM NEON backend
ARM SVE backend
```

Intel’s Intrinsics Guide documents C-style intrinsic functions for instruction sets including AVX/AVX2/AVX-512, and Arm documents intrinsics for Neon, Helium, and SVE. ([Intel][8])

Arm SVE is vector-length agnostic, so SVE code should not assume a fixed 128/256/512-bit lane count. ([learn.arm.com][9])

## Debuggability strategy

The project must be designed for a human with little C++ experience who relies on agents. Therefore:

```text
Every subsystem logs lifecycle state.
Every crash writes a state snapshot.
Every backend failure explains what capability or resource failed.
Every benchmark emits machine-readable metadata.
Every agent task includes a test or validation command.
```

Use AddressSanitizer and ThreadSanitizer profiles early. Clang’s AddressSanitizer detects memory errors such as out-of-bounds and use-after-free, while ThreadSanitizer detects data races. ([Clang][10])

---

# 1. Six workstreams overview

```text
Agent A — Build, diagnostics, logging, crash recovery, agent runner tools.
Agent B — Core runtime, platform abstraction, ECS, jobs, threading, scene lifecycle.
Agent C — Renderer interface layer, GPU backends, shader system, frame graph.
Agent D — Path tracing core, CPU ray path, SIMD, BVH, integrator, film pipeline.
Agent E — Assets, materials, shaders, scene packs, editor/demo controls.
Agent F — Benchmarking, validation, CI, experiments, profiling, release gates.
```

## Chronological integration gates

All agents work in parallel, but merges must respect these gates:

```text
Gate 0: (completed)
  Repo config builds a hello executable with CMake presets.

Gate 1: (completed)
  App runs headless, emits logs, emits build metadata, exits cleanly.

Gate 2: (completed)
  Core ECS + scene schema can load a tiny scene and dump a validated snapshot.

Gate 3: (completed)
  CPU scalar path tracer renders a tiny Cornell scene to PNG/EXR.

Gate 4: (completed)
  Vulkan compute backend renders the same tiny scene.

Gate 5: (completed)
  Benchmark CLI runs CPU and Vulkan paths and writes results.json.

Gate 6: (completed)
  Multithreaded CPU renderer, parallel BVH, and job system are validated.

Gate 7: (completed)
  SIMD CPU backends and backend performance experiments are available.

Gate 8: (completed)
  D3D12, Metal, and WebGPU adapters compile behind capability flags.

Gate 9: (completed)
  Material/shader library, asset import, debug views, and editor-lite controls exist.

Gate 10: (completed)
  Release candidate benchmark scene pack runs with reproducible artifacts.
```

---

# 2. Shared interface layer all agents must obey

This is the critical “interface payer/layer” so the project can support multiple platforms, graphics APIs, CPU backends, and future extensions.

## 2.1 Required top-level interfaces

```text
IApplicationMode
IPlatform
IWindow
IInput
IFileSystem
ITimeSource
ILogSink
ICrashReporter
IJobSystem
ISceneLoader
ISceneRuntime
IEcsWorld
IRenderBackend
IRenderDevice
IRenderCommandContext
IShaderCompiler
IShaderCache
IFrameGraph
IPathTracer
IRayAccelerator
ICpuRayKernel
ISimdKernel
IAssetImporter
IMaterialRegistry
IBenchmarkRunner
IProfiler
```

## 2.2 Required handle types

Use opaque handles everywhere.

```text
EntityHandle
StableEntityId
AssetId
MaterialId
TextureHandle
BufferHandle
PipelineHandle
ShaderHandle
SceneHandle
BackendDeviceHandle
AccelerationHandle
BenchmarkRunId
JobHandle
```

No agent should expose Vulkan/D3D12/Metal/WebGPU native handles outside backend modules except through explicit debug-only inspection APIs.

## 2.3 Required capability structs

```text
PlatformCapabilities
CpuCapabilities
SimdCapabilities
RenderBackendCapabilities
RayTracingCapabilities
ShaderCapabilities
TextureFormatCapabilities
MemoryBudgetCapabilities
BenchmarkCapabilities
```

## 2.4 Required control structures

```text
EngineConfig
BuildInfo
RuntimeConfig
FeatureFlags
SceneDocument
SceneSnapshot
WorldCommandBuffer
FrameContext
FrameGraphDesc
RenderSceneProxy
PathTraceSettings
IntegratorSettings
FilmSettings
BenchmarkRunDesc
BenchmarkResult
CrashStateSnapshot
```

## 2.5 Required layering rule

```text
Application
  may depend on Core, Platform, Scene, Renderer, Benchmark, Editor.

Scene
  may depend on Core, Assets, Serialization.

Renderer
  may depend on Core, Assets, ShaderSystem.

PathTracer
  may depend on Renderer interfaces and Scene render proxy.

Backends
  may depend on Renderer interfaces, Core, Platform native surface hooks.

CPU ray path
  may depend on Core, Scene render proxy, PathTracer interfaces.

Benchmark
  may depend on Scene, PathTracer, Renderer, PlatformBase, Diagnostics.

Assets/materials
  may not depend on backend-native APIs.

Editor/demo controls
  may not be required for headless benchmark.
```

---

# 3. Agent A — Build, diagnostics, logging, crash recovery, agent tools

## Mission

Make the project buildable, diagnosable, crash-readable, and easy for agents to run repeatedly.

## Owns

```text
CMake
presets
feature flags
build metadata
logging
crash reporter
diagnostic schema
agent runner commands
sanitizer profiles
basic CLI shell
```

## Does not own

```text
renderer implementation
pathtracing kernels
asset import internals
materials
scene editing
```

---

## [ ] A15 — Add artifact directory policy

**Deliverable:** standardized output folder layout.

```text
/artifacts/runs/<timestamp>_<run_id>/
/artifacts/crashes/<timestamp>_<run_id>/
/artifacts/benchmarks/<scene>_<backend>_<timestamp>/
```

**Implementation hints:** all outputs should be discoverable without knowing C++.

**Acceptance:** no agent writes random logs into repo root.

---

## [ ] A17 — Add application mode contract

**Deliverable:** `IApplicationMode` interface.

Required responsibilities:

```text
mode name
configure from RuntimeConfig
initialize
tick
shutdown
describe capabilities
```

**Implementation hints:** headless benchmark and desktop/editor modes should share startup plumbing behind this contract.

**Acceptance:** `ptapp` can select at least two modes through one unified mode interface.

---

# 4. Agent B — Core runtime, platform abstraction, ECS, jobs, threading, scene lifecycle

## Mission

Build the deterministic runtime spine: platform abstraction, ECS, scene loading, command buffers, transform control, and proper multithreading.

## Owns

```text
core services
platform interfaces
job system
task graph
ECS
scene document
scene runtime
transform hierarchy
deterministic command buffers
resource lifetime model
```

## Does not own

```text
graphics backend internals
pathtracing math kernels
shader library
benchmark scoring
```

---

# 5. Agent C — Renderer interface layer, GPU backends, shader system, frame graph

## Mission

Create the renderer abstraction and graphics backend strategy that supports Vulkan, D3D12, Metal, WebGPU, optional OpenGL compute experiment, compute path tracing, and future hardware RT.

## Owns

```text
IRenderBackend
IRenderDevice
GPU resource handles
frame graph
shader compiler/cache
backend capabilities
Vulkan backend
D3D12 backend
Metal backend
WebGPU backend
hardware RT capability layer
GPU timing
```

## Does not own

```text
CPU raytracing kernels
material semantics
scene ECS
benchmark scoring
```

---

## [ ] C14 — Add Metal backend skeleton

**Deliverable:** Metal backend compiles on macOS behind flag.

**Implementation hints:** implement device discovery, command queue, compute dispatch stub.

**Acceptance:** macOS build compiles with `PT_ENABLE_METAL`.

---

## [ ] C15 — Add Metal ray tracing capability probe

**Deliverable:** Metal RT support check.

**Implementation hints:** log whether acceleration-structure path is available.

**Acceptance:** Metal capability dump includes RT availability.

---

## [ ] C16 — Add WebGPU backend skeleton

**Deliverable:** WebGPU backend compiles for Emscripten/WebAssembly.

**Implementation hints:** WGSL compute first. No hardware RT assumption.

**Acceptance:** web preset builds a minimal WebGPU init path or cleanly reports unsupported environment.

---

## [ ] C17 — Add WebGPU compute dispatch test

**Deliverable:** browser/WASM compute smoke test.

**Implementation hints:** write a known pattern to a storage buffer or texture, read back if feasible, otherwise present debug texture.

**Acceptance:** WebGPU backend passes a minimal compute validation.

---

# 6. Agent D — Path tracing core, CPU ray path, SIMD, BVH, integrator, film pipeline

## Mission

Build the actual path tracer: CPU scalar path, CPU SIMD paths, GPU-compatible data layout, BVH, integrator, materials evaluation interface, and film output.

## Owns

```text
pathtracer domain API
CPU ray tracing
SIMD dispatch layer
BVH
ray/triangle/SDF intersections
integrator
sampling
RNG
film accumulation
tone mapping
CPU/GPU shared data layout
```

## Does not own

```text
graphics API device creation
asset import parsing
editor UI
benchmark report formatting
```

---

## [ ] D29 — Add AVX SIMD kernel path coverage

**Deliverable:** explicit AVX (non-AVX2) SIMD kernel contract/implementation.

**Implementation hints:** keep the same packet interface as scalar/AVX2/AVX-512 paths; runtime dispatch should choose AVX when AVX2 is unavailable.

**Acceptance:** SIMD mode listing and benchmark experiments can exercise AVX separately from AVX2.

---

# 7. Agent E — Assets, materials, shaders, scene packs, editor/demo controls

## Mission

Build the content side: importers, material registry, shader families, scene packs, debug views, and editor-lite/demo controls.

## Owns

```text
asset registry
importer interfaces
glTF/OBJ import
texture decode policy
material registry
material descriptors
shader family declarations
scene presets
debug views
editor-lite panels
demo controls
```

## Does not own

```text
GPU device implementation
BVH internals
CPU SIMD kernels
benchmark scoring
```

---

## [ ] E13 — Add glTF/GLB importer MVP

**Deliverable:** glTF/GLB mesh/material/texture import.

**Implementation hints:** focus on static meshes, base color, normal, roughness, metallic, emissive.

**Acceptance:** imported glTF scene creates mesh/material assets and deterministic hashes.

---

## [ ] E20 — Add editor command descriptors

**Deliverable:** editor command model.

Commands:

```text
create primitive
delete entity
duplicate entity
rename entity
set transform
assign material
set light parameter
set camera parameter
save scene
load scene
run benchmark
export image
```

**Implementation hints:** commands must be undoable/replayable later.

**Acceptance:** command log can replay a tiny editing session.

---

# 8. Agent F — Benchmarking, validation, CI, experiments, profiling, release gates

## Mission

Make the project measurable, reproducible, CI-friendly, and experiment-driven. This agent decides which renderer path is fastest on each backend based on data.

## Owns

```text
benchmark CLI
result schema
artifact schema
reference image comparison
profiling
CI smoke tests
backend experiments
SIMD experiments
performance gates
regression policy
release checklist
```

## Does not own

```text
renderer backend internals
pathtracing kernel implementation
asset importer implementation
editor UI implementation
```

---

## [ ] F11 — Add Vulkan compute vs Vulkan RT experiment

**Deliverable:** Vulkan renderer path comparison.

Compare:

```text
Vulkan software BVH compute path
Vulkan hardware RT path, if supported
```

Scenes:

```text
Cornell
SDF Complexity
Sponza Lite
Material Gauntlet
```

**Implementation hints:** hardware RT may not help SDF-heavy scenes unless SDFs are converted or handled separately.

**Acceptance:** benchmark records fastest path per scene class.

---

## [ ] F12 — Add D3D12 compute vs DXR experiment

**Deliverable:** D3D12 renderer path comparison.

Compare:

```text
D3D12 compute software BVH
D3D12 DXR, if supported
```

**Implementation hints:** log ray tracing tier and unsupported reasons.

**Acceptance:** Windows benchmark can recommend compute or DXR path.

---

## [ ] F13 — Add Metal compute vs Metal RT experiment

**Deliverable:** Metal renderer path comparison.

Compare:

```text
Metal compute software BVH
Metal acceleration-structure ray tracing, if supported
```

**Implementation hints:** measure Apple GPU behavior separately from discrete GPU assumptions.

**Acceptance:** macOS benchmark can recommend compute or Metal RT path.

---

## [ ] F14 — Add WebGPU workgroup-size experiment

**Deliverable:** WebGPU compute tuning sweep.

Parameters:

```text
workgroup size
tile dimensions
samples per dispatch
buffer layout variant
```

**Implementation hints:** WebGPU path is WGSL compute; do not assume hardware RT.

**Acceptance:** web benchmark selects best safe workgroup profile.

---

# 9. Cross-workstream dependency map

## Gate 0 dependencies

```text
A01, A02, A03
```

Needed before all serious work.

## Gate 1 dependencies

```text
A04, A05, A14
B01, B02, B03
```

Headless app with logs.

## Gate 2 dependencies

```text
B09, B10, B11, B14, B15
E01, E04
```

Scene can load and snapshot.

## Gate 3 dependencies

```text
D01, D02, D03, D04, D05, D06, D07, D21
E06, E12, E17
F01, F02, F03
```

CPU scalar path can render.

## Gate 4 dependencies

```text
C01, C02, C03, C05, C08, C10
D25
```

Vulkan compute path can render.

## Gate 5 dependencies

```text
F04, F05, F06, F07
A15
```

Benchmark artifacts work.

## Gate 6 dependencies

```text
B06, B07, B08
D11, D12
F08
```

Proper multithreaded CPU path.

## Gate 7 dependencies

```text
D13, D14, D15, D16, D17, D18, D19, D20
F09, F10
```

SIMD and CPU tuning.

## Gate 8 dependencies

```text
C11, C12, C13, C14, C15, C16, C17
F11, F12, F13, F14
```

Backend experiments.

## Gate 9 dependencies

```text
E07, E08, E09, E10, E11, E18, E19, E20, E21
D22, D23, D24, D26
```

Material/shader/demo richness.

## Gate 10 dependencies

```text
F15, F16, F17, F18, F19, F20
A07, A08, A13
C20
```

Release candidate robustness.

---

# 10. Required first MVP path

The fastest clean route to a working prototype is:

```text
1. Build/logging/headless app.
2. Scene schema and ECS snapshot.
3. CPU scalar path tracer.
4. Benchmark CLI artifacts.
5. Vulkan compute backend.
6. Multithreaded CPU tiles.
7. SIMD experiments.
8. Backend compute/RT experiments.
9. Material/shader expansion.
10. Editor-lite/demo controls.
```

Do **not** start with D3D12, Metal, WebGPU, hardware RT, advanced materials, or editor UI before the CPU scalar and Vulkan compute paths exist.

---

# 11. Logging requirements every agent must follow

Every agent-owned subsystem must log:

```text
subsystem name
version/interface version
config
feature flags
startup begin
startup success/failure
shutdown begin
shutdown success/failure
resource counts
last operation
last error
```

Every frame must be able to report:

```text
frame index
mode
scene hash
backend
renderer path
active camera
active scene
current stage
jobs queued/running/completed
resources pending upload
last render pass
last shader variant
```

Every crash artifact must include:

```text
last successful subsystem
last successful frame stage
active backend
active renderer path
active scene
active shader
active material
active asset import, if any
active job, if known
thread ID
build info
feature flags
last 1024 log events
```

This is not optional. This is what lets Codex agents debug the project without you having deep C++ knowledge.

---

# 12. Performance requirements

## CPU

Required modes:

```text
scalar
multithreaded scalar
AVX
AVX2
AVX-512
NEON
SVE
```

Required benchmark selection policy:

```text
detect supported modes
run short calibration
select fastest stable mode
record selected mode
allow CLI override
```

Required CLI examples:

```text
--cpu-simd scalar
--cpu-simd avx2
--cpu-simd avx512
--cpu-simd neon
--cpu-simd sve
--cpu-simd auto
```

## GPU

Required renderer paths:

```text
gpu-compute
gpu-hardware-rt
cpu-scalar
cpu-threaded
cpu-simd
```

Required path selection policy:

```text
explicit CLI path wins
benchmark-selected default second
backend-recommended path third
safe compute fallback fourth
CPU fallback last
```

Required CLI examples:

```text
--renderer-path gpu-compute
--renderer-path gpu-hardware-rt
--renderer-path cpu-threaded
--renderer-path cpu-simd
--renderer-path auto
```

---

# 13. Final agent assignment summary

## Agent A should finish first

Because every other agent needs build, logs, crash artifacts, and status files.

Highest priority:

```text
A01-A07
A10-A16
```

## Agent B should work immediately after A01-A03

Because all runtime and scene systems depend on it.

Highest priority:

```text
B01-B03
B06-B08
B09-B16
B18
```

## Agent C can start once interfaces exist

Highest priority:

```text
C01-C08
C10
C19-C20
```

Then expand:

```text
C11-C18
```

## Agent D can start after B15/B16

Highest priority:

```text
D01-D12
D21-D26
```

Then optimize:

```text
D13-D20
```

## Agent E can start once schema/material descriptors exist

Highest priority:

```text
E01-E06
E10-E12
E17-E18
```

Then expand:

```text
E07-E09
E13-E16
E19-E21
```

## Agent F should start early but depends on A/B/D

Highest priority:

```text
F01-F07
F17
```

Then performance and experiments:

```text
F08-F16
F18-F20
```

---

# 14. What the agents should build first

The first working milestone should be:

```text
ptapp --headless --version
ptdoctor --check-build
ptbench run --scene assets/scenes/cornell_native.scene.json --backend cpu --renderer-path cpu-scalar --resolution 256x256 --spp 8 --output artifacts/benchmarks/smoke
```

Expected output:

```text
beauty.png
beauty.exr
results.json
metadata.json
scene_snapshot.json
asset_manifest.json
logs.jsonl
latest_status.json
```

The second working milestone should be:

```text
ptbench run --scene assets/scenes/cornell_native.scene.json --backend vulkan --renderer-path gpu-compute --resolution 512x512 --spp 32 --output artifacts/benchmarks/vulkan_smoke
```

The third working milestone should be:

```text
ptbench run-experiments --scene-pack core --include cpu-simd --include gpu-paths --output artifacts/benchmarks/experiments
```

That gives you a clean, agent-debuggable, high-performance C++ pathtracer foundation with CPU, SIMD, GPU compute, and future hardware RT paths all planned correctly.

[1]: https://clang.llvm.org/cxx_status.html?utm_source=chatgpt.com "Clang - C++ Programming Language Status - LLVM"
[2]: https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html?utm_source=chatgpt.com "cmake-presets(7) — CMake 4.3.2 Documentation"
[3]: https://emscripten.org/?utm_source=chatgpt.com "Main — Emscripten 5.0.8-git (dev) documentation"
[4]: https://www.w3.org/TR/WGSL/?utm_source=chatgpt.com "WebGPU Shading Language"
[5]: https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html?utm_source=chatgpt.com "Ray Tracing :: Vulkan Documentation Project"
[6]: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html?utm_source=chatgpt.com "DirectX Raytracing (DXR) Functional Spec"
[7]: https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures?utm_source=chatgpt.com "Ray tracing with acceleration structures"
[8]: https://www.intel.com/content/www/us/en/docs/intrinsics-guide/index.html?utm_source=chatgpt.com "Intel® Intrinsics Guide"
[9]: https://learn.arm.com/learning-paths/servers-and-cloud-computing/sve/sve_basics/?utm_source=chatgpt.com "From Arm Neon to SVE"
[10]: https://clang.llvm.org/docs/AddressSanitizer.html?utm_source=chatgpt.com "AddressSanitizer — Clang 23.0.0git documentation - LLVM"


Add this as **Workstream 7 / Agent G — UI, Editor, Interaction, and Human-Debuggable Control Surface**.

This workstream should sit on top of the existing plan’s commitments to platform window/input/file-dialog abstraction, UI panel event routing, scene graph editing, camera/light/material panels, script lifecycle hooks, editor command history, asset management, benchmark panels, and structured diagnostics.

The key rule:

```text
The UI never directly mutates engine state.

The UI reads immutable snapshots and emits commands:
  UI event -> Editor interaction model -> EditorCommand -> ECS/Scene command buffer -> deterministic apply phase.
```

That keeps the app cross-platform, multi-backend, script-safe, benchmark-safe, and debuggable by agents.

---

# 1. New workstream overview

```text
Agent G — UI / Editor / Interaction Workstream

Owns:
  standard app window
  menu bar
  docking/floating panels
  viewport interactions
  selection/highlight system
  transform gizmos
  inspector panel
  ECS hierarchy tree
  grouping and merging workflows
  file/project asset panel
  drag/drop workflows
  Lua script attachment UI
  benchmark panel UI
  normalized benchmark score display
  UI event logging
  UI crash-state snapshots
  layout persistence

Does not own:
  ECS internals
  renderer backend internals
  pathtracing kernels
  asset importer internals
  benchmark scoring backend implementation
  Lua runtime internals
```

Agent G integrates with:

```text
Agent A:
  logging, diagnostics, crash state, config, artifact directories.

Agent B:
  ECS, scene graph, command buffers, transform hierarchy, jobs.

Agent C:
  viewport rendering, overlays, selection buffers, backend-agnostic UI renderer.

Agent D:
  render/proxy bounding boxes, camera rays, object picking support, film state.

Agent E:
  assets, materials, scripts, primitives, models, textures, scene presets.

Agent F:
  benchmark run descriptors, benchmark results, normalized score model.
```

---

# 2. UI architecture rule

The UI should be split into five layers:

```text
1. Platform UI bridge
   Reads window/input/DPI/clipboard/file-dialog events.

2. UI renderer bridge
   Draws UI and overlays through backend-neutral renderer interfaces.

3. UI state model
   Stores panels, layouts, selection, active tool, active modal, drag/drop state.

4. Editor command layer
   Converts interactions into undoable/replayable commands.

5. Engine integration layer
   Applies validated commands to ECS/Scene/Assets/Benchmark through scheduled phases.
```

No UI code should directly depend on Vulkan, D3D12, Metal, WebGPU, or OpenGL. The UI renderer should target an interface such as:

```text
IUiRenderer
IViewportOverlayRenderer
IUiPlatformBridge
IEditorCommandSink
ISelectionService
IInspectorModelProvider
ISceneTreeModelProvider
IAssetBrowserModelProvider
IBenchmarkPanelModelProvider
```

---

# 3. Standard window layout

The default desktop layout should be:

```text
┌──────────────────────────────────────────────────────────────┐
│ Menu Bar                                                     │
├───────────────┬───────────────────────────────┬──────────────┤
│ Scene/ECS     │                               │ Inspector    │
│ Tree          │        Pathtraced Viewport    │ Properties   │
│               │                               │              │
├───────────────┤                               ├──────────────┤
│ File/Assets   │                               │ Benchmark    │
│ Browser       │                               │ /Profiler    │
├───────────────┴───────────────────────────────┴──────────────┤
│ Console / Logs / Status Bar                                  │
└──────────────────────────────────────────────────────────────┘
```

Panels must be:

```text
dockable
floatable
closable
collapsible
resizable
layout-persistent
keyboard-focus aware
DPI-scale aware
```

Required layout presets:

```text
Default
Benchmark
Material Authoring
Scripting
Asset Management
Debug/Profiler
Minimal Viewport
Fullscreen Viewport With Overlay
```

---

# 4. Required top-level menu bar

The menu bar should expose every important interaction path so a user does not need to memorize shortcuts.

## File

```text
New Scene
Open Scene
Open Recent
Save Scene
Save Scene As
Clone Scene
Import Asset
Export Image
Export EXR
Export Benchmark Artifacts
Export Scene Snapshot
Project Settings
Preferences
Reveal Artifacts Folder
Exit
```

## Edit

```text
Undo
Redo
Cut
Copy
Paste
Duplicate
Delete
Rename
Select All
Select None
Invert Selection
Group Selection
Ungroup Selection
Merge Selection
Split Merged Object
Reparent Selection
Reset Transform
Reset Material Override
Reset Camera
Editor Command History
Clear Undo History
```

## View

```text
Panels
  Scene Tree
  Inspector
  Asset Browser
  Material Editor
  Script Editor
  Benchmark Panel
  Performance Overlay
  Console
  Shader/Backend Diagnostics
  Debug Views

Layouts
  Default
  Benchmark
  Material Authoring
  Scripting
  Asset Management
  Debug
  Minimal Viewport
  Reset Layout

Overlays
  Selection Bounding Boxes
  Transform Gizmos
  Light Gizmos
  Camera Frustums
  Physics Colliders
  BVH Debug
  Grid
  Axes
  Object IDs
  Material IDs
  Sample Count
  Variance
  NaN/Inf Highlight

Fullscreen
DPI / UI Scale
```

## Create

```text
Empty Entity
Group Entity
Camera
Light
  Point
  Spot
  Directional
  Sphere Area
  Rectangle Area
  Disk Area
  Line Area
  Portal
  Environment

Primitive Mesh
  Cube
  Sphere
  Plane
  Cylinder
  Cone
  Torus

SDF Primitive
  Sphere
  Box
  Rounded Box
  Cylinder
  Capsule
  Torus
  Plane
  Fractal
  CSG Group

Material
Script
Physics Body
Benchmark Marker
```

## Scene

```text
Validate Scene
Freeze Benchmark Snapshot
Reset Accumulation
Reload Scene
Reload Assets
Hot Reload Scripts
Scene Settings
Lighting Settings
Environment Settings
Camera Settings
Physics Settings
Script Settings
Animation Settings
```

## Render

```text
Backend
  Auto
  CPU
  Vulkan
  D3D12
  Metal
  WebGPU
  OpenGL Experimental, if enabled

Renderer Path
  Auto
  CPU Scalar
  CPU Threaded
  CPU SIMD
  GPU Compute
  GPU Hardware RT

Quality Presets
  Draft
  Preview
  Balanced
  High
  Reference

Resolution
Samples Per Pixel
Max Bounces
Denoiser
Tone Mapping
Exposure
Debug Channel
Shader Cache
Backend Capabilities
```

## Benchmark

```text
Run Current Scene
Run Scene Pack
Run CPU Calibration
Run GPU Calibration
Run SIMD Experiment
Run Backend Experiment
Compare Against Reference
Open Benchmark Artifacts
Export CSV/JSON
Normalized Score Settings
Benchmark History
```

## Assets

```text
Import Files
Reimport Selected
Refresh Asset Browser
Show Missing Assets
Show Import Diagnostics
Clear Generated Cache
Clear Shader Cache
Material Library
Texture Library
Model Library
Script Library
Primitive Library
```

## Scripts

```text
New Lua Script
Attach Script To Selection
Detach Script From Selection
Reload Scripts
Open Script Folder
Show Script Lifecycle Events
Show Script Errors
Show Script Profiler
Sandbox Settings
```

## Tools

```text
Doctor
Crash Artifacts
Profiler
Frame Capture
Shader Manifest
Asset Manifest
Scene Snapshot
Capability Matrix
Settings Dump
Run Startup Self-Test
```

## Help

```text
Controls
Shortcut Reference
About
Build Info
Feature Flags
Dependency Info
```

---

# 5. Interaction graph

Think of the editor as an interaction graph. Every node below needs an owner, state model, command path, logging, and crash snapshot visibility.

```text
Window Events
  -> UI Platform Bridge
  -> UI Event Router
  -> Active Panel / Viewport / Menu

Viewport Input
  -> Camera Controller
  -> Object Picking
  -> Selection Service
  -> Gizmo Controller
  -> Editor Command Stack
  -> ECS Command Buffer

Scene Tree Input
  -> Selection Service
  -> Hierarchy Commands
  -> Group/Merge Commands
  -> ECS Command Buffer

Inspector Input
  -> Property Binding Model
  -> Validation
  -> Editor Command Stack
  -> ECS/Material/Light/Camera Commands

Asset Browser Input
  -> Asset Selection
  -> Drag/Drop Service
  -> Import Validation
  -> Asset Registry Commands
  -> Scene Commands

File Drop Input
  -> File Type Detector
  -> Importer Registry
  -> Import Job Queue
  -> Asset Registry
  -> UI Notification

Script Panel Input
  -> Script Binding Model
  -> Lua Lifecycle Metadata
  -> Script Reload Commands
  -> Script Diagnostics

Benchmark Panel Input
  -> Benchmark Run Descriptor
  -> Benchmark Runner
  -> Result Model
  -> Normalized Score Model
  -> Artifact Browser

Renderer Overlay
  -> Selection Bounding Boxes
  -> Gizmo Draw
  -> Light/Camera/Physics Debug Draw
  -> Debug View Display

Logging / Crash Recorder
  <- every interaction node
```

---

# 6. Required UI data models

Agent G should define these models before building panels.

```text
UiRuntimeState
  current layout
  active panel
  focused widget
  active modal
  active drag/drop operation
  current viewport tool
  current transform mode
  current selection mode

SelectionState
  selected entity IDs
  active entity ID
  hovered entity ID
  selection source
  selection timestamp/frame
  selection bounding boxes
  group membership

SceneTreeModel
  visible nodes
  expanded/collapsed state
  sibling order
  parent/child relationships
  icons/status badges
  script/physics/material/light/camera markers

InspectorModel
  selected object type
  common properties
  mixed-value properties
  editable fields
  validation warnings
  pending edits
  committed edits

AssetBrowserModel
  asset categories
  files
  generated assets
  missing assets
  import diagnostics
  thumbnails/previews
  drag/drop capabilities

BenchmarkPanelModel
  current scene
  selected backend
  selected renderer path
  raw metrics
  normalized score
  calibration status
  artifact links
  result history

UiLayoutDocument
  panel positions
  dock/floating state
  sizes
  active layout preset
  DPI scale
```

---

# 7. UI logging and crash-state requirements

Because the human operator has little C++ experience and depends on agents, this workstream must be unusually observable.

Every UI interaction should log:

```text
event type
panel ID
widget ID
entity ID or asset ID
old value
new value
command emitted
validation result
frame index
thread ID
```

The crash snapshot must include:

```text
active layout
active panel
active modal
active drag/drop operation
active gizmo
hovered entity
selected entities
active entity
last clicked entity
last menu action
last inspector property edit
last scene tree operation
last file drop path
last benchmark command
last 256 UI events
last 256 editor commands
panel visibility state
tree expanded/collapsed state
current viewport camera
current renderer debug channel
```

Required files:

```text
artifacts/crashes/<run>/ui_state.json
artifacts/crashes/<run>/ui_events.jsonl
artifacts/crashes/<run>/editor_commands.jsonl
artifacts/crashes/<run>/selection_state.json
artifacts/crashes/<run>/layout_state.json
```

---

# 8. Workstream 7 todos

## [ ] G17 — Add viewport panel

**Deliverable:** central render viewport panel.

Must display:

```text
pathtraced image
render resolution
current backend
renderer path
SPP accumulated
camera mode
debug view
selection overlay
gizmo overlay
```

**Implementation hints:** viewport should consume renderer output through an abstract texture/display handle.

**Acceptance:** viewport can show placeholder texture before renderer integration.

---

## [ ] G18 — Add viewport camera controls

**Deliverable:** interactive camera controller.

Modes:

```text
orbit
FPS
turntable
benchmark path preview
```

Controls:

```text
pan
orbit
dolly
walk
reset view
focus selected
frame all
```

**Implementation hints:** camera changes emit commands and trigger accumulation reset.

**Acceptance:** camera interaction logs old/new camera state.

---

## [ ] G20 — Add hover highlight

**Deliverable:** hover state and visual highlight.

Visuals:

```text
hover outline
hover bounding box
hover entity name tooltip
scene tree row hover sync
```

**Implementation hints:** hover should not change selection.

**Acceptance:** moving cursor over entity updates `hovered_entity_id`.

---

## [ ] G21 — Add selection highlight with bounding boxes

**Deliverable:** selected object bounding boxes.

Requirements:

```text
single selected entity bounding box
multi-selected per-entity bounding boxes
aggregate multi-selection bounding box
active primary entity distinct marker
tree row selection highlight
inspector target highlight
```

**Implementation hints:** bounding boxes should come from render proxy or ECS bounds cache.

**Acceptance:** selecting from viewport or tree produces matching bounding boxes.

---

## [ ] G22 — Add marquee/box selection

**Deliverable:** drag rectangle selection in viewport.

Modes:

```text
replace selection
add to selection
remove from selection
toggle selection
```

**Implementation hints:** support Ctrl/Shift modifiers consistently with tree selection.

**Acceptance:** marquee selection updates `SelectionState` and logs selected IDs.

---

## [ ] G24 — Add transform gizmo framework

**Deliverable:** gizmo controller and tool modes.

Modes:

```text
translate
rotate
scale
universal
disabled
```

Spaces:

```text
local
world
parent
camera/view
```

Pivots:

```text
individual origins
active entity
selection center
bounding-box center
group origin
```

**Implementation hints:** gizmos emit grouped editor commands during drag and one committed command at release.

**Acceptance:** transform drag can be undone in one undo operation.

---

## [ ] G25 — Add translation gizmo

**Deliverable:** move handles.

Required handles:

```text
X axis
Y axis
Z axis
XY plane
YZ plane
XZ plane
free move, optional
```

**Implementation hints:** support snapping and numeric delta display later.

**Acceptance:** selected entity moves and accumulation resets.

---

## [ ] G26 — Add rotation gizmo

**Deliverable:** rotation handles.

Required handles:

```text
X ring
Y ring
Z ring
screen-space ring
```

**Implementation hints:** store rotation edits in a stable transform representation; avoid silently changing parent/child transforms incorrectly.

**Acceptance:** rotating selected entity updates inspector and scene tree state.

---

## [ ] G27 — Add scale gizmo

**Deliverable:** scale handles.

Required handles:

```text
uniform scale
X scale
Y scale
Z scale
plane scale, optional
```

**Implementation hints:** support negative-scale warnings if renderer/physics/import pipeline cannot handle them safely.

**Acceptance:** scale edits are undoable and logged.

---

## [ ] G28 — Add transform snapping controls

**Deliverable:** snapping model.

Controls:

```text
grid translation snap
angle snap
scale snap
toggle snapping
snap step settings
```

**Implementation hints:** expose snapping in toolbar and inspector.

**Acceptance:** snapping setting persists in UI config.

---

## [ ] G29 — Add multi-object transform behavior

**Deliverable:** multi-selection transform policy.

Modes:

```text
transform as group
transform around active entity
transform each around own origin
transform around selection bounds center
```

**Implementation hints:** command must store per-entity before/after transforms.

**Acceptance:** multi-object transform can be undone exactly.

---

## [ ] G30 — Add inspector panel shell

**Deliverable:** inspector panel with selected object summary.

Must show:

```text
selected count
active entity
entity name
entity ID
component list
warnings
mixed-value state for multi-select
```

**Implementation hints:** inspector reads `InspectorModel`, not ECS directly.

**Acceptance:** selecting entities updates inspector.

---

## [ ] G31 — Add transform inspector

**Deliverable:** numeric transform editor.

Controls:

```text
position XYZ numeric inputs
rotation numeric inputs
scale XYZ numeric inputs
local/world toggle
reset buttons
copy/paste transform
```

**Implementation hints:** editing numeric fields emits commands with validation.

**Acceptance:** changing a value updates viewport and can be undone.

---

## [ ] G32 — Add material inspector

**Deliverable:** material property editor.

Controls:

```text
material assignment picker
base color
roughness
metallic
IOR
transmission
emission
normal map
alpha mode
clearcoat
anisotropy
shader/material family selector
texture slot pickers
```

**Implementation hints:** multi-select should show mixed values and allow batch assignment.

**Acceptance:** assigning material to selected entities updates render scene proxy.

---

## [ ] G33 — Add light inspector

**Deliverable:** light property editor.

Controls:

```text
light type
intensity
unit
color
color temperature
radius/size
spot angle
softness
visibility toggles
sampling weight
```

**Implementation hints:** intensity units should show conversion notes if photometric support is partial.

**Acceptance:** changing light parameters resets accumulation and logs command.

---

## [ ] G34 — Add camera inspector

**Deliverable:** camera property editor.

Controls:

```text
FOV
focal length
sensor size
aperture
focus distance
ISO
shutter
f-stop
white balance
exposure compensation
motion blur toggle
set active camera
```

**Implementation hints:** camera inspector must update viewport camera when active camera is selected.

**Acceptance:** camera edits update rendered image/camera overlay.

---

## [ ] G35 — Add component inspector framework

**Deliverable:** generic component editor.

Component sections:

```text
Transform
Mesh Renderer
SDF Primitive
Material Override
Light
Camera
Physics Body
Script
Animation
Benchmark Tag
Metadata
```

**Implementation hints:** component editors should be registered by module so future components do not require hardcoded UI rewrites.

**Acceptance:** unknown component types display safe read-only metadata.

---

## [ ] G36 — Add multi-selection inspector

**Deliverable:** multi-object property editor.

Behavior:

```text
common properties shown
different values show “mixed”
editing a mixed field applies new value to all selected
batch material assignment
batch visibility toggle
batch transform mode
batch script attachment
```

**Implementation hints:** avoid applying invalid component changes to objects that do not support them.

**Acceptance:** batch edit logs affected entity IDs and skipped entity IDs.

---

## [ ] G40 — Add grouping workflow

**Deliverable:** non-destructive group operation.

Behavior:

```text
Ctrl-click adds items to selection
Group Selection creates parent group entity
selected items become children
group has aggregate bounds
group transform affects children
ungroup restores children
```

**Implementation hints:** group command must preserve world transforms when creating parent.

**Acceptance:** group/ungroup round trip preserves object transforms.

---

## [ ] G41 — Add merge workflow

**Deliverable:** explicit merge operation.

Merge types:

```text
mesh merge
SDF CSG union
SDF smooth union
material-compatible batch merge
compound physics shape merge, optional
```

**Implementation hints:** merging should be destructive only after confirmation. Prefer creating a generated asset plus retaining source references for undo.

**Acceptance:** Merge Selection produces a generated merged entity and can be undone.

---

## [ ] G42 — Add split merged object workflow

**Deliverable:** undo/split interface for merged results.

**Implementation hints:** split should restore original children when merge metadata exists. If metadata is missing, show warning.

**Acceptance:** merged object can be split back into originals in test scenes.

---

## [ ] G43 — Add asset/file management panel shell

**Deliverable:** project file browser.

Categories:

```text
Scenes
Materials
Scripts
Primitives
Models
Textures
Animations
Benchmarks
Generated Assets
Missing Assets
Imports
```

**Implementation hints:** use virtual project paths, not raw OS paths everywhere.

**Acceptance:** panel lists fixture assets and category filters.

---

## [ ] G44 — Add asset search/filter/sort

**Deliverable:** asset browser filtering.

Controls:

```text
search box
type filter
status filter
sort by name
sort by type
sort by modified time
sort by import status
sort by size
```

**Implementation hints:** large asset lists should be generated from background job snapshots, not per-frame filesystem scans.

**Acceptance:** filtering does not stall viewport.

---

## [ ] G45 — Add asset preview cards

**Deliverable:** preview cards for assets.

Previews:

```text
material swatch
texture thumbnail
model thumbnail placeholder
script icon
scene icon
primitive icon
benchmark scene icon
missing asset warning
```

**Implementation hints:** thumbnail generation can be async and optional.

**Acceptance:** asset browser remains usable if thumbnails are unavailable.

---

## [ ] G46 — Add drag/drop from asset browser to viewport

**Deliverable:** asset-to-scene drag/drop.

Rules:

```text
model dropped into viewport creates model instance
material dropped onto entity assigns material
texture dropped onto material slot assigns texture
script dropped onto entity attaches script
primitive dropped into viewport creates primitive
benchmark scene dropped into benchmark panel selects scene
```

**Implementation hints:** drag payload must include asset ID, asset type, and allowed target types.

**Acceptance:** invalid drops are rejected with clear UI feedback.

---

## [ ] G47 — Add OS file drag/drop

**Deliverable:** external file drop support.

Supported drop targets:

```text
viewport
asset browser
material texture slots
script picker
scene open area
benchmark reference image picker
```

**Implementation hints:** validate by extension, magic/header where possible, and importer registry support.

**Acceptance:** dropping unsupported file type shows rejection reason.

---

## [ ] G48 — Add file picker controls

**Deliverable:** reusable file picker widget.

Features:

```text
browse button
drag/drop target
accepted file types
current file path
validation state
clear button
open containing folder
```

**Implementation hints:** every file picker should use importer/file-type validation, not only extension strings.

**Acceptance:** material texture picker rejects non-texture files.

---

## [ ] G49 — Add import validation modal

**Deliverable:** import confirmation/diagnostics modal.

Shows:

```text
detected file type
selected importer
asset category
lossy conversion notes
unsupported fields
target path
import options
cancel/import buttons
```

**Implementation hints:** should support batch imports.

**Acceptance:** importing invalid file never crashes; it reports reason.

---

## [ ] G50 — Add script panel

**Deliverable:** Lua script management panel.

Features:

```text
attached scripts for selected entity
available scripts
new script
attach
detach
reload
open script file
sandbox status
script parameters
script errors
script event log
```

**Implementation hints:** Script panel should operate through scripting module interfaces and editor commands.

**Acceptance:** selected entity can attach/detach script through UI command.

---

## [ ] G51 — Add Lua lifecycle visibility

**Deliverable:** script lifecycle view.

Display lifecycle hooks:

```text
on_load
on_spawn
on_enable
on_disable
on_update
on_fixed_update
on_late_update
on_collision
on_trigger
on_animation_event
on_animation_loop
on_keyframe_reached
on_destroy
on_unload
```

**Implementation hints:** show which hooks are implemented by the selected script and last time they fired.

**Acceptance:** script lifecycle panel updates from script diagnostics.

---

## [ ] G52 — Add script parameter inspector

**Deliverable:** script-exposed parameter UI.

Supported parameter controls:

```text
number input
slider
boolean toggle
string input
vector input
color input
entity picker
asset picker
enum/dropdown
```

**Implementation hints:** parameters must be typed and validated before command emission.

**Acceptance:** editing script parameter emits command and logs old/new value.

---

## [ ] G58 — Add benchmark history panel

**Deliverable:** history list of recent runs.

Display:

```text
scene
backend
renderer path
score
raw throughput
date/time
artifacts folder
regression/improvement marker
```

**Implementation hints:** load from benchmark artifacts, not transient UI memory.

**Acceptance:** restarting app still shows benchmark history.

---

## [ ] G59 — Add console/log panel

**Deliverable:** in-app log viewer.

Features:

```text
filter by severity
filter by subsystem
search
copy selected logs
open crash artifacts
clear view
pause auto-scroll
```

**Implementation hints:** consume Agent A log ring buffer.

**Acceptance:** UI errors and editor commands appear in log panel.

---

## [ ] G62 — Add editor command history panel

**Deliverable:** visual undo/redo history.

Shows:

```text
command name
target entity/asset
timestamp/frame
grouped transaction
undoable state
redoable state
```

**Implementation hints:** useful for agents diagnosing “what action broke the app.”

**Acceptance:** clicking a command shows details.

---

## [ ] G63 — Add modal and notification system

**Deliverable:** standard UI modals/toasts.

Required types:

```text
confirmation modal
blocking error modal
non-blocking warning toast
import result toast
benchmark complete toast
script error toast
crash recovery prompt
```

**Implementation hints:** modal state must be serialized in crash snapshots.

**Acceptance:** failed import produces visible warning and log event.

---

## [ ] G65 — Add UI accessibility/scaling controls

**Deliverable:** UI scaling and readability controls.

Controls:

```text
UI scale
DPI scale display
large text mode
high-contrast selection outline option
reduced motion option
tooltip delay
```

**Implementation hints:** avoid relying on color alone for selected/hover states.

**Acceptance:** UI remains usable under different scaling.

---

## [ ] G66 — Add web UI compatibility plan

**Deliverable:** web-compatible UI mode.

Constraints:

```text
browser canvas event binding
no native OS menu bar
menu rendered inside canvas/app
reduced file dialog behavior
drag/drop support through browser file events
limited filesystem persistence
WebGPU backend capability warnings
```

**Implementation hints:** share models with desktop UI; only platform bridge changes.

**Acceptance:** Web build can show menu/panels inside canvas.

---

## [ ] G67 — Add UI performance budget

**Deliverable:** UI profiling metrics.

Metrics:

```text
UI frame time
panel build time
scene tree build time
inspector build time
asset browser build time
thumbnail job time
event routing time
overlay draw time
```

**Implementation hints:** UI should not stall pathtracing accumulation.

**Acceptance:** benchmark mode can hide or reduce UI overhead.

---

## [ ] G68 — Add UI-thread policy

**Deliverable:** thread ownership policy.

Rules:

```text
UI state lives on main/UI thread.
Background jobs may produce immutable snapshots.
Asset scans/imports run on job system.
UI applies job results only through main-thread event queue.
No background thread mutates UI state directly.
```

**Implementation hints:** log thread violations as fatal in debug.

**Acceptance:** ThreadSanitizer-friendly design.

---

# 9. Benchmark score design

The benchmark panel should show a **hardware-normalized efficiency score**, not just FPS.

The user-facing promise:

```text
Changing settings like SPP, resolution, bounces, or rays per pixel should
change raw cost, but the normalized score should remain mostly stable if
the renderer is performing efficiently relative to the hardware.
```

The panel should show three numbers:

```text
Raw Performance:
  what actually happened.
  Example: FPS, samples/sec, paths/sec, GPU time.

Workload Cost:
  how expensive the selected settings are.
  Example: normalized workload units.

Hardware-Normalized Score:
  measured throughput divided by expected throughput for this device,
  backend, renderer path, and workload class.
```

Required score fields:

```text
normalized_score
raw_samples_per_second
raw_paths_per_second
raw_gpu_ms
raw_cpu_ms
workload_units
expected_units_per_second
measured_units_per_second
calibration_profile_id
score_confidence
calibration_valid
```

The score should be invalidated when any of these change:

```text
GPU/CPU device
driver/runtime
backend
renderer path
build flags
shader hash
major scene class
major material class
major integrator settings
SIMD mode
hardware RT availability
```

The UI must never imply high SPP is “free.” It should show:

```text
Raw FPS decreased because workload increased.
Normalized efficiency score remained stable because measured throughput
matches expected hardware-scaled performance.
```

---

# 10. Interaction surface coverage matrix

| Surface               | Required behavior                                  | Covered by todos |
| --------------------- | -------------------------------------------------- | ---------------- |
| Standard window       | Resizable app shell, status bar, root layout       | G05, G60         |
| Menu bar              | All app operations reachable from menus            | G06–G13          |
| Floating panels       | Dock, float, close, resize, persist                | G14–G16          |
| Viewport              | Render output, camera controls, overlays           | G17–G18          |
| Click selection       | Pick entity, update selection/tree/inspector       | G19–G23          |
| Bounding boxes        | Hover/selected/entity/group bounds                 | G20–G21          |
| Gizmos                | Translate, rotate, scale, snapping, multi-object   | G24–G29          |
| Inspector             | Numeric/sliders/toggles/property edits             | G30–G36          |
| Multi-select          | Ctrl-click, Shift range, mixed values, batch edits | G22–G23, G36     |
| Grouping              | Ctrl-click selection, group/ungroup                | G40              |
| Merging               | Mesh/SDF/compound merge and split                  | G41–G42          |
| ECS tree              | Parent/child/sibling hierarchy                     | G37–G39          |
| Drag reorder          | Reparent and sibling reorder                       | G39              |
| Script hooks          | Attach scripts, lifecycle visibility, params       | G50–G52          |
| File panel            | Materials/scripts/primitives/models/textures       | G43–G45          |
| File drag/drop        | Drop assets/files onto viewport/pickers            | G46–G49          |
| Benchmark panel       | Run benchmark, show raw + normalized score         | G53–G58          |
| Logging               | Event logs, command logs, crash UI state           | G59–G62          |
| Human debugging       | Status, modals, diagnostics, crash artifacts       | G60–G63          |
| Multithreading safety | UI thread policy, async snapshots                  | G67–G69          |
| Web compatibility     | Canvas UI, browser drag/drop constraints           | G66              |

---

# 11. Updated integration gates

Add these to the previous project gates.

## UI Gate 1 — Window and menu shell

Depends on:

```text
A01-A07
B01-B05
G01-G08
```

Pass condition:

```text
Window opens, menu bar works, UI state logs and serializes.
```

---

## UI Gate 2 — Docking panels and layout persistence

Depends on:

```text
G09-G16
A10
```

Pass condition:

```text
Panels dock/float/close, layout survives restart, crash dump includes layout.
```

---

## UI Gate 3 — Scene tree and selection

Depends on:

```text
B09-B16
D02 or bounds placeholder
G17-G23
G37-G39
```

Pass condition:

```text
Clicking viewport or tree selects entity, shows bounding box, inspector updates.
```

---

## UI Gate 4 — Gizmos and inspector editing

Depends on:

```text
B11-B13
G24-G36
```

Pass condition:

```text
Translate/rotate/scale/numeric edits work through undoable commands.
```

---

## UI Gate 5 — Grouping, merging, hierarchy editing

Depends on:

```text
B12-B15
G38-G42
```

Pass condition:

```text
Group, ungroup, merge, split, reparent, and sibling reorder work and are undoable.
```

---

## UI Gate 6 — Asset browser and drag/drop

Depends on:

```text
E01-E16
G43-G49
```

Pass condition:

```text
Materials/scripts/primitives/models/textures appear in file panel; valid drops import/assign; invalid drops explain why.
```

---

## UI Gate 7 — Lua script UI

Depends on:

```text
B ECS script component
E script asset model
Lua scripting module
G50-G52
```

Pass condition:

```text
User can attach script, view lifecycle hooks, edit exposed params, and see script errors.
```

---

## UI Gate 8 — Benchmark panel

Depends on:

```text
F01-F08
F benchmark result schema
G53-G58
```

Pass condition:

```text
Benchmark panel can run a benchmark descriptor, display raw metrics, display normalized hardware-relative score, and open artifacts.
```

---

## UI Gate 9 — Human-debuggable UI

Depends on:

```text
A07-A16
G59-G70
```

Pass condition:

```text
A crash after any UI action produces useful UI state, last events, command history, and selected object data.
```

---

# 12. First UI milestone target

The first UI milestone should not wait for the full renderer.

Target command:

```text
ptapp --scene assets/scenes/cornell_native.scene.json --backend null
```

Expected behavior:

```text
standard window opens
menu bar appears
default layout appears
scene tree shows entities
viewport shows placeholder render target
clicking tree selects item
selected item gets inspector data
selected item has placeholder bounding box
logs panel shows UI events
layout saves on exit
```

Second milestone:

```text
ptapp --scene assets/scenes/cornell_native.scene.json --backend cpu --renderer-path cpu-scalar
```

Expected behavior:

```text
viewport shows CPU render
clicking object selects it
bounding box appears
translate/rotate/scale gizmos emit commands
inspector edits update render state
asset browser lists materials/scripts/models/textures
benchmark panel can run CPU scalar benchmark
```

Third milestone:

```text
ptapp --scene assets/scenes/cornell_native.scene.json --backend vulkan --renderer-path gpu-compute
```

Expected behavior:

```text
viewport shows Vulkan render
selection overlay works
benchmark panel shows raw metrics
normalized hardware score appears after calibration
UI remains responsive during accumulation
logs and crash snapshots remain useful
```

---

# 13. Final UI workstream statement

Use this as the top of the UI/editor plan:

> Build a standard cross-platform editor UI for the C++23 pathtracer with a menu bar, dockable/floating panels, viewport selection, bounding-box highlighting, translate/rotate/scale gizmos, an inspector with numeric/slider/toggle controls, ECS hierarchy tree editing, multi-selection, grouping, merging, drag/drop asset management, Lua script lifecycle controls, and a benchmark panel that separates raw performance from hardware-normalized efficiency. The UI must be command-driven, backend-agnostic, multithread-safe, layout-persistent, and fully logged so agents can diagnose crashes and user actions without requiring the human operator to understand C++ internals.
---

# 15. Audit reconciliation backlog

Audit date: 2026-05-05.

This section records gaps found while reconciling checked todos against the current implementation. Items above were re-opened when the implementation was model-only, documented-only, partially wired, or missing acceptance coverage.

## 15.1 Audit hardening todos

- [ ] AUD01 - Add a repeatable todo-audit command that lists checked todo IDs, required evidence, and validation commands.
- [ ] AUD02 - Add a rule that a todo can only be checked when its acceptance command or source evidence is documented beside the todo.
- [ ] AUD03 - Add CI coverage for the smoke command, release gate, scene validation pack, and UI model smoke path.

## 15.2 Cross-workstream gap todos

- [ ] AUD05 - Add the missing shared contracts from the master interface list: `IApplicationMode`, `ICrashReporter`, `EngineConfig`, `FeatureFlags`, `RenderSceneProxy`, `FrameContext`, `FrameGraphDesc`, `IBenchmarkRunner`, `IProfiler`, `IMaterialRegistry`, `IRayAccelerator`, and `ISimdKernel`.
- [ ] AUD06 - Add validation tests for crash artifact file names and contents, including logs, runtime config, frame state, resource state, backend state, and scene state.
- [ ] AUD10 - Add real backend adapter coverage for Metal, WebGPU/WGSL, OpenGL experimental gating, and backend factory registration.
- [ ] AUD11 - Add real asset/importer infrastructure for asset IDs, importer registry, glTF/GLB, OBJ/MTL, PNG/JPEG texture decoding, and EXR support policy.
- [ ] AUD12 - Add benchmark experiment implementations for D3D12/DXR, Metal, WebGPU, Vulkan RT, GPU memory pressure, shader variants, and hardware-normalized scoring.
- [ ] AUD13 - Replace model-only UI todos with actual docked/floating panels, viewport picking, hover/selection overlays, transform gizmos, inspectors, asset browser, script panel, benchmark panel, log panel, status bar, modals, shortcuts, and accessibility controls.
- [ ] AUD15 - Add Web UI compatibility implementation or explicitly defer Web UI claims until a browser canvas/WebGPU build exists.

---

# 16. GPU-resident tessellation backlog

Goal: make low-poly authored meshes, especially the showcase sphere mesh, render as smooth path-traced geometry without paying tessellation cost every frame. Generated geometry must be cached by source mesh, tessellation settings, displacement inputs, and transform policy, then reused until one of those inputs changes.

## [ ] TESS01 - Add scene-level tessellation metadata for mesh geometry

**Deliverable:** Each `geometry` entry can declare cached GPU tessellation settings such as `enabled`, `mode`, `factor`, `gpu`, `cache`, `projection`, and `displacement`.

**Implementation hints:** Treat missing metadata as off. Clamp or reject unsafe factors. The material showcase sphere mesh should opt in with a uniform factor plus `projection: sphere` so generated points land on the sphere surface instead of staying on flat source-triangle planes.

**Acceptance:** `SceneDocument::load_from_file` preserves tessellation metadata through load/serialize and scene hashes change when tessellation settings change.

## [ ] TESS02 - Add RT scene tessellation requests and cache keys

**Deliverable:** `BuildSceneDataFromDocument` emits compact tessellation requests that identify source triangle ranges, requested output sizes, and a stable cache key.

**Implementation hints:** Start with uniform triangle subdivision and duplicated edge vertices. The key must include source geometry, tess factor, projection mode, displacement state, and transform policy. It must not include camera state.

**Acceptance:** Loading `material_shader_physics_showcase.json` reports at least one tessellation request, and changing only the camera does not change the generated-geometry cache key.

## [ ] TESS03 - Add D3D12 compute tessellation shader

**Deliverable:** Add an HLSL compute shader that takes source vertices/indices plus a uniform factor/projection mode and writes generated vertex/index buffers at deterministic offsets.

**Implementation hints:** Avoid append buffers for the first version. Pre-size buffers on the CPU, use one source-triangle work item/group, and write each source triangle into a known output slice. Duplicate edge vertices until the pipeline is proven.

**Acceptance:** A GPU smoke path can tessellate a small triangle mesh and read back the expected generated vertex/index counts.

## [ ] TESS04 - Feed generated buffers into D3D12 DXR BLAS

**Deliverable:** D3D12 uses cached generated geometry buffers as BLAS input when tessellation is enabled.

**Implementation hints:** Keep source mesh buffers, generated mesh buffers, BLAS scratch/result, film, and tonemap output GPU-resident. Rebuild BLAS only when the generated-geometry cache key changes. Prefer BLAS refit/update when only supported transform inputs change.

**Acceptance:** Re-rendering a static tessellated scene for additional samples does not redispatch tessellation or rebuild BLAS.

## [ ] TESS05 - Add GPU BVH path for non-DXR compute backends

**Deliverable:** The compute path tracer can trace against GPU-generated tessellated buffers without CPU BVH rebuild/readback.

**Implementation hints:** Start with LBVH/Morton build or another simple GPU builder before optimizing SAH. Keep CPU BVH as fallback.

**Acceptance:** D3D12/Vulkan compute can trace tessellated geometry with no geometry readback in the hot path.

## [ ] TESS06 - Add adaptive factors and displacement after the fixed path is stable

**Deliverable:** Tessellation factors can be derived from screen size, curvature, displacement scale, or user preset while still respecting the frame timing budget.

**Implementation hints:** Adaptive tessellation must still resolve to explicit output buffer sizes before dispatch. Budget logic should degrade factors before stealing time from ray generation.

**Acceptance:** A high-performance preset produces smooth spheres while maintaining the configured polygon frame budget and rays-per-second target.

---

# 17. Lua ECS scripting backlog

Goal: let scene entities carry Lua scripts as ECS components while preserving deterministic scheduling, command-buffer mutation, transform-authority diagnostics, benchmark reproducibility, and renderer/backend isolation. Scripts should make entity behavior easy to author, but they must not become a back door into mutable renderer state or unscheduled ECS writes.

## [ ] LUA01 - Expand `ScriptComponent` into a runtime-ready ECS attachment

**Deliverable:** `ScriptComponent` keeps the existing source/path string for compatibility and adds explicit metadata for enabled state, language, entry point/module id, reload-on-save policy, and parameter block ownership.

**Implementation hints:** Treat missing metadata as `enabled=true`, `language=lua`, `reload_on_save=true`. Keep old scene files valid. Do not require Lua to be compiled in for scene parsing to preserve script metadata.

**Acceptance:** Scene JSON with `script.source` round-trips unchanged; scene JSON with `script.enabled=false` still creates a script component but the runtime skips dispatch.

## [ ] LUA02 - Add script schema validation and migration rules

**Deliverable:** Scene validation reports script path/source issues, unsupported languages, invalid parameter shapes, missing files when path validation is enabled, and deprecated script fields.

**Implementation hints:** Validation should distinguish hard schema errors from runtime warnings. Missing script files should be warnings for asset-library browsing and errors only when the scene is launched with scripts enabled.

**Acceptance:** Invalid script metadata produces actionable diagnostics without breaking scenes that only store dormant script attachments.

## [ ] LUA03 - Add engine-side scripting module boundary

**Deliverable:** Create `src/scripting` with runtime-facing interfaces and data contracts: `IScriptRuntime`, `ScriptBinding`, `ScriptExecutionContext`, `ScriptDiagnostic`, `ScriptDispatchSummary`, and lifecycle hook IDs.

**Implementation hints:** This module may depend on `core` and `scene`; `scene` must not depend on Lua headers or scripting implementation details. Keep the first runtime usable when `PT_ENABLE_LUA=OFF`.

**Acceptance:** Default builds compile a no-Lua script runtime that can scan ECS script components and report that scripts are dormant.

## [ ] LUA04 - Add stable ECS script binding discovery

**Deliverable:** The script runtime scans `SceneWorld` for `ComponentKind::Script`, resolves stable entity IDs, filters disabled/empty scripts, and stores bindings in deterministic entity order.

**Implementation hints:** Use `SceneWorld::all_entities()` order as the first stable order source. Record script source, language, entity id, and display name for diagnostics.

**Acceptance:** Repeated scans of the same loaded scene produce byte-identical binding summaries.

## [ ] LUA05 - Add lifecycle dispatch shell without Lua execution

**Deliverable:** Implement lifecycle dispatch methods for `on_load`, `on_update`, `on_fixed_update`, `on_late_update`, `on_destroy`, and `on_unload` that produce summaries and diagnostics even before real Lua execution exists.

**Implementation hints:** The no-Lua runtime should never emit ECS commands. It should report skipped runnable scripts when Lua is disabled, so UI panels and release gates have real status.

**Acceptance:** A scene with two script components reports two bindings, zero commands, and a clear "Lua disabled" diagnostic when `PT_ENABLE_LUA=OFF`.

## [ ] LUA06 - Add command-writer API for scripts

**Deliverable:** Scripts emit `WorldCommandBuffer` commands through a narrow command writer: set transform, add/remove component, assign material, assign light/camera, create entity, destroy entity, reparent entity, and reorder siblings.

**Implementation hints:** Do not expose `SceneWorld&` to Lua. Script command methods should validate entity existence and normalize writer names before enqueueing commands.

**Acceptance:** Script-authored transform commands replay through `WorldCommandBuffer` and use `TransformAuthority::ScriptControlled`.

## [ ] LUA07 - Add script execution phases to the frame lifecycle

**Deliverable:** Add engine-owned calls for script phases that align with the planned schedule: `ScriptEarly`, `ScriptFixed`, and `ScriptLate` or a documented equivalent.

**Implementation hints:** Existing `WorldSystemPhase` has `ScriptEarly` but not `ScriptFixed`; either extend the enum or map fixed scripts through the existing fixed-update frame stage with clear diagnostics.

**Acceptance:** Frame timing records show script collection, command replay, transform assembly, and render extract as separate stages.

## [ ] LUA08 - Add deterministic script command merge policy

**Deliverable:** Script command buffers merge by frame, phase, stable entity id, script binding id, and command sequence number.

**Implementation hints:** Avoid wall-clock ordering. If multiple scripts write the same transform, let existing transform authority and writer tie-breaks report the conflict.

**Acceptance:** Running the same scripted scene twice in deterministic mode produces identical command order and scene hash after each scripted frame.

## [ ] LUA09 - Add Lua 5.4 dependency wiring behind `PT_ENABLE_LUA`

**Deliverable:** CMake discovers and links Lua only when `PT_ENABLE_LUA=ON`; default builds do not require Lua headers or libraries.

**Implementation hints:** Use CMake `FindLua` first. Create an imported `Lua::Lua` target if the CMake version only exposes legacy variables. Keep Lua definitions private to the scripting implementation.

**Acceptance:** `PT_ENABLE_LUA=OFF` builds with no Lua installation; `PT_ENABLE_LUA=ON` fails at configure time with a clear missing-Lua message if Lua is unavailable.

## [ ] LUA10 - Add Lua state lifecycle and script cache

**Deliverable:** Runtime owns Lua state creation/destruction, script load/compile results, binding cache, reload invalidation, and per-script error state.

**Implementation hints:** Start with one state on the engine thread. Do not share one Lua state across render, physics, or UI threads. Cache compiled chunks by resolved script URI and content hash.

**Acceptance:** Loading a script once and binding it to multiple entities does not re-read or recompile the same file per entity.

## [ ] LUA11 - Add sandboxed standard library policy

**Deliverable:** Lua runtime opens only approved libraries and registers only the host API needed for scripts.

**Implementation hints:** Do not call `luaL_openlibs` for runtime scripts. Omit or block `io`, `os`, `package`, `debug`, `dofile`, `loadfile`, native module loading, and bytecode loading.

**Acceptance:** A script attempting `io.open`, `os.execute`, `require`, or `debug.sethook` receives a structured script error and cannot affect the host process.

## [ ] LUA12 - Add instruction and memory budgets

**Deliverable:** Script execution enforces per-hook instruction limits and runtime memory limits.

**Implementation hints:** Use a Lua debug hook or equivalent VM interrupt for instruction budgets and a custom allocator for memory caps. Budget failures should disable the offending binding until reload or manual reset.

**Acceptance:** An infinite loop script is interrupted, logged, and prevented from stalling the UI/render loop.

## [ ] LUA13 - Add typed script context bindings

**Deliverable:** Scripts receive a context with entity id, frame index, dt, fixed dt, deterministic flag, command writer, read-only transform query, children query, component presence query, parameters, and diagnostics.

**Implementation hints:** Keep all bindings value-oriented or handle-oriented. Do not expose raw pointers, renderer objects, platform window objects, or mutable component references.

**Acceptance:** A Lua script can read its transform, enqueue a transform update, log a diagnostic, and inspect child entities without direct ECS mutation.

## [ ] LUA14 - Add script parameter model

**Deliverable:** Scene files can declare per-entity script params with typed values and optional editor metadata.

**Implementation hints:** Start with bool, number, string, vec3, color, entity id, asset path, and enum-like string. Preserve unknown params for forward compatibility.

**Acceptance:** Params round-trip through JSON, appear in the script panel/inspector, and are available to Lua through `ctx.params`.

## [ ] LUA15 - Add script diagnostics and profiler data

**Deliverable:** Every binding records last hook called, last fired frame, last error, skip reason, hook duration, command count, and memory estimate when available.

**Implementation hints:** Diagnostics should be consumable by the existing script panel and JSONL logs. Use stable subsystem name `scripts`.

**Acceptance:** Script errors appear in logs, the script panel model, and crash/status artifacts with entity id and script source.

## [ ] LUA16 - Add hot reload pipeline

**Deliverable:** Runtime can reload changed script files, reset binding error state, preserve compatible params, and fire unload/load hooks in deterministic order.

**Implementation hints:** Start with explicit menu command reload. Add file watching later if the platform abstraction exposes it cleanly.

**Acceptance:** Editing a script and invoking reload changes behavior without restarting the app and reports reload errors without losing the previous stable scene.

## [ ] LUA17 - Add editor attach/detach command application

**Deliverable:** `AttachScriptCommand` and `DetachScriptCommand` mutate ECS/scene state through the scheduled command path instead of remaining model-only UI payloads.

**Implementation hints:** Convert editor commands to `WorldCommandBuffer` or a scene-document command adapter. Preserve undo data and update script bindings after replay.

**Acceptance:** Attaching a `.lua` file to the selected entity adds a `ScriptComponent`, updates the script panel, and persists on scene save.

## [ ] LUA18 - Add benchmark and strict-determinism policy

**Deliverable:** Benchmark mode disables scripts by default unless a benchmark descriptor explicitly allows deterministic script events.

**Implementation hints:** Record script policy in benchmark artifacts, scene hash inputs, and status output. Refuse nondeterministic APIs while strict determinism is active.

**Acceptance:** A scripted scene run as a benchmark reports scripts disabled by default; explicitly enabled scripted benchmarks include script source hashes and phase order in artifacts.

## [ ] LUA19 - Add renderer invalidation policy for script commands

**Deliverable:** Script-driven scene changes reset or partially invalidate accumulation according to command type.

**Implementation hints:** Transform-only updates should eventually use the same dynamic instance update path as physics. Material/light/camera changes can start with full accumulation reset.

**Acceptance:** Scripted transform animation is visible in the preview and never keeps accumulating stale samples across motion.

## [ ] LUA20 - Add first scripted demo scene

**Deliverable:** Add a small scene with at least one scripted light, one scripted camera or object transform, and one disabled script binding used for UI diagnostics.

**Implementation hints:** Keep geometry tiny and CPU-friendly. Scripts should demonstrate command emission, params, diagnostics, and deterministic ordering.

**Progress:** Added `assets/scenes/ecs_lifecycle_scripting_demo.json` plus Lua lifecycle example scripts under `assets/scripts/`. The no-Lua smoke path now loads the scene, verifies script file references, and dispatches lifecycle hooks through the stub runtime.

**Acceptance:** The scene opens in Qt, shows script bindings in the script panel, and produces visible scripted motion when scripts are enabled.

## [ ] LUA21 - Add scripting smoke and release gates

**Deliverable:** Add CLI/UI model checks for script component parsing, binding discovery, no-Lua skip behavior, Lua-enabled command emission, sandbox rejection, hot reload, and benchmark-disable policy.

**Implementation hints:** The no-Lua tests should run in every build. Lua-enabled tests should be conditional on `PT_ENABLE_LUA`.

**Acceptance:** The existing `lua.attach` release gate can move from deferred to passed with evidence and validation commands.

## [ ] LUA22 - Add documentation for script authors

**Deliverable:** Add script author docs covering lifecycle hooks, context API, command rules, sandbox limits, determinism rules, params, and examples.

**Implementation hints:** Include one minimal script and one parameterized script. Document what is intentionally unavailable.

**Acceptance:** A new user can write a script that moves an entity without reading C++ source.

## [ ] LUA23 - Add third-person action scene prototype

**Deliverable:** Add a third-person playable demo scene with a human character, follow camera, action-game movement feel, and visible walk/run state changes authored through Lua scripts.

**Implementation hints:** Keep gameplay rules in Lua. C++ should only expose generic script host APIs for input, transform/camera/light commands, entity lookup, and deterministic command replay. Do not hardcode the third-person controller into `main.cpp` or the editor FPS camera path.

**Acceptance:** Loading the scene in the Qt preview and enabling script playback lets the player move a human character with WASD/arrow input while the camera follows from a third-person view.

## [ ] LUA24 - Add Lua-driven humanoid walk animation prototype

**Deliverable:** Add an articulated humanoid character made from engine-supported renderable parts and animate idle/walk/run poses from Lua until full skinned skeletal animation exists.

**Implementation hints:** Use stable child entity ids for torso, head, arms, and legs. Lua should drive limb transforms based on movement state and elapsed time. Keep this separate from glTF skeletal animation, which needs a later asset/runtime task.

**Acceptance:** The human character visibly transitions from idle to a walking/running cycle when movement input is active, without adding gameplay animation rules to C++.

## [ ] LUA25 - Add Lua action-game controller API coverage

**Deliverable:** Extend the scripting smoke path to cover Lua-enabled transform writes, camera assignment, input snapshots, entity lookup, and command replay needed by the third-person scene.

**Implementation hints:** No-Lua builds should keep the existing skip diagnostics. Lua-enabled checks should be conditional and report missing Lua as a configure/runtime capability issue, not as a default-build failure.

**Acceptance:** Focused smoke tests prove that the third-person scene's required host APIs work before relying on manual Qt preview testing.

---

# 19. Motion transaction and threading backlog

Goal: make physics, animation, editor gizmos, and script transform motion use an explicit transactional transform-update path. Motion may update instance metadata, instance buffers, dynamic bounds, dynamic TLAS/BVH state, and accumulation state. Motion must not silently trigger full scene reloads, full/static acceleration rebuilds, document/world snapshot rebuilds, CPU vertex rebakes, or structural scene hash changes.

## [ ] MOTION01 - Add transform-update result, reason, and fallback policy types

**Deliverable:** Add `RenderUpdateReason`, `TransformFallbackPolicy`, `InstanceTransformUpdateOptions`, `InstanceTransformUpdatePlan`, and `InstanceTransformUpdateResult` to the render/pathtracer interface layer.

**Implementation hints:** Keep `RTInstanceTransformUpdate` as TRS-only. Do not add matrix payloads yet. Define applied statuses separately from blocked/unsupported/failed statuses. Include source frame/system fields for diagnostics.

**Acceptance:** Callers can distinguish metadata-only, instance-buffer-only, dynamic-acceleration, full/static-rebuild-required, full-scene-reload-required, unsupported, and failed outcomes without relying on a boolean.

## [ ] MOTION02 - Carry transform update policy through `RenderCoordinator`

**Deliverable:** Replace the coordinator's vector-only transform command with an `InstanceTransformCommand` carrying updates plus options.

**Implementation hints:** Keep a temporary legacy overload that logs and defaults to `NoFallback`. Coalesce only commands with compatible reason/policy. Physics, animation, editor drag, and script transform motion should default to `AllowDynamicAcceleration`.

**Acceptance:** `RenderCoordinator` can reject a backend-reported full/static rebuild or full scene reload for `PhysicsMotion`, `AnimationMotion`, `EditorGizmoMotion`, and `ScriptTransformMotion`.

## [ ] MOTION03 - Make coordinator transform updates transactional

**Deliverable:** Change `RenderCoordinator` to plan first, policy-check second, apply backend state third, and commit the coordinator scene mirror last.

**Implementation hints:** Do not call broad `ApplyInstanceTransformUpdates()` speculatively. A rejected or failed transform command must leave coordinator scene, generation, sample index, tracer state, backend mirrors, and acceleration structures unchanged.

**Acceptance:** Policy-forbidden transform updates increment a rejection/stale metric and do not mutate committed render state.

## [ ] MOTION04 - Split transform apply helpers by work class

**Deliverable:** Add resolve/validate helpers, metadata-only apply helpers, and full-rebuild apply helpers for `RTInstanceTransformUpdate`.

**Implementation hints:** Metadata-only apply may update `RTInstance` TRS, flags, and revision only. Full-rebuild apply may rebake CPU vertices from local geometry ranges. Use the repo's existing result type instead of `std::expected`.

**Acceptance:** Fast motion paths never rebake CPU vertices, and full-rebuild paths are the only callers that can update baked scene vertices.

## [ ] MOTION05 - Make scalar CPU transform updates honest and atomic

**Deliverable:** Update `ScalarCpuPathTracer` so the new plan/apply transform API reports `BlockedNeedsFullStaticAccelRebuild` or `Unsupported` until CPU dynamic TLAS support exists.

**Implementation hints:** Do not mutate `m_scene` before policy approval. Do not call `build_or_update_acceleration()` inside the fast transform apply path. Do not treat external accelerator `build_info().built` as transform-update success.

**Acceptance:** A CPU physics-motion transform update with `AllowDynamicAcceleration` is rejected or marked stale without changing `m_scene`, rebuilding the BVH, or reporting success.

## [ ] MOTION06 - Make D3D12 transform updates staged and classified

**Deliverable:** Return `AppliedDynamicAccelUpdate` for successful D3D12 dynamic instance/TLAS updates and make the update transactional.

**Implementation hints:** Follow the existing scene-delta pattern: copy `m_sceneData`/packed instance mirrors, apply updates to the copies, build/refit staged dynamic acceleration, upload, then commit mirrors only after all work succeeds.

**Acceptance:** Injected D3D12 upload/TLAS failure leaves `m_sceneData`, packed instance data, and dynamic acceleration mirrors unchanged.

## [ ] MOTION07 - Stop Qt physics from auto-reloading on blocked motion

**Deliverable:** Route Qt physics transform publishes with `RenderUpdateReason::PhysicsMotion` and `AllowDynamicAcceleration`, and remove automatic `qtReloadEditedScene("physics simulation")` fallback for blocked CPU dynamic motion.

**Implementation hints:** On blocked motion, mark preview stale and surface a status message such as CPU backend lacks fast dynamic transform support. Manual scene reloads may still use `AllowFullSceneReload`.

**Acceptance:** With CPU backend lacking dynamic TLAS, moving physics bodies do not trigger automatic full scene reloads from the Qt physics loop.

## [ ] MOTION08 - Add CPU dynamic BLAS/TLAS acceleration

**Deliverable:** Split CPU acceleration into static flattened BVH plus dynamic local-geometry BLAS and dynamic instance TLAS.

**Implementation hints:** Use existing `RTInstance` dynamic flags, local vertex/index ranges, and TRS. Static BVH should remain unchanged when a dynamic rigid instance moves. Dynamic TLAS may refit or rebuild only dynamic instance bounds.

**Acceptance:** Moving one dynamic CPU instance returns `AppliedDynamicAccelUpdate`, leaves the static BVH build count unchanged, and updates/refits only dynamic acceleration state.

## [ ] MOTION09 - Split scene revisions and hashes by domain

**Deliverable:** Separate structural, geometry, material, light, camera, and transform revisions/hashes.

**Implementation hints:** Transform-only motion increments transform revision but does not change structural, geometry, material, or light hashes. Snapshot consumers should be able to detect transform-only changes without treating them as structural scene edits.

**Acceptance:** Moving an entity transform leaves structural hash unchanged and increments transform revision.

## [ ] MOTION10 - Replace global transform recompute with dirty-subtree recompute

**Deliverable:** Replace the `SceneWorld::recompute_world_transforms()` global cache clear with dirty-root/subtree recomputation.

**Implementation hints:** Compact dirty roots by removing descendants when an ancestor is dirty. Moving one leaf should recompute one transform; moving one parent should recompute the parent and descendants.

**Acceptance:** A 10,000-entity scene moving one leaf recomputes one entity plus descendants, not all entities.

## [ ] MOTION11 - Move physics to async command/result ticks

**Deliverable:** Add a physics command/result API that submits a tick and collects completed transform writes without multiple UI-thread `future.get()` barriers.

**Implementation hints:** Keep blocking full sync for scene load, explicit resync, script reload, debug repair, and structural body changes. Runtime dynamic bodies should flow physics-to-transform-to-render; ECS should only push dynamic poses for explicit teleports.

**Acceptance:** The steady-state Qt physics loop does not block on separate `step_fixed()` and `extract_transform_writes()` calls.

## [ ] MOTION12 - Add motion transaction instrumentation and gates

**Deliverable:** Add counters and tests for transform command counts, applied work class, policy rejections, full scene reloads from motion, full/static acceleration rebuilds from motion, dynamic TLAS refits/rebuilds, scene snapshot builds from motion, world transform recompute counts, and physics worker wait time.

**Implementation hints:** Add focused tests for CPU rejection atomicity, external accelerator fake success, coordinator rejection atomicity, D3D12 upload failure atomicity, D3D12 dynamic update classification, no CPU vertex rebake on fast paths, Qt physics no-auto-reload on blocked CPU motion, scene hash split, and dirty transform recompute.

**Acceptance:** A single moving physics body produces zero full scene reloads and zero full/static acceleration rebuilds in the motion perf gate.

---

# 20. Audio system backlog

Goal: add a small, native, engine-owned audio runtime that can support the third-person action prototype and Relay Yard vertical slice without forcing FMOD/Wwise-style middleware into the core engine. The first implementation should be deterministic where the engine needs determinism, async-safe in the audio callback, easy to disable in benchmarks/headless runs, and structured so a middleware backend can be added later behind the same interfaces.

Current completed audio items are marked below with implementation evidence. The remaining open items are intentionally left unchecked where the current code is still an MVP or does not satisfy the full acceptance criteria.

## [x] AUDIO01 - Add audio feature flags and build wiring

**Deliverable:** Add `PT_ENABLE_AUDIO`, `PT_ENABLE_MINIAUDIO`, and `PT_AUDIO_BACKEND` build/configuration plumbing.

**Implementation hints:** Follow the existing `PT_ENABLE_JOLT` and `PT_ENABLE_LUA` patterns. Default audio off for minimal/headless builds until the subsystem is stable. If miniaudio is used, keep it isolated under `src/audio/backends/miniaudio/` or `vendor/` and expose no miniaudio types outside the backend module.

**Acceptance:** Configure output lists audio as enabled/disabled, `EngineFeatureFlags` exposes audio state, and `PT_ENABLE_AUDIO=OFF` builds without audio headers or libraries.

**Completed:** CMake now exposes `PT_ENABLE_AUDIO`, `PT_ENABLE_MINIAUDIO`, and `PT_AUDIO_BACKEND`, reports enabled/disabled audio status during configure, defines miniaudio separately from the engine audio feature, and keeps miniaudio headers/libraries out of `PT_ENABLE_AUDIO=OFF` builds.

## [ ] AUDIO02 - Define the audio module boundary

**Deliverable:** Add `src/audio/` interfaces for `IAudioSystem`, `IAudioBackend`, `IAudioAssetStore`, `IAudioCommandQueue`, and `IAudioDiagnostics`.

**Implementation hints:** Keep interfaces handle-based: `AudioClipId`, `AudioStreamId`, `AudioEventId`, `AudioVoiceHandle`, `AudioBusId`, and `AudioListenerId`. The rest of the engine should only talk to these handles and value structs.

**Acceptance:** Application, scene, scripting, and editor code can compile against audio interfaces without including backend-specific headers.

## [ ] AUDIO03 - Add audio runtime lifecycle logging

**Deliverable:** Audio startup/shutdown logs match the project diagnostic standard.

**Implementation hints:** Log backend name, device name, sample rate, channel count, buffer size, requested latency, format, feature flags, loaded clip count, active voice count, and last backend error.

**Acceptance:** `ptapp --status` or the existing status artifact path can report whether audio is disabled, unavailable, initialized, or failed.

## [x] AUDIO04 - Add a no-op audio backend

**Deliverable:** Implement a backend that accepts all commands, records diagnostics, and produces no sound.

**Implementation hints:** Use this in headless tests, CI, benchmarks, unsupported platforms, and builds where real devices are unavailable. It should exercise command validation and event resolution without opening an audio device.

**Acceptance:** Audio-enabled tests can run on machines with no audio hardware and still verify event dispatch, asset lookup, and voice-handle lifecycle.

**Completed:** `AudioSystemConfig.backend = "noop"` selects the no-output backend path, preserves event lookup and diagnostics, and `pt_scripting_smoke` initializes it for CI-style audio coverage without opening a device.

## [ ] AUDIO05 - Add miniaudio backend MVP

**Deliverable:** Implement device init, shutdown, callback wiring, basic clip playback, and backend diagnostics using miniaudio.

**Implementation hints:** Keep all miniaudio ownership inside the backend. The audio callback must not allocate, read files, log synchronously, or wait on engine locks. Any callback-visible state must be preallocated or swapped atomically from the engine thread.

**Acceptance:** A small smoke executable or app command plays one bundled WAV clip through the default output device and exits cleanly.

## [ ] AUDIO06 - Add audio thread and command ownership rules

**Deliverable:** Document and enforce the audio thread model.

**Implementation hints:** Game/editor/script threads enqueue commands. The audio backend consumes commands at safe sync points. Do not let scene, renderer, Lua, physics, or UI objects be accessed from the audio callback.

**Acceptance:** A thread sanitizer build can run the no-op backend smoke without data races in audio command submission and consumption.

## [ ] AUDIO07 - Add lock-free or bounded audio command queue

**Deliverable:** Add a bounded command queue for play, stop, pause, resume, parameter changes, listener updates, bus changes, and asset state changes.

**Implementation hints:** Start with a single producer on the engine thread if needed, but keep the queue API compatible with future multi-producer use. Overflow must be explicit: reject newest, drop oldest by priority, or report a fatal diagnostic depending on command type.

**Acceptance:** Queue overflow is testable, counted, and reported without corrupting active playback state.

## [x] AUDIO08 - Add audio asset records to the scene/schema layer

**Deliverable:** Extend scene documents so audio clips, streams, buses, and events can be declared as assets.

**Implementation hints:** Use existing asset definition patterns. Record URI, asset type, load policy, default bus, default volume, loop flag, spatial flag, max voices, and optional tags.

**Acceptance:** Audio asset records round-trip through JSON and appear in scene asset manifests without changing render-only scene hashes unexpectedly.

**Completed:** Scene asset records with `audio/*` and `sound/*` types are loaded by `AudioSystem::load_scene_audio`, and `AudioListenerComponent`/`AudioEmitterComponent` round-trip through scene load/export and ECS extraction.

## [ ] AUDIO09 - Add short-clip loading

**Deliverable:** Load short sound effects into memory as `AudioClip` resources.

**Implementation hints:** Start with WAV for deterministic test fixtures. Record decoded format, duration, sample rate, channels, memory bytes, and content hash. Reject clips above a configurable in-memory size threshold.

**Acceptance:** A test loads a short clip, verifies metadata, plays it through the no-op backend, and releases it with no leaked handles.

## [ ] AUDIO10 - Add streamed audio sources

**Deliverable:** Add `AudioStream` for music, ambience, radio loops, and long dialogue.

**Implementation hints:** Streaming can be backend-owned, but engine metadata must expose stream state, buffering diagnostics, underrun count, loop state, and seek support. Pick the first streaming format deliberately; do not assume every target supports every decoder.

**Acceptance:** A long ambience/music file can loop for 60 seconds without loading the entire decoded stream into memory.

## [ ] AUDIO11 - Add asset hot reload and missing-file behavior

**Deliverable:** Audio assets can reload on explicit asset reload, and missing assets produce stable placeholder behavior.

**Implementation hints:** Missing clips should not crash event playback. Report missing asset URI, referring event, scene path, and fallback action.

**Acceptance:** Replacing a WAV file and invoking reload updates subsequent plays; deleting it produces a structured diagnostic and a silent placeholder.

## [ ] AUDIO12 - Add playback voice handles

**Deliverable:** `play_clip` and `post_event` return handles that can be queried and controlled.

**Implementation hints:** Handles need generation counters so stale handles cannot stop unrelated future sounds. A voice should expose playing, paused, stopped, virtualized, stolen, and failed states.

**Acceptance:** Tests prove that stopping an old handle after voice reuse does not stop the new voice.

## [ ] AUDIO13 - Add voice stealing and priority policy

**Deliverable:** Define max global voices, max voices per event, and priority-based stealing.

**Implementation hints:** Weapon fire, enemy barks, UI, music, ambience, and footsteps need different priorities. Stolen voices must fade out if possible and report the event/voice that caused the steal.

**Acceptance:** Spawning more voices than the configured limit keeps high-priority UI/music/weapon cues audible and records predictable stolen-voice diagnostics.

## [ ] AUDIO14 - Add mixer buses

**Deliverable:** Add `master`, `sfx`, `music`, `ui`, `voice`, `ambience`, and `debug` buses.

**Implementation hints:** Buses should support volume, mute, solo for editor/debug, pause behavior, and parent routing. Keep bus names stable because scripts and scene files will reference them.

**Acceptance:** Master mute silences all buses, music mute leaves SFX active, and bus state is visible in diagnostics.

## [ ] AUDIO15 - Add fades and parameter ramps

**Deliverable:** Add click-free volume, pitch, pan, and bus gain ramps.

**Implementation hints:** Runtime commands should specify target value and ramp duration. Use sample-accurate or buffer-accurate ramps in the backend; do not step volume once per frame for long fades.

**Acceptance:** Starting, stopping, pausing, and changing volume on looping ambience does not produce audible clicks in manual testing and produces ramp diagnostics in tests.

## [ ] AUDIO16 - Add basic 3D spatial audio

**Deliverable:** Add listener pose, emitter pose, distance attenuation, stereo panning, and per-event spatial enablement.

**Implementation hints:** Use scene units as meters unless a scene config says otherwise. Start with one listener. Expose min/max distance, rolloff curve, spread, and doppler disabled by default.

**Acceptance:** Moving an emitter left/right/front/back around the listener changes perceived pan and gain, and no-op backend tests can verify calculated spatial parameters.

## [x] AUDIO17 - Add `AudioListenerComponent`

**Deliverable:** Add an ECS component that marks the active audio listener.

**Implementation hints:** The camera-follow listener should be the default for the third-person game. Support explicit entity binding so cutscenes or editor preview can choose a different listener.

**Acceptance:** A scene with one listener component updates audio listener pose from `SceneWorld` every frame.

**Completed:** `AudioListenerComponent` is part of the scene/ECS component set, JSON load/export supports `audio_listener`, and the Qt runtime updates the audio listener pose each frame from the active camera/listener path.

## [x] AUDIO18 - Add `AudioEmitterComponent`

**Deliverable:** Add an ECS component for positional looping and one-shot audio emitters.

**Implementation hints:** Fields should include event id/path, bus, autoplay, loop, spatial, gain, pitch, radius/min/max distance, start delay, and enabled state.

**Acceptance:** A scene entity with an autoplay looping generator hum starts when the scene opens and stops when the entity/component is disabled.

**Completed:** `AudioEmitterComponent` is part of the scene/ECS component set, JSON load/export supports `audio_emitter`, and scene audio load posts enabled autoplay emitters with loop, spatial, volume, pitch, and distance settings.

## [ ] AUDIO19 - Add audio event definitions

**Deliverable:** Add an event layer above raw clips for random variants, volume/pitch ranges, bus routing, spatial defaults, cooldown, and max instances.

**Implementation hints:** Use simple JSON first. Events should cover `weapon.rifle.fire`, `weapon.shotgun.fire`, `ui.radar.ping`, `objective.terminal.disabled`, and `enemy.heavy.warning` for the vertical slice.

**Acceptance:** Posting the same event repeatedly can choose variants without the caller knowing file paths.

## [ ] AUDIO20 - Add deterministic randomization policy for events

**Deliverable:** Audio event variant selection can be deterministic when strict determinism or benchmark capture requests it.

**Implementation hints:** Use frame index, stable entity id, event id, and event sequence where deterministic output matters. Normal gameplay can use runtime randomness if recorded diagnostics identify the seed.

**Acceptance:** Running the same scripted event sequence twice in deterministic mode resolves the same clip variants and pitch/volume offsets.

## [ ] AUDIO21 - Add scripting audio APIs

**Deliverable:** Lua scripts can post audio events and control entity-bound emitters through a narrow API.

**Implementation hints:** Expose `ctx.audio.post_event(event, options)`, `ctx.audio.stop(handle)`, and entity-relative helpers. Do not expose backend objects, raw buffers, or file paths beyond event/asset ids.

**Acceptance:** A Lua script can play a footstep or weapon event on movement/fire without mutating audio internals directly.

## [ ] AUDIO22 - Add input/gameplay trigger hooks for the action prototype

**Deliverable:** Wire audio event posts for movement, aim/fire, reload, pickups, objective interaction, damage, death, and extraction state changes.

**Implementation hints:** Keep game-specific triggers in scripts or gameplay layer code, not the renderer or low-level input code. Avoid playing weapon loops directly from input polling; route through gameplay state transitions.

**Acceptance:** The third-person action demo can produce player rifle fire, reload, pickup, and objective sounds from gameplay events.

## [ ] AUDIO23 - Add weapon audio event set for Relay Yard

**Deliverable:** Define events for pistol, assault rifle, shotgun, LMG, dry fire, reloads, grenade throw, grenade explosion, bullet whiz, and bullet impacts.

**Implementation hints:** Separate player and enemy weapon buses/priority if needed. Impacts should route by surface tag: dirt, concrete, metal, sandbag, flesh, and armor.

**Acceptance:** A test scene or scripted combat fixture can fire each weapon event and resolve all referenced clips.

## [ ] AUDIO24 - Add UI and objective audio event set

**Deliverable:** Define events for radar ping, objective update, pickup collect, terminal interact loop, terminal disabled, alarm siren, extraction cue, and mission complete sting.

**Implementation hints:** UI audio should be non-spatial by default and protected from low-priority voice stealing. Looping terminal/alarm cues need explicit stop or fade commands.

**Acceptance:** Objective progression in a small fixture starts and stops looping UI/objective sounds correctly.

## [ ] AUDIO25 - Add ambience and world-loop support

**Deliverable:** Add looping ambience emitters for desert wind, generator hum, radio chatter, fire crackle, and extraction flare.

**Implementation hints:** Use streaming for long ambience and clip loops for short mechanical hums. Support fade-in/out on scene load, pause, and objective state changes.

**Acceptance:** Loading the Relay Yard prototype starts wind ambience and a positional generator loop with correct bus routing.

## [ ] AUDIO26 - Add footstep and surface tagging support

**Deliverable:** Add surface-aware footstep event selection for player and enemies.

**Implementation hints:** Start with authored surface tags on terrain/props: dirt, metal, concrete, wood. Footstep timing can be script-driven until skeletal animation events exist.

**Acceptance:** Moving the player over dirt versus metal posts different footstep events and respects per-entity cooldown.

## [ ] AUDIO27 - Add impact audio from physics/collision events

**Deliverable:** Physics or gameplay collision events can emit impact sounds with impulse-scaled volume.

**Implementation hints:** Keep this generic: collision material pair, impact impulse, contact point, and source entity. Route bullets through gameplay hit events if physics does not own projectile collision.

**Acceptance:** A grenade/barrel/crate fixture produces impact/explosion sounds once per relevant event without spamming every physics contact.

## [ ] AUDIO28 - Add audio zones and simple reverb sends

**Deliverable:** Add optional `AudioZoneComponent` with bus send amount, low-pass amount, and interior/exterior flag.

**Implementation hints:** Keep this simple for now. Relay Yard mostly needs open-air audio, but tents/containers can use mild filtering later.

**Acceptance:** Entering a zone changes an ambience or emitter send/filter parameter through a ramp and restores it on exit.

## [ ] AUDIO29 - Add occlusion and obstruction MVP

**Deliverable:** Add a cheap ray/visibility query from listener to emitter that can reduce volume and apply low-pass filtering.

**Implementation hints:** Use scene collision or simplified cover geometry. Run at a throttled rate per emitter, not every audio sample and not every frame for every voice.

**Acceptance:** A generator behind a concrete barrier is quieter/darker than the same generator with line of sight.

## [ ] AUDIO30 - Add audio diagnostics panel data

**Deliverable:** Editor model exposes device state, buses, loaded assets, active voices, event history, warnings, and underruns.

**Implementation hints:** Start model-only like existing Qt panels. Include voice id, event id, clip uri, entity id, bus, gain, pitch, spatial state, age, and stop reason.

**Acceptance:** Qt validation can build an audio diagnostics panel model with at least one active voice and one loaded clip.

## [ ] AUDIO31 - Add asset browser audio support

**Deliverable:** Audio files appear in the asset browser, can be filtered, previewed, and attached to scene audio assets/events.

**Implementation hints:** Recognize `.wav` first; add other extensions only after backend decode support is confirmed. Preview should play through the `ui` or `debug` bus and auto-stop when preview selection changes.

**Acceptance:** Dragging or selecting an audio asset can create/update an audio asset reference without manual JSON edits.

## [ ] AUDIO32 - Add editor controls for emitters and listeners

**Deliverable:** Inspector can edit `AudioEmitterComponent`, `AudioListenerComponent`, and event references.

**Implementation hints:** Controls need event picker, bus picker, spatial toggle, loop toggle, volume, pitch, min/max distance, autoplay, and enabled state.

**Acceptance:** Editing an emitter in the inspector updates the scene document, runtime component, and saved JSON.

## [ ] AUDIO33 - Add command-line audio controls

**Deliverable:** Add app options for audio backend, mute, device selection, sample rate, buffer size, and diagnostics dump.

**Implementation hints:** Examples: `--audio-backend noop`, `--audio-backend miniaudio`, `--audio-mute`, `--audio-device default`, `--audio-dump-devices`.

**Acceptance:** Running with `--audio-backend noop` prevents device initialization while preserving audio event diagnostics.

## [ ] AUDIO34 - Add benchmark and capture policy

**Deliverable:** Benchmarks default to no-op or muted audio unless explicitly requested.

**Implementation hints:** Benchmark artifacts should record audio backend, mute state, event count, dropped command count, and whether audio timing was included or excluded.

**Acceptance:** Existing render benchmarks do not vary because of audio device latency or missing hardware.

## [ ] AUDIO35 - Add crash/status artifact integration

**Deliverable:** Audio contributes state to crash recorder and status files.

**Implementation hints:** Include backend state, last 128 audio commands/events, active voice summary, loaded asset summary, underrun count, dropped command count, and last device error.

**Acceptance:** A forced crash while audio is active includes enough audio state to identify a bad event, missing asset, or backend failure.

## [x] AUDIO36 - Add audio smoke tests

**Deliverable:** Add no-op backend tests for asset load metadata, event resolution, command queue overflow, voice handle generation, bus routing, deterministic variant choice, and scene component extraction.

**Implementation hints:** Keep tests independent from real audio devices. Use tiny checked-in WAV fixtures generated or committed under `tests/assets/audio/`.

**Acceptance:** Audio smoke tests pass with `PT_ENABLE_AUDIO=ON` on CI without opening an output device.

**Completed:** `pt_scripting_smoke` loads the audio Lua demo, validates file-backed audio assets, initializes the no-op backend, checks loaded clips/events/autoplay requests, verifies Lua-posted audio requests, and exercises the follow camera/mouse-look path in game mode.

## [x] AUDIO37 - Add real-device manual test scene

**Deliverable:** Add a small scene that plays looping ambience, a positional generator, UI ping, weapon one-shots, and an objective loop.

**Implementation hints:** Keep geometry tiny and CPU-friendly. This scene is for human audio verification, not benchmark quality.

**Acceptance:** Opening the scene in Qt lets a human verify clip playback, panning, distance falloff, bus mute, and event stopping in under two minutes.

**Completed:** `assets/scenes/audio_lua_interaction_demo.json` provides a small Qt scene with recorded footstep variants, file-backed ambience, generated UI/weapon/objective cues, a listener camera, an autoplay emitter, and Lua controls for movement/fire/pickup/terminal playback.

## [x] AUDIO38 - Add Relay Yard audio asset manifest

**Deliverable:** Create a manifest listing required, placeholder, and missing audio assets for Level 01.

**Implementation hints:** Use the `game/ASSETS.md` audio list as source material. Group by weapons, player/enemy, UI/objectives, ambience, impacts, and music/stingers.

**Acceptance:** The manifest clearly shows which clips are available, which are placeholders, and which block a polished vertical slice.

**Completed:** `game/AUDIO_ASSET_MANIFEST.md` lists Relay Yard audio by weapons, player/enemy, UI/objectives, ambience/world loops, and impacts with `available`, `placeholder`, and `missing` status.

## [x] AUDIO39 - Add licensing and attribution tracking for audio assets

**Deliverable:** Every audio asset has source, license, author, modification notes, and allowed usage recorded.

**Implementation hints:** Mirror the existing asset source-note pattern used under `assets/`. Do not let generated or downloaded sounds enter the game folder without attribution metadata.

**Acceptance:** A release gate can fail if an audio asset referenced by a scene/event lacks source/license metadata.

**Completed:** `assets/audio/SOURCE.txt` records source, author, license, modification notes, and allowed usage for the file-backed audio assets currently referenced by the audio Lua demo.

## [x] AUDIO40 - Add audio author documentation

**Deliverable:** Add docs covering supported formats, event JSON, buses, emitter/listener components, Lua APIs, editor workflow, benchmark policy, and troubleshooting.

**Implementation hints:** Include one minimal non-spatial UI event, one spatial generator event, one random-variant weapon event, and one looping ambience example.

**Acceptance:** A new contributor can add a pickup sound and wire it to a scene without reading C++ source.

**Completed:** `docs/audio_authoring.md` covers build/runtime controls, supported URI styles, event JSON, buses, listener/emitter components, Lua posting, benchmark policy, troubleshooting, and a pickup-sound workflow.
