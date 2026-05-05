#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Audits todos.md checkbox state and writes an agent-readable report.

.PARAMETER TodosPath
    Path to the todo document. Defaults to repo-root todos.md.

.PARAMETER Output
    JSON report path. Defaults to artifacts/status/todo_audit.json.

.PARAMETER RequireEvidence
    When set, every checked todo section must include an Evidence: or Validation:
    marker before the next todo heading.
#>
param(
    [string]$TodosPath,
    [string]$Output,
    [switch]$RequireEvidence
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($TodosPath)) {
    $TodosPath = Join-Path $RepoRoot "todos.md"
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path (Join-Path (Join-Path $RepoRoot "artifacts") "status") "todo_audit.json"
}

if (-not (Test-Path -LiteralPath $TodosPath)) {
    throw "todo file not found: $TodosPath"
}

$lines = Get-Content -LiteralPath $TodosPath
$headingPattern = '^(## )\[(?<state>[ xX])\] (?<id>[A-Z]+[0-9]+)\b(?<title>.*)$'
$auditPattern = '^- \[(?<state>[ xX])\] (?<id>AUD[0-9]+)\s*-\s*(?<title>.*)$'

$items = New-Object System.Collections.Generic.List[object]
$byId = @{}
$duplicates = New-Object System.Collections.Generic.List[string]

for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    $match = [regex]::Match($line, $headingPattern)
    $kind = "todo"
    if (-not $match.Success) {
        $match = [regex]::Match($line, $auditPattern)
        $kind = "audit"
    }
    if (-not $match.Success) {
        continue
    }

    $id = $match.Groups["id"].Value
    $state = $match.Groups["state"].Value
    $checked = $state -eq "x" -or $state -eq "X"
    $title = $match.Groups["title"].Value.Trim()
    $lineNumber = $i + 1

    $nextHeading = $lines.Count
    for ($j = $i + 1; $j -lt $lines.Count; $j++) {
        if ($lines[$j] -match $headingPattern -or $lines[$j] -match $auditPattern) {
            $nextHeading = $j
            break
        }
    }
    $section = if ($nextHeading -gt $i + 1) {
        [string]::Join("`n", $lines[($i + 1)..($nextHeading - 1)])
    } else {
        ""
    }
    $hasAcceptance = $section -match '(?im)^\*\*Acceptance:\*\*|^Acceptance:'
    $hasEvidence = $section -match '(?im)^Evidence:|^Validation:|^\*\*Evidence:\*\*|^\*\*Validation:\*\*'

    if ($byId.ContainsKey($id)) {
        [void]$duplicates.Add($id)
    } else {
        $byId[$id] = $true
    }

    [void]$items.Add([pscustomobject]@{
        id = $id
        title = $title
        kind = $kind
        checked = $checked
        line = $lineNumber
        has_acceptance = [bool]$hasAcceptance
        has_evidence = [bool]$hasEvidence
    })
}

$checkedItems = @($items | Where-Object { $_.checked })
$uncheckedItems = @($items | Where-Object { -not $_.checked })
$checkedWithoutEvidence = @($checkedItems | Where-Object { -not $_.has_evidence })
$checkedWithoutAcceptance = @($checkedItems | Where-Object { $_.kind -eq "todo" -and -not $_.has_acceptance })

$status = "ok"
if ($duplicates.Count -gt 0) {
    $status = "duplicate_ids"
}
if ($RequireEvidence -and $checkedWithoutEvidence.Count -gt 0) {
    $status = "checked_without_evidence"
}

$report = [pscustomobject]@{
    schema = "vkpt.todo_audit.v1"
    generated_utc = (Get-Date).ToUniversalTime().ToString("o")
    status = $status
    todos_path = (Resolve-Path -LiteralPath $TodosPath).Path
    counts = [pscustomobject]@{
        total = $items.Count
        checked = $checkedItems.Count
        unchecked = $uncheckedItems.Count
        audit_items = @($items | Where-Object { $_.kind -eq "audit" }).Count
        duplicate_ids = $duplicates.Count
        checked_without_acceptance = $checkedWithoutAcceptance.Count
        checked_without_evidence = $checkedWithoutEvidence.Count
    }
    duplicate_ids = @($duplicates | Sort-Object -Unique)
    checked_without_acceptance = @($checkedWithoutAcceptance | Select-Object id, title, line)
    checked_without_evidence = @($checkedWithoutEvidence | Select-Object id, title, line)
}

$outputDir = Split-Path -Parent $Output
if (-not [string]::IsNullOrWhiteSpace($outputDir)) {
    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding UTF8

Write-Host "todo audit: $($report.status)"
Write-Host "  checked=$($report.counts.checked) unchecked=$($report.counts.unchecked) total=$($report.counts.total)"
Write-Host "  report=$Output"

if ($status -ne "ok") {
    exit 1
}
