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
$BuildDir   = Join-Path (Join-Path $RepoRoot "build") "smoke_$Preset"
$BinDir     = Join-Path $BuildDir "bin"
$Ptapp      = Join-Path $BinDir "ptapp"
$Ptdoctor   = Join-Path $BinDir "ptdoctor"
$IsWindowsHost = [System.IO.Path]::DirectorySeparatorChar -eq '\'
if ($IsWindowsHost) {
    $Ptapp += ".exe"
    $Ptdoctor += ".exe"
}
$TodoAudit  = Join-Path (Join-Path $RepoRoot "tools") "todo_audit.ps1"
$TodoAuditReport = Join-Path (Join-Path (Join-Path $RepoRoot "artifacts") "status") "todo_audit.json"
$SourceSizeReport = Join-Path (Join-Path $RepoRoot "tools") "source_size_report.ps1"
$SourceSizeReportJson = Join-Path (Join-Path (Join-Path $RepoRoot "artifacts") "status") "source_size_report.json"
$StatusFile = Join-Path (Join-Path (Join-Path $RepoRoot "artifacts") "status") "latest_status.json"
$SmokeExr   = Join-Path (Join-Path $RepoRoot "artifacts") "smoke_out.exr"

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

function Find-CompilerPath([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $cmd -and -not [string]::IsNullOrWhiteSpace($cmd.Source)) {
        return $cmd.Source
    }

    if ($IsWindowsHost) {
        $candidates = @()
        if (-not [string]::IsNullOrWhiteSpace($env:LLVM_INSTALL_DIR)) {
            $candidates += (Join-Path (Join-Path $env:LLVM_INSTALL_DIR "bin") "$Name.exe")
        }
        $candidates += (Join-Path "C:\Program Files\LLVM\bin" "$Name.exe")
        foreach ($candidate in $candidates) {
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    return $null
}

function ConvertTo-CMakePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $Path
    }
    return $Path.Replace('\', '/')
}

function Find-WindowsResourceCompilerPath {
    $kitsRoot = "C:\Program Files (x86)\Windows Kits\10\bin"
    if (-not (Test-Path -LiteralPath $kitsRoot)) {
        return $null
    }

    $versions = Get-ChildItem -LiteralPath $kitsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending
    foreach ($version in $versions) {
        $candidate = Join-Path (Join-Path $version.FullName "x64") "rc.exe"
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return $null
}

function Get-CMakeConfigureArgs {
    $args = @("--preset", $Preset, "-B", $BuildDir)
    if ($Preset -like "*clang*") {
        $cxx = Find-CompilerPath "clang++"
        $cc = Find-CompilerPath "clang"
        $rc = Find-CompilerPath "llvm-windres"
        if ($IsWindowsHost -and [string]::IsNullOrWhiteSpace($rc)) {
            $rc = Find-WindowsResourceCompilerPath
        }
        if (-not [string]::IsNullOrWhiteSpace($cxx)) {
            $args += "-DCMAKE_CXX_COMPILER=$(ConvertTo-CMakePath $cxx)"
        }
        if (-not [string]::IsNullOrWhiteSpace($cc)) {
            $args += "-DCMAKE_C_COMPILER=$(ConvertTo-CMakePath $cc)"
        }
        if ($IsWindowsHost -and -not [string]::IsNullOrWhiteSpace($rc)) {
            $args += "-DCMAKE_RC_COMPILER=$(ConvertTo-CMakePath $rc)"
        }
    }
    return $args
}

Push-Location $RepoRoot
try {
    Write-Host "`n=== vkPathTracer smoke test (preset: $Preset) ===`n"

    # ---- 1. Configure -------------------------------------------------------
    if (-not $NoBuild) {
        Step "configure" {
            $configureArgs = Get-CMakeConfigureArgs
            $oldErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            try {
                $out = cmake @configureArgs 2>&1
                $code = $LASTEXITCODE
            } finally {
                $ErrorActionPreference = $oldErrorActionPreference
            }
            if ($code -ne 0) { throw "cmake configure exited ${code}: $out" }
        }

        # ---- 2. Build -------------------------------------------------------
        Step "build" {
            cmake --build $BuildDir --target ptapp 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "cmake build exited $LASTEXITCODE" }
            cmake --build $BuildDir --target ptdoctor 2>&1 | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "cmake ptdoctor build exited $LASTEXITCODE" }
        }
    }

    # ---- 3. Binary exists ---------------------------------------------------
    Step "binary-exists" {
        if (-not (Test-Path $Ptapp)) { throw "ptapp not found at $Ptapp" }
        if (-not (Test-Path $Ptdoctor)) { throw "ptdoctor not found at $Ptdoctor" }
    }

    # ---- 4. --version -------------------------------------------------------
    Step "version" {
        $out = & $Ptapp --version 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited $LASTEXITCODE" }
        if (-not ($out -match '\d+\.\d+')) { throw "version output malformed: $out" }
    }

    # ---- 5. --doctor --------------------------------------------------------
    Step "doctor" {
        $out = & $Ptdoctor 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited $LASTEXITCODE" }
        if (-not ($out -match '\[ok\s*\]' -or $out -match 'doctor: ok')) {
            throw "doctor output unexpected: $out"
        }
    }

    # ---- 6. --render (minimal) ----------------------------------------------
    Step "render" {
        $out = & $Ptapp --render --width $Width --height $Height --spp $Spp `
                        --exr-output $SmokeExr 2>&1
        if ($LASTEXITCODE -ne 0) { throw "exited ${LASTEXITCODE}: $out" }
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

    # ---- 10. docs/todos.md audit ------------------------------------------
    Step "todo-audit" {
        if (-not (Test-Path $TodoAudit)) { throw "todo audit tool not found: $TodoAudit" }
        $out = & $TodoAudit -Output $TodoAuditReport 2>&1
        if (-not $?) { throw "todo audit failed: $out" }
        if (-not (Test-Path -LiteralPath $TodoAuditReport)) { throw "todo audit report missing: $TodoAuditReport" }
        $auditJson = Get-Content -LiteralPath $TodoAuditReport -Raw | ConvertFrom-Json
        if ($auditJson.status -ne "ok") { throw "todo audit status unexpected: $($auditJson.status)" }
    }

    # ---- 11. Warning-only source size guardrails ---------------------------
    Step "source-size-guardrails" {
        if (-not (Test-Path $SourceSizeReport)) { throw "source size report tool not found: $SourceSizeReport" }
        $out = & $SourceSizeReport -Output $SourceSizeReportJson -CheckGuardrails 2>&1
        if (-not $?) { throw "source size report failed: $out" }
        if (-not (Test-Path -LiteralPath $SourceSizeReportJson)) {
            throw "source size report missing: $SourceSizeReportJson"
        }
        $sizeJson = Get-Content -LiteralPath $SourceSizeReportJson -Raw | ConvertFrom-Json
        if ($sizeJson.schema -ne "vkpt.source_size_report.v1") {
            throw "source size report schema unexpected: $($sizeJson.schema)"
        }
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
