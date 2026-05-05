param(
  [string]$Ptbench = "build\d3d12-optimization\bin\ptbench.exe",
  [string]$Scene = "assets/scenes/cornell_native.json",
  [string]$Resolution = "96x96",
  [int]$Spp = 2,
  [int]$Repeats = 3,
  [string]$OutDir = "experiments\d3d12_dxr_optimization_graph\results"
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

function Read-JsonFile {
  param([string]$Path)
  if (!(Test-Path $Path)) { return $null }
  return Get-Content $Path -Raw | ConvertFrom-Json
}

function Invoke-Step {
  param(
    [string]$Name,
    [string]$FilePath,
    [string[]]$Arguments,
    [hashtable]$Env = @{},
    [switch]$AllowFailure
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
  if ($code -ne 0 -and !$AllowFailure) {
    throw "$Name failed with exit code $code"
  }
  return $code
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

function Run-Bench {
  param(
    [string]$Name,
    [string]$Backend,
    [string]$Renderer,
    [hashtable]$Env = @{},
    [int]$LocalSpp = $Spp
  )
  $parentDir = Join-Path $OutDir ("ptbench\" + $Name)
  Reset-Dir $parentDir
  $reps = @()
  for ($rep = 1; $rep -le [math]::Max(1, $Repeats); ++$rep) {
    $dir = Join-Path $parentDir ("rep" + $rep)
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $args = @(
      "run", "--scene", $Scene, "--backend", $Backend, "--renderer-path", $Renderer,
      "--resolution", $Resolution, "--spp", "$LocalSpp", "--output", $dir
    )
    $code = Invoke-Step -Name "ptbench_$Name`_rep$rep" -FilePath $Ptbench -Arguments $args -Env $Env -AllowFailure
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
  $result = if ($ok.Count -gt 0) { $ok[0].result } else { $null }
  return [pscustomobject]@{
    name = $Name
    backend = $Backend
    renderer = $Renderer
    env = $Env
    exit_code = if ($ok.Count -gt 0) { 0 } else { 1 }
    status = if ($ok.Count -gt 0) { "ok" } else { "failed" }
    artifact = $parentDir
    result = $result
    repeats = $reps
    median_render_ms = Median -Values @($ok | ForEach-Object { [double]$_.result.timing.render_ms })
    median_build_ms = Median -Values @($ok | ForEach-Object { [double]$_.result.timing.build_ms })
    median_paths_per_sec = Median -Values @($ok | ForEach-Object { [double]$_.result.throughput.paths_per_sec })
    median_samples_per_sec = Median -Values @($ok | ForEach-Object { [double]$_.result.throughput.samples_per_sec })
  }
}

function Median {
  param([double[]]$Values)
  $sorted = @($Values | Sort-Object)
  if ($sorted.Count -eq 0) { return 0.0 }
  $mid = [int]($sorted.Count / 2)
  if (($sorted.Count % 2) -eq 1) {
    return [double]$sorted[$mid]
  }
  return ([double]$sorted[$mid - 1] + [double]$sorted[$mid]) / 2.0
}

function Env-With {
  param([hashtable]$Base, [hashtable]$Overrides)
  $merged = @{}
  foreach ($key in $Base.Keys) {
    $merged[$key] = $Base[$key]
  }
  foreach ($key in $Overrides.Keys) {
    $merged[$key] = $Overrides[$key]
  }
  return $merged
}

function Metric {
  param($Run, [string]$Field)
  if (!$Run -or !$Run.result) { return 0.0 }
  if ($Field -eq "paths") { return [double]$Run.median_paths_per_sec }
  if ($Field -eq "samples") { return [double]$Run.median_samples_per_sec }
  if ($Field -eq "render_ms") { return [double]$Run.median_render_ms }
  if ($Field -eq "build_ms") { return [double]$Run.median_build_ms }
  return 0.0
}

function Speedup {
  param($Base, $Candidate, [string]$Field = "paths")
  $b = Metric $Base $Field
  $c = Metric $Candidate $Field
  if ($b -le 0) { return 0.0 }
  return $c / $b
}

function LowerMsSpeedup {
  param($Base, $Candidate, [string]$Field)
  $b = Metric $Base $Field
  $c = Metric $Candidate $Field
  if ($c -le 0) { return 0.0 }
  return $b / $c
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

function Dxc-Compile {
  param([string]$Name, [string[]]$CompileArgs)
  $dxc = Get-Command dxc -ErrorAction SilentlyContinue
  if (!$dxc) {
    $sdkDxc = "C:\VulkanSDK\1.4.341.1\Bin\dxc.exe"
    if (Test-Path $sdkDxc) { $dxc = [pscustomobject]@{ Source = $sdkDxc } }
  }
  $dir = Join-Path $OutDir "shader_static"
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  if (!$dxc) {
    return [pscustomobject]@{ name=$Name; status="failed"; reason="dxc not found"; artifact=$dir; bytes=0 }
  }
  $out = Join-Path $dir ($Name + ".dxil")
  $fullArgs = $CompileArgs + @("-Fo", $out)
  $code = Invoke-Step -Name ("dxc_" + $Name) -FilePath $dxc.Source -Arguments $fullArgs -AllowFailure
  $bytes = if (Test-Path $out) { (Get-Item $out).Length } else { 0 }
  return [pscustomobject]@{
    name = $Name
    status = if ($code -eq 0 -and $bytes -gt 0) { "ok" } else { "failed" }
    reason = if ($code -eq 0) { "" } else { "dxc exit $code" }
    artifact = $out
    bytes = $bytes
  }
}

if (!(Test-Path $Ptbench)) {
  throw "ptbench not found: $Ptbench"
}

Note ""
Note "## Run $(Get-Date -Format o)"
Note "ptbench=$Ptbench"
Note "scene=$Scene resolution=$Resolution spp=$Spp repeats=$Repeats"

$runs = @{}
$common = @{
  PT_D3D12_RAYS_PER_PIXEL = "1"
  PT_D3D12_FORCE_READBACK_EVERY_SAMPLE = "0"
  PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS = "1"
  PT_D3D12_DXR_BUILD_MODE = "fast_trace"
}

$runs["cpu_scalar"] = Run-Bench -Name "cpu_scalar" -Backend "cpu" -Renderer "cpu-scalar"
$runs["d3d12_compute"] = Run-Bench -Name "d3d12_compute_rpp1" -Backend "d3d12" -Renderer "d3d12-compute" -Env $common
$runs["d3d12_dxr"] = Run-Bench -Name "d3d12_dxr_fast_trace" -Backend "d3d12-dxr" -Renderer "dxr" -Env $common
$runs["compute_rpp2"] = Run-Bench -Name "d3d12_compute_rpp2" -Backend "d3d12" -Renderer "d3d12-compute" -Env (Env-With $common @{ PT_D3D12_RAYS_PER_PIXEL = "2" })
$runs["compute_rpp4"] = Run-Bench -Name "d3d12_compute_rpp4" -Backend "d3d12" -Renderer "d3d12-compute" -Env (Env-With $common @{ PT_D3D12_RAYS_PER_PIXEL = "4" })
$runs["compute_rpp8"] = Run-Bench -Name "d3d12_compute_rpp8" -Backend "d3d12" -Renderer "d3d12-compute" -Env (Env-With $common @{ PT_D3D12_RAYS_PER_PIXEL = "8" })
$runs["compute_readback_every"] = Run-Bench -Name "d3d12_compute_readback_every" -Backend "d3d12" -Renderer "d3d12-compute" -Env (Env-With $common @{ PT_D3D12_FORCE_READBACK_EVERY_SAMPLE = "1" })
$runs["compute_dynamic_off"] = Run-Bench -Name "d3d12_compute_dynamic_off" -Backend "d3d12" -Renderer "d3d12-compute" -Env (Env-With $common @{ PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS = "0" })
$runs["dxr_fast_build"] = Run-Bench -Name "d3d12_dxr_fast_build" -Backend "d3d12-dxr" -Renderer "dxr" -Env (Env-With $common @{ PT_D3D12_DXR_BUILD_MODE = "fast_build" })
$runs["dxr_none"] = Run-Bench -Name "d3d12_dxr_build_none" -Backend "d3d12-dxr" -Renderer "dxr" -Env (Env-With $common @{ PT_D3D12_DXR_BUILD_MODE = "none" })
$runs["dxr_dynamic_off"] = Run-Bench -Name "d3d12_dxr_dynamic_off" -Backend "d3d12-dxr" -Renderer "dxr" -Env (Env-With $common @{ PT_D3D12_DYNAMIC_INSTANCE_TRANSFORMS = "0" })

$csPath = Join-Path $RepoRoot "src\shaders\gpu\pathtrace_cs.hlsl"
$rtPath = Join-Path $RepoRoot "src\shaders\gpu\pathtrace_rt.hlsl"
$shaderMain = Dxc-Compile -Name "pathtrace_cs_main" -CompileArgs @("-T", "cs_6_0", "-E", "main", $csPath)
$shaderTonemap = Dxc-Compile -Name "pathtrace_cs_tonemap" -CompileArgs @("-T", "cs_6_0", "-E", "tonemap_main", $csPath)
$shaderDxr = Dxc-Compile -Name "pathtrace_rt_lib" -CompileArgs @("-T", "lib_6_3", $rtPath)

$shaderText = (Get-Content $csPath -Raw) + "`n" + (Get-Content $rtPath -Raw)
$smallIntPowCount = ([regex]::Matches($shaderText, "pow\s*\([^;\r\n]*(2\.0|5\.0|6\.0)\s*\)")).Count

$fastBuildBuildSpeedup = LowerMsSpeedup $runs["d3d12_dxr"] $runs["dxr_fast_build"] "build_ms"
$fastBuildRenderSpeedup = Speedup $runs["d3d12_dxr"] $runs["dxr_fast_build"] "paths"
$fastBuildDecision = if ($runs["dxr_fast_build"].status -eq "ok" -and
                         $fastBuildBuildSpeedup -gt 1.02 -and
                         $fastBuildRenderSpeedup -gt 0.98) { "take_candidate_build" } else { "keep_fast_trace" }
$noneBuildSpeedup = LowerMsSpeedup $runs["d3d12_dxr"] $runs["dxr_none"] "build_ms"
$noneRenderSpeedup = Speedup $runs["d3d12_dxr"] $runs["dxr_none"] "paths"
$noneDecision = if ($runs["dxr_none"].status -eq "ok" -and
                    $noneBuildSpeedup -gt 1.02 -and
                    $noneRenderSpeedup -gt 0.98) { "take_candidate_build" } else { "keep_fast_trace" }

$edges = [System.Collections.ArrayList]::new()
Add-Edge $edges "edge_backend_cpu_to_d3d12_compute" "backend.cpu_scalar" "backend.d3d12_compute" "ptbench_run" `
  $runs["d3d12_compute"].status $(if ((Speedup $runs["cpu_scalar"] $runs["d3d12_compute"] "samples") -gt 1.02) { "take_candidate" } else { "keep_baseline" }) `
  (Speedup $runs["cpu_scalar"] $runs["d3d12_compute"] "samples") $runs["d3d12_compute"].artifact
Add-Edge $edges "edge_backend_d3d12_compute_to_dxr" "backend.d3d12_compute" "backend.d3d12_dxr" "ptbench_run" `
  $runs["d3d12_dxr"].status $(if ($runs["d3d12_dxr"].result.cpu_simd_mode -eq "d3d12-dxr" -and (Speedup $runs["d3d12_compute"] $runs["d3d12_dxr"] "samples") -gt 1.02) { "take_candidate" } else { "keep_or_fallback_compute" }) `
  (Speedup $runs["d3d12_compute"] $runs["d3d12_dxr"] "samples") $runs["d3d12_dxr"].artifact ("mode=" + $runs["d3d12_dxr"].result.cpu_simd_mode)
Add-Edge $edges "edge_compute_rpp_1_to_2" "d3d12.compute.rpp1" "d3d12.compute.rpp2" "ptbench_env_sweep" $runs["compute_rpp2"].status `
  $(if ((Speedup $runs["d3d12_compute"] $runs["compute_rpp2"] "paths") -gt 1.02) { "take_candidate" } else { "keep_baseline" }) `
  (Speedup $runs["d3d12_compute"] $runs["compute_rpp2"] "paths") $runs["compute_rpp2"].artifact
Add-Edge $edges "edge_compute_rpp_1_to_4" "d3d12.compute.rpp1" "d3d12.compute.rpp4" "ptbench_env_sweep" $runs["compute_rpp4"].status `
  $(if ((Speedup $runs["d3d12_compute"] $runs["compute_rpp4"] "paths") -gt 1.02) { "take_candidate" } else { "keep_baseline" }) `
  (Speedup $runs["d3d12_compute"] $runs["compute_rpp4"] "paths") $runs["compute_rpp4"].artifact
Add-Edge $edges "edge_compute_rpp_1_to_8" "d3d12.compute.rpp1" "d3d12.compute.rpp8" "ptbench_env_sweep" $runs["compute_rpp8"].status `
  $(if ((Speedup $runs["d3d12_compute"] $runs["compute_rpp8"] "paths") -gt 1.02) { "take_candidate" } else { "keep_baseline" }) `
  (Speedup $runs["d3d12_compute"] $runs["compute_rpp8"] "paths") $runs["compute_rpp8"].artifact
Add-Edge $edges "edge_compute_readback_final_to_every" "d3d12.compute.readback_final" "d3d12.compute.readback_every_sample" "ptbench_env_sweep" $runs["compute_readback_every"].status `
  $(if ((Speedup $runs["d3d12_compute"] $runs["compute_readback_every"] "paths") -gt 1.05 -and (Metric $runs["compute_readback_every"] "render_ms") -lt (Metric $runs["d3d12_compute"] "render_ms")) { "take_candidate" } else { "keep_final_only" }) `
  (Speedup $runs["d3d12_compute"] $runs["compute_readback_every"] "paths") $runs["compute_readback_every"].artifact
Add-Edge $edges "edge_compute_dynamic_instances_on_to_off" "d3d12.compute.dynamic_instances_on" "d3d12.compute.dynamic_instances_off" "ptbench_env_sweep" $runs["compute_dynamic_off"].status `
  $(if ((Speedup $runs["d3d12_compute"] $runs["compute_dynamic_off"] "paths") -gt 1.05) { "take_candidate" } else { "keep_dynamic_on" }) `
  (Speedup $runs["d3d12_compute"] $runs["compute_dynamic_off"] "paths") $runs["compute_dynamic_off"].artifact
Add-Edge $edges "edge_dxr_fast_trace_to_fast_build" "dxr.as.fast_trace" "dxr.as.fast_build" "ptbench_env_sweep" $runs["dxr_fast_build"].status `
  $fastBuildDecision $fastBuildBuildSpeedup $runs["dxr_fast_build"].artifact ("render_speedup=" + $fastBuildRenderSpeedup)
Add-Edge $edges "edge_dxr_fast_trace_to_none" "dxr.as.fast_trace" "dxr.as.none" "ptbench_env_sweep" $runs["dxr_none"].status `
  $noneDecision $noneBuildSpeedup $runs["dxr_none"].artifact ("render_speedup=" + $noneRenderSpeedup)
Add-Edge $edges "edge_dxr_dynamic_instances_on_to_off" "dxr.dynamic_instances_on" "dxr.dynamic_instances_off" "ptbench_env_sweep" $runs["dxr_dynamic_off"].status `
  $(if ((Speedup $runs["d3d12_dxr"] $runs["dxr_dynamic_off"] "paths") -gt 1.05) { "take_candidate" } else { "keep_dynamic_on" }) `
  (Speedup $runs["d3d12_dxr"] $runs["dxr_dynamic_off"] "paths") $runs["dxr_dynamic_off"].artifact
Add-Edge $edges "edge_shader_compute_main_compile" "shader.compute.main_source" "shader.compute.main_dxil" "dxc_static" $shaderMain.status `
  $(if ($shaderMain.status -eq "ok") { "compiled" } else { "blocked" }) 0.0 $shaderMain.artifact $shaderMain.reason
Add-Edge $edges "edge_shader_compute_tonemap_compile" "shader.compute.tonemap_source" "shader.compute.tonemap_dxil" "dxc_static" $shaderTonemap.status `
  $(if ($shaderTonemap.status -eq "ok") { "compiled" } else { "blocked" }) 0.0 $shaderTonemap.artifact $shaderTonemap.reason
Add-Edge $edges "edge_shader_dxr_library_compile" "shader.dxr.library_source" "shader.dxr.library_dxil" "dxc_static" $shaderDxr.status `
  $(if ($shaderDxr.status -eq "ok") { "compiled" } else { "blocked" }) 0.0 $shaderDxr.artifact $shaderDxr.reason
Add-Edge $edges "edge_shader_small_int_pow_scan" "shader.pow_builtin_small_int" "shader.pow_mul_small_int" "source_scan" "ok" `
  $(if ($smallIntPowCount -eq 0) { "already_applied" } else { "needs_lowering" }) 0.0 $csPath ("small_int_pow_mentions=" + $smallIntPowCount)

$graph = Read-JsonFile (Join-Path $ScriptDir "graph.json")
$declared = @($graph.edges | ForEach-Object { $_.id })
$explored = @($edges | ForEach-Object { $_.id })
$missing = @($declared | Where-Object { $explored -notcontains $_ })

$runRows = @($runs.GetEnumerator() | ForEach-Object {
  $r = $_.Value
  [pscustomobject]@{
    name = $r.name
    status = $r.status
    backend = $r.backend
    renderer = $r.renderer
    cpu_simd_mode = if ($r.result) { $r.result.cpu_simd_mode } else { "" }
    repeat_count = @($r.repeats).Count
    render_ms = Metric $r "render_ms"
    build_ms = Metric $r "build_ms"
    paths_per_sec = Metric $r "paths"
    samples_per_sec = Metric $r "samples"
    artifact = $r.artifact
  }
})

$summary = [pscustomobject]@{
  schema = "ptopt_d3d12_dxr_summary.v1"
  generated_at_local = (Get-Date -Format o)
  scene = $Scene
  resolution = $Resolution
  spp = $Spp
  ptbench = $Ptbench
  all_declared_edges_explored = ($missing.Count -eq 0)
  edge_count = $edges.Count
  missing_edges = $missing
  runs = $runRows
  edges = $edges
}
$summaryPath = Join-Path $OutDir "graph_summary.json"
$summary | ConvertTo-Json -Depth 8 | Set-Content $summaryPath

$dotPath = Join-Path $OutDir "graph.dot"
$dot = "digraph d3d12_dxr_optimization {`n  rankdir=LR;`n"
foreach ($edge in $edges) {
  $label = "$($edge.id)\n$($edge.decision)\n$([math]::Round($edge.speedup, 4))x"
  $color = if ($edge.status -ne "ok") { "red" } elseif ($edge.decision -like "take*") { "green" } else { "gray" }
  $dot += "  `"$($edge.from)`" -> `"$($edge.to)`" [label=`"$label`", color=$color];`n"
}
$dot += "}`n"
Set-Content -Path $dotPath -Value $dot

Note ""
Note "summary=$summaryPath"
Note "dot=$dotPath"
Note "all_declared_edges_explored=$($missing.Count -eq 0) edge_count=$($edges.Count)"
foreach ($row in $runRows | Sort-Object name) {
  Note ("run {0} status={1} mode={2} build_ms={3:n6} render_ms={4:n6} paths_per_sec={5:n3}" -f `
      $row.name, $row.status, $row.cpu_simd_mode, $row.build_ms, $row.render_ms, $row.paths_per_sec)
}
foreach ($edge in $edges) {
  Note ("edge {0} status={1} decision={2} speedup={3:n6} reason={4}" -f `
      $edge.id, $edge.status, $edge.decision, $edge.speedup, $edge.reason)
}

Write-Host "Graph complete: $summaryPath"
Write-Host "DOT graph: $dotPath"
if ($missing.Count -gt 0) {
  Write-Host "Missing edges: $($missing -join ', ')"
  exit 2
}
