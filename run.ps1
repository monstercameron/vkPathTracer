param(
    [switch]$NoBuild,
    [string]$Preset = "windows-clangcl-d3d12-qt-debug",
    [string]$Scene = "assets\scenes\cornell_native.json",
    [string]$Backend = "d3d12-dxr",
    [string]$D3D12AcceleratorPreset = "high-performance",
    [string]$D3D12AdapterNameFilter = "B580;UHD Graphics 770;UHD 770",
    [uint32]$Width = 960,
    [uint32]$Height = 540,
    [uint32]$MaxDepth = 2,
    [uint32]$D3D12RaysPerPixel = 8,
    [uint32]$D3D12ReadbackInterval = 4,
    [uint32]$UiPresentHz = 60,
    [switch]$Console,
    [switch]$Terminal,
    [string]$EnvFile = ".env",
    [switch]$NoEnvFile
)

# Project flag reference (searched 2026-05-05):
# - run.ps1 parameters:
#   -NoBuild, -Preset, -Scene, -Backend, -Width, -Height, -MaxDepth, -D3D12RaysPerPixel,
#   -D3D12ReadbackInterval, -UiPresentHz, -Console, -Terminal, -EnvFile, -NoEnvFile.
# - run.ps1 loads .env from the repo root by default when it exists. Existing
#   process environment variables win over .env values, and explicit PowerShell
#   parameters win over both.
# - This launcher maps to:
#   ptapp --window --platform qt --backend <backend> --scene <path> --width <px>
#   --height <px> --max-depth <depth> --ui-present-hz <hz> [--console] [--env-file <path>].
#   Console/terminal output is opt-in for Qt GUI builds; use -Console or -Terminal
#   when interactive stdout/stderr diagnostics are needed.
# - Current ptapp flags:
#   -h/--help, --version [--json], --doctor, --check-build, --check-cpu,
#   --check-backends, --check-assets, --check-shaders, --check-job-system,
#   --check-scene-schema, --check-bench-write, --dump-config,
#   --config <path>, --env-file <path>, --no-env-file, --list-backends,
#   --list-accelerators, --list-gpus,
#   --headless, --window, --platform <auto|raw|qt|headless>,
#   --window-width <px>, --window-height <px>, --ui-present-hz <hz>,
#   --frames <n>, --exit, --console/--terminal, --scene <path>, --backend <name>,
#   --log-level <n>, --crash-test, --ui-model-smoke, --ui-release-gate,
#   --render, --output <png>, --exr-output <exr>, --width <px>,
#   --height <px>, --spp <samples>, --max-depth <depth>.
# - ptapp config-file keys accepted through --config:
#   backend, scene_path, log_level, platform, headless, benchmark_mode,
#   ui_present_hz, render_width, render_height, spp, max_depth, output_path,
#   exr_output_path, benchmark_warmup, benchmark_warmup_frames,
#   benchmark_tile_size, write_status_file, status_file_path,
#   crash_artifact_dir.
# - ptapp/runtime env flags:
#   PT_D3D12_RAYS_PER_PIXEL, PT_D3D12_READBACK_INTERVAL,
#   PT_D3D12_ACCELERATOR_PRESET, PT_D3D12_INCLUDE_CPU,
#   PT_D3D12_INCLUDE_INTEGRATED_GPU, PT_D3D12_INCLUDE_WARP,
#   PT_D3D12_ADAPTER_NAME_FILTER,
#   PTAPP_BACKEND, PTAPP_LOG_LEVEL, PTAPP_SCENE, PTAPP_PLATFORM,
#   PTAPP_HEADLESS, PTAPP_BENCHMARK_MODE, PTAPP_UI_PRESENT_HZ,
#   PTAPP_RENDER_WIDTH, PTAPP_RENDER_HEIGHT, PTAPP_SPP, PTAPP_MAX_DEPTH,
#   PTAPP_OUTPUT_PATH, PTAPP_EXR_OUTPUT_PATH,
#   PTAPP_BENCHMARK_WARMUP_FRAMES, PTAPP_BENCHMARK_TILE_SIZE,
#   PTAPP_WRITE_STATUS_FILE, PTAPP_STATUS_FILE_PATH,
#   PTAPP_CRASH_ARTIFACT_DIR, PTAPP_CONSOLE.
# - run.ps1-only env flags:
#   VKPT_RUN_PRESET.
# - ptdoctor delegates to ptapp and accepts the same diagnostic/runtime flags;
#   VKPT_PTAPP_PATH overrides the delegated ptapp executable.
# - ptbench commands:
#   run, echo-desc, list-scenes, list-backends, list-renderer-paths,
#   validate-scene, validate-artifacts, compare, dump-capabilities,
#   run-experiments, backend-experiments, gpu-mem-pressure, shader-matrix,
#   release-check, thread-sweep, simd-sweep, tile-sweep.
# - ptbench flags:
#   --desc, --echo-desc, --scene, --backend, --renderer-path, --resolution,
#   --spp, --seed, --max-depth, --duration, --warmup-frames,
#   --reference-image, --tolerance-policy, --output, --workers,
#   --tile-size, --deterministic, --dir, --json, --reference, --image,
#   --disable-heatmap, --max-mb, --step-mb, --scene-pack, --include.
# - Tool/script flags:
#   tools/smoke.ps1: -Preset, -Width, -Height, -Spp, -NoBuild.
#   tools/ui_qt_smoke.ps1: -DisabledPreset, -QtPreset, -DisabledBuildDir,
#   -QtBuildDir, -Width, -Height, -Spp, -WindowSeconds, -WindowFrames,
#   -UiPresentHz, -MinimumQtDockCount, -RequiredQtDockIds, -NoBuild,
#   -SkipQt, -SkipQtShellProbe, -QtQpaPlatform.
#   tools/ui_qt_smoke.sh: --disabled-preset, --qt-preset,
#   --disabled-build-dir, --qt-build-dir, --width, --height, --spp,
#   --window-seconds, --window-frames, --ui-present-hz, --no-build,
#   --skip-qt, -h/--help.
#   tools/todo_audit.ps1: -TodosPath, -Output, -RequireEvidence.
# - CMake/build flags used by the project:
#   --preset, --build, --target, --list-presets, -B, -D<cache>=<value>.
# - CMake/cache feature flags:
#   PT_ENABLE_VULKAN, PT_ENABLE_D3D12, PT_ENABLE_METAL, PT_ENABLE_WEBGPU,
#   PT_ENABLE_OPENGL_EXPERIMENTAL, PT_ENABLE_CPU_RAYTRACER,
#   PT_ENABLE_CPU_SIMD, PT_ENABLE_AVX, PT_ENABLE_AVX2, PT_ENABLE_AVX512,
#   PT_ENABLE_NEON, PT_ENABLE_SVE, PT_ENABLE_JOLT, PT_ENABLE_LUA,
#   PT_ENABLE_EDITOR, PT_ENABLE_BENCHMARK, PT_ENABLE_PROFILING,
#   PT_ENABLE_SANITIZERS, PT_STRICT_DETERMINISM, PT_ENABLE_QT,
#   PT_ENABLE_QT_EDITOR, PT_ENABLE_QT_RHI, PT_ENABLE_RAW_DESKTOP,
#   PT_WINDEPLOYQT_EXECUTABLE, PT_JOLT_GIT_TAG, PT_SANITIZER
#   (disabled|address|thread).
# - Tool-only env/build flags:
#   VKPT_QT_QPA_PLATFORM controls Qt smoke QPA platform; CMake's windeployqt
#   step passes --debug/--release, --no-translations, and
#   --include-plugins qoffscreen.
# - Build-generated/internal symbols, not launcher knobs:
#   PT_SHADER_HLSL_PATH, PT_SHADER_SPV_PATH, PT_JOLT_GIT_TAG_STRING,
#   VKPT_HAS_QT, VKPT_QT_EDITOR, VKPT_ARCH_X86, VKPT_ARCH_ARM64,
#   VKPT_LIFECYCLE_CONSTRUCT, VKPT_LIFECYCLE_CONFIGURE.
# - Docs/todos also mention planned or historical flags that are not current
#   ptapp parser flags: --benchmark, --nee, --cpu-simd, --qt-style,
#   --qt-scale, --qt-no-native-menubar, --qt-trace-events,
#   --qt-trace-paint, --qt-trace-input, --viewport-mode.
#
# Multi-device ray generation reference:
# - The recent planner has an internal accelerator_preset field.
# - auto: choose one eligible accelerator in priority order:
#   discrete GPU, then integrated GPU, then CPU.
# - high-performance: use every eligible real accelerator at once:
#   discrete GPU + integrated GPU + capped CPU workers.
# - This launcher requests high-performance GPU-only D3D12 policy for Intel B580
#   plus UHD 770 class adapters and disables CPU participation. The current Qt
#   path tracer still renders through one concrete D3D12 tracer instance; explicit
#   d3d12/d3d12-dxr backend selection refuses CPU fallback if GPU init fails.
# - WARP / Microsoft Basic Render Driver is a software fallback and should stay
#   opt-in; it is listed for diagnostics but not part of default high-performance
#   ray generation.
# - ptapp --list-accelerators prints both auto and high-performance plans today.
# - There is not yet a ptapp CLI flag for accelerator_preset. Keep the process
#   environment policy here so planner/runtime code can consume it without
#   changing the launcher command line.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

function Get-ProcessEnv([string]$Name) {
    return [Environment]::GetEnvironmentVariable($Name, "Process")
}

function Convert-DotEnvValue([string]$Value) {
    $text = $Value.Trim()
    if ($text.Length -ge 2 -and $text[0] -eq '"' ) {
        $escaped = $false
        for ($i = 1; $i -lt $text.Length; $i++) {
            if ($escaped) {
                $escaped = $false
                continue
            }
            if ($text[$i] -eq '\') {
                $escaped = $true
                continue
            }
            if ($text[$i] -eq '"') {
                $inner = $text.Substring(1, $i - 1)
                return $inner.Replace('\n', "`n").Replace('\r', "`r").Replace('\t', "`t").Replace('\"', '"').Replace('\\', '\')
            }
        }
    }
    if ($text.Length -ge 2 -and $text[0] -eq "'") {
        $end = $text.IndexOf("'", 1)
        if ($end -gt 0) {
            return $text.Substring(1, $end - 1)
        }
    }
    return (($text -replace '\s+#.*$', '').Trim())
}

function Import-DotEnvFile([string]$Path) {
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $Path -ErrorAction Stop) {
        $lineNumber++
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed) -or $trimmed.StartsWith("#")) {
            continue
        }
        if ($trimmed.StartsWith("export ")) {
            $trimmed = $trimmed.Substring(7).Trim()
        }
        $eq = $trimmed.IndexOf("=")
        if ($eq -lt 1) {
            continue
        }
        $name = $trimmed.Substring(0, $eq).Trim()
        if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
            throw "Invalid .env key '$name' at ${Path}:${lineNumber}"
        }
        if ($null -ne (Get-ProcessEnv $name)) {
            continue
        }
        $value = Convert-DotEnvValue $trimmed.Substring($eq + 1)
        [Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

function Use-EnvString([string]$ParamName, [string]$EnvName, [ref]$Target) {
    if ($PSBoundParameters.ContainsKey($ParamName)) {
        return
    }
    $value = Get-ProcessEnv $EnvName
    if (-not [string]::IsNullOrWhiteSpace($value)) {
        $Target.Value = $value
    }
}

function Use-EnvUInt32([string]$ParamName, [string]$EnvName, [ref]$Target) {
    if ($PSBoundParameters.ContainsKey($ParamName)) {
        return
    }
    $value = Get-ProcessEnv $EnvName
    if ([string]::IsNullOrWhiteSpace($value)) {
        return
    }
    $parsed = 0
    if ([uint32]::TryParse($value, [ref]$parsed)) {
        $Target.Value = $parsed
    }
}

function Test-EnvTruthy([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        return $false
    }
    return @("1", "true", "yes", "on") -contains $Value.Trim().ToLowerInvariant()
}

$loadedEnvFilePath = ""
$resolvedEnvFilePath = if ([System.IO.Path]::IsPathRooted($EnvFile)) {
    $EnvFile
} else {
    Join-Path $root $EnvFile
}

if (-not $NoEnvFile) {
    if (Test-Path -LiteralPath $resolvedEnvFilePath) {
        Import-DotEnvFile $resolvedEnvFilePath
        $loadedEnvFilePath = $resolvedEnvFilePath
    } elseif ($PSBoundParameters.ContainsKey("EnvFile")) {
        Write-Error "[run] .env file was not found at $resolvedEnvFilePath."
        exit 1
    }
}

Use-EnvString "Preset" "VKPT_RUN_PRESET" ([ref]$Preset)
Use-EnvString "Scene" "PTAPP_SCENE" ([ref]$Scene)
Use-EnvString "Backend" "PTAPP_BACKEND" ([ref]$Backend)
Use-EnvString "D3D12AcceleratorPreset" "PT_D3D12_ACCELERATOR_PRESET" ([ref]$D3D12AcceleratorPreset)
Use-EnvString "D3D12AdapterNameFilter" "PT_D3D12_ADAPTER_NAME_FILTER" ([ref]$D3D12AdapterNameFilter)
Use-EnvUInt32 "Width" "PTAPP_RENDER_WIDTH" ([ref]$Width)
Use-EnvUInt32 "Height" "PTAPP_RENDER_HEIGHT" ([ref]$Height)
Use-EnvUInt32 "MaxDepth" "PTAPP_MAX_DEPTH" ([ref]$MaxDepth)
Use-EnvUInt32 "UiPresentHz" "PTAPP_UI_PRESENT_HZ" ([ref]$UiPresentHz)
Use-EnvUInt32 "D3D12RaysPerPixel" "PT_D3D12_RAYS_PER_PIXEL" ([ref]$D3D12RaysPerPixel)
Use-EnvUInt32 "D3D12ReadbackInterval" "PT_D3D12_READBACK_INTERVAL" ([ref]$D3D12ReadbackInterval)
if (-not $PSBoundParameters.ContainsKey("Console") -and -not $PSBoundParameters.ContainsKey("Terminal")) {
    if (Test-EnvTruthy (Get-ProcessEnv "PTAPP_CONSOLE")) {
        $Console = $true
    }
}

$build = Join-Path $root "build\presets\$Preset"
$bin = Join-Path $build "bin"
$exe = Join-Path $bin "ptapp.exe"
$scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) {
    $Scene
} else {
    Join-Path $root $Scene
}

if (-not $NoBuild) {
    if (-not (Test-Path $build)) {
        Write-Host "[run] Configuring preset $Preset..." -ForegroundColor Cyan
        cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) {
            Write-Error "[run] Configure failed (exit $LASTEXITCODE)"
            exit $LASTEXITCODE
        }
    }

    Write-Host "[run] Building ptapp with preset $Preset..." -ForegroundColor Cyan
    cmake --build --preset $Preset --target ptapp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[run] Build failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
    Write-Host "[run] Build succeeded." -ForegroundColor Green
}

if (-not (Test-Path $exe)) {
    Write-Error "[run] ptapp was not found at $exe. Build the $Preset preset first or run without -NoBuild."
    exit 1
}

if (-not (Test-Path $scenePath)) {
    Write-Error "[run] Scene was not found at $scenePath."
    exit 1
}

$env:PATH = "$bin;$env:PATH"
$env:PT_D3D12_RAYS_PER_PIXEL = [string]$D3D12RaysPerPixel
$env:PT_D3D12_READBACK_INTERVAL = [string]$D3D12ReadbackInterval
$env:PT_D3D12_ACCELERATOR_PRESET = $D3D12AcceleratorPreset
$env:PT_D3D12_INCLUDE_CPU = "0"
$env:PT_D3D12_INCLUDE_INTEGRATED_GPU = "1"
$env:PT_D3D12_INCLUDE_WARP = "0"
$env:PT_D3D12_ADAPTER_NAME_FILTER = $D3D12AdapterNameFilter

$normalizedBackend = $Backend.Trim().ToLowerInvariant()
if ($normalizedBackend -in @("auto", "cpu", "cpu-tiled", "cputiled")) {
    Write-Error "[run] CPU backends are disabled by this launcher. Use -Backend d3d12-dxr for DXR or -Backend d3d12 for compute."
    exit 1
}

$ptappArgs = @(
    "--window",
    "--platform", "qt",
    "--backend", $Backend,
    "--scene", $scenePath,
    "--width", [string]$Width,
    "--height", [string]$Height,
    "--max-depth", [string]$MaxDepth,
    "--ui-present-hz", [string]$UiPresentHz
)

if ($NoEnvFile) {
    $ptappArgs += "--no-env-file"
} elseif (-not [string]::IsNullOrWhiteSpace($loadedEnvFilePath)) {
    $ptappArgs += @("--env-file", $loadedEnvFilePath)
} else {
    $ptappArgs += "--no-env-file"
}

if ($Console -or $Terminal) {
    $ptappArgs += "--console"
}

$consoleMode = if ($Console -or $Terminal) { "console diagnostics on" } else { "console diagnostics off" }
$acceleratorMode = "D3D12 preset ${D3D12AcceleratorPreset}, adapters '${D3D12AdapterNameFilter}', CPU off, iGPU on, WARP off"
Write-Host "[run] Launching Qt demo ($Backend backend, scene '$Scene', internal ${Width}x${Height}, depth ${MaxDepth}, ${D3D12RaysPerPixel} rays/pixel/dispatch, readback every ${D3D12ReadbackInterval} batches, ${UiPresentHz}Hz UI, ${acceleratorMode}, ${consoleMode})..." -ForegroundColor Cyan
& $exe @ptappArgs
