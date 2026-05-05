# protopt — D3D12 Cornell box path tracer

A disposable Windows/Direct3D 12 experiment for feeling out the larger C++23 path-tracer project without building the full engine.

It opens a Win32 window and runs a **physically-based path tracer entirely in an HLSL pixel shader** — Cornell box scene, next-event estimation, diffuse/glossy/mirror materials, and a slowly orbiting camera. The window title updates every frame with live benchmark metrics.

```
protopt D3D12 | 1280x720 | 476 FPS | 2.1 ms | 0.62 Grays/s | frame 8432 | Qualcomm Adreno X1-85
```

## What's inside

**Rendering**

- Analytic Cornell box (floor, ceiling, back wall, red/blue side walls)
- Area light with next-event estimation (NEE) — explicit shadow ray to a random point on the light each bounce, weighted by the geometric term
- Three material types per surface:
  - `diffuse` — Lambertian, cosine-weighted hemisphere sampling
  - `glossy` — perturbed specular with controllable roughness
  - `mirror` — perfect specular reflection
- Tall box: mirror (`roughness = 0`, `albedo = 0.95 grey`)
- Short box: glossy (`roughness = 0.08`, warm gold tint)
- Up to 8 indirect bounces per path; Russian roulette-style throughput cutoff
- Reinhard tonemapping + gamma 2.2

**Infrastructure**

- Single C++ source file (`main.cpp`) — Win32, D3D12, and the full HLSL shader embedded as a string
- Shader compiled at startup via `D3DCompile` (fxc / `d3dcompiler_47.dll`), target `ps_5_0`
- Resizable swapchain with tearing support
- WARP software fallback if no hardware adapter is found
- Log file written to the working directory for debugging init failures

## Requirements

- Windows 10/11
- Windows SDK with Direct3D 12 headers/libs (`d3d12`, `dxgi`, `d3dcompiler`)
- CMake 3.25+ (a local copy ships in `tooling/cmake/`)
- Visual Studio 17 2022 with C++ workload (clang-cl is used if available, MSVC v143 otherwise)

## Quickstart

```powershell
cd experiments\protopt
powershell -ExecutionPolicy Bypass -File .\run-clangcl.ps1
```

`run-clangcl.ps1` auto-detects clang-cl, falls back to MSVC v143, configures CMake, builds Release, and launches the exe. The window appears as soon as the shader compiles (~5 seconds on first run).

## Manual build

```powershell
cd experiments\protopt
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cd build\Release
.\protopt.exe
```

## Debugging

Check `protopt_log.txt` in the working directory if the window doesn't appear. It logs:

- D3D12 adapter selection
- Shader compile errors and warnings (line numbers point into `protopt_embedded.hlsl`)
- Swapchain resize events
- Shutdown

Common failure: shader compile error `X3511` means fxc tried to unroll a loop and gave up. Make sure any `for` loop in the HLSL has a `[loop]` attribute, not `[unroll]`.

## Architecture notes

This is **not** the scalable engine. It's a single-file spike to validate the D3D12 rendering loop before building the real module structure described in `docs/plan.md`.

Intentionally absent:
- ECS / scene graph
- Asset system
- UI panels
- Shader cache
- Accumulation buffer / temporal reprojection
- Hardware ray tracing

The path from here to the full project is described in [docs/plan.md](../../docs/plan.md).
