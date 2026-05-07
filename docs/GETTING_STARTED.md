# Getting Started

This document is written for humans and code agents. It prioritizes exact commands,
known paths, verification checks, and the places where this repository expects
toolchains to live.

## Fast Path: Windows D3D12 + Qt

This is the richest local development path for the current editor shell.

```powershell
cmake --preset windows-clangcl-d3d12-qt-debug
cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp

.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe `
  --window `
  --platform qt `
  --backend d3d12 `
  --scene assets\scenes\cornell_native.json `
  --window-width 1280 --window-height 720 `
  --ui-present-hz 60
```

Useful non-GUI verification commands:

```powershell
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --doctor --platform headless --console --no-env-file
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --ui-model-smoke --platform headless --console --no-env-file
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --dynamic-physics-gate --platform headless --backend d3d12 --console --no-env-file
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --third-person-script-gate --platform headless --backend d3d12 --console --no-env-file
```

## Required Tools

Minimum baseline:

| Tool | Required For | Expected Version | Official Download |
|------|--------------|------------------|-------------------|
| CMake | configure/build | 3.25+ | https://cmake.org/download/ |
| Ninja | CMake presets | current release is fine | https://github.com/ninja-build/ninja/releases |
| LLVM/Clang | default presets | Clang 16+ | https://releases.llvm.org/download.html |
| Visual Studio Build Tools | Windows SDK, MSVC libraries, `clang-cl` link environment | VS 2022 recommended | https://visualstudio.microsoft.com/downloads/ |
| Qt | Qt editor/window presets | Qt 6.x, MSVC 64-bit kit on Windows | https://doc.qt.io/qt-6/get-and-install-qt.html |
| Vulkan SDK | Vulkan presets and shader compilation | SDK matching local preset path or adjusted cache var | https://vulkan.lunarg.com/sdk/home |

Optional, depending on what you are building:

| Tool | Required For | Download |
|------|--------------|----------|
| Emscripten | `web-emscripten-webgpu-debug` | https://emscripten.org/docs/getting_started/downloads.html |
| PIX on Windows | GPU debugging/profiling D3D12 work | https://devblogs.microsoft.com/pix/download/ |

## Windows Setup Notes

Install these components:

1. CMake, with `cmake.exe` on `PATH`.
2. Ninja, with `ninja.exe` on `PATH`.
3. LLVM, preferably installed to `C:\Program Files\LLVM`.
4. Visual Studio 2022 Build Tools with:
   - Desktop development with C++
   - MSVC v143 build tools
   - Windows 10 or Windows 11 SDK
   - C++ Clang tools for Windows, if using Visual Studio's Clang instead of the LLVM installer
5. Qt 6 MSVC 64-bit kit. The checked-in Qt presets assume:

```text
C:\Qt\6.7.3\msvc2019_64
```

If Qt is installed somewhere else, override the preset path during configure:

```powershell
cmake --preset windows-clangcl-d3d12-qt-debug -DCMAKE_PREFIX_PATH=C:\Qt\6.x.y\msvc2019_64
```

The Windows Clang presets assume LLVM tools at:

```text
C:\Program Files\LLVM\bin\clang.exe
C:\Program Files\LLVM\bin\clang++.exe
C:\Program Files\LLVM\bin\clang-cl.exe
C:\Program Files\LLVM\bin\llvm-rc.exe
```

If your LLVM install differs, pass compiler cache variables explicitly or create
a local CMake user preset. Do not edit `CMakePresets.json` just to point at a
machine-local toolchain.

## Presets

Use CMake presets first. They encode the intended feature switches and output
directories.

| Preset | Use |
|--------|-----|
| `desktop-clang-debug` | portable CPU/debug build |
| `desktop-clang-release` | portable CPU/release build |
| `desktop-clang-qt-debug` | Qt shell without D3D12-specific Windows preset |
| `desktop-clang-benchmark` | benchmark target build |
| `windows-clang-vulkan-debug` | Windows Vulkan backend |
| `windows-clangcl-d3d12-debug` | Windows D3D12 backend, no Qt |
| `windows-clangcl-d3d12-qt-debug` | Windows D3D12 backend + Qt editor shell |
| `windows-clangcl-d3d12-qt-release` | Windows optimized D3D12 + Qt |
| `linux-clang-vulkan-debug` | Linux Vulkan backend |
| `macos-clang-metal-debug` | macOS Metal backend |
| `web-emscripten-webgpu-debug` | WebGPU/Wasm experiment |
| `headless-benchmark-release` | deterministic benchmark-oriented build |

List available presets:

```sh
cmake --list-presets
cmake --build --list-presets
```

## Build Recipes

Portable desktop debug:

```sh
cmake --preset desktop-clang-debug
cmake --build --preset desktop-clang-debug --target ptapp
```

Windows D3D12 no Qt:

```powershell
cmake --preset windows-clangcl-d3d12-debug
cmake --build --preset windows-clangcl-d3d12-debug --target ptapp
```

Windows D3D12 + Qt:

```powershell
cmake --preset windows-clangcl-d3d12-qt-debug
cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp
```

Benchmark target:

```sh
cmake --preset desktop-clang-benchmark
cmake --build --preset desktop-clang-benchmark --target ptbench
```

Output binaries are under:

```text
build/presets/<preset-name>/bin/
```

## Running

Headless render:

```powershell
.\build\presets\windows-clangcl-d3d12-debug\bin\ptapp.exe `
  --render `
  --backend d3d12 `
  --scene assets\scenes\cornell_native.json `
  --width 1280 --height 720 `
  --spp 16 `
  --max-depth 6 `
  --output artifacts\cornell.png
```

Headless platform smoke:

```powershell
.\build\presets\windows-clangcl-d3d12-debug\bin\ptapp.exe --headless --platform headless --console --no-env-file
```

Qt editor:

```powershell
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe `
  --window `
  --platform qt `
  --backend d3d12 `
  --scene assets\scenes\cornell_native.json
```

On Windows Qt builds, `ptapp.exe` uses the GUI subsystem. Add `--console` only
when you need stdout/stderr diagnostics.

## Local Runtime Defaults

Copy `.env.example` to `.env` for local defaults:

```powershell
Copy-Item .env.example .env
```

Common keys:

```text
PTAPP_BACKEND=d3d12
PTAPP_PLATFORM=qt
PTAPP_SCENE=assets/scenes/cornell_native.json
PTAPP_RENDER_WIDTH=960
PTAPP_RENDER_HEIGHT=540
PTAPP_MAX_DEPTH=2
PTAPP_UI_PRESENT_HZ=60
PTAPP_CONSOLE=false
PT_D3D12_RAYS_PER_PIXEL=8
PT_D3D12_READBACK_INTERVAL=4
```

Precedence is:

```text
defaults < --config < .env < process environment < explicit CLI flags
```

Use `--no-env-file` in automated checks so results are not affected by a local
developer configuration.

## Agent Workflow

Recommended first commands for a code agent:

```powershell
git status --short
cmake --list-presets
rg -n "TODO|FIXME|HACK" src tests docs
```

Before editing:

- Treat existing modified files as user work. Do not revert unrelated changes.
- Prefer the nearest existing preset over inventing a new build directory.
- Prefer `rg`/`rg --files` for code search.
- Use `--no-env-file` for repeatable smoke commands.
- Keep generated artifacts in `artifacts/` or the relevant `build/` tree.

High-signal verification commands:

```powershell
cmake --build --preset windows-clangcl-d3d12-qt-debug --target ptapp
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --ui-model-smoke --platform headless --console --no-env-file
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --dynamic-physics-gate --platform headless --backend d3d12 --console --no-env-file
.\build\presets\windows-clangcl-d3d12-qt-debug\bin\ptapp.exe --third-person-script-gate --platform headless --backend d3d12 --console --no-env-file
```

If the linker reports `permission denied` writing `bin\ptapp.exe`, the previous
GUI process or a transient scanner may still hold the file. Check:

```powershell
Get-Process ptapp -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,Path,StartTime
```

Do not kill processes blindly. If no process is shown, retry the build once.

## Troubleshooting

`cmake --preset ...` cannot find Qt:

- Confirm the installed Qt kit path.
- Pass `-DCMAKE_PREFIX_PATH=<Qt kit root>`.
- For Windows clang-cl presets, use a MSVC 64-bit Qt kit.

`clang-cl` cannot link Windows system libraries:

- Run from a Visual Studio Developer PowerShell, or install the Visual Studio
  Build Tools C++ workload and Windows SDK.

Vulkan preset cannot find SDK:

- Install the Vulkan SDK.
- Update `VULKAN_SDK` or pass `-DVULKAN_SDK=<path>`.

Qt app starts but plugins are missing:

- Rebuild the Qt preset; `windeployqt` runs after linking.
- See `docs/qt_diagnostics.md`.

D3D12 path fails on startup:

- Run `ptapp --doctor --check-backends --console --no-env-file`.
- Confirm the GPU and driver support the requested path.
- Try `--backend cpu` to separate app/runtime issues from GPU backend issues.

## Source Map

Important directories:

| Path | Purpose |
|------|---------|
| `src/app/` | runtime, CLI, Qt editor shell, viewport interaction |
| `src/gpu/` | Vulkan and D3D12 path tracer backends |
| `src/pathtracer/` | CPU path tracing, scene conversion, film/image code |
| `src/scene/` | document/ECS/world state |
| `src/render/` | backend-neutral render coordination and frame handoff |
| `src/platform/` | raw/headless/Qt platform shells |
| `src/editor/` | UI/editor models |
| `assets/scenes/` | scene JSON fixtures |
| `assets/models/` | mesh/model assets |
| `assets/scripts/` | Lua scripts |
| `tests/` | smoke and runtime tests |
| `tools/` | local smoke/audit scripts |
| `artifacts/` | generated logs, benchmark outputs, rendered images |

## Documentation Pointers

- `README.md`: project overview, CLI examples, architecture notes.
- `docs/qt_diagnostics.md`: Qt runtime and plugin diagnostics.
- `docs/qt_threading.md`: Qt/render-thread rules.
- `docs/optimization_guide.md`: performance guidance and benchmark context.
- `docs/ci-smoke-plan.md`: smoke targets and expected checks.
