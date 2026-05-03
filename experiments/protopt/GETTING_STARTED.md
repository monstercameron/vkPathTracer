# Getting Started (protopt)

## What this is
This folder contains a D3D12 path-tracing Cornell box experiment built as a single executable:
`protopt`.

## Paths used by the workflow
- Project root: `C:\Users\mreca\repos\vkPathTracer\experiments\protopt`
- Bootstrap script: `C:\Users\mreca\repos\vkPathTracer\experiments\protopt\bootstrap-tooling.ps1`
- CMake binary used: `C:\Users\mreca\repos\vkPathTracer\experiments\protopt\tooling\cmake\bin\cmake.exe`
- Run script used: `C:\Users\mreca\repos\vkPathTracer\experiments\protopt\run-clangcl.ps1`
- Built executable: `C:\Users\mreca\repos\vkPathTracer\experiments\protopt\build\Release\protopt.exe`
- Source: `C:\Users\mreca\repos\vkPathTracer\experiments\protopt\main.cpp`

## Prerequisites
1. Visual Studio 2022 installed.
2. PowerShell.
3. Visual Studio components for C++:
   - Desktop development with C++ workload
   - Windows 10/11 SDK for D3D12 development
4. ClangCL availability for `run-clangcl.ps1` (if absent it can fall back to MSVC v143).

## Install dependencies
Run these commands once on a fresh checkout:

```powershell
cd C:\Users\mreca\repos\vkPathTracer\experiments\protopt

# Download and install local CMake (used by every build command below)
powershell -ExecutionPolicy Bypass -File .\bootstrap-tooling.ps1

# Optional: reset tooling and re-download CMake
# .\bootstrap-tooling.ps1 -Mode clean
```

What `bootstrap-tooling.ps1` does:
- Creates `tooling\` and installs CMake to `tooling\cmake`.
- Fetches the latest CMake Windows x86_64 release from GitHub.
- Leaves `tooling\cmake\bin\cmake.exe` available for builds.
- Use `Test-Path .\tooling\cmake\bin\cmake.exe` to confirm installation.

## Build
From an elevated PowerShell prompt (if needed), run:

```powershell
cd C:\Users\mreca\repos\vkPathTracer\experiments\protopt

# Configure (uses local CMake by default)
.\tooling\cmake\bin\cmake.exe -S . -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL

# Build release target
.\tooling\cmake\bin\cmake.exe --build build --config Release
```

If ClangCL is unavailable on your machine, `run-clangcl.ps1` can fall back to MSVC; the same build command above still applies when configured with that toolchain.

## Run
From the same folder:

```powershell
.\build\Release\protopt.exe
```

Or use the helper script:

```powershell
powershell -ExecutionPolicy Bypass -File .\run-clangcl.ps1
```

## Notes
- Render settings and scene logic live in `main.cpp`.
- `protopt` accumulates frames over time for convergence.
- Stop the app by closing the window or pressing `Esc`.
- Kill stale processes if needed:
  - `Get-Process protopt -ErrorAction SilentlyContinue | Stop-Process -Force`
