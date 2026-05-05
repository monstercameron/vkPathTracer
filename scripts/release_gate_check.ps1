$ErrorActionPreference = "Stop"

function Run-Step($name, $cmd) {
  Write-Host "== $name =="
  Write-Host $cmd
  powershell -NoProfile -Command $cmd
  if ($LASTEXITCODE -ne 0) {
    throw "$name failed with exit code $LASTEXITCODE"
  }
}

Run-Step "Configure (debug)" "cmake --preset desktop-clang-debug"
Run-Step "Build (debug)" "cmake --build --preset desktop-clang-debug"

$ptapp = ".\build\presets\desktop-clang-debug\bin\ptapp.exe"
$ptbench = ".\build\presets\desktop-clang-debug\bin\ptbench.exe"
$releaseDir = "artifacts/release_check"
$descriptor = Join-Path $releaseDir "cornell_descriptor.json"
New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null
@'
{
  "scene_path": "assets/scenes/cornell_native.json",
  "backend": "cpu",
  "renderer_path": "cpu-scalar",
  "resolution": { "width": 64, "height": 64 },
  "samples_per_pixel": 1,
  "duration": 0,
  "warmup_frames": 0,
  "seed": 195936478,
  "output_directory": "artifacts/release_check/descriptor_echo",
  "reference_image": "",
  "tolerance_policy": "abs=0.001",
  "max_depth": 6,
  "worker_count": 0,
  "tile_height": 16,
  "deterministic": false
}
'@ | Set-Content -Encoding UTF8 -Path $descriptor

Run-Step "Doctor" "$ptapp --doctor"
Run-Step "Crash test artifacts" "$ptapp --headless --crash-test"

Run-Step "Descriptor parse/echo" "$ptbench run --desc $descriptor --echo-desc"
Run-Step "Scene validation (pack)" "$ptbench release-check --scene-pack assets/scenes --output $releaseDir"
Run-Step "Artifact contract" "$ptbench validate-artifacts --dir artifacts/release_check/cpu_scalar_cornell"
Run-Step "Thread scaling sweep" "$ptbench thread-sweep --scene assets/scenes/cornell_native.json --workers 1,2 --resolution 64x64 --spp 1 --output artifacts/release_check/thread_scaling"
Run-Step "Backend experiment skips" "$ptbench backend-experiments --output artifacts/release_check"
Run-Step "Shader matrix" "$ptbench shader-matrix --output artifacts/release_check"
Run-Step "GPU mem pressure (sim)" "$ptbench gpu-mem-pressure --max-mb 256 --step-mb 64 --output artifacts/release_check"

if (Test-Path ".\tools\ui_qt_smoke.ps1") {
  Run-Step "UI release gate smoke" "powershell -NoProfile -File .\tools\ui_qt_smoke.ps1 -NoBuild -DisabledBuildDir .\build\presets\desktop-clang-debug"
}

Write-Host "`nrelease gate: ok"

