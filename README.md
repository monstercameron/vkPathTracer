# vkPathTracer

Started life as a WebGL path tracer in a browser tab. Now it's growing up.

The goal is to prove the same idea — interactive path-traced demo meets rigorous render benchmark — can run as a proper native C++ program instead of a pile of JavaScript fighting the browser for GPU time. Same spirit, completely different foundation.

---

## What it is

Two things in one box:

**A demo toy.** Real-time path tracing with editable scenes, materials, lights, physics, scripting, debug views, and a camera you can actually control. The kind of thing you load up to show someone what path tracing looks like and end up playing with for an hour.

**A benchmark platform.** Deterministic, headless-capable, reproducible. Fixed seed, fixed scene hash, structured output — JSON, CSV, EXR, PNG diffs against reference images. The kind of thing you wire into CI and trust the numbers.

Neither mode is an afterthought. Both are first-class.

---

## What it runs on

Native backends: Vulkan, D3D12, Metal.  
Web target: C++ → WebAssembly via Emscripten + WebGPU/WGSL.  
All written in C++23, Clang/LLVM first, MSVC as a fallback.

The web version isn't a JavaScript rewrite. It's the same C++ compiled differently.

---

## What's in the scene

The renderer speaks the usual language:

- Diffuse, glossy, mirror, emissive, and layered materials
- Area lights, point lights, spots, directional, environment, blackbody emitters, mesh emissives
- NEE + MIS sampling, configurable Russian roulette, firefly clamping, caustics policy
- SDF primitives alongside polygon meshes
- Physically-based camera model — focal length, sensor size, aperture, focus distance, shutter, ISO
- Physics via Jolt (deterministic mode available, knowingly costs performance)
- Lua scripting for anything that doesn't belong in C++
- Animation timelines with transform, morph, material, light, and camera tracks

---

## What the benchmark produces

A `BenchmarkResult` is more than a frame time:

```
timing          — GPU and CPU, per-pass
throughput      — paths/sec, samples/sec
memory          — resource budget usage
backend info    — adapter, driver, feature set
build info      — compiler, flags, commit
scene hash      — what exactly was rendered
shader hash     — what variant was running
image hash      — pixel-level identity
reference diff  — pass/fail against golden image with configurable tolerance
```

All of it serialized, all of it reproducible, all of it explainable.

---

## What it is not

Not a game engine. Not an offline production renderer. Not a spectral renderer. Not a USD authoring tool. Not trying to beat V-Ray. 

A cleanly structured, benchmarkable, interactive path-tracing platform that still feels like a demo toy. That's the line.

---

## Where things are

```
plan.md       full architecture spec
experiments/  throwaway spikes and feel-tests
```

---

## Getting Started

### Prerequisites

| Tool | Minimum | Notes |
|------|---------|-------|
| CMake | 3.25 | `cmake --version` to check |
| Ninja | any | `ninja --version` |
| Clang / clang++ | 16+ | MSVC works as a fallback on Windows |
| C++ std | 23 | set automatically by the presets |

On Windows you can get Clang and Ninja through the **LLVM installer** or via Visual Studio's "Clang tools for Windows" component. CMake is available from cmake.org.

---

### Build

The project uses **CMake presets**. Pick the one that matches your platform:

| Preset | Platform | Config |
|--------|----------|--------|
| `desktop-clang-debug` | Any (default) | Debug, Clang |
| `desktop-clang-release` | Any | Release, Clang |
| `desktop-clang-benchmark` | Any | Release + benchmark targets |
| `windows-clangcl-d3d12-debug` | Windows | Debug, D3D12 backend |
| `linux-clang-vulkan-debug` | Linux | Debug, Vulkan backend |
| `macos-clang-metal-debug` | macOS | Debug, Metal backend |

```sh
# 1. Configure
cmake --preset desktop-clang-debug

# 2. Build the app
cmake --build build/presets/desktop-clang-debug --target ptapp

# Binary lands at:
#   build/presets/desktop-clang-debug/bin/ptapp      (Linux/macOS)
#   build/presets/desktop-clang-debug/bin/ptapp.exe  (Windows)
```

Build all targets (app + bench + hello):

```sh
cmake --build build/presets/desktop-clang-debug
```

Release build:

```sh
cmake --preset desktop-clang-release
cmake --build build/presets/desktop-clang-release --target ptapp
```

---

### CLI — headless render

Render the Cornell box to a PNG:

```sh
./build/presets/desktop-clang-debug/bin/ptapp \
  --render \
  --scene assets/scenes/cornell_native.json \
  --backend vulkan \
  --width 1280 --height 720 \
  --spp 64 \
  --max-depth 6 \
  --output render_out.png
```

Also write an EXR alongside the PNG:

```sh
  --exr-output render_out.exr
```

Key render flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--scene <path>` | — | Scene JSON to load |
| `--backend <name>` | — | `vulkan`, `d3d12`, `metal`, `cpu` |
| `--width <px>` | 1280 | Render width |
| `--height <px>` | 720 | Render height |
| `--spp <n>` | 16 | Samples per pixel |
| `--max-depth <n>` | 6 | Maximum ray bounce depth |
| `--output <path>` | — | PNG output path |
| `--exr-output <path>` | — | EXR output path |

---

### GUI — interactive window

Open a live preview window that progressively path-traces the scene at up to 60 FPS (16 ms tracing budget per frame, stochastic pixel order):

```sh
./build/presets/desktop-clang-debug/bin/ptapp \
  --window \
  --window-width 1280 --window-height 720 \
  --scene assets/scenes/cornell_native.json \
  --backend vulkan
```

The window accumulates samples continuously. Close it to exit.

On Windows, replace the path with `.\build\presets\desktop-clang-debug\bin\ptapp.exe` (or use the MSVC `build/bin/Debug/ptapp.exe` output if you built through Visual Studio).

---

### Diagnostics

Check that everything is wired up correctly before rendering:

```sh
# Full self-test (build info, CPU, backends, assets, shaders, job system, scene schema, artifact write)
ptapp --doctor

# Individual checks
ptapp --check-cpu
ptapp --check-backends
ptapp --check-assets

# Print resolved runtime config
ptapp --dump-config

# List available backends and their capability flags
ptapp --list-backends
```

---

### Full flag reference

```
ptapp [options]
  --version             Print build metadata and exit
  --version --json      Print build metadata as JSON
  --doctor              Run full self-diagnostics
  --check-build         Check build metadata
  --check-cpu           Check CPU capabilities
  --check-backends      Check render backends
  --check-assets        Check asset directories
  --check-shaders       Check shader directories
  --check-job-system    Check job system smoke test
  --check-scene-schema  Check scene schema (cornell_native.json)
  --check-bench-write   Check benchmark artifact write
  --dump-config         Print resolved runtime config as JSON
  --config <path>       Load a config file (key=value format)
  --list-backends       Print known render backends and capabilities
  --headless            Initialize headless platform
  --window              Open desktop window and keep app running
  --window-width <px>   Window width (default 1280)
  --window-height <px>  Window height (default 720)
  --scene <path>        Set startup scene
  --backend <name>      Select backend
  --log-level <n>       Select log level
  --crash-test          Simulate a crash and write crash artifacts
  --render              Render using scalar CPU path tracer
  --output <path>       Render output PNG path
  --exr-output <path>   Render output EXR path
  --width <px>          Render width
  --height <px>         Render height
  --spp <samples>       Samples per pixel
  --max-depth <depth>   Max ray depth
```


---

## Project goals

The system operates in two equally important modes:

**Interactive demo mode** — a visually rich, editable path-tracing viewport with scene controls, materials, lights, cameras, physics, scripting, debug views, and image exports.

**Benchmark mode** — a deterministic, headless-capable render harness that produces reproducible, machine-readable results: image artifacts, reference diffs, backend metadata, shader hashes, scene hashes, and performance metrics.

---

## Architecture at a glance

```
Core
├── Platform (Win32 / Wayland / Cocoa / WASM)
├── Application lifecycle + mode switching
├── ECS + phase-scheduled world
├── Scene document → runtime → render proxy pipeline
├── Asset import (mesh, material, texture, animation)
├── Renderer backends
│   ├── Vulkan
│   ├── D3D12
│   ├── Metal
│   └── WebGPU / WGSL (via Emscripten)
├── Path tracer (NEE, BSDF sampling, multiple material models)
├── Physics (Jolt, deterministic mode available)
├── Scripting (Lua)
├── Benchmark harness (structured JSON/CSV/EXR output)
└── Editor shell (scene graph, inspector, gizmos, timeline)
```

**Hard separation rules** — scene code never sees Vulkan/D3D12/Metal internals; material descriptors never reference native GPU objects; benchmark code never depends on editor UI; renderer backends never own scene identity.

---

## Application modes

| Mode | Description |
|------|-------------|
| `DemoMode` | Interactive path-tracing viewport, camera/material/light controls, benchmark panel, PNG export |
| `EditorMode` | Scene graph, inspector, gizmos, asset browser, material editor, command history, timeline, physics authoring |
| `BenchmarkMode` | Headless-capable, deterministic, fixed seed, structured artifact export (JSON/CSV/EXR/PNG) |
| `ValidationMode` | Scene schema, shader variant, asset import, backend capability checks |
| `ReferenceMode` | CPU reference image generation, golden-image update, diff artifact workflow |
| `WebMode` | Browser canvas via WebGPU/WGSL, reduced capability set, browser-safe asset/export flow |

---

## C++ / toolchain baseline

- **Language**: C++23 (C++2c/26 features behind explicit gates)
- **Compiler**: Clang/LLVM first; MSVC as fallback on Windows
- **Web target**: C++ → WebAssembly via Emscripten + WebGPU backend
- **Build system**: CMake 3.25+
- **Physics**: Jolt (deterministic mode opt-in with known performance trade-off)
- **Scripting**: Lua
- **Scene format**: versioned JSON document with explicit migration

---

## Determinism policy

Three explicit modes — not a vague goal:

| Mode | Description |
|------|-------------|
| **Strict** | Fixed seed, stable entity visit order, deferred command buffers, physics determinism on — used for benchmark and tests |
| **Interactive deterministic** | Fixed-step update, stable command ordering, live editing allowed |
| **Free interactive** | Editor/demo convenience over reproducibility |

Physics determinism requires the same binary and the same call order; this is a policy choice per run, not an assumption.

---

## ECS frame schedule

```
PreFrame → EditorInput → ScriptEarly → AnimationSample → PhysicsFixed → ScriptFixed
→ TransformAssembly → SceneCommandApply → RenderExtract → UploadSchedule → PostFrame
```

Transform authority is explicit per entity — Physics, Animation, Script, Editor, and Authored each have declared priority, and conflicts are reported in diagnostics with entity/component/phase/writer detail.

---

## Status

Early prototyping phase. See [`experiments/protopt/`](experiments/protopt/) for the current working spike: a single-file D3D12 Cornell box path tracer with NEE, diffuse/glossy/mirror materials, and a rotating camera — used to validate the D3D12 rendering loop before building the module structure above.

Full architecture is specified in [`plan.md`](plan.md).
