$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$toolingCmake = Join-Path $PSScriptRoot "tooling\cmake\bin\cmake.exe"
if (-not (Test-Path $toolingCmake)) {
  throw "Required tool not found: $toolingCmake. Run .\bootstrap-tooling.ps1 to download local CMake."
}

$toolset = "ClangCL"
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
  Write-Host "ClangCL toolchain unavailable. Falling back to MSVC v143."
  $toolset = "v143"
}

& $toolingCmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T $toolset
& $toolingCmake --build build --config Release
& .\build\Release\protopt.exe
