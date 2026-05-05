For **D3D12 on an Intel Arc B580**, I’d use a **three-layer profiling stack**:

1. **PIX for D3D12 GPU captures/timing/counters**
2. **Tracy or Superluminal for CPU flamegraph-style profiling**
3. **Intel GPA as a secondary Intel-specific GPU profiler**

The key point: a classic “flamegraph” is mostly a **CPU profiling concept**. For D3D12 GPU work, the equivalent is usually a **GPU event timeline + draw/dispatch cost + hardware counters + queue/fence latency**.

## Best practical setup

| Need                              |         Best tool | Why                                                                                                                       |
| --------------------------------- | ----------------: | ------------------------------------------------------------------------------------------------------------------------- |
| D3D12 GPU frame breakdown         | **Microsoft PIX** | Best D3D12-native tool; captures command lists, events, barriers, timings, counters, resources, DXR data                  |
| CPU flamegraph / frame timeline   |         **Tracy** | Free, excellent for instrumented C++ engines, supports CPU zones and D3D12 GPU zones                                      |
| Commercial CPU profiler           |  **Superluminal** | Very good timeline + call tree + flamegraph-style workflow for games                                                      |
| Intel-specific GPU metrics        |     **Intel GPA** | Intel’s own Graphics Performance Analyzers include Frame Analyzer, Trace Analyzer, and System Analyzer tools ([Intel][1]) |
| Vendor-neutral graphics debugging |     **RenderDoc** | Great capture/debugger, less ideal than PIX for D3D12 perf counters                                                       |

## What I would use first: PIX

PIX is the primary D3D12 performance tool. It gives you **GPU captures** for draw/dispatch/event analysis, **timing captures** for CPU/GPU work correlation, and hardware counters on AMD, Intel, and NVIDIA GPUs. Microsoft’s PIX docs explicitly support hardware counters for Intel GPUs, including timing-analysis counters, event-list counters, and high-frequency counters. ([Microsoft for Developers][2])

PIX timing captures are useful because they show when CPU and GPU work actually happened, including CPU thread distribution, latency between CPU submission and GPU execution, file I/O, memory allocations, and GPU memory behavior. ([Microsoft for Developers][3])

For a path tracer, PIX is where I’d look for:

| Question                           | PIX view / metric to inspect                           |
| ---------------------------------- | ------------------------------------------------------ |
| Is the GPU actually busy?          | GPU queue timeline, occupancy/counters                 |
| Are command lists submitted late?  | CPU/GPU timing capture                                 |
| Is the CPU starving the GPU?       | Queue bubbles, submit latency, CPU call stacks         |
| Are barriers killing performance?  | Resource barrier events and timings                    |
| Are copies/upload stalls bad?      | Copy queue, upload heap, residency/paging              |
| Is ray tracing dispatch expensive? | DispatchRays event timing, shader table/resource state |
| Is shader execution divergent?     | Counter trends, occupancy, shader cost                 |
| Is present/sync throttling you?    | Present timing, fence waits, queue idle gaps           |

For GPU event profiling, use **PIX GPU Capture → Collect Timing Data**. PIX replays the captured API calls multiple times and averages GPU operation timings to reduce noise. ([Microsoft for Developers][4])

## Add Tracy inside your engine

For your own C++ renderer, I would absolutely integrate **Tracy** early. Tracy is a real-time, nanosecond-resolution, remote telemetry profiler for games and applications, and it supports CPU/GPU profiling workflows. ([GitHub][5])

Use Tracy for:

```cpp
void TiledCpuPathTracer::render_sample_batch()
{
    ZoneScopedN("CPU render_sample_batch");

    // tile scheduling
    {
        ZoneScopedN("Build tile jobs");
    }

    // wait
    {
        ZoneScopedN("Wait tile jobs");
    }
}
```

For D3D12 GPU zones, instrument passes like:

```cpp
TracyD3D12Zone(g_tracyCtx, cmdList, "PathTrace Dispatch");
cmdList->Dispatch(...);
```

Use zones like:

```text
Frame
  Update camera
  Build TLAS/BLAS
  Upload constants
  PathTrace Dispatch
  Denoise
  Tonemap
  Copy/Present
```

That gives you the closest thing to a **flamegraph for your renderer’s frame**, while PIX gives you the authoritative GPU capture.

## For Intel B580 specifically

The B580 is an Arc B-series/Battlemage card with **20 Xe-cores**, **20 ray tracing units**, **12 GB GDDR6**, **192-bit memory bus**, and **456 GB/s memory bandwidth**. ([Intel][6])

That matters because you should profile for these bottlenecks:

| Bottleneck class                | Why it matters on B580                                                                              |
| ------------------------------- | --------------------------------------------------------------------------------------------------- |
| CPU driver overhead             | Intel Arc has historically been more sensitive to CPU/driver overhead than NVIDIA in some workloads |
| Memory bandwidth                | 456 GB/s is solid for the price, but path tracing can become bandwidth-heavy fast                   |
| RT traversal cost               | Use PIX around `DispatchRays` / ray query workloads                                                 |
| Shader divergence               | Path tracers often lose efficiency from incoherent rays/material branches                           |
| Queue starvation                | D3D12 apps can accidentally leave the GPU idle between frames or dispatches                         |
| Descriptor/root signature churn | Excessive binding changes can hurt CPU-side submission                                              |

There has been public analysis showing the B580 can be sensitive to CPU/driver overhead depending on CPU pairing, so I would explicitly measure CPU submission cost and GPU idle gaps rather than assuming the shader is the only bottleneck. ([Chips and Cheese][7])

## Recommended workflow

### 1. Add debug/profiling names everywhere

Do this before deep profiling:

```cpp
object->SetName(L"PathTrace Output UAV");
cmdList->BeginEvent(0, L"PathTrace Dispatch", sizeof(L"PathTrace Dispatch"));
cmdList->Dispatch(...);
cmdList->EndEvent();
```

Or use `PIXBeginEvent` / `PIXEndEvent`.

Name:

```text
Command queues
Command allocators
Command lists
Root signatures
Pipeline states
Descriptor heaps
Buffers/textures
BLAS/TLAS buffers
UAVs/SRVs
Fence objects
```

PIX becomes 10x more useful when every event/resource has a human name.

### 2. Capture one representative frame in PIX

Look for:

```text
CPU frame time
GPU frame time
GPU queue idle time
Command list submit timing
Dispatch/draw timing
Barrier cost
Copy/upload cost
Present wait
Memory residency / paging
```

Do not optimize shaders first unless PIX proves the GPU is saturated.

### 3. Run a PIX Timing Capture

This tells you whether the app is:

```text
CPU-bound
GPU-bound
sync-bound
present-bound
driver-overhead-bound
upload/copy-bound
```

A lot of D3D12 “GPU performance” problems are actually bad CPU/GPU synchronization.

### 4. Add Tracy zones for engine code

Use Tracy to see your own systems:

```text
Scene update
Camera update
BVH build/update
Material upload
Texture upload
Command recording
Descriptor allocation
JobSystem wait
Render thread idle
Present wait
```

For your path tracer architecture, Tracy is especially good for finding:

```text
UI thread touching render state
render thread waiting on jobs
tile workers imbalanced
sample loop cancellation latency
descriptor heap rebuilds
camera reset causing full rebuild
CPU-side accumulation/reset overhead
```

### 5. Use Intel GPA as a second opinion

Intel GPA is worth trying on Arc because it is Intel’s own toolchain. The package includes **Graphics Frame Analyzer**, **Graphics Trace Analyzer**, and **System Analyzer**. ([Intel][8])

I would use it after PIX, not before. PIX is still the better D3D12-native workflow; GPA may expose Intel-flavored counters or visualization that helps confirm whether the issue is EU occupancy, memory, sampler, ray tracing, or front-end behavior.

## Metrics I’d track in your renderer

For a D3D12 path tracer, add your own per-frame telemetry:

```text
frame_index
resolution
samples_per_pixel_total
samples_this_frame
ms_cpu_frame
ms_cpu_command_recording
ms_cpu_scene_update
ms_cpu_bvh_update
ms_gpu_pathtrace
ms_gpu_denoise
ms_gpu_tonemap
ms_gpu_copy_present
gpu_queue_idle_ms
num_dispatches
num_barriers
num_resource_transitions
descriptor_allocations
upload_bytes
readback_bytes
tlas_build_ms
blas_build_ms
rays_per_second
paths_per_second
samples_per_second
VRAM_used
```

For path tracing specifically, calculate:

```text
rays/sec = width * height * spp_this_frame * avg_rays_per_sample / gpu_pathtrace_seconds
```

Even a rough estimate is useful. Track it over commits.

## Build settings matter

For useful CPU profiling:

```text
RelWithDebInfo / Release + PDBs
/O2 or /Ox enabled
/Zi or /Z7 debug info
/DEBUG linker option
frame pointers enabled if your profiler benefits from them
no Debug CRT for perf runs
```

Avoid profiling a Debug build. It lies.

For GPU profiling:

```text
Use optimized shaders
Keep shader debug info available when needed
Use stable camera/scene
Disable vsync for raw perf testing
Also test with vsync on for real frame pacing
Warm up shaders/pipelines before measuring
Capture after a few seconds, not first frame
```

## My preferred stack for your project

For your D3D12 path tracer prototype:

```text
Required:
  PIX
  Tracy

Optional:
  Intel GPA
  RenderDoc
  PresentMon
  GPUView / WPA

Commercial nice-to-have:
  Superluminal
```

Use them like this:

```text
PIX:
  "Why is this GPU frame slow?"

Tracy:
  "Where is my engine spending CPU time frame-to-frame?"

Superluminal:
  "What exact CPU call stacks are hot?"

Intel GPA:
  "What Intel GPU units/counters look suspicious?"

PresentMon:
  "What is the real frame pacing / present behavior?"
```

## The optimization order I’d follow

```text
1. Confirm whether you are CPU-bound, GPU-bound, sync-bound, or present-bound.
2. Fix GPU idle gaps first.
3. Fix accidental CPU/GPU synchronization.
4. Fix excessive barriers/resource transitions.
5. Fix command recording/descriptor churn.
6. Fix shader/path tracing hotspots.
7. Fix memory bandwidth/cache behavior.
8. Then tune denoising/upscaling/present path.
```

For your case, especially on Intel B580, I would start with **PIX Timing Capture + Tracy instrumentation**. That combination will quickly tell you whether the bottleneck is the D3D12 submission path, your render-thread ownership model, GPU occupancy, RT dispatch cost, memory bandwidth, or plain synchronization mistakes.

[1]: https://www.intel.com/content/www/us/en/developer/tools/graphics-performance-analyzers/overview.html?utm_source=chatgpt.com "Intel® Graphics Performance Analyzers"
[2]: https://devblogs.microsoft.com/pix/hardware-counters-in-gpu-captures/?utm_source=chatgpt.com "Hardware Counters in GPU Captures - PIX on Windows"
[3]: https://devblogs.microsoft.com/pix/timing-captures-new/?utm_source=chatgpt.com "Timing Captures - PIX on Windows"
[4]: https://devblogs.microsoft.com/pix/gpu-captures/?utm_source=chatgpt.com "GPU Captures - PIX on Windows"
[5]: https://github.com/wolfpld/tracy?utm_source=chatgpt.com "wolfpld/tracy: Frame profiler"

## Repo PIX run notes, 2026-05-05

PIX was installed with `winget` and the working CLI was:

```text
C:\Program Files\Microsoft PIX\2603.25\pixtool.exe
```

Developer Mode had to be enabled before PIX counter playback was useful. After that, `pixtool open-capture <capture>.wpix list-counters` exposed Intel B580 counters such as `GPU Time Elapsed`, `CS Invocations`, `XVE Inst Executed ...`, `XVE Threads Occupancy All`, and RT traversal counters.

The headless `ptbench` D3D12 compute path needed two capture-specific changes:

```text
1. Add programmatic capture around the benchmark with WinPixEventRuntime / pix3.h.
2. Run the capture on a Direct queue via PT_D3D12_COMMAND_QUEUE=direct.
```

The default D3D12 compute queue did produce `.wpix` files, but `save-event-list` failed with `PIXTOOL4 - No context`. A Direct queue can still execute compute dispatches, and PIX exported the event list reliably from that capture.

Useful command shape:

```powershell
cmake --build --preset windows-clangcl-d3d12-debug --target ptbench --config Debug

& 'C:\Program Files\Microsoft PIX\2603.25\pixtool.exe' `
  open-capture 'artifacts\pix_probe\gpu_capture_compute\d3d12_compute_pix3.wpix' `
  save-event-list 'artifacts\pix_probe\gpu_capture_compute\event_list_pix3_xve_clean.csv' `
  --counters='*GPU*Time*' `
  --counters='*CS*Invocation*' `
  --counters='*XVE*Inst*Executed*CS*' `
  --counters='*XVE*Inst*Executed*Send*' `
  --counters='*XVE*Stall*' `
  --counters='*XVE*Threads*Occupancy*' `
  --counters='*L3*'
```

The local autorun config used for repeatable PIX captures is:

```text
build/presets/windows-clangcl-d3d12-debug/bin/ptbench_pix_autorun.cfg
```

Key compute-path result on `assets/scenes/cornell_native.json`, 512x512, spp 8, max depth 6, `PT_D3D12_RAYS_PER_PIXEL=8`, `PT_D3D12_READBACK_INTERVAL=8`:

| Metric | Unpacked triangles | Packed triangles | Change |
| --- | ---: | ---: | ---: |
| Avg path-trace dispatch GPU time | 20149.875 | 14888.25 | -26.11% |
| XVE ALU0 CS instructions | 1403290113.875 | 1254658708.125 | -10.59% |
| XVE ALU1 CS instructions | 1170631864 | 1008592279.625 | -13.84% |
| XVE Send instructions | 412961694.375 | 306888338 | -25.69% |
| XVE occupancy | 95.875 | 95.5 | -0.39% |

Interpretation: packed triangles are a real shader optimization. The improvement comes mostly from fewer send/load-style instructions and fewer ALU instructions, not from fixing occupancy. Tonemap is not the bottleneck; PIX reported the tonemap dispatch at about 9-10 GPU-time units versus about 14.9k for each path-trace dispatch.

## Current DXR status

Measured on the same scene and settings with `PT_D3D12_COMMAND_QUEUE=direct`, `PT_D3D12_RAYS_PER_PIXEL=8`, and `PT_D3D12_READBACK_INTERVAL=8`:

```text
D3D12 compute packed path:
  image_hash = baa3711a10205e30
  render_ms  = 92.296 ms after quiet-log cleanup
  render_ms  = 694.906 ms at spp 64

D3D12 DXR path, no hardware shadow rays:
  image_hash = b78ab8d693f7c10a
  render_ms  = 29.016 ms
  render_ms  = 183.726 ms at spp 64

D3D12 DXR path, hardware shadow rays enabled:
  render_ms  = 20.719 ms
  render_ms  = 107.458 ms at spp 64
  render_ms  = 49.793 ms during programmatic PIX capture
```

The current DXR default enables hardware shadow rays. Disable for A/B testing with `PT_D3D12_DXR_SHADOW_RAYS=0`.

Implementation changes from this optimization pass:

```text
1. Gated D3D12 first-dispatch probe logging and DXR InfoQueue draining behind PT_D3D12_VERBOSE_LOG.
2. Delayed DXR runtime object creation until set_prefer_dxr(true), so the compute path does not create unused DXR objects.
3. Reduced the DXR payload from 64 bytes to 56 bytes by packing done/depth/specular flags into one uint state.
4. Compiled out shadow-ray checks when PT_D3D12_DXR_SHADOW_RAYS=0.
5. Added hardware shadow rays with ACCEPT_FIRST_HIT_AND_END_SEARCH and SKIP_CLOSEST_HIT_SHADER, then made them the default because the measured Cornell path improved and compute already performs shadow occlusion.
```

PIX DXR event-list export with focused RT counters worked with:

```powershell
& 'C:\Program Files\Microsoft PIX\2603.25\pixtool.exe' `
  open-capture 'artifacts\dxr_opt\pix_default_shadow_final\d3d12_dxr_default_shadow_final.wpix' `
  save-event-list 'artifacts\dxr_opt\pix_default_shadow_final\event_list_dxr_default_shadow_final.csv' `
  --counters='*GPU*Time*' `
  --counters='*RT*Ray*Count*' `
  --counters='*RT*Traversal*' `
  --counters='*RT*Hit*Thread*' `
  --counters='*RT*Miss*Thread*'
```

The broad `*RT*`, `*XVE*`, and `*L3*` DXR export caused `pixtool` to return `0x8000ffff`, so keep DXR counter exports narrow.

DXR event-list observations:

```text
Older no-shadow DispatchRays GPU time per sample dispatch: about 3524-3625
Current default-shadow DispatchRays count: 8
Current default-shadow avg DispatchRays GPU time: 1843
Current default-shadow avg RT input/traversal rays: 5.175 million
Current default-shadow avg closest-hit dispatches: 3.004 million
Current default-shadow avg any-hit dispatches: 2.788 million
Current default-shadow avg miss dispatches: 2.171 million
Current default-shadow avg traversal stall: 18.625
Current default-shadow avg traversal step rays: 24.662 million
DXR tonemap dispatch: about 9
```

This says the DXR dispatch itself is substantially faster than the software BVH compute shader on this scene. The shadow-ray path is also closer to the compute renderer's direct-light occlusion behavior, but the DXR image hash still differs from compute. Validate material behavior, culling, SDF/procedural coverage, double-sided/alpha expectations, and sample ordering before treating DXR as fully interchangeable with compute.

DXR optimization surface still open:

```text
1. Correctness parity first: compare DXR and compute outputs scene-by-scene before tuning.
2. Profile build/setup separately: ptbench build_ms is high for DXR, while PIX GPU AS build events are small, so CPU-side shader compile, PSO creation, upload, or scene prep likely dominate startup.
3. Keep static BLAS/TLAS persistent and update/refit only dynamic instances when possible.
4. Consider AS compaction after static BLAS builds if VRAM pressure or cache behavior becomes visible.
5. Move BLAS build source geometry out of upload heaps for production profiling; upload heaps are convenient but not the intended fast GPU-local source path.
6. Audit RAY_FLAG_CULL_BACK_FACING_TRIANGLES. It is fast, but it changes behavior for double-sided materials or scenes that expect backface hits.
7. Split closest-hit shader tables by material class if PIX later shows hit-shader divergence as the dominant cost.
8. Tune PT_D3D12_RAYS_PER_PIXEL for DXR separately from compute. The best batch size may differ because DispatchRays uses RT hardware traversal.
9. For compute, the next meaningful surface is BVH traversal/data layout, such as a wider BVH or fewer stack/global-memory operations. PIX already showed tonemap is not the problem.
10. For DXR, the next meaningful render-time surface is reducing closest-hit work and validating whether any-hit/shadow statistics remain favorable across non-Cornell scenes.
```
[6]: https://www.intel.com/content/www/us/en/products/sku/241598/intel-arc-b580-graphics/specifications.html?utm_source=chatgpt.com "Intel® Arc™ B580 Graphics - Product Specifications"
[7]: https://old.chipsandcheese.com/2025/01/07/digging-into-driver-overhead-on-intels-b580/?utm_source=chatgpt.com "Digging into Driver Overhead on Intel's B580"
[8]: https://www.intel.com/content/www/us/en/developer/tools/graphics-performance-analyzers/download.html?utm_source=chatgpt.com "Download Intel® Graphics Performance Analyzers"
