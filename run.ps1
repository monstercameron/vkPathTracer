param(
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = "$root\build\d3d12-test"
$exe   = "$build\bin\Debug\ptapp.exe"

if (-not $NoBuild) {
    Write-Host "[run] Building ptapp (Debug)..." -ForegroundColor Cyan
    cmake --build $build --config Debug --target ptapp
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[run] Build failed (exit $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
    Write-Host "[run] Build succeeded." -ForegroundColor Green
}

Write-Host "[run] Launching Cornell box demo (D3D12)..." -ForegroundColor Cyan
& $exe --window --backend d3d12 --scene "$root\assets\scenes\cornell_native.json"
