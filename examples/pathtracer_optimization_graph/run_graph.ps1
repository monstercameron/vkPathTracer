param(
  [string]$Ptbench = "",
  [string]$Scene = "assets/scenes/cornell_native.json",
  [string]$Resolution = "96x96",
  [int]$Spp = 1,
  [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
if ([string]::IsNullOrWhiteSpace($OutDir)) {
  $OutDir = Join-Path $ScriptDir "results"
}

function Resolve-RepoPath {
  param([string]$Path)
  if ([System.IO.Path]::IsPathRooted($Path)) {
    return [System.IO.Path]::GetFullPath($Path)
  }
  return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

$OutDir = Resolve-RepoPath $OutDir
$LogDir = Join-Path $OutDir "logs"
New-Item -ItemType Directory -Force -Path $OutDir, $LogDir | Out-Null

if ([string]::IsNullOrWhiteSpace($Ptbench)) {
  $Ptbench = Join-Path $RepoRoot "build\default\bin\ptbench.exe"
}
$Ptbench = Resolve-RepoPath $Ptbench

function Invoke-Step {
  param(
    [string]$Name,
    [string]$FilePath,
    [string[]]$Arguments,
    [switch]$AllowFailure
  )

  $stdout = Join-Path $LogDir "$Name.out.txt"
  $stderr = Join-Path $LogDir "$Name.err.txt"
  $quoted = $Arguments | ForEach-Object {
    if ($_ -match "\s") { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
  }
  Write-Host "[$Name] $FilePath $($quoted -join ' ')"
  $process = Start-Process -FilePath $FilePath -ArgumentList $quoted -WorkingDirectory $RepoRoot `
    -NoNewWindow -PassThru -Wait -RedirectStandardOutput $stdout -RedirectStandardError $stderr
  if ($process.ExitCode -ne 0 -and -not $AllowFailure) {
    $errText = ""
    if (Test-Path $stderr) {
      $errText = Get-Content $stderr -Raw
    }
    throw "$Name failed with exit code $($process.ExitCode). $errText"
  }
  return [pscustomobject]@{
    name = $Name
    exit_code = $process.ExitCode
    stdout = $stdout
    stderr = $stderr
  }
}

function Find-ClangCompiler {
  $cache = Join-Path $RepoRoot "build\default\CMakeCache.txt"
  if (Test-Path $cache) {
    $line = Get-Content $cache | Where-Object { $_ -like "CMAKE_CXX_COMPILER:STRING=*" } | Select-Object -First 1
    if ($line) {
      $path = $line.Substring($line.IndexOf("=") + 1)
      if (Test-Path $path) {
        return $path
      }
    }
  }
  $fallback = "C:\Program Files\LLVM\bin\clang++.exe"
  if (Test-Path $fallback) {
    return $fallback
  }
  $cmd = Get-Command clang++ -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }
  throw "Unable to find clang++ for the standalone math microbench."
}

function Find-RcCompiler {
  $cache = Join-Path $RepoRoot "build\default\CMakeCache.txt"
  if (Test-Path $cache) {
    $line = Get-Content $cache | Where-Object { $_ -like "CMAKE_RC_COMPILER:*" } | Select-Object -First 1
    if ($line) {
      $path = $line.Substring($line.IndexOf("=") + 1)
      if (Test-Path $path) {
        return $path
      }
    }
  }
  foreach ($candidate in @(
      "C:\Program Files\LLVM\bin\llvm-rc.exe",
      "C:\Program Files\LLVM\bin\llvm-windres.exe"
    )) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }
  $cmd = Get-Command llvm-rc -ErrorAction SilentlyContinue
  if ($cmd) {
    return $cmd.Source
  }
  return ""
}

function Read-JsonFile {
  param([string]$Path)
  if (!(Test-Path $Path)) {
    return $null
  }
  return Get-Content $Path -Raw | ConvertFrom-Json
}

function First-ResultJson {
  param([string]$Dir)
  $path = Join-Path $Dir "results.json"
  return Read-JsonFile $path
}

function Json-Escape {
  param([string]$Text)
  if ($null -eq $Text) { return "" }
  return $Text.Replace("\", "\\").Replace('"', '\"')
}

function Write-ShaderVariants {
  param([string]$OutputDir)

  $glslc = Get-Command glslc -ErrorAction SilentlyContinue
  $spirvDis = Get-Command spirv-dis -ErrorAction SilentlyContinue
  $rows = @()
  $shaderSrc = Join-Path $RepoRoot "src\shaders\gpu\pathtrace.comp"
  $sourceText = Get-Content $shaderSrc -Raw
  New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

  $variants = @(
    @{ id = "gpu.shader.wg8x8"; x = 8; y = 8; pow = $false },
    @{ id = "gpu.shader.wg16x8"; x = 16; y = 8; pow = $false },
    @{ id = "gpu.shader.wg8x16"; x = 8; y = 16; pow = $false },
    @{ id = "gpu.shader.wg16x16"; x = 16; y = 16; pow = $false },
    @{ id = "gpu.shader.pow_small_int_mul"; x = 8; y = 8; pow = $true }
  )

  foreach ($variant in $variants) {
    $text = $sourceText
    $text = $text -replace "layout\(local_size_x = 8, local_size_y = 8, local_size_z = 1\) in;",
      "layout(local_size_x = $($variant.x), local_size_y = $($variant.y), local_size_z = 1) in;"
    if ($variant.pow) {
      $helper = @"

float pow2_fast(float x) { return x * x; }
float pow5_fast(float x) { float x2 = x * x; return x2 * x2 * x; }
float pow6_fast(float x) { float x2 = x * x; return x2 * x2 * x2; }
"@
      if ($text -notmatch "float\s+pow2_fast\s*\(") {
        $text = $text -replace "float rand_f\(inout uint rng\) \{\r?\n    rng = pcg\(rng\);\r?\n    return float\(rng\) / 4294967296.0;\r?\n\}",
          "float rand_f(inout uint rng) {`n    rng = pcg(rng);`n    return float(rng) / 4294967296.0;`n}$helper"
      }
      $text = $text.Replace("pow(clamp(1.0 - abs(rd.y), 0.0, 1.0), 2.0)", "pow2_fast(clamp(1.0 - abs(rd.y), 0.0, 1.0))")
      $text = $text.Replace("pow(clamp(1.0 - abs(dot(n, -rd)), 0.0, 1.0), 2.0)", "pow2_fast(clamp(1.0 - abs(dot(n, -rd)), 0.0, 1.0))")
      $text = $text.Replace("pow(clamp(dot(n, -rd), 0.0, 1.0), 6.0)", "pow6_fast(clamp(dot(n, -rd), 0.0, 1.0))")
      $text = $text.Replace("pow(1.0 - cos_view, 5.0)", "pow5_fast(1.0 - cos_view)")
      $text = $text.Replace("pow(1.0 - cosTheta, 5.0)", "pow5_fast(1.0 - cosTheta)")
    }

    $variantPath = Join-Path $OutputDir "$($variant.id).comp"
    $spvPath = Join-Path $OutputDir "$($variant.id).spv"
    $disPath = Join-Path $OutputDir "$($variant.id).spvasm"
    Set-Content -Path $variantPath -Value $text -NoNewline

    $status = "skipped"
    $reason = ""
    $spvBytes = 0
    $opPow = 0
    $opExtInst = 0
    if ($glslc) {
      $step = Invoke-Step -Name "glslc_$($variant.id)" -FilePath $glslc.Source `
        -Arguments @("-fshader-stage=comp", $variantPath, "-o", $spvPath) -AllowFailure
      if ($step.exit_code -eq 0 -and (Test-Path $spvPath)) {
        $status = "ok"
        $spvBytes = (Get-Item $spvPath).Length
        if ($spirvDis) {
          $disStep = Invoke-Step -Name "spirvdis_$($variant.id)" -FilePath $spirvDis.Source `
            -Arguments @($spvPath, "-o", $disPath) -AllowFailure
          if ($disStep.exit_code -eq 0 -and (Test-Path $disPath)) {
            $asm = Get-Content $disPath -Raw
            $opPow = ([regex]::Matches($asm, "Pow")).Count
            $opExtInst = ([regex]::Matches($asm, "OpExtInst")).Count
          }
        }
      } else {
        $status = "failed"
        $reason = "glslc failed"
      }
    } else {
      $reason = "glslc not found"
    }

    $rows += [pscustomobject]@{
      id = $variant.id
      status = $status
      reason = $reason
      local_size_x = $variant.x
      local_size_y = $variant.y
      pow_small_int_mul = [bool]$variant.pow
      spv_bytes = $spvBytes
      op_pow_mentions = $opPow
      op_ext_inst_count = $opExtInst
      source = $variantPath
      spv = $spvPath
    }
  }

  $out = Join-Path $OutputDir "shader_variants.json"
  [pscustomobject]@{
    schema = "ptopt_shader_variants.v1"
    source = $shaderSrc
    rows = $rows
  } | ConvertTo-Json -Depth 6 | Set-Content $out
  return $out
}

function Add-Edge {
  param(
    [System.Collections.ArrayList]$Edges,
    [string]$Id,
    [string]$From,
    [string]$To,
    [string]$Runner,
    [string]$Status,
    [string]$Decision,
    [double]$Speedup = 0.0,
    [string]$Artifact = "",
    [string]$Reason = ""
  )
  [void]$Edges.Add([pscustomobject]@{
    id = $Id
    from = $From
    to = $To
    runner = $Runner
    explored = $true
    status = $Status
    decision = $Decision
    speedup = $Speedup
    artifact = $Artifact
    reason = $Reason
  })
}

Set-Location $RepoRoot

$compiler = Find-ClangCompiler
$rcCompiler = Find-RcCompiler
$compilerForCmake = $compiler -replace "\\", "/"
$rcCompilerForCmake = $rcCompiler -replace "\\", "/"
$buildDir = Join-Path $OutDir "microbench_build"
if (Test-Path $buildDir) {
  Remove-Item -LiteralPath $buildDir -Recurse -Force
}
$configureArgs = @("-S", $ScriptDir, "-B", $buildDir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=$compilerForCmake")
if (![string]::IsNullOrWhiteSpace($rcCompiler)) {
  $configureArgs += "-DCMAKE_RC_COMPILER=$rcCompilerForCmake"
}
Invoke-Step -Name "cmake_configure_microbench" -FilePath "cmake" -Arguments $configureArgs | Out-Null
Invoke-Step -Name "cmake_build_microbench" -FilePath "cmake" `
  -Arguments @("--build", $buildDir, "--config", "Release") | Out-Null

$microExe = Join-Path $buildDir "ptopt_math_microbench.exe"
if (!(Test-Path $microExe)) {
  $microExe = Join-Path $buildDir "ptopt_math_microbench"
}
$microJson = Join-Path $OutDir "microbench_results.json"
Invoke-Step -Name "math_microbench" -FilePath $microExe -Arguments @("--output", $microJson) | Out-Null

$ptbenchDir = Join-Path $OutDir "ptbench"
New-Item -ItemType Directory -Force -Path $ptbenchDir | Out-Null

if (!(Test-Path $Ptbench)) {
  throw "ptbench not found: $Ptbench"
}

$cpuScalarDir = Join-Path $ptbenchDir "cpu_scalar"
$cpuTiledDir = Join-Path $ptbenchDir "cpu_tiled_auto"
$gpuComputeDir = Join-Path $ptbenchDir "gpu_compute"
$threadDir = Join-Path $ptbenchDir "thread_sweep"
$tileDir = Join-Path $ptbenchDir "tile_sweep"
$simdDir = Join-Path $ptbenchDir "simd_sweep"
$shaderMatrixDir = Join-Path $ptbenchDir "shader_matrix"
$memDir = Join-Path $ptbenchDir "gpu_mem_pressure"

Invoke-Step -Name "ptbench_cpu_scalar" -FilePath $Ptbench -Arguments @(
  "run", "--scene", $Scene, "--backend", "cpu", "--renderer-path", "cpu-scalar",
  "--resolution", $Resolution, "--spp", "$Spp", "--output", $cpuScalarDir
) | Out-Null

Invoke-Step -Name "ptbench_cpu_tiled_auto" -FilePath $Ptbench -Arguments @(
  "run", "--scene", $Scene, "--backend", "cpu", "--renderer-path", "cpu-tiled",
  "--resolution", $Resolution, "--spp", "$Spp", "--workers", "0", "--output", $cpuTiledDir
) | Out-Null

Invoke-Step -Name "ptbench_gpu_compute" -FilePath $Ptbench -Arguments @(
  "run", "--scene", $Scene, "--backend", "vulkan", "--renderer-path", "gpu-compute",
  "--resolution", $Resolution, "--spp", "$Spp", "--output", $gpuComputeDir
) -AllowFailure | Out-Null

Invoke-Step -Name "ptbench_thread_sweep" -FilePath $Ptbench -Arguments @(
  "thread-sweep", "--scene", $Scene, "--workers", "1,2,4,8",
  "--resolution", $Resolution, "--spp", "$Spp", "--output", $threadDir
) | Out-Null

Invoke-Step -Name "ptbench_tile_sweep" -FilePath $Ptbench -Arguments @(
  "tile-sweep", "--scene", $Scene, "--workers", "0",
  "--resolution", $Resolution, "--spp", "$Spp", "--output", $tileDir
) | Out-Null

Invoke-Step -Name "ptbench_simd_sweep" -FilePath $Ptbench -Arguments @(
  "simd-sweep", "--rays", "300000", "--triangles", "1024", "--output", $simdDir
) | Out-Null

Invoke-Step -Name "ptbench_shader_matrix" -FilePath $Ptbench -Arguments @(
  "shader-matrix", "--output", $shaderMatrixDir
) -AllowFailure | Out-Null

Invoke-Step -Name "ptbench_gpu_mem_pressure" -FilePath $Ptbench -Arguments @(
  "gpu-mem-pressure", "--max-mb", "128", "--step-mb", "64", "--output", $memDir
) -AllowFailure | Out-Null

$shaderVariantJson = Write-ShaderVariants -OutputDir (Join-Path $OutDir "gpu_shader_variants")

$edges = [System.Collections.ArrayList]::new()
$micro = Read-JsonFile $microJson
foreach ($row in $micro.edges) {
  $status = if ($row.accuracy_pass) { "ok" } else { "rejected" }
  Add-Edge -Edges $edges -Id $row.id -From $row.from -To $row.to -Runner "math_microbench" `
    -Status $status -Decision $row.decision -Speedup ([double]$row.speedup) -Artifact $microJson `
    -Reason ("max_abs_error=" + $row.max_abs_error)
}

$cpuScalar = First-ResultJson $cpuScalarDir
$cpuTiled = First-ResultJson $cpuTiledDir
if ($cpuScalar -and $cpuTiled) {
  $scalarSps = [double]$cpuScalar.throughput.samples_per_sec
  $tiledSps = [double]$cpuTiled.throughput.samples_per_sec
  $speedup = if ($scalarSps -gt 0) { $tiledSps / $scalarSps } else { 0.0 }
  $decision = if ($speedup -gt 1.02) { "take_candidate" } else { "keep_baseline" }
  Add-Edge -Edges $edges -Id "edge_ptbench_tiled_auto" -From "ptbench.cpu_scalar" -To "ptbench.cpu_tiled_auto" `
    -Runner "ptbench_run" -Status "ok" -Decision $decision -Speedup $speedup -Artifact $cpuTiledDir
}

$threadJson = Join-Path $threadDir "thread_sweep.json"
$thread = Read-JsonFile $threadJson
if ($thread) {
  $best = $thread.rows | Where-Object { $_.status -eq "ok" } | Sort-Object -Property samples_per_sec -Descending | Select-Object -First 1
  Add-Edge -Edges $edges -Id "edge_ptbench_thread_sweep" -From "ptbench.cpu_tiled_auto" -To "ptbench.thread_sweep" `
    -Runner "ptbench_thread_sweep" -Status "ok" -Decision ("best_workers=" + $best.workers) `
    -Speedup ([double]$best.speedup_vs_one_worker) -Artifact $threadJson
}

$tileJson = Join-Path $tileDir "tile_sweep.json"
$tile = Read-JsonFile $tileJson
if ($tile) {
  $bestTile = $tile.best_tile_height
  Add-Edge -Edges $edges -Id "edge_ptbench_tile_sweep" -From "ptbench.cpu_tiled_auto" -To "ptbench.tile_sweep" `
    -Runner "ptbench_tile_sweep" -Status "ok" -Decision ("best_tile_height=" + $bestTile) `
    -Artifact $tileJson
}

$simdJson = Join-Path $simdDir "simd_sweep.json"
$simd = Read-JsonFile $simdJson
if ($simd) {
  $bestSimd = $simd.best_mode
  $bestRow = $simd.results | Where-Object { $_.mode -eq $bestSimd } | Select-Object -First 1
  Add-Edge -Edges $edges -Id "edge_ptbench_simd_sweep" -From "ptbench.scalar_packet" -To "ptbench.simd_packet" `
    -Runner "ptbench_simd_sweep" -Status "ok" -Decision ("best_mode=" + $bestSimd) `
    -Speedup ([double]$bestRow.speedup_vs_scalar) -Artifact $simdJson
}

$gpuResult = First-ResultJson $gpuComputeDir
if ($gpuResult) {
  $gpuSps = [double]$gpuResult.throughput.samples_per_sec
  $scalarSps = if ($cpuScalar) { [double]$cpuScalar.throughput.samples_per_sec } else { 0.0 }
  $speedup = if ($scalarSps -gt 0) { $gpuSps / $scalarSps } else { 0.0 }
  $mode = [string]$gpuResult.cpu_simd_mode
  $decision = if ($mode -like "*simulated*") { "runtime_simulated_do_not_choose" } elseif ($speedup -gt 1.02) { "take_candidate" } else { "keep_baseline" }
  Add-Edge -Edges $edges -Id "edge_ptbench_gpu_compute" -From "ptbench.cpu_scalar" -To "ptbench.gpu_compute" `
    -Runner "ptbench_run" -Status "ok" -Decision $decision -Speedup $speedup -Artifact $gpuComputeDir -Reason ("mode=" + $mode)
} else {
  Add-Edge -Edges $edges -Id "edge_ptbench_gpu_compute" -From "ptbench.cpu_scalar" -To "ptbench.gpu_compute" `
    -Runner "ptbench_run" -Status "failed" -Decision "blocked" -Artifact $gpuComputeDir
}

$shaderVariants = Read-JsonFile $shaderVariantJson
if ($shaderVariants) {
  foreach ($target in @(
    @{ id = "edge_gpu_workgroup_16x8"; to = "gpu.shader.wg16x8" },
    @{ id = "edge_gpu_workgroup_8x16"; to = "gpu.shader.wg8x16" },
    @{ id = "edge_gpu_workgroup_16x16"; to = "gpu.shader.wg16x16" }
  )) {
    $row = $shaderVariants.rows | Where-Object { $_.id -eq $target.to } | Select-Object -First 1
    Add-Edge -Edges $edges -Id $target.id -From "gpu.shader.wg8x8" -To $target.to `
      -Runner "glslc_static" -Status $row.status -Decision "compiled_static_no_runtime_winner" `
      -Artifact $shaderVariantJson -Reason ("spv_bytes=" + $row.spv_bytes)
  }
  $powRow = $shaderVariants.rows | Where-Object { $_.id -eq "gpu.shader.pow_small_int_mul" } | Select-Object -First 1
  $baseRow = $shaderVariants.rows | Where-Object { $_.id -eq "gpu.shader.wg8x8" } | Select-Object -First 1
  $shaderSourceText = Get-Content (Join-Path $RepoRoot "src\shaders\gpu\pathtrace.comp") -Raw
  $smallIntPowRemaining = ([regex]::Matches($shaderSourceText, "pow\s*\([^;\r\n]*(2\.0|5\.0|6\.0)\s*\)")).Count
  $powDecision = if ($powRow.status -eq "ok" -and $powRow.op_pow_mentions -lt $baseRow.op_pow_mentions) {
    "take_candidate_static"
  } elseif ($powRow.status -eq "ok" -and $smallIntPowRemaining -eq 0) {
    "already_applied"
  } else {
    "keep_baseline"
  }
  Add-Edge -Edges $edges -Id "edge_gpu_pow_small_int_mul" -From "gpu.shader.pow_builtin" -To "gpu.shader.pow_small_int_mul" `
    -Runner "glslc_static" -Status $powRow.status -Decision $powDecision -Artifact $shaderVariantJson `
    -Reason ("op_pow_mentions=" + $powRow.op_pow_mentions + "; baseline=" + $baseRow.op_pow_mentions + "; small_int_source_mentions=" + $smallIntPowRemaining)
}

$shaderMatrixJson = Join-Path $shaderMatrixDir "shader_matrix.json"
if (Test-Path $shaderMatrixJson) {
  Add-Edge -Edges $edges -Id "edge_gpu_shader_matrix" -From "gpu.backend.simulated_or_real" -To "gpu.backend.shader_matrix" `
    -Runner "ptbench_shader_matrix" -Status "ok" -Decision "capabilities_recorded" -Artifact $shaderMatrixJson
}

$memJson = Join-Path $memDir "gpu_mem_pressure.json"
if (Test-Path $memJson) {
  $mem = Read-JsonFile $memJson
  $status = if ($mem.ok) { "ok" } else { "failed" }
  Add-Edge -Edges $edges -Id "edge_gpu_memory_pressure" -From "gpu.backend.simulated_or_real" -To "gpu.backend.memory_pressure" `
    -Runner "ptbench_gpu_mem_pressure" -Status $status -Decision ("allocated_mb=" + $mem.allocated_mb) -Artifact $memJson
}

$graph = Read-JsonFile (Join-Path $ScriptDir "graph.json")
$declared = @($graph.edges | ForEach-Object { $_.id })
$seen = @($edges | ForEach-Object { $_.id })
$missing = @($declared | Where-Object { $seen -notcontains $_ })
if ($missing.Count -gt 0) {
  throw "Graph run did not explore all declared edges: $($missing -join ', ')"
}

$bestCpuMath = @($edges | Where-Object {
  $_.runner -eq "math_microbench" -and ($_.decision -eq "take_candidate" -or $_.decision -eq "candidate_requires_rng_contract_update")
} | Sort-Object -Property speedup -Descending)

$summaryPath = Join-Path $OutDir "graph_summary.json"
[pscustomobject]@{
  schema = "ptopt_graph_summary.v1"
  generated_at_local = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ssK")
  scene = $Scene
  resolution = $Resolution
  spp = $Spp
  ptbench = $Ptbench
  all_declared_edges_explored = $true
  edge_count = $edges.Count
  best_cpu_math_edges = $bestCpuMath
  edges = $edges
} | ConvertTo-Json -Depth 8 | Set-Content $summaryPath

$dotPath = Join-Path $OutDir "graph.dot"
$dot = New-Object System.Text.StringBuilder
[void]$dot.AppendLine("digraph pathtracer_optimization_graph {")
[void]$dot.AppendLine("  rankdir=LR;")
foreach ($edge in $edges) {
  $color = if ($edge.decision -like "take*") { "green" } elseif ($edge.status -eq "failed" -or $edge.decision -like "reject*") { "red" } else { "gray" }
  $label = "$($edge.id)\n$($edge.decision)\n$([math]::Round([double]$edge.speedup, 3))x"
  [void]$dot.AppendLine("  `"$($edge.from)`" -> `"$($edge.to)`" [label=`"$(Json-Escape $label)`", color=$color];")
}
[void]$dot.AppendLine("}")
Set-Content -Path $dotPath -Value $dot.ToString()

Write-Host "Graph complete: $summaryPath"
Write-Host "DOT graph: $dotPath"
