param(
  [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
  [string]$InputGlob = "game/models/*.glb",
  [string]$OutputRoot = "game/models/lods",
  [double[]]$Ratios = @(0.75, 0.5, 0.25, 0.1),
  [string[]]$HintNames = @("near", "mid", "far", "very_far"),
  [string[]]$HintRanges = @("0-15m", "15-35m", "35-70m", "70m+"),
  [double]$SimplificationError = 0.01,
  [switch]$RebuildTool,
  [switch]$KeepIntermediate,
  [switch]$WhatIf
)

$ErrorActionPreference = "Stop"

function Format-RatioTag([double]$value) {
  return ([string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:0.###}", $value)).Replace(".", "p")
}

function Format-RatioArg([double]$value) {
  return [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:0.###}", $value)
}

function ConvertTo-RepoPath([string]$path) {
  $full = if (Test-Path $path) {
    (Resolve-Path $path).Path
  } elseif ([IO.Path]::IsPathRooted($path)) {
    [IO.Path]::GetFullPath($path)
  } else {
    [IO.Path]::GetFullPath((Join-Path $RepoRoot $path))
  }
  $root = (Resolve-Path $RepoRoot).Path
  if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
    $relative = $full.Substring($root.Length).TrimStart("\", "/")
    return $relative.Replace("\", "/")
  }
  return $full.Replace("\", "/")
}

function Write-JsonFile([string]$path, [object]$value) {
  $json = $value | ConvertTo-Json -Depth 20
  Set-Content -Path $path -Value $json -Encoding ASCII
}

function Invoke-Checked([string]$exe, [string[]]$arguments) {
  & $exe @arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$exe failed with exit code $LASTEXITCODE"
  }
}

function Get-UniqueTexturePath([string]$textureDir, [string]$fileName, [string]$sourcePath) {
  $base = [IO.Path]::GetFileNameWithoutExtension($fileName)
  $ext = [IO.Path]::GetExtension($fileName)
  $candidate = Join-Path $textureDir $fileName
  if (-not (Test-Path $candidate)) {
    return $candidate
  }

  $srcHash = (Get-FileHash -Algorithm SHA256 -Path $sourcePath).Hash
  $dstHash = (Get-FileHash -Algorithm SHA256 -Path $candidate).Hash
  if ($srcHash -eq $dstHash) {
    return $candidate
  }

  $shortHash = $srcHash.Substring(0, 8).ToLowerInvariant()
  return Join-Path $textureDir "$base`_$shortHash$ext"
}

function Move-ExternalTexturesToSharedFolder([string]$gltfPath, [string]$lodDir, [string]$textureDir) {
  $doc = Get-Content -Path $gltfPath -Raw | ConvertFrom-Json
  $textures = New-Object System.Collections.Generic.List[object]

  if ($null -eq $doc.images) {
    Write-JsonFile $gltfPath $doc
    return $textures
  }

  for ($i = 0; $i -lt $doc.images.Count; ++$i) {
    $image = $doc.images[$i]
    if ($null -eq $image.uri -or [string]::IsNullOrWhiteSpace($image.uri)) {
      continue
    }

    $imageName = [IO.Path]::GetFileName([string]$image.uri)
    $sourceImage = Join-Path $lodDir $imageName
    if (-not (Test-Path $sourceImage)) {
      continue
    }

    $targetImage = Get-UniqueTexturePath $textureDir $imageName $sourceImage
    if (-not (Test-Path $targetImage)) {
      Move-Item -LiteralPath $sourceImage -Destination $targetImage
    } else {
      Remove-Item -LiteralPath $sourceImage -Force
    }

    $relativeUri = "../textures/$([IO.Path]::GetFileName($targetImage))"
    $image.uri = $relativeUri
    [void]$textures.Add([pscustomobject]@{
      index = $i
      uri = $relativeUri
      file = ConvertTo-RepoPath $targetImage
      bytes = (Get-Item $targetImage).Length
    })
  }

  Write-JsonFile $gltfPath $doc
  return $textures
}

$meshoptRoot = Join-Path $RepoRoot "vendor/meshoptimizer"
$buildDir = Join-Path $RepoRoot "build/vendor/meshoptimizer-gltfpack"
$gltfpack = Join-Path $buildDir "Release/gltfpack.exe"
$workRoot = Join-Path $RepoRoot "build/lod-work"
$resolvedOutputRoot = Join-Path $RepoRoot $OutputRoot

if (-not (Test-Path $meshoptRoot)) {
  throw "meshoptimizer submodule is missing at $meshoptRoot. Run: git submodule update --init --recursive"
}

if ($Ratios.Count -eq 0) {
  throw "At least one LOD ratio is required"
}

foreach ($ratio in $Ratios) {
  if ($ratio -le 0.0 -or $ratio -gt 1.0) {
    throw "LOD ratio must be in the range (0, 1], got $ratio"
  }
}

if ($RebuildTool -or -not (Test-Path $gltfpack)) {
  if ($WhatIf) {
    Write-Host "cmake -S $meshoptRoot -B $buildDir -DMESHOPT_BUILD_GLTFPACK=ON"
    Write-Host "cmake --build $buildDir --target gltfpack --config Release --parallel"
  } else {
    Invoke-Checked "cmake" @("-S", $meshoptRoot, "-B", $buildDir, "-DMESHOPT_BUILD_GLTFPACK=ON")
    Invoke-Checked "cmake" @("--build", $buildDir, "--target", "gltfpack", "--config", "Release", "--parallel")
  }
}

if (-not $WhatIf -and -not (Test-Path $gltfpack)) {
  throw "gltfpack executable was not produced at $gltfpack"
}

$inputs = Get-ChildItem -Path (Join-Path $RepoRoot $InputGlob) -File
if ($inputs.Count -eq 0) {
  throw "No input models matched $InputGlob"
}

if (-not $WhatIf) {
  New-Item -ItemType Directory -Force -Path $resolvedOutputRoot | Out-Null
  New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
}

$allModels = New-Object System.Collections.Generic.List[object]

foreach ($input in $inputs) {
  $modelName = $input.BaseName
  $modelOutDir = Join-Path $resolvedOutputRoot $modelName
  $textureDir = Join-Path $modelOutDir "textures"
  $modelWorkDir = Join-Path $workRoot $modelName
  $lodRecords = New-Object System.Collections.Generic.List[object]

  if (-not $WhatIf) {
    New-Item -ItemType Directory -Force -Path $modelOutDir | Out-Null
    New-Item -ItemType Directory -Force -Path $textureDir | Out-Null
    New-Item -ItemType Directory -Force -Path $modelWorkDir | Out-Null
  }

  Write-Host "Model: $($input.Name)"

  for ($i = 0; $i -lt $Ratios.Count; ++$i) {
    $ratio = $Ratios[$i]
    $ratioTag = Format-RatioTag $ratio
    $hintName = if ($i -lt $HintNames.Count) { $HintNames[$i] } else { "lod$i" }
    $hintRange = if ($i -lt $HintRanges.Count) { $HintRanges[$i] } else { "" }
    $lodFolderName = "lod$($i)_$($hintName)_r$ratioTag"
    $assetStem = "$modelName`__lod$($i)_$($hintName)_r$ratioTag"
    $rawDir = Join-Path $modelWorkDir $lodFolderName
    $lodDir = Join-Path $modelOutDir $lodFolderName
    $rawGltf = Join-Path $rawDir "$assetStem.gltf"
    $finalGltf = Join-Path $lodDir "$assetStem.gltf"
    $report = Join-Path $lodDir "$assetStem.report.json"

    $gltfpackArgs = @(
      "-i", $input.FullName,
      "-o", $rawGltf,
      "-si", (Format-RatioArg $ratio),
      "-se", (Format-RatioArg $SimplificationError),
      "-noq",
      "-vpf",
      "-vtf",
      "-kn",
      "-km",
      "-r", $report
    )

    if ($WhatIf) {
      Write-Host "  gltfpack $($gltfpackArgs -join ' ')"
      Write-Host "  npx --yes @gltf-transform/cli copy $rawGltf $finalGltf"
      continue
    }

    New-Item -ItemType Directory -Force -Path $rawDir | Out-Null
    New-Item -ItemType Directory -Force -Path $lodDir | Out-Null

    Write-Host "  $lodFolderName"
    Invoke-Checked $gltfpack $gltfpackArgs
    Invoke-Checked "npx" @("--yes", "@gltf-transform/cli", "copy", $rawGltf, $finalGltf)

    $textures = Move-ExternalTexturesToSharedFolder $finalGltf $lodDir $textureDir
    $reportJson = if (Test-Path $report) { Get-Content -Path $report -Raw | ConvertFrom-Json } else { $null }
    $triangleCount = if ($null -ne $reportJson) { $reportJson.render.triangleCount } else { $null }
    $binaryBytes = if ($null -ne $reportJson) { $reportJson.data.binary } else { $null }

    [void]$lodRecords.Add([pscustomobject]@{
      index = $i
      hint = $hintName
      distance_hint = $hintRange
      ratio = $ratio
      folder = ConvertTo-RepoPath $lodDir
      gltf = ConvertTo-RepoPath $finalGltf
      report = ConvertTo-RepoPath $report
      triangle_count = $triangleCount
      binary_bytes = $binaryBytes
      textures = $textures
    })
  }

  $modelManifest = [pscustomobject]@{
    source = ConvertTo-RepoPath $input.FullName
    source_bytes = $input.Length
    generated_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    generator = [pscustomobject]@{
      gltfpack = ConvertTo-RepoPath $gltfpack
      meshoptimizer = ConvertTo-RepoPath $meshoptRoot
      gltf_transform = "npx --yes @gltf-transform/cli copy"
    }
    output = ConvertTo-RepoPath $modelOutDir
    texture_folder = ConvertTo-RepoPath $textureDir
    lods = $lodRecords
  }

  if (-not $WhatIf) {
    Write-JsonFile (Join-Path $modelOutDir "manifest.json") $modelManifest
    if (-not $KeepIntermediate -and (Test-Path $modelWorkDir)) {
      Remove-Item -LiteralPath $modelWorkDir -Recurse -Force
    }
  }

  [void]$allModels.Add($modelManifest)
}

if (-not $WhatIf) {
  $rootManifest = [pscustomobject]@{
    generated_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    input_glob = $InputGlob
    output_root = ConvertTo-RepoPath $resolvedOutputRoot
    ratios = $Ratios
    hint_names = $HintNames
    hint_ranges = $HintRanges
    models = $allModels
  }
  Write-JsonFile (Join-Path $resolvedOutputRoot "manifest.json") $rootManifest
}
