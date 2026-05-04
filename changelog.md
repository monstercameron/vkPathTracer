# Changelog

## 2026-05-03 (session 8)

### CPU path tracer bring-up — window preview, multithreading, SIMD dispatch

**Commits**
| Commit | Scope | Summary |
|--------|-------|---------|
| `8d9f0f5` | scene | Cornell box — sphere, area light mesh, five materials, camera at (0,1,3) |
| `68c925b` | cpu | SIMD dispatch scaffold — `SimdBackend` enum, ARM NEON/VCE/SME, x86 SSE/AVX/AMX detection |
| `9967c76` | pathtracer | Multithreaded CPU tracer — stripe-interleaved thread pool, atomic counters, SIMD dispatch wiring |
| `308326b` | app/platform | Win32 preview window — GDI blit, Fisher-Yates pixel queue, 16ms tracing budget, overlay HUD |

**Cornell box scene** (`assets/scenes/cornell_native.json`)
Fully specified Cornell box: floor, ceiling, back/left/right walls, area light mesh on ceiling, SDF sphere in center. Five materials (white/red/green/light/sphere). Camera at (0,1,3) targeting (0,1,2). Point light at (0,1.85,0).

**SIMD dispatch scaffold** (`src/cpu/CpuFeatures.h/.cpp`)
`SimdBackend` enum (`Scalar`, `ArmNeon`, `ArmVce`, `ArmSme`, `X86Sse`, `X86Avx`, `X86Amx`). `SimdDispatchInfo` struct with `preferred` and `available` backends. `BuildSimdDispatchInfo()` selects best backend at runtime: ARM prefers SME > VCE > NEON; x86 prefers AVX > SSE; AMX is placeholder pending kernel support. VCE mapped as SVE‖SVE2; SME detected via `__ARM_FEATURE_SME`. `vce` and `sme` fields added to `CpuFeatureSet`, serialized to JSON.

**Multithreaded CPU path tracer** (`src/pathtracer/PathTracer.h/.cpp`)
`render_sample_pixels` now runs a `std::thread` worker pool. Thread count is `hardware_concurrency()` (12 on this machine). Work is stripe-interleaved: thread `t` processes pixel indices `t, t+N, t+2N, …`. Each thread accumulates into a `LocalAccum` struct (no mutex on hot path); results are merged after all joins. All `SampleCounters` increments use `std::atomic_ref<uint64_t>` to eliminate data races. `read_counters()` moved out-of-line with atomic loads. `configure()` initializes `m_worker_count` and `m_simd_dispatch` and logs both.

**Win32 preview window** (`src/app/main.cpp`, `src/platform/DesktopPlatform.h/.cpp`)
`--window` mode creates a Win32 window via `DesktopPlatform`. Each frame: up to 16ms of CPU path tracing via `render_sample_pixels`, then film resolve and GDI `StretchBlt` DIBSection upload to client area. `WM_ERASEBKGND` returns 1 and `InvalidateRect(FALSE)` suppress flicker. Pixel processing order is Fisher-Yates shuffled per-sample so convergence is stochastic across the whole canvas rather than top-down. Overlay HUD (white text, black shadow) shows frame/sample/non-black stats. Tracer initialised with scene loaded from `--scene` arg; falls back to checkerboard diagnostic texture on load failure.

---

## 2026-05-03 (session 7, Gate 10)

### Gate 10 complete — Release candidate: reproducible benchmark artifacts

**Gate 10 acceptance:** *"Release candidate benchmark scene pack runs with reproducible artifacts."*

**F17 — Startup self-test extended** (`src/app/main.cpp`)
Added `CheckJobSystem()` (creates `JobSystem(1)`, submits a job, waits, verifies completion, shuts down), `CheckSceneSchema()` (loads and validates `cornell_native.json` using `SceneDocument::load_from_file` if present), and `CheckBenchmarkArtifactWrite()` (writes a probe JSON to `artifacts/self_test/` and verifies). `RunDoctor` and the `--doctor` flag now cover all 8 subsystems: build, cpu, backends, assets, shaders, job_system, scene_schema, benchmark_artifact_write. Added `--check-job-system`, `--check-scene-schema`, `--check-bench-write` flags.

**F18 — Profiler event schema** (`src/benchmark/BenchmarkSchema.h/.cpp`)
`ProfilerEventKind` enum (8 kinds: CpuZone, GpuZone, JobTiming, FrameStage, AssetImport, BvhBuild, ShaderCompile, RenderPass). `ProfilerEvent` struct (kind, name, category, thread_id, start_ms, duration_ms). `ProfilerEventKindName()`, `SerializeProfilerEvent()`, `SerializeProfilerTrace()`. `ptbench run` now writes `profiler_trace.json` alongside `results.json` with scene_build, render_samples, resolve_and_write, and total events.

**F15/F16/F19/F20** — Already implemented in previous sessions (GPU memory pressure experiment, shader variant compile matrix, CI smoke plan, release gate check scripts).

---

## 2026-05-03 (session 6, Gates 8–9)

### Commits
| Commit | Scope | Summary |
|--------|-------|---------|
| `98b3c7f` | build/platform | Gate 8 — multi-backend capability flags and platform extensions (C11-C17) |
| `7834502` | pathtracer | Gate 9 — NEE, MIS, film resolve pipeline (D22/D23/D26) |
| `84ebd4a` | materials/shaders | Gate 9 — material evaluation interface, pack registries, shader/SDF manifests (D24/E07-E11) |
| `eceae0d` | render/editor | Gate 9 — debug view registry and editor-lite control model (E18-E21) |
| `3d7ac6c` | diagnostics | Gate 9 — crash recorder with minidump metadata and structured log |
| `9bdbf35` | app/bench | Gate 9 — app command wiring, benchmark schema extensions |
| `e43808b` | docs/scripts | CI smoke plan and release gate check scripts |


### Gate 9 complete — Material/shader library, asset import, debug views, and editor-lite controls

**Gate 9 acceptance:** *"Material/shader library, asset import, debug views, and editor-lite controls exist."*

**D22 — Next-event estimation** (`src/pathtracer/PathTracer.h/.cpp`)
Direct light sampling added to `ScalarCpuPathTracer`. `RenderSettings::enable_nee` flag gates the feature. `sample_direct_light()` selects a random `RTHitLight`, casts a shadow ray, and accumulates Lambert direct contribution. Cornell scene converges faster with `--nee`.

**D23 — MIS** (`src/pathtracer/PathTracer.h/.cpp`)
`MisWeight()` power heuristic (beta=2). `RenderSettings::enable_mis` flag. When both NEE and MIS are enabled, BSDF and light PDFs are balanced via the power heuristic, reducing fireflies without bias.

**D24 — Material evaluation interface** (`src/materials/MaterialInterface.h/.cpp`)
`IMaterial` abstract interface: `evaluate`, `sample`, `pdf`, `is_delta`, `is_emissive`, `energy_check`. Concrete implementations: `DiffuseMaterial` (Lambertian), `MirrorMaterial` (delta reflection), `GlassMaterial` (Schlick Fresnel refraction), `EmissiveMaterial`.

**D26 — Film resolve pipeline** (`src/pathtracer/PathTracer.h/.cpp`)
`ToneMapMode` enum (Linear, Reinhard, FilmicApprox, AcesApprox), `FilmResolveSettings` (exposure, white_balance placeholder, tone_map_mode, gamma, clamp_output), `ApplyFilmResolve()` free function.

**E07–E09 — Material pack registries** (`src/materials/MaterialDescriptors.h/.cpp`)
`MaterialFamily` enum, `ImplementationStatus` enum, `MaterialDescriptor` struct. Full registry: Pack 1 (13 benchmark-core materials, implemented), Pack 2 (19 experimental), Pack 3 (15 backlog), Advanced (25 deferred). `SerializeMaterialRegistry()` exports JSON.

**E10 — Shader family manifest** (`src/shaders/ShaderFamilyManifest.h/.cpp`)
`ShaderFamily` enum (14 families), `ShaderFamilyDescriptor`, `GetShaderFamilyManifest()`, `SerializeShaderFamilyManifest()`. JSON export. CPU-path families marked implemented.

**E11 — SDF feature inventory** (`src/shaders/SdfShaderInventory.h/.cpp`)
`SdfFeature` enum (21 features), `SdfFeatureDescriptor`, `GetSdfFeatureInventory()`, `SerializeSdfFeatureInventory()`. JSON export. Sphere, Box, RoundedBox, Capsule, Torus, Plane marked implemented.

**E18 — Debug view declarations** (`src/render/DebugViews.h/.cpp`)
`DebugViewId` enum (20 views), `DebugViewDescriptor`, `GetDebugViewRegistry()`, `IsDebugViewAvailable()`, `FindDebugView()`, `SerializeDebugViewRegistry()`. UI/CLI can list and query views.

**E19 — Editor-lite control model** (already in `src/editor/UiModels.h`)
Confirmed complete: SelectionState, UiRuntimeState (debug view selector, inspector/material/light/camera/benchmark panel interfaces).

**E20 — Editor command descriptors** (already in `src/editor/UiModels.h`)
Confirmed complete: EditorCommand, 21 command kinds, all payloads, EditorCommandHistory.

**E21 — Demo camera controls** (`src/editor/UiModels.h/.cpp`)
`CameraControllerMode` (Orbit, Fps, Turntable, ScriptedBenchmarkPath), `CameraOrbitState`, `CameraFpsState`, `CameraWaypoint`, `CameraControllerState`, `CameraController` class. Dirty-flag protocol triggers accumulation reset on camera change.

### Gate 8 complete — D3D12, Metal, and WebGPU adapters compile behind capability flags

**Gate 8 acceptance:** *"D3D12, Metal, and WebGPU adapters compile behind capability flags."*

**C11 — Vulkan hardware RT capability probe** (`src/render/backends/VulkanBackend.h/.cpp`, `src/render/interface/RenderContracts.h/.cpp`)
Extended `RenderBackendCapabilities` with `ray_query_supported`, `acceleration_structure_supported`, `shader_group_handle_size`, and `max_as_size`. `VulkanComputeBackend::capabilities()` reports Vulkan RT availability; `ptbench dump-capabilities` includes all RT fields.

**C12–C13 — D3D12 backend skeleton and DXR capability probe**
`PT_ENABLE_D3D12` CMake option guards D3D12 compilation. `BackendKind::D3d12` registered in `RenderContracts.h`; `BackendKindToString()` and `SerializeBackendCapabilities()` cover D3D12 and DXR tier fields.

**C14–C15 — Metal backend skeleton and ray tracing capability probe**
`PT_ENABLE_METAL` CMake option added. `BackendKind::Metal` registered; Metal RT availability included in capability serialization.

**C16–C17 — WebGPU backend skeleton and capability probe**
`PT_ENABLE_WEBGPU` CMake option added. `BackendKind::WebGpu` registered; WebGPU compute, storage, and presentation fields in `RenderBackendCapabilities`.

**RenderBackendCapabilities extensions**
Added: `supports_present`, `supports_multiqueue`, `max_workgroup_size_{x,y,z}`, `max_buffer_alignment`, `memory_model`. All new fields serialized in `SerializeBackendCapabilities()`.

**CMakeLists.txt**
New options: `PT_ENABLE_D3D12`, `PT_ENABLE_METAL`, `PT_ENABLE_WEBGPU`, `PT_ENABLE_OPENGL_EXPERIMENTAL`, `PT_ENABLE_EDITOR`, `PT_ENABLE_PROFILING`, `PT_ENABLE_SANITIZERS`, `PT_STRICT_DETERMINISM`. Build prints active/disabled feature flag summary at configure time.

**E01 — Editor UI model layer** (`src/editor/UiModels.h/.cpp`)
Complete data-model layer for the editor UI. Data types: `UiPanelState`, `UiLayoutDocument` (8 presets), `UiRuntimeState`, `SelectionState`, `SceneEntityBounds`, `MenuBar`/`MenuItem`, `UiShortcut`. Command layer: `EditorCommand` variant over 21 kinds (entity selection/CRUD, transform, material, light/camera property, script attach/detach, asset import/assign, benchmark run, unsupported-action passthrough). `EditorCommandHistory` (capped vector) and `UiEventLog` (capped deque). Interfaces: `IUiSystem`, `IEditorCommandSink`, `ISelectionService`, `IUiPlatformBridge`, `IInspectorModelProvider`, `ISceneTreeModelProvider`, `IAssetBrowserModelProvider`, `IBenchmarkPanelModelProvider`, `IUiLogger`. Layout factories, JSON/JSONL serializers for runtime state, selection, layout document, menu bar, and command log. Asset drop validation and default keyboard shortcut map with conflict detection.

**CMakeLists.txt**
Added `src/editor/UiModels.cpp` to both `ptapp` and `ptbench` source lists.

---

## 2026-05-03 (session 5, Gate 7)

### Gate 7 complete — SIMD CPU backends and backend performance experiments

**Gate 7 acceptance:** *"SIMD CPU backends and backend performance experiments are available."*

**D13 — SIMD abstraction layer** (`src/cpu/SimdKernel.h`)
`SimdMode` enum (`Scalar`, `NEON`, `SVE`, `AVX`, `AVX2`, `AVX512`), `SimdModeName()`, and `SelectBestSimdMode()`. On AArch64 prefers SVE > NEON > Scalar; on x86 prefers AVX512 > AVX2 > AVX > Scalar. All selection logic is compile-time guarded so non-participating ISAs do not appear in binaries.

**D14 — x86 CPU feature detection** (`src/cpu/CpuFeatures.h/.cpp`)
`CpuFeatureSet` struct with fields for SSE2, SSE4.1/4.2, AVX, AVX2, AVX-512F/DQ/BW/VL, and FMA. `QueryCpuFeatures()` uses `__get_cpuid` / `__get_cpuid_count` (GCC/Clang) or `__cpuid` / `__cpuidex` (MSVC) guarded by `VKPT_ARCH_X86`. `SerializeCpuFeatures()` returns a JSON object string for embedding in `dump-capabilities` output.

**D15 — ARM CPU feature detection** (`src/cpu/CpuFeatures.h/.cpp`)
Same `CpuFeatureSet` and `QueryCpuFeatures()` for AArch64: NEON always true (implied by AArch64 ABI), SVE/SVE2/FP16/dot-product detected from compiler predefined macros (`__ARM_FEATURE_SVE` etc.) guarded by `VKPT_ARCH_ARM64`.

**D16 — Packet ray interface** (`src/cpu/PacketRay.h`)
`RayPacket` (SoA layout, up to 16 lanes), `HitPacket` (hit mask + t/u/v/material per lane), `TriangleSOA` (v0, e1, e2, material_index). Maximum packet width `kMaxPacketWidth = 16`.

**D17 — AVX2 8-wide intersection kernel** (`src/cpu/SimdKernelAvx2.h`)
`intersect_triangle_packet_avx2()` — 8-lane Möller–Trumbore using `__m256` with FMA (`_mm256_fmadd_ps`, `_mm256_fmsub_ps`). `intersect_triangle_packet_avx2_full()` for arbitrary count with scalar tail. Guarded by `#if defined(__AVX2__)`.

**D18 — AVX-512 16-wide intersection kernel** (`src/cpu/SimdKernelAvx512.h`)
`intersect_triangle_packet_avx512()` — 16-lane Möller–Trumbore using `__m512` and `__mmask16` predication with `_mm512_mask_cmp_ps_mask`. Guarded by `#if defined(__AVX512F__)`. Notes warn about potential frequency throttling.

**D19 — NEON 4-wide intersection kernel** (`src/cpu/SimdKernelNeon.h`)
`intersect_triangle_packet_neon_4()` — 4-lane Möller–Trumbore using `float32x4_t`. Uses Newton–Raphson refinement (`vrecpsq_f32`) for `vrecpeq_f32` precision. `intersect_triangle_packet_neon()` for arbitrary count. Guarded by `#if defined(__ARM_NEON)`.

**D20 — SVE variable-width intersection kernel** (`src/cpu/SimdKernelSve.h`)
`intersect_triangle_packet_sve()` — variable-length Möller–Trumbore using `svfloat32_t`, `svbool_t`, and `svcntw()` for runtime lane width. `svwhilelt_b32` predication for arbitrary packet count. Guarded by `#if defined(__ARM_FEATURE_SVE)`.

**F09 — SIMD sweep experiment** (`src/benchmark/ptbench.cpp` — `SimdSweepCommand`)
`ptbench simd-sweep [--rays N] [--triangles N] [--output dir]` — generates random rays and triangles, benchmarks all compiled/available kernels (scalar, NEON, SVE, AVX2, AVX-512) in Mrays/s, identifies best, writes `simd_sweep.json`. CPU features and best SIMD mode reported.

**F10 — Tile-size sweep experiment** (`src/benchmark/ptbench.cpp` — `TileSweepCommand`)
`ptbench tile-sweep --scene <path> [--workers N] [--spp N] [--resolution WxH] [--output dir]` — runs cpu-tiled with tile heights 8, 16, 32, 64, measures Msamples/s per configuration, identifies best, writes `tile_sweep.json`.

**dump-capabilities enhancement**
JSON output now includes `"cpu"` section (architecture + all feature flags) and `"simd_mode"` before the backends array.

**CMakeLists.txt**
Added `src/cpu/CpuFeatures.cpp` to both `ptapp` and `ptbench` source lists.



**Gate 6 acceptance:** *"Multithreaded CPU renderer, parallel BVH, and job system are validated."*

**B08 — Deterministic job mode** (`src/jobs/JobSystem.h/.cpp`)
Added `set_deterministic(bool)` support. In deterministic mode, workers acquire a `std::mutex m_serialMutex` before executing each job, ensuring sequential one-at-a-time execution with stable ordering. Added missing standard library headers (`<deque>`, `<thread>`, `<condition_variable>`, `<memory>`) to the header. Fixed `m_jobs` from `std::vector<JobEntry>` to `std::vector<std::unique_ptr<JobEntry>>` to allow non-moveable members (`std::mutex`, `std::atomic`, `std::condition_variable`) inside `JobEntry`.

**D11 — Parallel BVH builder** (`src/cpu/ParallelBvhBuilder.h/.cpp`)
`ParallelBvhBuilder::build()` partitions AABBs by midpoint split on longest axis. When primitive count ≥ 256 and a `IJobSystem` is provided, left/right subtrees are built as parallel jobs via `jobs->submit_job()` + `wait_group`. Uses `std::stable_partition` in deterministic mode. Reports `BvhBuildStats` including `node_count`, `leaf_count`, `build_ms`, `worker_count`.

**D12 — Tile-based CPU renderer** (`src/cpu/TiledCpuPathTracer.h/.cpp`)
`TiledCpuPathTracer` implements `IPathTracer` and partitions the image into horizontal tiles. Each tile owns a `ScalarCpuPathTracer` instance. `render_sample_batch()` dispatches tile jobs in parallel via the internal `JobSystem`, then merges tile films into the master `FilmBuffer` via `FilmBuffer::import_tile()`. Added `FilmBuffer::import_tile()` and `ScalarCpuPathTracer::film()` accessor to `PathTracer.h/.cpp`.

**F08 — Multithreaded CPU benchmark** (`src/benchmark/ptbench.cpp`)
`ptbench run --renderer-path cpu-tiled` selects `TiledCpuPathTracer`. New CLI options: `--workers N`, `--tile-size H`, `--deterministic`. Diagnostics in `results.json`: `renderer=cpu-tiled`, `worker_count`, `tile_height_rows`, `bvh_nodes`, `bvh_build_ms`, `deterministic`, `speedup_estimate_vs_scalar`.

**CMakeLists.txt** — Added `src/jobs/JobSystem.cpp`, `src/cpu/ParallelBvhBuilder.cpp`, `src/cpu/TiledCpuPathTracer.cpp` to both `ptapp` and `ptbench` targets.

**Verification:** `ptbench run --backend cpu --renderer-path cpu-tiled --workers 2` produces `results.json` with all expected diagnostics. Clean full rebuild (36/36 objects) with only pre-existing warnings.

## 2026-05-03 (session 4)

### Gate 5 complete — Benchmark CLI, artifact contract, scene validation, image compare

**Gate 5 acceptance:** *"Benchmark CLI runs CPU and Vulkan paths and writes results.json."*

All Gate 5 dependencies were already implemented in `ptbench`. Verified and marked complete:

**F01 — Benchmark result schema** (`src/benchmark/BenchmarkSchema.h/.cpp`)
`BenchmarkResult` with fields: run_id, scene, backend, renderer_path, resolution, spp, seed, timing, throughput, memory, image_hash, reference_error, diagnostics. All numeric metrics carry units.

**F02 — Benchmark run descriptor** (`src/benchmark/BenchmarkSchema.h/.cpp`)
`BenchmarkRunDesc` with scene path, backend, renderer-path, resolution, spp, duration, warmup-frames, seed, output dir, reference image, tolerance policy. Serializes to/from JSON; replayable.

**F03 — Benchmark CLI shell** (`src/benchmark/ptbench.cpp`)
`ptbench` with 8 commands: `run`, `list-scenes`, `list-backends`, `list-renderer-paths`, `validate-scene`, `compare`, `dump-capabilities`, `run-experiments`.

**F04 — Benchmark artifact contract** (`src/benchmark/ptbench.cpp`)
`ptbench run` writes all required artifacts: `results.json`, `results.csv`, `metadata.json`, `scene_snapshot.json`, `shader_manifest.json`, `asset_manifest.json`, `beauty.png`, `beauty.exr`, `logs.jsonl`.

**F05 — Scene validation command** (`ptbench validate-scene`)
Validates schema version, asset refs, materials, lights, camera, benchmark settings, backend compatibility. Outputs human text or `--json`.

**F06 — Image comparison pipeline** (`ptbench compare`)
Computes mean absolute error, max error, RMSE, NaN/Inf count; writes diff heatmap PNG. Tolerance policy is explicit.

**F07 — CPU scalar benchmark** (`ptbench run --backend cpu --renderer-path cpu-scalar`)
Correctness baseline — renders scene and writes full artifact set.

Verified run:
```
ptbench run --scene assets/scenes/cornell_native.json --backend cpu \
            --renderer-path cpu-scalar --resolution 64x64 --spp 4 \
            --output artifacts/gate5_test
-> run complete; artifacts: beauty.png, beauty.exr, results.json, ...
```

**todos.md:** F01–F07 marked `[x]`; F08+ retain `[ ]`; Gate 5 annotated `(completed)`.

---

## 2026-05-03 (session 3)

### Gate 4 complete — Vulkan compute render path wired

**Gate 4 acceptance:** *"Vulkan compute backend renders the same tiny scene."*

`--render --backend vulkan` now routes through `RunVulkanBVHPass` instead of `ScalarCpuPathTracer`:
- Uploads vertex/index buffers to the simulated Vulkan allocator
- Runs 4-pass frame graph: `bvh_upload → bvh_build → pathtracer → film_resolve`
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

### A07–A13, A16, B04–B07, C10

Implemented all remaining Gate-5 infrastructure and wired it into the main binary.

**A12 — PT_ASSERT / PT_VERIFY / PT_FATAL macros** (`src/core/Assert.h/.cpp`)
Assertion layer with three tiers: `PT_ASSERT` (debug-only), `PT_VERIFY` (always-on), `PT_FATAL` (always aborts). All three write a crash artifact JSON before terminating so failures are recoverable in headless/CI runs.

**A10 — Runtime config system** (`src/core/Config.h/.cpp`)
Key-value config file parser (`key = value`, `#` comments), `PTAPP_*` env-var overrides, CLI flag overlay. Each value carries its `ConfigSource` (Default / ConfigFile / EnvVar / CliFlag). Exposed via `--config <path>` and `--dump-config` in `ptapp`.

**A07 — Crash flight recorder** (`src/diagnostics/CrashRecorder.h/.cpp`)
Singleton that tracks active backend, frame stage, pass name, shader, scene, and last 1 024 log events. On `flush()` it writes `artifacts/crashes/crash_<timestamp>/crash_state.json` + `last_log_events.jsonl` + `build_info.json`.

**A08 — Platform crash hooks** (`src/diagnostics/CrashHooks.h/.cpp`)
`install_crash_hooks()` wires `SetUnhandledExceptionFilter` (Windows) / `sigaction` (POSIX) to call `CrashRecorder::flush()` before the process dies.

**A13 — Status file writer** (`src/diagnostics/StatusFile.h/.cpp`)
Writes `artifacts/status/latest_status.json` on every run with build status, last run result, selected backend/scene/renderer-path, last error, crash artifact path, and a performance summary.

**A09 — ptdoctor** (`src/app/main.cpp`)
Full `--doctor` / `--check-build` / `--check-cpu` / `--check-backends` / `--check-assets` / `--check-shaders` implementation. Each check prints `[ok ]` or `[FAIL]`. Also added `--crash-test`, `--config`, `--dump-config`.

**A16 — Integration smoke CI script** (`tools/smoke.ps1`)
9-step PowerShell script: configure → build → binary-exists → version → doctor → render → status-file → exr-output → dump-config. Returns non-zero exit code on any failure for CI consumption.

**B04 — Desktop window stub** (`src/platform/DesktopPlatform.h/.cpp`)
`DesktopWindow` and `DesktopFileSystem` behind `IWindow` / `IFileSystem`. Tracks open/focus/resize state and drains input events.

**B05 — Input event normalization** (`src/platform/DesktopPlatform.h/.cpp`)
`DesktopInput` queues and emits key, mouse-move, mouse-button, mouse-wheel, focus-change, close-requested events as normalized `InputEvent` structs.

**B06 — Job system foundation** (`src/jobs/JobSystem.h/.cpp`)
Thread-pool `JobSystem` implementing `IJobSystem`: `submit_job`, `submit_range_job`, `wait`, `wait_group`, `worker_count`, `pump_main_thread`, `shutdown`, and `deterministic` mode toggle.

**B07 — Task graph scheduling** (`src/jobs/TaskGraph.h`)
Dependency-aware `TaskGraph` with topological sort, cycle detection, per-task timing via `TaskExecutionSample`, and optional `IJobSystem` dispatch for parallel execution.

**C10 — Vulkan software-BVH compute pass** (`src/render/backends/VulkanBackend.h/.cpp`)
`RunVulkanBVHPass()` uploads vertex/index buffers, initialises a BVH node buffer (`2N-1` nodes × 32 bytes), allocates film texture, and drives a 4-pass frame graph (`bvh_upload → bvh_build → pathtracer → film_resolve`). Returns a `VulkanBVHPassResult` with upload stats.

**Resource registry** (`src/render/interface/ResourceRegistry.h`)
`ResourceLifetimeRegistry` tracks per-handle lease info (kind, label, size, frame acquired/last-accessed, ref-count) for render resource lifetime diagnostics.

**Build** (`CMakeLists.txt`)
Added `Assert.cpp`, `Config.cpp`, `CrashRecorder.cpp`, `CrashHooks.cpp`, `StatusFile.cpp` to both `ptapp` and `ptbench` targets.

---

## 2026-05-03

### Commit notes

- 9d7b71a — feat(app): add minimal ptapp entrypoint and --version output
- 7ae1d90 — build: add CMake presets and build metadata wiring
- e47db3e — chore: add initial project scaffolding layout
- b7b04e6 — feat(core): add primitive types and headless platform foundation
- 43f8d28 — feat(app): implement headless app shell and diagnostic logging
- 2d48124 — docs: mark Gate 1 tasks complete for implemented milestones
- 22252de — fix(platform): resolve HeadlessFileSystem stream declaration parse issues
- 1056e65 - feat(scene): add gate 2 scene ECS schema and snapshot system
- 278f505 - feat(app): wire scene loading into startup path
- 810141a — feat(benchmark): add gate3 benchmark schemas, scene manifests, and ptbench CLI
- 74d7157 — feat(app): implement scalar CPU path-tracing render path and Gate 3 render CLI flags
- e98b4e0 — feat(render): finish Gate 4 backend scaffold (interfaces, factory, null/vulkan backends, frame graph, layout manifest API), add backend-capability diagnostics in ptapp/ptbench, and wire manifest serialization
- 66c7ff5 — fix(benchmark): correct PNG scanline indexing in ptbench image compare loader

