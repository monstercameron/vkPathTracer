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

Run-Step "Doctor" "$ptapp --doctor"
Run-Step "Crash test artifacts" "$ptapp --headless --crash-test"

Run-Step "Scene validation (pack)" "$ptbench release-check --scene-pack assets/scenes --output artifacts/release_check"
Run-Step "Shader matrix" "$ptbench shader-matrix --output artifacts/release_check"
Run-Step "GPU mem pressure (sim)" "$ptbench gpu-mem-pressure --max-mb 256 --step-mb 64 --output artifacts/release_check"

Write-Host "`nrelease gate: ok"

