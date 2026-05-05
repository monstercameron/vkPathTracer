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
[6]: https://www.intel.com/content/www/us/en/products/sku/241598/intel-arc-b580-graphics/specifications.html?utm_source=chatgpt.com "Intel® Arc™ B580 Graphics - Product Specifications"
[7]: https://old.chipsandcheese.com/2025/01/07/digging-into-driver-overhead-on-intels-b580/?utm_source=chatgpt.com "Digging into Driver Overhead on Intel's B580"
[8]: https://www.intel.com/content/www/us/en/developer/tools/graphics-performance-analyzers/download.html?utm_source=chatgpt.com "Download Intel® Graphics Performance Analyzers"
