param(
    [switch]$NoBuild,
    [string]$Preset = "windows-clangcl-d3d12-qt-debug",
    [string]$Scene = "assets\scenes\cornell_native.json",
    [uint32]$Width = 960,
    [uint32]$Height = 540,
    [uint32]$MaxDepth = 2,
    [uint32]$D3D12RaysPerPixel = 8,
    [uint32]$D3D12ReadbackInterval = 4,
    [uint32]$UiPresentHz = 60
)

# Project flag reference (searched 2026-05-05):
# - run.ps1 parameters:
#   -NoBuild, -Preset, -Scene, -Width, -Height, -MaxDepth, -D3D12RaysPerPixel,
#   -D3D12ReadbackInterval, -UiPresentHz.
# - This launcher maps to:
#   ptapp --window --platform qt --backend d3d12 --scene <path> --width <px>
#   --height <px> --max-depth <depth> --ui-present-hz <hz>.
# - Current ptapp flags:
#   -h/--help, --version [--json], --doctor, --check-build, --check-cpu,
#   --check-backends, --check-assets, --check-shaders, --check-job-system,
#   --check-scene-schema, --check-bench-write, --dump-config,
#   --config <path>, --list-backends, --list-accelerators, --list-gpus,
#   --headless, --window, --platform <auto|raw|qt|headless>,
#   --window-width <px>, --window-height <px>, --ui-present-hz <hz>,
#   --frames <n>, --exit, --scene <path>, --backend <name>,
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
#   PTAPP_BACKEND, PTAPP_LOG_LEVEL, PTAPP_SCENE, PTAPP_PLATFORM,
#   PTAPP_HEADLESS, PTAPP_BENCHMARK_MODE, PTAPP_UI_PRESENT_HZ,
#   PTAPP_RENDER_WIDTH, PTAPP_RENDER_HEIGHT, PTAPP_SPP, PTAPP_MAX_DEPTH,
#   PTAPP_OUTPUT_PATH, PTAPP_EXR_OUTPUT_PATH,
#   PTAPP_BENCHMARK_WARMUP_FRAMES, PTAPP_BENCHMARK_TILE_SIZE,
#   PTAPP_WRITE_STATUS_FILE, PTAPP_STATUS_FILE_PATH,
#   PTAPP_CRASH_ARTIFACT_DIR.
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
# - WARP / Microsoft Basic Render Driver is a software fallback and should stay
#   opt-in; it is listed for diagnostics but not part of default high-performance
#   ray generation.
# - ptapp --list-accelerators prints both auto and high-performance plans today.
# - There is not yet a ptapp runtime flag for accelerator_preset. When that lands,
#   add a launcher parameter here, default it to auto, and pass it through to ptapp.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
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

Write-Host "[run] Launching Qt Cornell box demo (D3D12, internal ${Width}x${Height}, depth ${MaxDepth}, ${D3D12RaysPerPixel} rays/pixel/dispatch, readback every ${D3D12ReadbackInterval} batches, ${UiPresentHz}Hz UI)..." -ForegroundColor Cyan
& $exe --window --platform qt --backend d3d12 --scene $scenePath --width $Width --height $Height --max-depth $MaxDepth --ui-present-hz $UiPresentHz
