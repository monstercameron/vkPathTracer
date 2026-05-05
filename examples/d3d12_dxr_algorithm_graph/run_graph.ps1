param(
  [string]$Ptbench = "build\d3d12-optimization\bin\ptbench.exe",
  [string]$Scene = "assets/scenes/cornell_native.json",
  [string]$Resolution = "64x64",
  [int]$Spp = 2,
  [int]$Repeats = 2,
  [string]$OutDir = "examples\d3d12_dxr_algorithm_graph\results"
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Resolve-Path (Join-Path $ScriptDir "..\..")
$OutDir = Join-Path $RepoRoot $OutDir
$Ptbench = Join-Path $RepoRoot $Ptbench
$Scene = Join-Path $RepoRoot $Scene
$NotesPath = Join-Path $ScriptDir "NOTES.md"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Note {
  param([string]$Text)
  Add-Content -Path $NotesPath -Value $Text
}

function Median {
  param([double[]]$Values)
  $sorted = @($Values | Sort-Object)
  if ($sorted.Count -eq 0) { return 0.0 }
  $mid = [int]($sorted.Count / 2)
  if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$mid] }
  return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

function Reset-Dir {
  param([string]$Path)
  $full = [System.IO.Path]::GetFullPath($Path)
  $root = [System.IO.Path]::GetFullPath($OutDir)
  if (!$full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing to reset path outside output dir: $full"
  }
  if (Test-Path $full) {
    Remove-Item -LiteralPath $full -Recurse -Force
  }
  New-Item -ItemType Directory -Force -Path $full | Out-Null
}

function Read-JsonFile {
  param([string]$Path)
  if (!(Test-Path $Path)) { return $null }
  return Get-Content $Path -Raw | ConvertFrom-Json
}

function Env-With {
  param([hashtable]$Base, [hashtable]$Overrides)
  $merged = @{}
  foreach ($key in $Base.Keys) { $merged[$key] = $Base[$key] }
  foreach ($key in $Overrides.Keys) { $merged[$key] = $Overrides[$key] }
  return $merged
}

function Invoke-Step {
  param(
    [string]$Name,
    [string]$FilePath,
    [string[]]$Arguments,
    [hashtable]$Env = @{}
  )
  Write-Host "[$Name] $FilePath $($Arguments -join ' ')"
  $old = @{}
  foreach ($key in $Env.Keys) {
    $old[$key] = [Environment]::GetEnvironmentVariable($key, "Process")
    [Environment]::SetEnvironmentVariable($key, [string]$Env[$key], "Process")
  }
  try {
    & $FilePath @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    $code = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
  } finally {
    foreach ($key in $Env.Keys) {
      [Environment]::SetEnvironmentVariable($key, $old[$key], "Process")
    }
  }
  return $code
}

function Run-Bench {
  param(
    [string]$Name,
    [hashtable]$Env
  )
  $parentDir = Join-Path $OutDir ("ptbench\" + $Name)
  Reset-Dir $parentDir
  $reps = @()
  for ($rep = 1; $rep -le [math]::Max(1, $Repeats); ++$rep) {
    $dir = Join-Path $parentDir ("rep" + $rep)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $args = @(
      "run", "--scene", $Scene, "--backend", "d3d12", "--renderer-path", "d3d12-compute",
      "--resolution", $Resolution, "--spp", "$Spp", "--output", $dir
    )
    $code = Invoke-Step -Name "ptbench_$Name`_rep$rep" -FilePath $Ptbench -Arguments $args -Env $Env
    $result = Read-JsonFile (Join-Path $dir "results.json")
    $reps += [pscustomobject]@{
      rep = $rep
      exit_code = $code
      status = if ($code -eq 0 -and $result) { "ok" } else { "failed" }
      artifact = $dir
      result = $result
    }
  }
  $ok = @($reps | Where-Object { $_.status -eq "ok" -and $_.result })
  return [pscustomobject]@{
    name = $Name
    env = $Env
    status = if ($ok.Count -gt 0) { "ok" } else { "failed" }
    artifact = $parentDir
    repeats = $reps
    median_render_ms = Median -Values @($ok | ForEach-Object { [double]$_.result.timing.render_ms })
    median_build_ms = Median -Values @($ok | ForEach-Object { [double]$_.result.timing.build_ms })
    median_paths_per_sec = Median -Values @($ok | ForEach-Object { [double]$_.result.throughput.paths_per_sec })
    median_samples_per_sec = Median -Values @($ok | ForEach-Object { [double]$_.result.throughput.samples_per_sec })
    image_hash = if ($ok.Count -gt 0) { $ok[0].result.image_hash } else { "" }
  }
}

function Speedup {
  param($Base, $Candidate)
  if (!$Base -or !$Candidate -or $Base.median_paths_per_sec -le 0) { return 0.0 }
  return [double]$Candidate.median_paths_per_sec / [double]$Base.median_paths_per_sec
}

function BuildSpeedup {
  param($Base, $Candidate)
  if (!$Base -or !$Candidate -or $Candidate.median_build_ms -le 0) { return 0.0 }
  return [double]$Base.median_build_ms / [double]$Candidate.median_build_ms
}

if (!(Test-Path $Ptbench)) {
  throw "ptbench not found: $Ptbench"
}

Note ""
Note "## Run $(Get-Date -Format o)"
Note "ptbench=$Ptbench"
Note "scene=$Scene resolution=$Resolution spp=$Spp repeats=$Repeats"

$baseEnv = @{
  PT_D3D12_RAYS_PER_PIXEL = "8"
  PT_D3D12_FORCE_READBACK_EVERY_SAMPLE = "0"
  PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS = "1"
  PT_D3D12_DXR_BUILD_MODE = "fast_trace"
  PT_D3D12_BVH_LEAF_SIZE = "4"
  PT_D3D12_BVH_BUCKETS = "8"
  PT_D3D12_BVH_SPLIT_MODE = "sah"
  PT_D3D12_SHADER_TRAVERSAL = "baseline"
}

$runs = New-Object System.Collections.ArrayList
$edges = New-Object System.Collections.ArrayList
$runByName = @{}

$leafSizes = @(1, 2, 4, 8, 16)
$bucketCounts = @(4, 8, 16)

foreach ($leaf in $leafSizes) {
  foreach ($bucket in $bucketCounts) {
    $name = "bvh_sah_leaf$leaf`_bucket$bucket"
    $env = Env-With $baseEnv @{
      PT_D3D12_BVH_LEAF_SIZE = "$leaf"
      PT_D3D12_BVH_BUCKETS = "$bucket"
      PT_D3D12_BVH_SPLIT_MODE = "sah"
      PT_D3D12_SHADER_TRAVERSAL = "baseline"
    }
    $run = Run-Bench -Name $name -Env $env
    [void]$runs.Add($run)
    $runByName[$name] = $run
  }
}

foreach ($leaf in $leafSizes) {
  $name = "bvh_median_leaf$leaf"
  $env = Env-With $baseEnv @{
    PT_D3D12_BVH_LEAF_SIZE = "$leaf"
    PT_D3D12_BVH_BUCKETS = "8"
    PT_D3D12_BVH_SPLIT_MODE = "median"
    PT_D3D12_SHADER_TRAVERSAL = "baseline"
  }
  $run = Run-Bench -Name $name -Env $env
  [void]$runs.Add($run)
  $runByName[$name] = $run
}

$baseline = $runByName["bvh_sah_leaf4_bucket8"]
$okBvh = @($runs | Where-Object { $_.status -eq "ok" })
$bestBvh = $okBvh | Sort-Object -Property @{ Expression = { [double]$_.median_paths_per_sec }; Descending = $true } | Select-Object -First 1

foreach ($run in $runs) {
  $speedup = Speedup $baseline $run
  $buildSpeedup = BuildSpeedup $baseline $run
  if ($run.name -eq $baseline.name) {
    $decision = "baseline"
  } elseif ($run.status -ne "ok") {
    $decision = "failed"
  } elseif ($run.name -eq $bestBvh.name) {
    $decision = "best_bvh_candidate"
  } elseif ($speedup -gt 1.0) {
    $decision = "faster_than_baseline"
  } else {
    $decision = "reject"
  }
  [void]$edges.Add([pscustomobject]@{
    id = "edge_" + $baseline.name + "_to_" + $run.name
    from = $baseline.name
    to = $run.name
    explored = $true
    status = $run.status
    decision = $decision
    paths_speedup = $speedup
    build_speedup = $buildSpeedup
    artifact = $run.artifact
  })
}

$shaderModes = @("bounds_helper", "near_order")
$shaderRuns = New-Object System.Collections.ArrayList
[void]$shaderRuns.Add($bestBvh)
foreach ($mode in $shaderModes) {
  $name = $bestBvh.name + "_shader_" + $mode
  $env = Env-With $bestBvh.env @{ PT_D3D12_SHADER_TRAVERSAL = $mode }
  $run = Run-Bench -Name $name -Env $env
  [void]$shaderRuns.Add($run)
  $speedup = Speedup $bestBvh $run
  [void]$edges.Add([pscustomobject]@{
    id = "edge_" + $bestBvh.name + "_to_" + $name
    from = $bestBvh.name
    to = $name
    explored = $true
    status = $run.status
    decision = if ($run.status -ne "ok") { "failed" } elseif ($speedup -gt 1.0) { "faster_than_best_bvh" } else { "reject" }
    paths_speedup = $speedup
    build_speedup = BuildSpeedup $bestBvh $run
    artifact = $run.artifact
  })
}

$okFinal = @($shaderRuns | Where-Object { $_.status -eq "ok" })
$bestFinal = $okFinal | Sort-Object -Property @{ Expression = { [double]$_.median_paths_per_sec }; Descending = $true } | Select-Object -First 1

$summary = [pscustomobject]@{
  schema = "ptopt_d3d12_algorithm_graph_summary.v1"
  generated_at = (Get-Date -Format o)
  scene = $Scene
  resolution = $Resolution
  spp = $Spp
  repeats = $Repeats
  baseline = $baseline
  best_bvh = $bestBvh
  best_final = $bestFinal
  runs = $runs
  shader_runs = $shaderRuns
  edges = $edges
  explored_edges = @($edges).Count
  failed_edges = @($edges | Where-Object { $_.status -ne "ok" }).Count
}

$summaryPath = Join-Path $OutDir "graph_summary.json"
$summary | ConvertTo-Json -Depth 12 | Set-Content -Path $summaryPath -Encoding UTF8

$dotPath = Join-Path $OutDir "graph.dot"
$dot = New-Object System.Collections.ArrayList
[void]$dot.Add("digraph d3d12_algorithm_graph {")
[void]$dot.Add("  rankdir=LR;")
foreach ($edge in $edges) {
  $color = if ($edge.status -ne "ok") { "red" } elseif ($edge.decision -like "*best*" -or $edge.decision -like "faster*") { "green" } else { "gray" }
  $label = "{0} paths={1:N3} build={2:N3}" -f $edge.decision, $edge.paths_speedup, $edge.build_speedup
  [void]$dot.Add(("  ""{0}"" -> ""{1}"" [label=""{2}"", color=""{3}""];" -f $edge.from, $edge.to, $label, $color))
}
[void]$dot.Add("}")
$dot | Set-Content -Path $dotPath -Encoding UTF8

Note ""
Note "### Summary"
Note "baseline=$($baseline.name) paths=$([math]::Round($baseline.median_paths_per_sec, 2)) build_ms=$([math]::Round($baseline.median_build_ms, 3))"
Note "best_bvh=$($bestBvh.name) paths=$([math]::Round($bestBvh.median_paths_per_sec, 2)) build_ms=$([math]::Round($bestBvh.median_build_ms, 3)) speedup=$([math]::Round((Speedup $baseline $bestBvh), 4))"
Note "best_final=$($bestFinal.name) paths=$([math]::Round($bestFinal.median_paths_per_sec, 2)) build_ms=$([math]::Round($bestFinal.median_build_ms, 3)) speedup_vs_baseline=$([math]::Round((Speedup $baseline $bestFinal), 4))"
Note "edges=$(@($edges).Count) failed=$(@($edges | Where-Object { $_.status -ne 'ok' }).Count)"
Note ""
Note "name,status,paths_per_sec,render_ms,build_ms,image_hash"
foreach ($run in ($runs + $shaderRuns | Sort-Object -Property @{ Expression = { [double]$_.median_paths_per_sec }; Descending = $true })) {
  Note "$($run.name),$($run.status),$([math]::Round($run.median_paths_per_sec,2)),$([math]::Round($run.median_render_ms,3)),$([math]::Round($run.median_build_ms,3)),$($run.image_hash)"
}

Write-Host "graph summary: $summaryPath"
Write-Host "graph dot: $dotPath"
Write-Host "best final: $($bestFinal.name)"
