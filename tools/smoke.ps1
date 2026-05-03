#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Integration smoke test for vkPathTracer (A16).
    Configures, builds, and validates a minimal end-to-end run of ptapp.

.PARAMETER Preset
    CMake preset to use (default: desktop-clang-release).

.PARAMETER Width
    Render width for the smoke render (default: 64).

.PARAMETER Height
    Render height for the smoke render (default: 64).

.PARAMETER Spp
    Samples per pixel for the smoke render (default: 4).

.PARAMETER NoBuild
    Skip configure and build; assume binary already exists.

.EXAMPLE
    pwsh tools/smoke.ps1
    pwsh tools/smoke.ps1 -Preset desktop-clang-debug -NoBuild
#>
param(
    [string]$Preset = "desktop-clang-release",
    [uint32]$Width  = 64,
    [uint32]$Height = 64,
    [uint32]$Spp    = 4,
    [switch]$NoBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot   = Split-Path -Parent $PSScriptRoot
$BuildDir   = Join-Path $RepoRoot "build" "smoke_$Preset"
$BinDir     = Join-Path $BuildDir "bin"
$Ptapp      = Join-Path $BinDir "ptapp"
if ($IsWindows -or $env:OS -eq "Windows_NT") { $Ptapp += ".exe" }
$StatusFile = Join-Path $RepoRoot "artifacts" "status" "latest_status.json"
$SmokeExr   = Join-Path $RepoRoot "artifacts" "smoke_out.exr"

$pass = 0
$fail = 0

function Step([string]$name, [scriptblock]$body) {
    Write-Host -NoNewline "  [$name] "
    try {
        & $body
        Write-Host "[ok]" -ForegroundColor Green
        $script:pass++
    } catch {
        Write-Host "[FAIL] $_" -ForegroundColor Red
        $script:fail++
    }
}

Push-Location $RepoRoot
try {
    Write-Host "`n=== vkPathTracer smoke test (preset: $Preset) ===`n"

    # ---- 1. Configure -------------------------------------------------------
    if (-not $NoBuild) {
        Step "configure" {
            cmake --preset $Preset -B $BuildDir 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "cmake configure exited $LASTEXITCODE" }
        }

        # ---- 2. Build -------------------------------------------------------
        Step "build" {
            cmake --build $BuildDir --target ptapp 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "cmake build exited $LASTEXITCODE" }
        }
    }

    # ---- 3. Binary exists ---------------------------------------------------
    Step "binary-exists" {
        if (-not (Test-Path $Ptapp)) { throw "ptapp not found at $Ptapp" }
    }

    # ---- 4. --version -------------------------------------------------------
    Step "version" {
        $out = & $Ptapp --version 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited $LASTEXITCODE" }
        if (-not ($out -match '\d+\.\d+')) { throw "version output malformed: $out" }
    }

    # ---- 5. --doctor --------------------------------------------------------
    Step "doctor" {
        $out = & $Ptapp --doctor 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited $LASTEXITCODE" }
        if (-not ($out -match '\[ok\]' -or $out -match 'check')) {
            throw "doctor output unexpected: $out"
        }
    }

    # ---- 6. --render (minimal) ----------------------------------------------
    Step "render" {
        $out = & $Ptapp --render --width $Width --height $Height --spp $Spp `
                        --exr-output $SmokeExr 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited $LASTEXITCODE: $out" }
        if (-not ($out -match 'render complete')) { throw "render output missing 'render complete'" }
    }

    # ---- 7. Status file -----------------------------------------------------
    Step "status-file" {
        if (-not (Test-Path $StatusFile)) { throw "status file not found: $StatusFile" }
        $json = Get-Content $StatusFile -Raw | ConvertFrom-Json
        if ($json.last_run_status -ne "render_ok" -and
            $json.last_run_status -ne "ok") {
            throw "unexpected last_run_status: $($json.last_run_status)"
        }
    }

    # ---- 8. EXR output file exists ------------------------------------------
    Step "exr-output" {
        if (-not (Test-Path $SmokeExr)) { throw "EXR output not found: $SmokeExr" }
        $size = (Get-Item $SmokeExr).Length
        if ($size -lt 100) { throw "EXR file suspiciously small: $size bytes" }
    }

    # ---- 9. --dump-config ---------------------------------------------------
    Step "dump-config" {
        $out = & $Ptapp --dump-config 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited $LASTEXITCODE" }
        if (-not ($out -match '"backend"')) { throw "dump-config output missing 'backend'" }
    }

    # ---- Summary ------------------------------------------------------------
    Write-Host ""
    if ($fail -eq 0) {
        Write-Host "PASSED ($pass/$($pass+$fail) checks)" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "FAILED ($fail/$($pass+$fail) checks)" -ForegroundColor Red
        exit 1
    }
} finally {
    Pop-Location
}
