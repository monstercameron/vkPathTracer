#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Reports the largest source files and writes a JSON artifact.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this tools directory.

.PARAMETER Output
    JSON report path. Defaults to artifacts/status/source_size_report.json.

.PARAMETER IncludeRoots
    Root directories to scan relative to RepoRoot. Defaults to src.

.PARAMETER Top
    Number of largest files to print to the console. Defaults to 30.

.PARAMETER CheckGuardrails
    Emit warning-only file-size guardrail rows. This never fails the command.
#>
param(
    [string]$RepoRoot,
    [string]$Output,
    [string[]]$IncludeRoots = @("src"),
    [uint32]$Top = 30,
    [switch]$CheckGuardrails,
    [uint32]$CppWarnAbove = 1500,
    [uint32]$HeaderWarnAbove = 700,
    [uint32]$RequireNoteAbove = 3000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    $RepoRoot = Split-Path -Parent $PSScriptRoot
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Join-Path (Join-Path $RepoRoot "artifacts") "status") "source_size_report.json"
}

$sourceExtensions = @(".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hxx", ".inl")
$excludedTopLevel = @(".git", "artifacts", "build", "builds", "out", "bin")

function Get-RelativeRepoPath([string]$Root, [string]$Path) {
    $rootFull = [System.IO.Path]::GetFullPath($Root)
    if (-not $rootFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $rootFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $pathFull = [System.IO.Path]::GetFullPath($Path)
    $rootUri = New-Object System.Uri($rootFull)
    $pathUri = New-Object System.Uri($pathFull)
    return [System.Uri]::UnescapeDataString($rootUri.MakeRelativeUri($pathUri).ToString())
}

function Get-GuardrailState([string]$Extension, [uint32]$LineCount) {
    if ($LineCount -ge $RequireNoteAbove) {
        return "requires_note"
    }
    if (($Extension -in @(".cpp", ".cc", ".cxx", ".c")) -and $LineCount -gt $CppWarnAbove) {
        return "warn"
    }
    if (($Extension -in @(".h", ".hpp", ".hxx", ".inl")) -and $LineCount -gt $HeaderWarnAbove) {
        return "warn"
    }
    return "ok"
}

$files = New-Object System.Collections.Generic.List[object]

foreach ($includeRoot in $IncludeRoots) {
    if ([string]::IsNullOrWhiteSpace($includeRoot)) {
        continue
    }
    $scanRoot = Join-Path $RepoRoot $includeRoot
    if (-not (Test-Path -LiteralPath $scanRoot)) {
        Write-Warning "scan root not found: $scanRoot"
        continue
    }

    Get-ChildItem -LiteralPath $scanRoot -Recurse -File |
        Where-Object {
            $sourceExtensions -contains $_.Extension.ToLowerInvariant()
        } |
        ForEach-Object {
            $relative = Get-RelativeRepoPath $RepoRoot $_.FullName
            $topLevel = ($relative -split '/')[0]
            if ($excludedTopLevel -contains $topLevel) {
                return
            }
            $lineCount = [uint32]((Get-Content -LiteralPath $_.FullName | Measure-Object -Line).Lines)
            $extension = $_.Extension.ToLowerInvariant()
            [void]$files.Add([pscustomobject]@{
                path = $relative
                line_count = $lineCount
                extension = $extension.TrimStart('.')
                guardrail = Get-GuardrailState $extension $lineCount
            })
        }
}

$sorted = @($files | Sort-Object line_count -Descending)
$guardrailFindings = @($sorted | Where-Object { $_.guardrail -ne "ok" })

$report = [pscustomobject]@{
    schema = "vkpt.source_size_report.v1"
    generated_utc = (Get-Date).ToUniversalTime().ToString("o")
    repo_root = $RepoRoot
    include_roots = $IncludeRoots
    thresholds = [pscustomobject]@{
        cpp_warn_above = $CppWarnAbove
        header_warn_above = $HeaderWarnAbove
        require_note_above = $RequireNoteAbove
    }
    counts = [pscustomobject]@{
        files = $sorted.Count
        guardrail_findings = $guardrailFindings.Count
    }
    largest = @($sorted | Select-Object -First $Top)
    guardrail_findings = $guardrailFindings
    files = $sorted
}

$outputDir = Split-Path -Parent $Output
if (-not [string]::IsNullOrWhiteSpace($outputDir)) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding UTF8

Write-Host "source size report: $($sorted.Count) files"
Write-Host "  report=$Output"
if ($sorted.Count -gt 0) {
    $sorted | Select-Object -First $Top path, line_count, extension, guardrail | Format-Table -AutoSize
}

if ($CheckGuardrails) {
    Write-Host "guardrail findings: $($guardrailFindings.Count)"
    if ($guardrailFindings.Count -gt 0) {
        $guardrailFindings | Select-Object path, line_count, extension, guardrail | Format-Table -AutoSize
    }
}
