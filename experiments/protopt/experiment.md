Created the one-hour prototype slice under:

```text
experiments/protopt
```

Download it here:

[Download `protopt_experiment.zip`](sandbox:/mnt/data/protopt_experiment.zip)

Individual files:

* [`main.cpp`](sandbox:/mnt/data/experiments/protopt/main.cpp)
* [`CMakeLists.txt`](sandbox:/mnt/data/experiments/protopt/CMakeLists.txt)
* [`CMakePresets.json`](sandbox:/mnt/data/experiments/protopt/CMakePresets.json)
* [`README.md`](sandbox:/mnt/data/experiments/protopt/README.md)
* [`run-clangcl.ps1`](sandbox:/mnt/data/experiments/protopt/run-clangcl.ps1)

This stays aligned with the larger plan’s C++/Windows/D3D12 direction while intentionally avoiding ECS, asset loading, UI panels, shader caches, scene schemas, and scalable abstractions for now. The main project plan explicitly targets C++23, Windows D3D12, window/input abstraction, renderer lifecycle, benchmark instrumentation, and early viewport/render loop delivery, so this experiment is scoped as a disposable feel-test for that path. 

## What it does

```text
Win32 window
Direct3D 12 device/swapchain
embedded HLSL shader
fullscreen triangle
pixel-shader ray/sphere intersection
rotating camera
simple floor/contact shadow
title-bar benchmark metrics
basic log file
resizable swapchain
WARP fallback if hardware adapter is unavailable
```

The window title updates with:

```text
FPS
ms/frame
approx primary Grays/s
CPU frame ms
resolution
frame index
adapter name
```

Example title:

```text
protopt D3D12 | 1280x720 | 850.0 FPS | 1.176 ms | 0.783 primary Grays/s | CPU frame 1.201 ms | frame 421 | NVIDIA ...
```

The app uses the standard D3D12 shape of device, command queue, swap chain, command list, render target, and fence synchronization. Microsoft’s D3D12 guide describes that basic device/swap-chain/command-queue setup, and D3D12 fences are the API’s synchronization primitive for CPU/GPU queue coordination. ([Microsoft Learn][1])

For speed and simplicity, the shader is embedded in `main.cpp` and compiled at startup with `D3DCompile`, which Microsoft documents as compiling HLSL source into bytecode for a target profile. This keeps the prototype self-contained. ([Microsoft Learn][2])

## Build

From a Windows Developer PowerShell:

```powershell
cd experiments\protopt
cmake --preset vs2022-clangcl-release
cmake --build --preset vs2022-clangcl-release
.\build\vs2022-clangcl\Release\protopt.exe
```

Or use the helper:

```powershell
cd experiments\protopt
.\run-clangcl.ps1
```

Ninja path:

```powershell
cd experiments\protopt
cmake --preset ninja-clangcl-release
cmake --build --preset ninja-clangcl-release
.\build\ninja-clangcl\protopt.exe
```

## Debugging

It writes a basic log file:

```text
protopt_log.txt
```

That log records:

```text
startup
D3D12 init stages
adapter selection
shader compiler output
resize events
fatal errors
shutdown
```

I could not compile/run it inside this Linux container because it depends on Win32 + Direct3D 12 headers/libs, but the scaffold is set up for Windows + clang-cl/MSVC.

[1]: https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-basic-direct3d-12-component?utm_source=chatgpt.com "Creating a basic Direct3D 12 component - Win32 apps"
[2]: https://learn.microsoft.com/en-us/windows/win32/api/d3dcompiler/nf-d3dcompiler-d3dcompile?utm_source=chatgpt.com "D3DCompile function (d3dcompiler.h) - Win32 apps"
