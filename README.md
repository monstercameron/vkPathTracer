# vkPathTracer

Started life as a WebGL path tracer in a browser tab. Now it's growing up.

The goal is to prove the same idea — interactive path-traced demo meets rigorous render benchmark — can run as a proper native C++ program instead of a pile of JavaScript fighting the browser for GPU time. Same spirit, completely different foundation.

---

## What it is

Two things in one box:

**A demo toy.** Real-time path tracing with editable scenes, materials, lights, physics, scripting, debug views, and a camera you can actually control. The kind of thing you load up to show someone what path tracing looks like and end up playing with for an hour.

**A benchmark platform.** Deterministic, headless-capable, reproducible. Fixed seed, fixed scene hash, structured output — JSON, CSV, EXR, PNG diffs against reference images. The kind of thing you wire into CI and trust the numbers.

Neither mode is an afterthought. Both are first-class.

The current native focus is the Qt editor shell, the Windows D3D12 path, and the CPU preview/reference path. The important recent work is making those pieces cooperate without the UI thread being held hostage by heavy render, physics, or scripting work.

---

## What it runs on

Native backends: Vulkan, D3D12, Metal.  
Web target: C++ → WebAssembly via Emscripten + WebGPU/WGSL.  
CPU paths: scalar reference, tiled preview rendering, and AVX2 where enabled.
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
- OBJ and glTF/GLB asset import/expansion with deterministic import hashes
- Qt editor shell with native docks, menus, status bar, inspector, asset browser, transform gizmos, selection overlays, and persisted dock layout
- Background render coordination for preview mode: camera/settings updates and dynamic instance transforms can be posted without rebuilding the whole scene

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
README.md                  project overview and quickstart
docs/GETTING_STARTED.md    operational setup, presets, agent workflow
src/                       engine, runtime, GPU backends, Qt shell, scripting, scene
assets/scenes/             scene JSON fixtures
assets/scripts/            Lua scripts (systems/ user/ test/ buckets)
tests/                     smoke targets
experiments/               throwaway spikes and feel-tests
```

---

## Getting Started

### Prerequisites

For a more operational setup guide, including download links, expected Windows
tool paths, smoke commands, and code-agent workflow notes, see
[`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md).

| Tool | Minimum | Notes |
|------|---------|-------|
| CMake | 3.25 | `cmake --version` to check |
| Ninja | any | `ninja --version` |
| Clang / clang++ | 16+ | MSVC works as a fallback on Windows |
| C++ std | 23 | set automatically by the presets |
| Qt | 6.x | Optional; required only for `PT_ENABLE_QT=ON` / Qt presets |

On Windows you can get Clang and Ninja through the **LLVM installer** or via Visual Studio's "Clang tools for Windows" component. CMake is available from cmake.org.

For Qt builds, install a Qt 6 kit that matches the compiler ABI you are using. On Windows, the MSVC 64-bit Qt kit is the expected match for `clang-cl`; either run CMake from the Qt developer environment or set `Qt6_DIR` to the kit's `lib/cmake/Qt6` directory. Missing runtime plugins are a deployment problem, not a renderer problem; the Qt presets run `windeployqt` after link to drop the plugin set next to `ptapp.exe`.

---

### Build

The project uses **CMake presets**. Pick the one that matches your platform:

| Preset | Platform | Config |
|--------|----------|--------|
| `desktop-clang-debug` | Any (default) | Debug, Clang |
| `desktop-clang-release` | Any | Release, Clang |
| `desktop-clang-qt-debug` | Any | Debug, Clang, Qt shell |
| `desktop-clang-qt-release` | Any | Release, Clang, Qt shell |
| `desktop-clang-benchmark` | Any | Release + benchmark targets |
| `windows-clangcl-d3d12-debug` | Windows | Debug, D3D12 backend |
| `windows-clangcl-d3d12-qt-debug` | Windows | Debug, D3D12 backend + Qt shell |
| `windows-clangcl-d3d12-qt-release` | Windows | Release, D3D12 backend + Qt shell |
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

Qt shell build:

```sh
cmake --preset desktop-clang-qt-debug
cmake --build --preset desktop-clang-qt-debug --target ptapp
```

Windows D3D12 + Qt shell build:

```sh
cmake --preset windows-clangcl-d3d12-qt-debug
cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp
```

The benchmark presets intentionally stay Qt-free unless a preset explicitly opts in. `ptapp --render`, `ptapp --headless`, `ptapp --doctor`, and benchmark runs must not require `QApplication` or Qt platform plugins.

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
  --platform raw \
  --window-width 1280 --window-height 720 \
  --scene assets/scenes/cornell_native.json \
  --backend vulkan
```

The window accumulates samples continuously. Close it to exit.

On Windows, replace the path with `.\build\presets\desktop-clang-debug\bin\ptapp.exe` (or use the MSVC `build/bin/Debug/ptapp.exe` output if you built through Visual Studio).

Platform shell selection:

```sh
# Explicit raw/native desktop shell
ptapp --window --platform raw --scene assets/scenes/cornell_native.json

# Force headless platform selection
ptapp --headless --platform headless

# Qt shell (compile-time gated; requires PT_ENABLE_QT=ON)
ptapp --window --platform qt
```

The `--platform` flag controls the window/platform shell only; render backend selection remains separate via `--backend`.

Runtime defaults can also come from a local `.env` file in the working directory. `.env` is loaded before `PTAPP_*` process environment variables are applied, so the precedence is defaults, `--config`, `.env`, process environment, then explicit CLI flags. Use `--env-file <path>` to load a specific dotenv file, or `--no-env-file` to disable auto-loading. Copy [`.env.example`](.env.example) to `.env` for common Qt/D3D12 launcher defaults.

Qt CPU preview example:

```sh
./build/presets/desktop-clang-qt-debug/bin/ptapp \
  --window \
  --platform qt \
  --backend cpu \
  --scene assets/scenes/cornell_native.json \
  --window-width 1280 --window-height 720 \
  --ui-present-hz 60
```

Windows D3D12 Qt preview example:

```powershell
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe `
  --window `
  --platform qt `
  --backend d3d12 `
  --scene assets\scenes\cornell_native.json `
  --window-width 1280 --window-height 720 `
  --ui-present-hz 60
```

The Qt path is a platform/editor shell. Renderer backends receive native handles and capability descriptors; they must not receive `QWidget*`, `QWindow*`, or other Qt objects. The preview path publishes throttled RGBA8 display frames into a latest-wins viewport handoff; `--ui-present-hz` controls the UI/motion cadence and preview publish cap and defaults to 60 Hz. Heavy preview rendering runs behind a render coordinator so the Qt/Win32 event loop can keep processing input. Camera changes, render settings, and dynamic instance transform updates are coalesced and posted to the render side instead of forcing full scene reloads. The D3D12 path uses the Qt viewport only as a native presentation surface. The Qt shell wraps the viewport in a `QMainWindow` with a native menu bar, native status bar, and dock widgets generated from the editor models. In the Qt preview, left-click selects a pickable object through front-facing mesh triangles and shows its projected 3D bounding box with translate, rotate, scale, and universal gizmo handles; `T`/`R`/`S`/`G` switch gizmo modes. Dragging the selected object or bounds directly across the scene is disabled; use gizmo handles for transforms. Right or middle drag orbits the camera, the mouse wheel dollies, and `F` toggles FPS camera mode with `W`/`A`/`S`/`D` plus `Q`/`E` movement.

On Windows Qt builds, `ptapp.exe` uses the GUI subsystem and does not open a separate terminal by default. Pass `--console` or `--terminal` only when you want interactive stdout/stderr diagnostics.

Native Qt docks and the native `QStatusBar` are wired. The shell shows scene graph, inspector, materials, lights, camera, render settings, benchmark, diagnostics, performance, debug views, asset browser, timeline, scripting, and physics docks, with dock layout persisted through `QSettings`.

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

### Qt troubleshooting quick hits

| Symptom | First checks |
|---------|--------------|
| Missing Qt platform plugin | Confirm the Qt kit is deployed beside the executable or `QT_PLUGIN_PATH` points at the kit plugins directory. On Windows, run `windeployqt` for distributable builds. |
| Black Qt viewport | Confirm the selected backend is producing frames, the CPU preview path receives non-empty RGBA8 data, and the viewport is not being repainted over a native swapchain. |
| Stale native handle | Reacquire the native handle after viewport creation and after surface recreation; never cache handles across widget destruction. |
| High-DPI mismatch | Compare logical widget size to physical framebuffer size and reset accumulation on physical-size changes. |
| Render thread does not stop | Close must signal render stop, join the render thread, flush the backend, then destroy Qt widgets. |
| D3D12 device removed | Log the device-removed reason, tear down swapchain resources before the Qt surface dies, and allow WARP or a clean fallback where supported. |

If a problem isn't covered above, run `ptapp --doctor --console --no-env-file` and inspect the structured JSON in `artifacts/`. Crashes write a self-contained bundle under `artifacts/crashes/<timestamp>/` (build_info, crash_state, last_log_events, ui_events, scene/runtime/resource state) — that's the primary diagnostic surface.

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
  --env-file <path>     Load .env variables before config/env resolution
  --no-env-file         Do not auto-load .env from the working directory
  --list-backends       Print known render backends and capabilities
  --list-accelerators   Print accelerator capability and ray budget plan
  --list-gpus           Enumerate Vulkan physical devices and select the best
  --headless            Initialize headless platform
  --window              Open desktop window and keep app running
  --platform <name>     Select platform: auto|raw|qt|headless
  --window-width <px>   Window width (default 1280)
  --window-height <px>  Window height (default 720)
  --ui-present-hz <hz>  UI/motion and preview publish rate (1..120, default 60)
  --frames <n>          Exit window mode after n frames (GUI smoke)
  --exit                Exit window mode after one frame unless --frames is set
  --console, --terminal Attach or create a console for GUI diagnostics
  --scene <path>        Set startup scene
  --backend <name>      Select backend (auto picks D3D12+DXR / Vulkan / CPU)
  --gpus <N>            Use N GPUs (0 = auto-detect; default 1)
  --include-integrated  When --gpus > 1, include integrated adapters (default skips < 1 GiB VRAM)
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
├── Asset import (OBJ, glTF/GLB metadata, mesh, material, texture, animation)
├── Renderer backends
│   ├── Vulkan
│   ├── D3D12 compute + DXR runtime objects
│   ├── Metal
│   └── WebGPU / WGSL (via Emscripten)
├── Path tracer (scalar CPU, tiled CPU, AVX2, NEE, BSDF sampling, multiple material models)
├── Render coordinator (latest-wins frame handoff, coalesced scene/camera/settings commands)
├── Physics (Jolt, deterministic mode available)
├── Scripting (Lua)
├── Benchmark harness (structured JSON/CSV/EXR output)
└── Editor shell (scene graph, inspector, gizmos, timeline)
```

**Hard separation rules** — scene code never sees Vulkan/D3D12/Metal internals; material descriptors never reference native GPU objects; benchmark code never depends on editor UI; renderer backends never own scene identity.

Qt follows the same rule: it is a platform/editor shell only. Qt code may own widgets, menus, docks, dialogs, event translation, and native surface exposure; engine code owns lifecycle, modes, scene state, command replay, render contracts, diagnostics, and benchmarks. Scene, material, path tracer, benchmark, and backend-neutral render interfaces must remain Qt-free.

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

Active native prototype. The single-file D3D12 spike in [`experiments/protopt/`](experiments/protopt/) is now historical context; the current app has a CMake-driven `ptapp`, Qt/raw/headless platform paths, CPU preview/reference rendering, D3D12 (compute + DXR) and Vulkan backends, frames-in-flight GPU pipelining, multi-GPU dispatch plumbing, Jolt physics, Lua scripting (sandboxed, instruction + memory + wall-clock budgets), asset import, editor models, benchmark tooling, and self-diagnostics. Auto backend detection picks D3D12+DXR when available, Vulkan next, CPU last.

For setup recipes, agent-friendly verification commands, and per-platform troubleshooting see [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md). Project history is in `git log`.
