param(
    [switch]$NoBuild,
    [string]$Preset = "windows-clangcl-d3d12-qt-debug",
    [string]$Scene = "assets\scenes\cornell_native.json",
    [uint32]$Width = 960,
    [uint32]$Height = 540,
    [uint32]$D3D12RaysPerPixel = 8,
    [uint32]$D3D12ReadbackInterval = 4,
    [uint32]$UiPresentHz = 60
)

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

Write-Host "[run] Launching Qt Cornell box demo (D3D12, internal ${Width}x${Height}, ${D3D12RaysPerPixel} rays/pixel/dispatch, readback every ${D3D12ReadbackInterval} batches, ${UiPresentHz}Hz UI)..." -ForegroundColor Cyan
& $exe --window --platform qt --backend d3d12 --scene $scenePath --width $Width --height $Height --ui-present-hz $UiPresentHz
