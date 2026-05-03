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

Gate 5:
  Benchmark CLI runs CPU and Vulkan paths and writes results.json.

Gate 6:
  Multithreaded CPU renderer, parallel BVH, and job system are validated.

Gate 7:
  SIMD CPU backends and backend performance experiments are available.

Gate 8:
  D3D12, Metal, and WebGPU adapters compile behind capability flags.

Gate 9:
  Material/shader library, asset import, debug views, and editor-lite controls exist.

Gate 10:
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

## [x] A01 — Create repository skeleton

**Deliverable:** canonical directory layout.

```text
/src/app
/src/core
/src/platform
/src/jobs
/src/diagnostics
/src/scene
/src/assets
/src/render/interface
/src/render/backends
/src/pathtracer
/src/cpu
/src/shaders
/src/materials
/src/benchmark
/src/editor
/tools
/tests
/assets/scenes
/cmake
/docs
```

**Implementation hints:** keep `render/interface` separate from `render/backends`. Add placeholder CMake targets for each module.

**Acceptance:** repo configures without backend implementations.

---

## [x] A02 — Add CMake preset matrix

**Deliverable:** `CMakePresets.json`.

Required presets:

```text
desktop-clang-debug
desktop-clang-release
desktop-clang-benchmark
desktop-clang-asan
desktop-clang-tsan
windows-clangcl-d3d12-debug
linux-clang-vulkan-debug
macos-clang-metal-debug
web-emscripten-webgpu-debug
headless-benchmark-release
tools-release
```

**Implementation hints:** make presets call shared cache variables. Never require users to remember long CMake commands.

**Acceptance:** `cmake --list-presets` shows all presets.

---

## [x] A03 — Add feature flag registry

**Deliverable:** generated or configured feature header.

Required flags:

```text
PT_ENABLE_VULKAN
PT_ENABLE_D3D12
PT_ENABLE_METAL
PT_ENABLE_WEBGPU
PT_ENABLE_OPENGL_EXPERIMENTAL
PT_ENABLE_CPU_RAYTRACER
PT_ENABLE_CPU_SIMD
PT_ENABLE_AVX
PT_ENABLE_AVX2
PT_ENABLE_AVX512
PT_ENABLE_NEON
PT_ENABLE_SVE
PT_ENABLE_JOLT
PT_ENABLE_LUA
PT_ENABLE_EDITOR
PT_ENABLE_BENCHMARK
PT_ENABLE_PROFILING
PT_ENABLE_SANITIZERS
PT_STRICT_DETERMINISM
```

**Implementation hints:** expose flags in both CMake and runtime `BuildInfo`.

**Acceptance:** app prints enabled/disabled features.

---

## [x] A04 — Add build metadata block

**Deliverable:** `BuildInfo` service.

Must report:

```text
git hash
build date
compiler name
compiler version
C++ standard mode
target OS
target architecture
build type
enabled feature flags
sanitizer mode
SIMD compile options
backend compile options
```

**Implementation hints:** generate via CMake configure step.

**Acceptance:** `ptapp --version --json` emits metadata.

---

## [x] A05 — Implement structured logging core

**Deliverable:** logging API with severity, subsystem, frame index, thread ID, and structured fields.

Required severities:

```text
trace
debug
info
warning
error
fatal
```

Required sinks:

```text
console
plain text file
jsonl file
in-memory ring buffer
```

**Implementation hints:** make logging thread-safe. Avoid dynamic allocation in crash path where possible.

**Acceptance:** logs from multiple threads are coherent and ordered by timestamp/sequence.

---

## [x] A06 — Implement subsystem lifecycle logging

**Deliverable:** standard lifecycle log macros or helper calls.

Every subsystem must log:

```text
construct
configure
initialize_begin
initialize_success
initialize_failure
shutdown_begin
shutdown_success
```

**Implementation hints:** include duration, config summary, and failure reason.

**Acceptance:** app startup log clearly shows which subsystem failed.

---

## [x] A07 — Add crash flight recorder

**Deliverable:** crash snapshot writer.

On crash, write:

```text
crash_state.json
last_1024_log_events.jsonl
build_info.json
runtime_config.json
last_frame_state.json
resource_state.json
active_backend_state.json
active_scene_state.json
```

**Implementation hints:** ring buffer should store last important state transitions. Include “last successful checkpoint.”

**Acceptance:** deliberate crash produces readable crash artifact folder.

---

## [x] A08 — Add platform crash hooks

**Deliverable:** crash handlers for Windows and POSIX platforms.

**Implementation hints:** support fatal exception, access violation, abort, terminate handler, and signal-based paths. Keep handler minimal; write pre-collected state.

**Acceptance:** crash tests work on at least one desktop platform and degrade gracefully elsewhere.

---

## [x] A09 — Add `ptdoctor`

**Deliverable:** diagnostic CLI tool.

Commands:

```text
ptdoctor --version
ptdoctor --check-build
ptdoctor --check-cpu
ptdoctor --check-backends
ptdoctor --check-assets
ptdoctor --check-shaders
ptdoctor --dump-config
ptdoctor --crash-test
```

**Implementation hints:** this is the human-facing “what is broken?” tool.

**Acceptance:** non-C++ user can run one command and get actionable status.

---

## [x] A10 — Add runtime config system

**Deliverable:** config parser and resolved runtime config output.

Inputs:

```text
CLI flags
config file
environment overrides
default config
```

**Implementation hints:** every resolved option should be dumpable to JSON.

**Acceptance:** `ptapp --dump-config` prints effective config.

---

## [x] A11 — Add sanitizer presets

**Deliverable:** ASAN and TSAN CMake presets.

**Implementation hints:** ASAN for memory errors; TSAN for data races. Disable incompatible high-performance flags when sanitizers are active.

**Acceptance:** `desktop-clang-asan` and `desktop-clang-tsan` configure and build.

---

## [x] A12 — Add assertion and fatal error policy

**Deliverable:** consistent `PT_ASSERT`, `PT_VERIFY`, `PT_FATAL`, and recoverable error flow.

**Implementation hints:** assertions should log subsystem, source location, and current frame before aborting.

**Acceptance:** assertion failure writes crash/log artifacts.

---

## [x] A13 — Add agent-readable task status file

**Deliverable:** generated `artifacts/status/latest_status.json`.

Contents:

```text
build status
last run status
enabled backend
selected scene
selected renderer path
last error
last crash artifact path
performance summary if available
```

**Implementation hints:** make this file easy for Codex agents to inspect after running.

**Acceptance:** every app run updates latest status.

---

## [x] A14 — Add minimal app shell

**Deliverable:** `ptapp` executable.

Modes:

```text
--version
--doctor
--headless
--scene <path>
--backend <name>
--log-level <level>
--crash-test
```

**Implementation hints:** no rendering required yet.

**Acceptance:** app starts, logs, dumps metadata, exits cleanly.

---

## [x] A15 — Add artifact directory policy

**Deliverable:** standardized output folder layout.

```text
/artifacts/runs/<timestamp>_<run_id>/
/artifacts/crashes/<timestamp>_<run_id>/
/artifacts/benchmarks/<scene>_<backend>_<timestamp>/
```

**Implementation hints:** all outputs should be discoverable without knowing C++.

**Acceptance:** no agent writes random logs into repo root.

---

## [x] A16 — Add integration smoke command

**Deliverable:** one command that agents can run before every PR.

Example behavior:

```text
configure
build
run ptdoctor
run ptapp --version
run minimal tests
write latest_status.json
```

**Implementation hints:** script can be Python, CMake workflow preset, or shell wrappers.

**Acceptance:** every agent has a single “did I break it?” command.

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

## [x] B01 — Define core primitive types

**Deliverable:** core ID, handle, span/view, result/error, and hash types.

Required concepts:

```text
StableId
RuntimeHandle
Result<T>
ErrorCode
Hash128 or Hash256
ByteSpan
StringView
FrameIndex
```

**Implementation hints:** avoid exposing raw pointers across module boundaries.

**Acceptance:** all modules can include core types without circular dependencies.

---

## [x] B02 — Define platform interfaces

**Deliverable:** interfaces for platform services.

Required:

```text
IPlatform
IWindow
IInput
IEvents
IFileSystem
ITimeSource
IClipboard
INativeSurfaceProvider
```

**Implementation hints:** `INativeSurfaceProvider` exposes only enough for renderer backend surface creation.

**Acceptance:** headless platform works without window creation.

---

## [x] B03 — Implement headless platform

**Deliverable:** platform implementation with no window.

**Implementation hints:** benchmark mode should not require graphics presentation.

**Acceptance:** `ptapp --headless` initializes platform and exits.

---

## [x] B04 — Implement desktop window stub

**Deliverable:** minimal desktop window abstraction.

**Implementation hints:** first implementation can be one OS or cross-platform wrapper, but must hide native details behind `IWindow`.

**Acceptance:** window opens, resizes, logs input/focus/close events.

---

## [x] B05 — Add input event normalization

**Deliverable:** normalized event stream.

Events:

```text
key down/up
mouse move
mouse button
mouse wheel
touch event
gamepad optional
window resize
focus change
close requested
```

**Implementation hints:** raw platform events become engine events before any editor/demo code sees them.

**Acceptance:** app logs normalized events in debug mode.

---

## [x] B06 — Implement job system foundation

**Deliverable:** `IJobSystem` with worker threads.

Required operations:

```text
submit job
submit range job
wait
wait group
worker count query
main-thread job pump
shutdown
```

**Implementation hints:** no subsystem should create arbitrary long-lived `std::thread`s. Use job system.

**Acceptance:** parallel range test processes deterministic output.

---

## [x] B07 — Add task graph scheduling

**Deliverable:** dependency-aware task graph.

**Implementation hints:** required for scene update, asset decode, BVH build, upload staging, CPU render tiles.

**Acceptance:** tasks execute respecting dependencies and record timing.

---

## [ ] B08 — Add deterministic job mode

**Deliverable:** deterministic scheduler option.

**Implementation hints:** deterministic mode can reduce parallel freedom but must preserve stable ordering for tests and benchmarks.

**Acceptance:** same deterministic task graph produces same result order across runs.

---

## [x] B09 — Add ECS registry

**Deliverable:** entity/component registry.

Required components:

```text
IdentityComponent
TransformComponent
HierarchyComponent
CameraComponent
LightComponent
MeshRendererComponent
SDFPrimitiveComponent
MaterialOverrideComponent
PhysicsBodyComponent
ScriptComponent
AnimationComponent
BenchmarkTagComponent
```

**Implementation hints:** separate stable entity ID from compact runtime handle.

**Acceptance:** entities/components can be created, queried, destroyed, and serialized.

---

## [x] B10 — Add ECS system phase model

**Deliverable:** fixed phase scheduler.

Phases:

```text
PreFrame
Input
ScriptEarly
AnimationSample
PhysicsFixed
TransformAssembly
SceneCommandApply
RenderExtract
PostFrame
```

**Implementation hints:** each system declares read/write intent.

**Acceptance:** scheduler reports phase order and conflicts.

---

## [x] B11 — Add deferred world command buffer

**Deliverable:** mutation command buffer.

Commands:

```text
create entity
destroy entity
set component
add component
remove component
set transform
assign material
assign light
assign camera
```

**Implementation hints:** scripts/editor/physics should emit commands, not mutate directly.

**Acceptance:** command replay creates identical scene state.

---

## [x] B12 — Add transform hierarchy

**Deliverable:** local/world transform resolver.

**Implementation hints:** support parent-child transforms, dirty propagation, stable traversal order.

**Acceptance:** hierarchy test validates world matrices after parent/child edits.

---

## [x] B13 — Add transform arbitration

**Deliverable:** transform authority resolver.

Authorities:

```text
BenchmarkFrozen
PhysicsControlled
AnimationControlled
ScriptControlled
EditorControlled
Authored
```

**Implementation hints:** log conflicts with entity ID, writer A, writer B, selected authority, and frame index.

**Acceptance:** conflicting writes do not silently corrupt state.

---

## [x] B14 — Add scene document schema v1

**Deliverable:** JSON scene schema.

Required sections:

```text
schema
metadata
assets
materials
geometry
sdf_primitives
entities
transforms
cameras
lights
benchmark
```

**Implementation hints:** support stable IDs from day one.

**Acceptance:** tiny Cornell scene document loads and validates.

---

## [x] B15 — Add scene snapshot/export

**Deliverable:** frozen `SceneSnapshot`.

Contents:

```text
scene hash
asset refs
entity list
renderable objects
materials
lights
camera
benchmark metadata
```

**Implementation hints:** benchmark and renderer should consume snapshots, not mutable editor state.

**Acceptance:** same scene exports same hash across runs.

---

## [ ] B16 — Add render extraction proxy

**Deliverable:** `RenderSceneProxy`.

**Implementation hints:** renderer consumes compact canonical render data, not ECS internals.

**Acceptance:** pathtracer can receive geometry/material/light/camera data without depending on ECS.

---

## [ ] B17 — Add resource lifetime registry

**Deliverable:** reference-count/lease-style registry for assets and runtime resources.

**Implementation hints:** needed for hot reload, resource eviction, and crash diagnostics.

**Acceptance:** shutdown reports zero leaked resource leases.

---

## [ ] B18 — Add frame lifecycle controller

**Deliverable:** central frame loop state machine.

Frame stages:

```text
FrameBegin
Input
CommandCollection
FixedUpdate
VariableUpdate
TransformAssembly
SceneMutationApply
RenderPreparation
RenderSubmit
PresentOrExport
FrameEnd
```

**Implementation hints:** log stage begin/end with durations.

**Acceptance:** if crash happens, crash state shows last frame stage.

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

## [x] C01 — Define renderer interface contracts

**Deliverable:** renderer interface headers.

Required interfaces:

```text
IRenderBackend
IRenderDevice
IRenderCommandContext
IRenderSwapchain
IRenderResourceAllocator
IShaderCompiler
IShaderCache
IFrameGraph
```

**Implementation hints:** no Vulkan/D3D12/Metal/WebGPU types outside backend folders.

**Acceptance:** CPU-only build can include renderer interfaces without graphics SDKs.

---

## [x] C02 — Define GPU resource descriptors

**Deliverable:** abstract descriptors.

Required descriptors:

```text
BufferDesc
TextureDesc
SamplerDesc
PipelineDesc
ComputePipelineDesc
RayTracingPipelineDesc
DescriptorLayoutDesc
ReadbackDesc
```

**Implementation hints:** include debug labels, usage flags, memory hints, and lifetime class.

**Acceptance:** descriptors serialize to logs for crash analysis.

---

## [x] C03 — Define backend capability schema

**Deliverable:** `RenderBackendCapabilities`.

Required capability groups:

```text
compute
storage buffers
storage textures
timestamp queries
subgroups/waves
descriptor indexing
bindless-like resources
texture formats
buffer alignment
max workgroup size
ray tracing
memory budget
presentation
readback
```

**Implementation hints:** every feature-dependent path must query this schema.

**Acceptance:** `ptdoctor --check-backends` dumps capabilities.

---

## [x] C04 — Implement null renderer backend

**Deliverable:** no-op backend.

**Implementation hints:** used for testing app lifecycle and scene extraction without GPU.

**Acceptance:** app runs full frame loop with null renderer.

---

## [x] C05 — Implement frame graph skeleton

**Deliverable:** pass graph with read/write declarations.

Pass types:

```text
upload
compute
raster
copy
resolve
readback
present
debug
```

**Implementation hints:** first version only validates dependencies and logs pass order.

**Acceptance:** invalid resource hazard produces a diagnostic.

---

## [ ] C06 — Implement shader manifest model

**Deliverable:** `ShaderManifest`.

Fields:

```text
shader family
entry point
backend
source hash
variant hash
defines
feature flags
resource layout version
compiler flags
artifact path
compile diagnostics
```

**Implementation hints:** shader failures must be visible in logs and crash snapshots.

**Acceptance:** fake shader manifest can be generated and dumped.

---

## [ ] C07 — Implement shader cache interface

**Deliverable:** `IShaderCache`.

Operations:

```text
query
store
invalidate
explain_miss
dump_manifest
```

**Implementation hints:** cache key includes source hash, backend, defines, feature flags, compiler flags.

**Acceptance:** repeated shader compile request reports cache hit.

---

## [x] C08 — Implement Vulkan compute backend MVP

**Deliverable:** Vulkan backend capable of compute dispatch and readback.

**Implementation hints:** start without swapchain if easier. Headless compute is enough for first pathtracing output.

**Acceptance:** compute shader writes known pattern to buffer/texture and readback validates.

---

## [ ] C09 — Add Vulkan timestamp query support

**Deliverable:** GPU timing for Vulkan path.

**Implementation hints:** if timestamp not supported, log fallback to CPU timing.

**Acceptance:** benchmark result can include Vulkan GPU time or explicit fallback reason.

---

## [x] C10 — Add Vulkan software-BVH compute path integration

**Deliverable:** Vulkan compute path can accept pathtracer buffers and dispatch baseline kernel.

**Implementation hints:** Agent D owns kernel semantics; Agent C owns resource binding and dispatch.

**Acceptance:** Vulkan backend can run the first GPU pathtracing pass.

---

## [ ] C11 — Add Vulkan hardware RT capability probe

**Deliverable:** capability detection for Vulkan RT.

Probe:

```text
VK_KHR_acceleration_structure
VK_KHR_ray_tracing_pipeline
VK_KHR_ray_query
shader group handle size
acceleration structure limits
```

**Implementation hints:** no full RT renderer yet; this task only detects and logs support.

**Acceptance:** `ptdoctor --check-backends` says whether Vulkan RT is available.

---

## [ ] C12 — Add D3D12 backend skeleton

**Deliverable:** D3D12 backend compiles on Windows behind flag.

**Implementation hints:** implement device creation, capability dump, buffer allocation, compute dispatch stub.

**Acceptance:** Windows build compiles with `PT_ENABLE_D3D12`.

---

## [ ] C13 — Add D3D12 DXR capability probe

**Deliverable:** DXR support check.

**Implementation hints:** detect ray tracing tier and log unsupported reasons.

**Acceptance:** backend capability dump reports DXR tier or absent support.

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

## [ ] C18 — Add optional OpenGL compute experiment gate

**Deliverable:** optional backend stub only behind `PT_ENABLE_OPENGL_EXPERIMENTAL`.

**Implementation hints:** OpenGL must not block main build. Treat it as debug/legacy compute experiment.

**Acceptance:** disabled by default; enabled only when explicitly requested.

---

## [ ] C19 — Add backend selection policy

**Deliverable:** deterministic backend chooser.

Selection order:

```text
explicit CLI backend
config backend
platform preferred backend
first compatible backend
null backend fallback only for non-render tests
```

**Implementation hints:** log every candidate and rejection reason.

**Acceptance:** user can see exactly why a backend was selected or rejected.

---

## [ ] C20 — Add renderer crash-state dump

**Deliverable:** backend state serializer.

Dump:

```text
selected backend
device name
capabilities
live buffers
live textures
live pipelines
last submitted frame
last shader variant
last pass name
last error
```

**Implementation hints:** must integrate with Agent A crash recorder.

**Acceptance:** renderer crash artifact explains last GPU/renderer state.

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

## D01 — Define pathtracer domain API

**Deliverable:** `IPathTracer`.

Required operations:

```text
configure
load_scene_snapshot
build_or_update_acceleration
reset_accumulation
render_sample_batch
resolve_film
read_counters
shutdown
```

**Implementation hints:** same high-level API must work for CPU and GPU implementations.

**Acceptance:** null pathtracer can be called by app/benchmark.

---

## D02 — Define canonical ray tracing scene data

**Deliverable:** `RTSceneData`.

Required sections:

```text
vertices
indices
instances
SDF primitives
materials
textures references
lights
camera
environment
```

**Implementation hints:** layout should be SoA-friendly where possible for CPU SIMD and GPU buffers.

**Acceptance:** `RenderSceneProxy` converts to `RTSceneData`.

---

## D03 — Define RNG protocol

**Deliverable:** deterministic sample key model.

Canonical key:

```text
frame_index
pixel_index
sample_index
dimension
path_depth
path_id
seed
```

**Implementation hints:** CPU and GPU must use comparable counter-based RNG behavior.

**Acceptance:** CPU replay of same sample key returns same random sequence.

---

## D04 — Implement scalar CPU ray path

**Deliverable:** scalar CPU path tracer.

Minimum features:

```text
primary rays
diffuse material
emissive material
sphere/SDF primitive
triangle mesh
simple camera
linear HDR film
```

**Implementation hints:** keep scalar clean and debuggable; it becomes reference for SIMD/GPU.

**Acceptance:** renders tiny Cornell-like scene to image.

---

## D05 — Implement CPU film accumulation

**Deliverable:** HDR film buffer.

Required:

```text
linear color accumulation
sample count buffer
resolve to LDR PNG-compatible data
resolve to HDR EXR-compatible data
NaN/Inf checks
```

**Implementation hints:** log if invalid colors are encountered.

**Acceptance:** scalar render exports valid image buffer.

---

## D06 — Implement camera ray generation

**Deliverable:** physical camera ray generator.

Features:

```text
FOV
aspect ratio
camera transform
jittered pixel sample
depth of field stub
```

**Implementation hints:** start with pinhole camera; leave DOF fields in settings.

**Acceptance:** camera movement changes render deterministically.

---

## D07 — Implement CPU triangle intersection

**Deliverable:** scalar ray/triangle path.

**Implementation hints:** include barycentric output, normal interpolation hooks, material index.

**Acceptance:** triangle test scene renders correctly.

---

## D08 — Implement SDF primitive intersection baseline

**Deliverable:** sphere-tracing or analytic/SDF intersection baseline.

Initial primitives:

```text
sphere
box
rounded box
plane
torus
capsule
```

**Implementation hints:** start with robust max step/epsilon/depth controls.

**Acceptance:** SDF test scene renders and logs step statistics.

---

## D09 — Implement BVH builder baseline

**Deliverable:** CPU-built BVH.

**Implementation hints:** begin with binary BVH; include node bounds, primitive ranges, deterministic build order.

**Acceptance:** BVH traversal matches brute-force intersection on tests.

---

## D10 — Implement BVH traversal baseline

**Deliverable:** scalar traversal.

**Implementation hints:** stack-based traversal first. Add stats for node visits and primitive tests.

**Acceptance:** renders same image as brute force within tolerance.

---

## D11 — Add parallel BVH build

**Deliverable:** job-system-based BVH build.

**Implementation hints:** use Agent B `IJobSystem`; deterministic mode must produce stable BVH or stable output.

**Acceptance:** parallel build passes correctness tests and reports build time.

---

## D12 — Add tile-based CPU renderer

**Deliverable:** multithreaded CPU rendering by image tiles.

**Implementation hints:** each tile job writes separate pixel ranges. Avoid false sharing by tile partitioning.

**Acceptance:** CPU renderer scales with worker count and matches scalar output within tolerance.

---

## D13 — Add SIMD abstraction layer

**Deliverable:** `ISimdKernel` and CPU dispatch policy.

Required modes:

```text
Scalar
SSE/SSE4 optional
AVX
AVX2
AVX512
NEON
SVE
```

**Implementation hints:** separate compile-time availability from runtime CPU detection.

**Acceptance:** app logs selected SIMD mode.

---

## D14 — Add x86 CPU feature detection

**Deliverable:** runtime detection for x86 SIMD.

Detect:

```text
SSE2
SSE4.1/SSE4.2
AVX
AVX2
AVX-512F
AVX-512DQ
AVX-512BW
AVX-512VL
FMA
```

**Implementation hints:** also check OS support for extended registers where needed.

**Acceptance:** `ptdoctor --check-cpu` reports x86 features.

---

## D15 — Add ARM CPU feature detection

**Deliverable:** runtime detection for ARM SIMD.

Detect:

```text
NEON
SVE
SVE2 if available
FP16 optional
dot-product optional
```

**Implementation hints:** keep platform-specific probing isolated.

**Acceptance:** `ptdoctor --check-cpu` reports ARM features where applicable.

---

## D16 — Add scalar-to-SIMD packet ray interface

**Deliverable:** packet ray kernel API.

**Implementation hints:** define ray packets, hit packets, mask handling, and lane compaction policy.

**Acceptance:** scalar and packet interfaces can share scene data.

---

## D17 — Implement AVX2 experiment kernel

**Deliverable:** AVX2 packet intersection experiment.

**Implementation hints:** start with ray/AABB and ray/triangle. Do not optimize all materials yet.

**Acceptance:** benchmark compares scalar vs AVX2 on CPU.

---

## D18 — Implement AVX-512 experiment kernel

**Deliverable:** AVX-512 packet intersection experiment.

**Implementation hints:** measure, do not assume it is faster. Include AVX-512 downclock caveat in experiment notes if observed.

**Acceptance:** benchmark reports AVX2 vs AVX-512 result.

---

## D19 — Implement NEON experiment kernel

**Deliverable:** ARM NEON packet intersection experiment.

**Implementation hints:** use same packet abstraction as x86.

**Acceptance:** ARM build can select NEON kernel.

---

## D20 — Implement SVE experiment kernel

**Deliverable:** ARM SVE packet experiment.

**Implementation hints:** keep vector-length agnostic.

**Acceptance:** SVE-capable build reports lane width and benchmark result.

---

## D21 — Add integrator baseline

**Deliverable:** path tracing integrator.

Features:

```text
path throughput
diffuse bounce
emissive hit
max depth
Russian roulette placeholder
environment color
```

**Implementation hints:** keep all PDFs explicit even before MIS is fully enabled.

**Acceptance:** multi-bounce diffuse scene converges.

---

## D22 — Add next-event estimation

**Deliverable:** direct light sampling.

**Implementation hints:** start with rectangle/sphere light sampling.

**Acceptance:** Cornell scene converges faster with NEE enabled.

---

## D23 — Add MIS

**Deliverable:** multiple importance sampling.

**Implementation hints:** track BSDF PDF and light PDF.

**Acceptance:** MIS test reduces noise without visible bias.

---

## D24 — Add material evaluation interface

**Deliverable:** material dispatch contract.

Material interface must support:

```text
evaluate
sample
pdf
is_delta
is_emissive
energy_check
```

**Implementation hints:** Agent E owns material registry; Agent D owns integrator contract.

**Acceptance:** diffuse, mirror, glass, emissive can plug into integrator.

---

## [x] D25 — Add GPU-compatible buffer layout

**Deliverable:** CPU structs mirrored to GPU layout manifest.

**Implementation hints:** avoid accidental padding mismatch. Emit layout diagnostics.

**Acceptance:** GPU backend can upload `RTSceneData`.

---

## D26 — Add film resolve pipeline

**Deliverable:** CPU-side resolve stages.

Stages:

```text
exposure
white balance placeholder
tone map: linear, filmic, ACES placeholder
gamma/output transform
```

**Implementation hints:** same settings later map to GPU resolve shader.

**Acceptance:** image export uses declared film settings.

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

## E01 — Define asset ID and registry

**Deliverable:** `AssetRegistry`.

Asset classes:

```text
MeshAsset
TextureAsset
MaterialAsset
SceneAsset
ShaderAsset
AnimationAsset
BenchmarkSceneAsset
```

**Implementation hints:** IDs should be stable URN/hash-based.

**Acceptance:** assets can be registered, queried, and dumped.

---

## E02 — Define importer interface

**Deliverable:** `IAssetImporter`.

Required methods:

```text
supported_extensions
supported_features
validate_source
import_source
emit_diagnostics
```

**Implementation hints:** importer diagnostics must record lossy conversion notes.

**Acceptance:** fake importer passes interface tests.

---

## E03 — Add texture asset model

**Deliverable:** canonical texture descriptor.

Fields:

```text
format
width
height
mip count
color space
channel semantic
source hash
sampler defaults
```

**Implementation hints:** distinguish sRGB, linear, normal map, data texture.

**Acceptance:** texture metadata serializes to asset manifest.

---

## E04 — Add material descriptor model

**Deliverable:** `MaterialDesc`.

Required fields:

```text
material family
base color
roughness
metallic
IOR
transmission
emission
normal map
alpha mode
clearcoat
sheen
anisotropy
texture bindings
sampler bindings
compatibility notes
```

**Implementation hints:** descriptors are data, not shader code.

**Acceptance:** material descriptor hashes consistently.

---

## E05 — Add material registry

**Deliverable:** `MaterialRegistry`.

Operations:

```text
register family
register preset
validate material
resolve fallback
query benchmark approval
```

**Implementation hints:** every material has fallback to benchmark-safe material.

**Acceptance:** invalid material produces warning and fallback.

---

## E06 — Register Material Pack 1

**Deliverable:** descriptors and presets for benchmark core materials.

Pack 1:

```text
Diffuse / Lambert
Emissive
Mirror
Specular
Glossy
GGX rough conductor
GGX rough dielectric
Metallic PBR
Dielectric / glass
Clearcoat
Normal-mapped PBR
Alpha mask
Environment emissive response
```

**Implementation hints:** these should be benchmark-approved only after Agent D supports sampling/evaluation.

**Acceptance:** registry lists all Pack 1 materials.

---

## E07 — Register Material Pack 2 as experimental

**Deliverable:** descriptors and presets, not necessarily full shader implementation.

Pack 2:

```text
Velvet
Charcoal
Rubber
X-Ray
Subsurface approximation
Spectral glass approximation
Procedural material
SDF fractal material
Volumetric shafts
Caustics-inspired material response
Thin-film / iridescent
Retroreflector
Voronoi cracks
Diffraction grating
Anisotropic GGX
Blackbody emission
Fire plasma
Toon surface
Bokeh/motion-blur stress material
```

**Implementation hints:** label experimental in material registry.

**Acceptance:** app can show these as unavailable/experimental instead of crashing.

---

## E08 — Register Material Pack 3 backlog

**Deliverable:** backlog material descriptors.

Pack 3:

```text
Plastic
Fabric / cloth
Porcelain / ceramic
Paint
Car paint
Wet surface
Frosted glass
Dirty glass
Corrosion / oxidation
Stone
Concrete
Plaster
Water / fluid surface
Ice / crystal
Stylized diffuse
```

**Implementation hints:** these should be data-visible but not required for MVP.

**Acceptance:** material manifest includes deferred implementation status.

---

## E09 — Register advanced material backlog

**Deliverable:** descriptors for advanced/future material families.

Advanced pack:

```text
Skin
Wax
Marble scattering
Hair / fur lobes
Energy-conserving layered materials
Volumetric medium
Mud
Sand
Terra/earth
Brushed metal
Ground metal
Fire/sparkle emission
Light-emitting textiles
Holographic coatings
Paper
Cardboard
Resin
Epoxy
Gemstone
Smoke
Chromatic dust
Pearl/lustre
Frosted acrylic
Translucent polymer
Rust progression
```

**Implementation hints:** no MVP dependency.

**Acceptance:** registry reports status as deferred.

---

## E10 — Define shader family manifest

**Deliverable:** shader family declarations.

Families:

```text
ray generation
camera sampling
BVH traversal
SDF intersection
triangle intersection
material evaluation
BSDF sampling
light sampling
shadow ray testing
film accumulation
resolve
denoise
debug visualization
editor overlay
```

**Implementation hints:** declarations feed Agent C shader manifest system.

**Acceptance:** shader family manifest exports to JSON.

---

## E11 — Define SDF shader family inventory

**Deliverable:** SDF feature manifest.

SDF features:

```text
sphere
box
rounded box
cylinder
cone
frustum
capsule
ellipsoid
torus
disk
plane
triangle
wedge
prism
metaballs
Mandelbulb/fractal
CSG union
CSG intersection
CSG subtraction
smooth blend
noise deformation
```

**Implementation hints:** not all must be implemented initially; track availability.

**Acceptance:** SDF manifest declares implemented vs planned.

---

## E12 — Define lighting feature inventory

**Deliverable:** light descriptor schema.

Light types:

```text
point
spot
directional
sphere area
rectangle area
disk area
line area
portal
mesh emissive
environment sky
open sky
blackbody emitter
visible emissive object
```

**Implementation hints:** include physical units even if conversion starts simple.

**Acceptance:** scene schema supports all light descriptors.

---

## E13 — Add glTF/GLB importer MVP

**Deliverable:** glTF/GLB mesh/material/texture import.

**Implementation hints:** focus on static meshes, base color, normal, roughness, metallic, emissive.

**Acceptance:** imported glTF scene creates mesh/material assets and deterministic hashes.

---

## E14 — Add OBJ/MTL importer MVP

**Deliverable:** OBJ mesh and basic MTL material import.

**Implementation hints:** map legacy diffuse/specular/opacity into PBR fallback descriptors with compatibility warnings.

**Acceptance:** OBJ fixture imports and renders with fallback material.

---

## E15 — Add PNG/JPEG texture path

**Deliverable:** basic image decode into texture assets.

**Implementation hints:** classify color vs normal/data maps based on binding context.

**Acceptance:** base color and normal textures appear in material manifest.

---

## E16 — Add EXR texture/output support plan hook

**Deliverable:** EXR asset/output interface.

**Implementation hints:** Agent D/F will use EXR for HDR/reference outputs.

**Acceptance:** EXR support is represented in asset/output capability matrix.

---

## E17 — Add benchmark scene descriptors

**Deliverable:** scene documents for first scene pack.

Initial scenes:

```text
Cornell Native
SDF Complexity
Material Gauntlet
Sponza Lite / Atrium
Shadow Study
Mirror Room
Glass Lab
Neon Room
Physics Chaos placeholder
```

**Implementation hints:** placeholder assets are acceptable if scene schema is valid.

**Acceptance:** all scene files validate.

---

## E18 — Add debug view declarations

**Deliverable:** debug output registry.

Views:

```text
beauty
albedo
normals
depth
world position
material ID
object ID
UV
roughness
metallic
emission
throughput
sample count
variance
BVH depth
SDF distance
light contribution
denoised output
difference heatmap
NaN/Inf highlight
```

**Implementation hints:** debug view can be “declared but unavailable” until renderer support lands.

**Acceptance:** UI/CLI can list debug views.

---

## E19 — Add editor-lite control model

**Deliverable:** UI-independent editor state model.

Controls:

```text
selection
scene graph model
inspector model
material panel model
light panel model
camera panel model
benchmark panel model
debug view selector
```

**Implementation hints:** no UI toolkit dependency yet.

**Acceptance:** editor state can be manipulated through commands.

---

## E20 — Add editor command descriptors

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

## E21 — Add demo camera controls

**Deliverable:** camera controller model.

Modes:

```text
orbit
FPS
turntable
scripted benchmark path
```

**Implementation hints:** controls emit camera commands; they do not directly mutate render state.

**Acceptance:** camera changes trigger accumulation reset event.

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

## F01 — Define benchmark result schema

**Deliverable:** `BenchmarkResult` JSON schema.

Fields:

```text
run_id
scene
backend
renderer_path
cpu_simd_mode
resolution
spp
seed
max_depth
build_info
device_info
scene_hash
asset_hash
shader_hash
timing
throughput
memory
image_hash
reference_error
diagnostics
```

**Implementation hints:** every numeric metric should have units.

**Acceptance:** schema validates a fake result.

---

## F02 — Define benchmark run descriptor

**Deliverable:** `BenchmarkRunDesc`.

Required fields:

```text
scene path
backend
renderer path
resolution
samples per pixel
duration
warmup frames
seed
output directory
reference image
tolerance policy
```

**Implementation hints:** descriptor should serialize and be replayable.

**Acceptance:** CLI can parse descriptor and echo resolved run.

---

## F03 — Add benchmark CLI shell

**Deliverable:** `ptbench`.

Commands:

```text
ptbench run
ptbench list-scenes
ptbench list-backends
ptbench list-renderer-paths
ptbench validate-scene
ptbench compare
ptbench dump-capabilities
ptbench run-experiments
```

**Implementation hints:** integrate with Agent A logging and artifacts.

**Acceptance:** `ptbench --help` is useful to non-C++ user.

---

## F04 — Add benchmark artifact contract

**Deliverable:** standardized artifact outputs.

Required files:

```text
results.json
results.csv
metadata.json
scene_snapshot.json
shader_manifest.json
asset_manifest.json
beauty.png
beauty.exr
reference.exr optional
diff_heatmap.png optional
logs.jsonl
```

**Implementation hints:** benchmark should fail if required artifact is missing.

**Acceptance:** artifact validator passes fake and real runs.

---

## F05 — Add scene validation command

**Deliverable:** `ptbench validate-scene`.

Validation:

```text
schema version
asset refs
materials
lights
camera
benchmark settings
backend compatibility
```

**Implementation hints:** output both human text and JSON.

**Acceptance:** invalid scene gives actionable error.

---

## F06 — Add image comparison pipeline

**Deliverable:** reference diff tool.

Metrics:

```text
mean absolute error
max error
RMSE
optional SSIM placeholder
NaN/Inf count
diff heatmap
```

**Implementation hints:** exact equality is not required across backends; tolerance policy must be explicit.

**Acceptance:** compare command produces diff metrics and heatmap.

---

## F07 — Add CPU scalar benchmark

**Deliverable:** benchmark path for scalar CPU renderer.

**Implementation hints:** this is the correctness baseline.

**Acceptance:** scalar CPU renders tiny scene and writes results.

---

## F08 — Add multithreaded CPU benchmark

**Deliverable:** benchmark for tile-based CPU renderer.

Metrics:

```text
worker count
tile size
samples/sec
paths/sec
speedup vs scalar
```

**Implementation hints:** test 1, 2, 4, 8 workers where available.

**Acceptance:** results show scaling or explain bottleneck.

---

## F09 — Add SIMD CPU experiment suite

**Deliverable:** SIMD comparison experiment.

Compare:

```text
Scalar
AVX
AVX2
AVX-512
NEON
SVE
```

**Implementation hints:** only run modes supported by current machine/build.

**Acceptance:** results identify selected best CPU kernel for that machine.

---

## F10 — Add tile-size experiment

**Deliverable:** CPU/GPU tile-size sweep.

Parameters:

```text
8x8
16x16
32x32
64x64
adaptive/auto
```

**Implementation hints:** measure scheduling overhead, cache locality, and frame time.

**Acceptance:** default tile size is chosen from data.

---

## F11 — Add Vulkan compute vs Vulkan RT experiment

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

## F12 — Add D3D12 compute vs DXR experiment

**Deliverable:** D3D12 renderer path comparison.

Compare:

```text
D3D12 compute software BVH
D3D12 DXR, if supported
```

**Implementation hints:** log ray tracing tier and unsupported reasons.

**Acceptance:** Windows benchmark can recommend compute or DXR path.

---

## F13 — Add Metal compute vs Metal RT experiment

**Deliverable:** Metal renderer path comparison.

Compare:

```text
Metal compute software BVH
Metal acceleration-structure ray tracing, if supported
```

**Implementation hints:** measure Apple GPU behavior separately from discrete GPU assumptions.

**Acceptance:** macOS benchmark can recommend compute or Metal RT path.

---

## F14 — Add WebGPU workgroup-size experiment

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

## F15 — Add GPU memory pressure experiment

**Deliverable:** memory budget and fallback test.

Test:

```text
texture size stress
BVH size stress
scene complexity stress
readback pressure
upload pressure
```

**Implementation hints:** log fallback decisions and memory budget if API exposes it.

**Acceptance:** app degrades or fails with clear diagnostics, not silent crash.

---

## F16 — Add shader variant compile matrix

**Deliverable:** CI/test matrix for shader variants.

Minimum matrix:

```text
Vulkan compute minimum
Vulkan RT optional
D3D12 compute
D3D12 DXR optional
Metal compute
Metal RT optional
WebGPU WGSL
CPU material validation
```

**Implementation hints:** unavailable SDKs should skip cleanly, not fail unrelated platforms.

**Acceptance:** shader compile failures produce manifest diagnostics.

---

## F17 — Add startup self-test

**Deliverable:** app self-test sequence.

Tests:

```text
logging
job system
scene schema
asset registry
renderer selection
shader cache
CPU feature detection
benchmark artifact write
```

**Implementation hints:** this becomes default `ptdoctor --check-build`.

**Acceptance:** non-C++ user can identify failed subsystem.

---

## F18 — Add profiler event schema

**Deliverable:** profiling event model.

Events:

```text
CPU zone
GPU zone
job timing
frame stage timing
asset import timing
BVH build timing
shader compile timing
render pass timing
```

**Implementation hints:** integrate with logs but keep profiling data separately queryable.

**Acceptance:** benchmark result contains timing breakdown.

---

## F19 — Add CI smoke plan

**Deliverable:** CI test matrix design.

Required jobs:

```text
configure only
build debug
build benchmark
run ptdoctor
run CPU scalar tiny render
run scene validation
run artifact schema validation
run sanitizer smoke where feasible
```

**Implementation hints:** graphics backend runtime tests can be optional/manual if CI lacks GPU.

**Acceptance:** CI catches broken headers/interfaces quickly.

---

## F20 — Add release gate checklist

**Deliverable:** release candidate checklist.

Must pass:

```text
all presets configure or skip cleanly
ptdoctor passes
CPU scalar render passes
CPU threaded render passes
Vulkan compute render passes where available
benchmark artifacts validate
crash test produces artifacts
scene pack validates
material manifest validates
shader manifest validates
latest_status.json generated
```

**Implementation hints:** checklist should be runnable by agents.

**Acceptance:** release readiness is not subjective.

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

## G01 — Define UI interface layer

**Deliverable:** UI-facing interfaces.

Required interfaces:

```text
IUiSystem
IUiRenderer
IUiPlatformBridge
IViewportPanel
IViewportOverlayRenderer
IEditorCommandSink
ISelectionService
IInspectorModelProvider
ISceneTreeModelProvider
IAssetBrowserModelProvider
IBenchmarkPanelModelProvider
IUiLogger
```

**Implementation hints:** no concrete renderer API types should appear here. No Vulkan, D3D12, Metal, WebGPU, or OpenGL native handles in public UI headers.

**Acceptance:** UI interfaces compile in a CPU-only/headless build.

---

## G02 — Define UI state model

**Deliverable:** `UiRuntimeState`.

Must track:

```text
active layout
visible panels
focused panel
active modal
hovered widget
active drag/drop operation
active viewport tool
active gizmo mode
DPI scale
UI scale
selected debug view
```

**Implementation hints:** make this serializable so crash snapshots can include it.

**Acceptance:** app can dump `ui_state.json` even before real panels exist.

---

## G03 — Define editor selection model

**Deliverable:** `SelectionState`.

Must support:

```text
single selection
multi-selection
active primary entity
hovered entity
selected group
selection source
aggregate selection bounds
per-item bounds
```

**Implementation hints:** selection state should use stable entity IDs, not raw runtime pointers.

**Acceptance:** selection can be serialized and restored from a test fixture.

---

## G04 — Define editor command interface for UI

**Deliverable:** UI-specific command definitions.

Required command classes:

```text
SelectEntityCommand
ToggleSelectEntityCommand
ClearSelectionCommand
SetTransformCommand
SetMaterialCommand
SetLightPropertyCommand
SetCameraPropertyCommand
SetComponentPropertyCommand
CreateEntityCommand
DeleteEntityCommand
DuplicateEntityCommand
GroupEntitiesCommand
UngroupEntitiesCommand
MergeEntitiesCommand
ReparentEntityCommand
ReorderSiblingCommand
AttachScriptCommand
DetachScriptCommand
ImportAssetCommand
AssignAssetCommand
RunBenchmarkCommand
```

**Implementation hints:** commands should carry enough metadata for undo/redo, replay, logging, and crash recovery.

**Acceptance:** command list can serialize to `editor_commands.jsonl`.

---

## G05 — Add standard desktop app window shell

**Deliverable:** standard resizable app window with title, status bar, and root layout area.

**Implementation hints:** use the existing platform abstraction. Do not hardcode OS-specific window behavior into UI code.

**Acceptance:** app opens a window, logs resize/focus/close events, and writes UI state on exit.

---

## G06 — Add menu bar shell

**Deliverable:** top-level menu bar with empty or disabled actions.

Required menus:

```text
File
Edit
View
Create
Scene
Render
Benchmark
Assets
Scripts
Tools
Help
```

**Implementation hints:** disabled menu entries should explain why they are disabled in a tooltip/status message.

**Acceptance:** all top-level menus appear and emit log events when opened/clicked.

---

## G07 — Populate File menu

**Deliverable:** File menu actions.

Actions:

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
Preferences
Reveal Artifacts Folder
Exit
```

**Implementation hints:** actions may be wired to stubs initially, but they must emit typed UI commands/logs.

**Acceptance:** each File action produces a command or explicit “not implemented yet” diagnostic.

---

## G08 — Populate Edit menu

**Deliverable:** Edit menu actions.

Actions:

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
Command History
```

**Implementation hints:** grey out actions that require selection when nothing is selected.

**Acceptance:** Edit menu reflects current selection state.

---

## G09 — Populate View menu

**Deliverable:** View menu with panel toggles, overlay toggles, and layout presets.

Required groups:

```text
Panels
Layouts
Overlays
Debug Views
Fullscreen
UI Scale
Reset Layout
```

**Implementation hints:** toggling a panel updates `UiLayoutDocument`.

**Acceptance:** closing and reopening a panel preserves prior size/location.

---

## G10 — Populate Create menu

**Deliverable:** Create menu for entities and primitives.

Required entries:

```text
Empty Entity
Group Entity
Camera
Lights
Mesh Primitives
SDF Primitives
Material
Script
Physics Body
Benchmark Marker
```

**Implementation hints:** creation should emit editor commands; no direct ECS mutation.

**Acceptance:** selected scene receives create commands through command buffer.

---

## G11 — Populate Render menu

**Deliverable:** render settings menu.

Required entries:

```text
Backend
Renderer Path
Quality Presets
Resolution
SPP
Max Bounces
Denoiser
Tone Mapping
Exposure
Debug Channel
Shader Cache
Backend Capabilities
```

**Implementation hints:** render setting changes must trigger accumulation reset when appropriate.

**Acceptance:** render settings show in logs with old/new values.

---

## G12 — Populate Benchmark menu

**Deliverable:** benchmark menu.

Required entries:

```text
Run Current Scene
Run Scene Pack
Run CPU Calibration
Run GPU Calibration
Run SIMD Experiment
Run Backend Experiment
Compare Against Reference
Open Artifacts
Export CSV/JSON
Benchmark History
```

**Implementation hints:** menu actions should produce `BenchmarkRunDesc` commands, not call benchmark internals directly.

**Acceptance:** benchmark menu can create a valid run descriptor.

---

## G13 — Populate Assets and Scripts menus

**Deliverable:** asset/script menu actions.

Asset actions:

```text
Import Files
Reimport Selected
Refresh Browser
Show Missing Assets
Show Import Diagnostics
Clear Generated Cache
Clear Shader Cache
```

Script actions:

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

**Implementation hints:** unavailable script actions should show why, such as “Lua disabled in this build.”

**Acceptance:** menu availability matches feature flags.

---

## G14 — Add dock/floating panel system

**Deliverable:** dockable and floating panel manager.

Required panel behavior:

```text
dock
undock
float
close
collapse
resize
move
restore
save layout
load layout
reset layout
```

**Implementation hints:** panel state belongs to `UiLayoutDocument`.

**Acceptance:** layout survives app restart.

---

## G15 — Add default layout preset

**Deliverable:** default layout.

Panels:

```text
Viewport
Scene Tree
Inspector
Asset Browser
Benchmark
Console/Logs
Status Bar
```

**Implementation hints:** default layout should be usable at 1080p without overlapping essential controls.

**Acceptance:** first launch uses default layout and writes layout state.

---

## G16 — Add specialized layout presets

**Deliverable:** additional layout presets.

Required presets:

```text
Benchmark
Material Authoring
Scripting
Asset Management
Debug/Profiler
Minimal Viewport
Fullscreen Viewport With Overlay
```

**Implementation hints:** menu should switch layouts without destroying panel state.

**Acceptance:** user can switch layouts and return to previous selection.

---

## G17 — Add viewport panel

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

## G18 — Add viewport camera controls

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

## G19 — Add viewport object picking

**Deliverable:** click-to-select support.

Picking paths:

```text
object ID buffer, preferred
CPU ray pick fallback
scene tree selection fallback
```

**Implementation hints:** use stable entity IDs in pick results. The renderer can later provide object ID buffer; CPU ray fallback keeps UI functional.

**Acceptance:** clicking an object selects it and updates tree/inspector.

---

## G20 — Add hover highlight

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

## G21 — Add selection highlight with bounding boxes

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

## G22 — Add marquee/box selection

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

## G23 — Add Ctrl-click multi-select

**Deliverable:** Ctrl-click selection toggling.

Behavior:

```text
click selects one
Ctrl-click toggles one
Shift-click range selects in tree
Ctrl+Shift applies additive range where applicable
```

**Implementation hints:** tree and viewport must share the same selection service.

**Acceptance:** selected entities remain synchronized across viewport/tree/inspector.

---

## G24 — Add transform gizmo framework

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

## G25 — Add translation gizmo

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

## G26 — Add rotation gizmo

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

## G27 — Add scale gizmo

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

## G28 — Add transform snapping controls

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

## G29 — Add multi-object transform behavior

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

## G30 — Add inspector panel shell

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

## G31 — Add transform inspector

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

## G32 — Add material inspector

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

## G33 — Add light inspector

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

## G34 — Add camera inspector

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

## G35 — Add component inspector framework

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

## G36 — Add multi-selection inspector

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

## G37 — Add ECS scene tree panel

**Deliverable:** hierarchical ECS tree.

Must show:

```text
root entities
children
siblings
entity names
entity icons
component badges
visibility state
lock state
script badge
physics badge
material badge
camera/light badges
selection highlight
hover highlight
```

**Implementation hints:** tree must use stable entity IDs and support large scene virtualization later.

**Acceptance:** tree reflects scene hierarchy and selection state.

---

## G38 — Add ECS tree parent/child operations

**Deliverable:** hierarchy editing commands.

Operations:

```text
reparent
unparent
create child
delete subtree
duplicate subtree
rename
lock/unlock
hide/show
```

**Implementation hints:** parent changes must preserve world transform by default unless user chooses local transform preservation.

**Acceptance:** reparenting is undoable and produces deterministic hierarchy.

---

## G39 — Add ECS tree drag/drop sibling reorder

**Deliverable:** drag/drop reordering in tree.

Supported drops:

```text
drop before sibling
drop after sibling
drop onto parent
drop to root
```

**Implementation hints:** hierarchy should include sibling order index. Reordering must not change transforms unless reparenting requires policy choice.

**Acceptance:** sibling order persists after save/load.

---

## G40 — Add grouping workflow

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

## G41 — Add merge workflow

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

## G42 — Add split merged object workflow

**Deliverable:** undo/split interface for merged results.

**Implementation hints:** split should restore original children when merge metadata exists. If metadata is missing, show warning.

**Acceptance:** merged object can be split back into originals in test scenes.

---

## G43 — Add asset/file management panel shell

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

## G44 — Add asset search/filter/sort

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

## G45 — Add asset preview cards

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

## G46 — Add drag/drop from asset browser to viewport

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

## G47 — Add OS file drag/drop

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

## G48 — Add file picker controls

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

## G49 — Add import validation modal

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

## G50 — Add script panel

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

## G51 — Add Lua lifecycle visibility

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

## G52 — Add script parameter inspector

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

## G53 — Add benchmark panel shell

**Deliverable:** benchmark panel.

Required sections:

```text
selected scene
backend
renderer path
resolution
SPP
max depth
warmup
duration
seed
run button
stop/cancel button
artifact location
result summary
```

**Implementation hints:** panel creates `BenchmarkRunDesc`; Agent F runs it.

**Acceptance:** panel can start a benchmark command or show why it cannot.

---

## G54 — Add raw benchmark metric display

**Deliverable:** raw metrics in benchmark panel.

Metrics:

```text
FPS
frame time
GPU time
CPU time
samples/sec
paths/sec
path vertices/sec
SPP accumulated
memory estimate
BVH build time
shader compile time
```

**Implementation hints:** raw metrics must remain visible even if normalized score is the headline.

**Acceptance:** benchmark result appears in panel after run.

---

## G55 — Add hardware-normalized score model UI

**Deliverable:** normalized score display.

Score goal:

```text
Show an efficiency score that remains approximately stable relative to
hardware capability even when the user changes expensive settings such as
rays per pixel, resolution, or bounce count.
```

Important distinction:

```text
Raw FPS goes down when workload increases.
Normalized score should stay stable if the renderer is using the hardware efficiently.
```

Required display:

```text
Normalized Score
Raw Throughput
Workload Units
Hardware Calibration Profile
Confidence
Warnings
Last Calibration Date
```

**Implementation hints:** do not hide actual performance cost. Show both raw metrics and normalized efficiency.

**Acceptance:** increasing SPP lowers raw FPS but benchmark panel still reports normalized efficiency against expected workload.

---

## G56 — Add benchmark calibration panel

**Deliverable:** CPU/GPU calibration UI.

Calibration actions:

```text
run CPU scalar calibration
run CPU threaded calibration
run CPU SIMD calibration
run GPU compute calibration
run hardware RT calibration if available
run backend comparison
save calibration profile
invalidate calibration profile
```

**Implementation hints:** calibration profile should key on device, backend, driver/runtime, compiler, build flags, shader hash, and feature flags.

**Acceptance:** score panel shows calibrated/un-calibrated status.

---

## G57 — Add workload complexity graph

**Deliverable:** workload graph visualization.

Graph dimensions:

```text
resolution
SPP
max depth
light count
material complexity
triangle count
SDF step count
BVH node count
texture memory
denoiser enabled
backend path
```

**Implementation hints:** represent workload as normalized cost units, not just settings.

**Acceptance:** benchmark panel explains why a workload is expensive.

---

## G58 — Add benchmark history panel

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

## G59 — Add console/log panel

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

## G60 — Add status bar

**Deliverable:** bottom status bar.

Must show:

```text
active scene
selected backend
renderer path
SPP
FPS/raw frame time
normalized benchmark score, if available
selected entity count
active tool
last warning/error
background job count
```

**Implementation hints:** clicking warning opens log panel.

**Acceptance:** status bar always tells the user what the app is doing.

---

## G61 — Add UI event black box recorder

**Deliverable:** last-N UI event recorder.

Events:

```text
menu click
panel open/close
entity click
hover
drag start/end
drop
gizmo drag
inspector edit
tree reorder
file import
script attach
benchmark run
modal confirm/cancel
```

**Implementation hints:** integrate with Agent A crash recorder.

**Acceptance:** crash artifact includes last 256 UI events.

---

## G62 — Add editor command history panel

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

## G63 — Add modal and notification system

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

## G64 — Add keyboard shortcut model

**Deliverable:** shortcut registry.

Required shortcuts:

```text
Ctrl+S save
Ctrl+O open
Ctrl+Z undo
Ctrl+Y redo
Ctrl+D duplicate
Delete delete
F focus selected
W translate
E rotate
R scale
Q select
Ctrl+G group
Ctrl+Shift+G ungroup
Ctrl+B run benchmark, optional
F11 fullscreen
```

**Implementation hints:** shortcut conflicts should be reported.

**Acceptance:** shortcut list appears in Help menu.

---

## G65 — Add UI accessibility/scaling controls

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

## G66 — Add web UI compatibility plan

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

## G67 — Add UI performance budget

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

## G68 — Add UI-thread policy

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

## G69 — Add UI snapshot testing

**Deliverable:** headless UI model tests.

Tests:

```text
menu command creation
selection state transitions
tree reorder command generation
inspector mixed-value behavior
asset drop validation
benchmark descriptor creation
layout save/load
```

**Implementation hints:** tests should not need GPU.

**Acceptance:** CI can test UI interaction models without rendering.

---

## G70 — Add UI release gate

**Deliverable:** UI readiness checklist.

Must pass:

```text
window opens
menu bar works
layout persists
panels dock/float
scene tree displays hierarchy
viewport selection works
bounding boxes display
translate/rotate/scale gizmos work
inspector edits selected entity
multi-select works
group/ungroup works
merge/split works
asset browser imports valid files
invalid file drops are rejected
Lua script attach UI works
benchmark panel runs benchmark descriptor
normalized score displays
logs panel shows errors
crash snapshot includes UI state
```

**Acceptance:** Agent G cannot mark done until all checklist items are validated or explicitly flagged as deferred.

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
