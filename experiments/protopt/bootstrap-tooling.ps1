param(
    [string]$Mode = "install"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$toolingRoot = Join-Path $root "tooling"
$cmakeRoot = Join-Path $toolingRoot "cmake"
$cmakeBin = Join-Path $cmakeRoot "bin\cmake.exe"

New-Item -ItemType Directory -Path $toolingRoot -Force | Out-Null

if ($Mode -ne "clean" -and (Test-Path $cmakeBin)) {
  Write-Host "Tooling already present: $cmakeBin"
  exit 0
}

if ($Mode -eq "clean") {
  if (Test-Path $cmakeRoot) {
    Remove-Item -Recurse -Force $cmakeRoot
  }
}

Write-Host "Fetching CMake release metadata..."
$apiUrl = "https://api.github.com/repos/Kitware/CMake/releases/latest"
$release = Invoke-RestMethod -Uri $apiUrl -Headers @{ "User-Agent" = "Codex-Agent" }
$asset = $release.assets | Where-Object { $_.name -like "cmake-*-windows-x86_64.zip" } | Select-Object -First 1

if (-not $asset) {
  throw "Could not locate a windows x86_64 CMake asset from GitHub releases."
}

$downloadUrl = $asset.browser_download_url
$tempZip = Join-Path $env:TEMP "cmake-latest-win64.zip"
$zipTmp = Join-Path $env:TEMP "cmake-extract"

Write-Host "Downloading CMake: $($asset.name)"
Invoke-WebRequest -Uri $downloadUrl -OutFile $tempZip -UseBasicParsing

if (Test-Path $zipTmp) {
  Remove-Item -Recurse -Force $zipTmp
}
New-Item -ItemType Directory -Path $zipTmp | Out-Null
Expand-Archive -Path $tempZip -DestinationPath $zipTmp -Force

$sourceRoot = Get-ChildItem -Path $zipTmp -Directory | Where-Object { $_.Name -like "cmake-*" } | Select-Object -First 1
if (-not $sourceRoot) {
  throw "Unable to find extracted CMake folder."
}

Move-Item -Path $sourceRoot.FullName -Destination $cmakeRoot -Force
Write-Host "CMake installed to: $cmakeRoot"
Write-Host "Installed binary: $cmakeBin"
