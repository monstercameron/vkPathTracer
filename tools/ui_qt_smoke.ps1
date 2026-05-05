#!/usr/bin/env powershell
<#
.SYNOPSIS
    Local UI/Qt checklist smoke for vkPathTracer.

.DESCRIPTION
    Validates that the Qt-disabled path still builds and runs CLI/headless/render
    checks, then opportunistically validates a Qt-enabled build and bounded Qt
    window launch when Qt is available.

.PARAMETER DisabledPreset
    CMake preset used for the Qt-disabled smoke build.

.PARAMETER QtPreset
    CMake preset used for the Qt-enabled smoke build.

.PARAMETER NoBuild
    Skip configure/build and use existing preset binaries.

.PARAMETER SkipQt
    Skip the Qt-enabled configure/build/window smoke.

.PARAMETER WindowFrames
    Number of frames to run before the bounded Qt window smoke exits.

.PARAMETER UiPresentHz
    UI present rate passed to Qt window smoke when the binary supports
    --ui-present-hz. Use 0 to disable this optional flag.

.PARAMETER MinimumQtDockCount
    Minimum dock widgets expected in the Qt shell readiness log emitted during
    the bounded offscreen window smoke.

.PARAMETER RequiredQtDockIds
    Dock identifiers expected in the Qt shell readiness log. This verifies the
    shell created the core editor/property panels without GUI interaction.

.PARAMETER SkipQtShellProbe
    Skip the Qt shell menu/status/dock readiness assertion.

.EXAMPLE
    powershell -NoProfile -File tools/ui_qt_smoke.ps1
    powershell -NoProfile -File tools/ui_qt_smoke.ps1 -QtPreset windows-clangcl-d3d12-qt-debug
    powershell -NoProfile -File tools/ui_qt_smoke.ps1 -NoBuild -SkipQt
#>
param(
    [string]$DisabledPreset = "desktop-clang-debug",
    [string]$QtPreset = "desktop-clang-qt-debug",
    [string]$DisabledBuildDir = "",
    [string]$QtBuildDir = "",
    [uint32]$Width = 64,
    [uint32]$Height = 64,
    [uint32]$Spp = 2,
    [uint32]$WindowSeconds = 8,
    [uint32]$WindowFrames = 5,
    [uint32]$UiPresentHz = 30,
    [uint32]$MinimumQtDockCount = 8,
    [string[]]$RequiredQtDockIds = @(
        "scene_graph",
        "inspector",
        "materials",
        "lights",
        "camera",
        "render_settings",
        "diagnostics",
        "performance"
    ),
    [switch]$NoBuild,
    [switch]$SkipQt,
    [switch]$SkipQtShellProbe,
    [string]$QtQpaPlatform = $env:VKPT_QT_QPA_PLATFORM
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ArtifactsDir = Join-Path $RepoRoot "artifacts"
$SmokeDir = Join-Path $ArtifactsDir "ui_qt_smoke"
$NoQtExr = Join-Path $SmokeDir "noqt_smoke.exr"
$QtRenderPng = Join-Path $SmokeDir "qt_render_headless.png"
$QtRenderExr = Join-Path $SmokeDir "qt_render_headless.exr"
$QtStdout = Join-Path $SmokeDir "qt_window_stdout.log"
$QtStderr = Join-Path $SmokeDir "qt_window_stderr.log"

$script:Pass = 0
$script:Fail = 0
$script:Skip = 0

function Test-IsWindowsHost {
    if ($env:OS -eq "Windows_NT") { return $true }
    $var = Get-Variable -Name IsWindows -ErrorAction SilentlyContinue
    if ($null -ne $var) { return [bool]$var.Value }
    return $false
}

function Get-SafeName([string]$Name) {
    return ($Name -replace '[^A-Za-z0-9_.-]', '_')
}

function Resolve-RepoPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $Path }
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $RepoRoot $Path)
}

function Resolve-ToolPath([string]$Name, [string[]]$Fallbacks) {
    $tool = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $tool) { return $tool.Source }
    foreach ($fallback in $Fallbacks) {
        if (-not [string]::IsNullOrWhiteSpace($fallback) -and (Test-Path $fallback)) {
            return $fallback
        }
    }
    return $Name
}

$CmakeExe = Resolve-ToolPath "cmake" @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "$env:ProgramFiles\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
)

function Get-PresetBuildDir([string]$Preset) {
    $presetFile = Join-Path $RepoRoot "CMakePresets.json"
    if (-not (Test-Path $presetFile)) { return $null }
    try {
        $json = Get-Content $presetFile -Raw | ConvertFrom-Json
        foreach ($presetInfo in @($json.configurePresets)) {
            if ($presetInfo.name -eq $Preset -and $presetInfo.binaryDir) {
                return ([string]$presetInfo.binaryDir).Replace('${sourceDir}', $RepoRoot)
            }
        }
    } catch {
        return $null
    }
    return $null
}

function Get-DefaultBuildDir([string]$Preset, [string]$Kind) {
    if ($NoBuild) {
        $presetDir = Get-PresetBuildDir $Preset
        if (-not [string]::IsNullOrWhiteSpace($presetDir)) { return $presetDir }
    }
    return (Join-Path (Join-Path $RepoRoot "build") ("ui_smoke_{0}_{1}" -f $Kind, (Get-SafeName $Preset)))
}

if ([string]::IsNullOrWhiteSpace($DisabledBuildDir)) {
    $DisabledBuildDir = Get-DefaultBuildDir $DisabledPreset "noqt"
} else {
    $DisabledBuildDir = Resolve-RepoPath $DisabledBuildDir
}

if ([string]::IsNullOrWhiteSpace($QtBuildDir)) {
    $QtBuildDir = Get-DefaultBuildDir $QtPreset "qt"
} else {
    $QtBuildDir = Resolve-RepoPath $QtBuildDir
}

function Get-PtappPath([string]$BuildDir) {
    $name = "ptapp"
    if (Test-IsWindowsHost) { $name += ".exe" }
    return (Join-Path (Join-Path $BuildDir "bin") $name)
}

function Get-OutputTail([string]$Text, [int]$MaxChars = 4000) {
    if ([string]::IsNullOrEmpty($Text)) { return "" }
    if ($Text.Length -le $MaxChars) { return $Text }
    return $Text.Substring($Text.Length - $MaxChars)
}

function Invoke-Tool([string]$File, [string[]]$Arguments) {
    try {
        $lines = & $File @Arguments 2>&1 | ForEach-Object { $_.ToString() }
        $code = $LASTEXITCODE
        return [pscustomobject]@{
            ExitCode = $code
            Output = ($lines -join [Environment]::NewLine)
        }
    } catch {
        return [pscustomobject]@{
            ExitCode = 1
            Output = $_.Exception.Message
        }
    }
}

function Assert-ToolOk([string]$Name, [string]$File, [string[]]$Arguments, [string]$ExpectedPattern = "") {
    $result = Invoke-Tool $File $Arguments
    if ($result.ExitCode -ne 0) {
        throw ("{0} exited {1}`n{2}" -f $Name, $result.ExitCode, (Get-OutputTail $result.Output))
    }
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPattern) -and $result.Output -notmatch $ExpectedPattern) {
        throw ("{0} output did not match /{1}/`n{2}" -f $Name, $ExpectedPattern, (Get-OutputTail $result.Output))
    }
    return $result.Output
}

function Test-PtappOptionSupported([string]$Exe, [string]$Option) {
    $result = Invoke-Tool $Exe @("--help")
    return ($result.ExitCode -eq 0 -and $result.Output -match [regex]::Escape($Option))
}

function Pass-Step([string]$Name) {
    Write-Host "  [$Name] [ok]" -ForegroundColor Green
    $script:Pass++
}

function Fail-Step([string]$Name, [string]$Message) {
    Write-Host "  [$Name] [FAIL] $Message" -ForegroundColor Red
    $script:Fail++
}

function Skip-Step([string]$Name, [string]$Reason) {
    Write-Host "  [$Name] [skip] $Reason" -ForegroundColor Yellow
    $script:Skip++
}

function Step([string]$Name, [scriptblock]$Body) {
    try {
        & $Body
        Pass-Step $Name
        return $true
    } catch {
        Fail-Step $Name ($_.Exception.Message)
        return $false
    }
}

function Test-QtUnavailable([string]$Output) {
    return ($Output -match 'PT_ENABLE_QT=ON requires Qt6' -or
            $Output -match 'Could not find.*Qt6' -or
            $Output -match 'Qt6.*not found' -or
            $Output -match 'No package.*Qt6')
}

function Stop-SmokeProcess([System.Diagnostics.Process]$Process) {
    if ($null -eq $Process) { return }
    try {
        if (-not $Process.HasExited) {
            $Process.Kill()
            [void]$Process.WaitForExit(5000)
        }
    } catch {
    }
}

function Quote-ProcessArgument([string]$Arg) {
    if ($Arg -notmatch '[\s"]') { return $Arg }

    $result = New-Object System.Text.StringBuilder
    [void]$result.Append('"')
    $backslashes = 0
    foreach ($ch in $Arg.ToCharArray()) {
        if ($ch -eq '\') {
            $backslashes++
        } elseif ($ch -eq '"') {
            [void]$result.Append(('\' * (($backslashes * 2) + 1)))
            [void]$result.Append('"')
            $backslashes = 0
        } else {
            if ($backslashes -gt 0) {
                [void]$result.Append(('\' * $backslashes))
                $backslashes = 0
            }
            [void]$result.Append($ch)
        }
    }
    if ($backslashes -gt 0) {
        [void]$result.Append(('\' * ($backslashes * 2)))
    }
    [void]$result.Append('"')
    return $result.ToString()
}

function Join-ProcessArguments([string[]]$Arguments) {
    return (($Arguments | ForEach-Object { Quote-ProcessArgument $_ }) -join " ")
}

function Test-UiPresentHzCoverage([string]$Exe) {
    if ($UiPresentHz -eq 0) {
        Skip-Step "qt-ui-present-hz-option" "disabled"
        return $false
    }
    if (Test-PtappOptionSupported $Exe "--ui-present-hz") {
        Pass-Step "qt-ui-present-hz-option"
        return $true
    }
    Skip-Step "qt-ui-present-hz-option" "binary help does not advertise --ui-present-hz"
    return $false
}

function Test-QtReadyField([string]$Text, [string[]]$Names) {
    foreach ($name in $Names) {
        $escaped = [regex]::Escape($name)
        if ($Text -match "(?i)$escaped[^A-Za-z0-9]*(true|yes|present|ready|1)") {
            return $true
        }
    }
    return $false
}

function Assert-QtShellSurfaceAvailabilityFromLogs {
    $stdoutText = if (Test-Path $QtStdout) { Get-Content -LiteralPath $QtStdout -Raw -ErrorAction SilentlyContinue } else { "" }
    $stderrText = if (Test-Path $QtStderr) { Get-Content -LiteralPath $QtStderr -Raw -ErrorAction SilentlyContinue } else { "" }
    $combined = $stdoutText + [Environment]::NewLine + $stderrText
    $probeLines = @($combined -split "(`r`n|`n|`r)" | Where-Object {
        $_ -match '(?i)qt shell ready|qt_shell_ready|qt-shell-ready|qt shell surfaces'
    })

    if ($probeLines.Count -eq 0) {
        throw ("Qt shell readiness marker missing. Expected a startup log like " +
               "'qt shell ready menu_bar=true status_bar=true dock_count={0} docks={1}'.`nstdout:`n{2}`nstderr:`n{3}" -f
               $MinimumQtDockCount,
               ($RequiredQtDockIds -join ","),
               (Get-OutputTail $stdoutText),
               (Get-OutputTail $stderrText))
    }

    $probe = [string]$probeLines[-1]
    if (-not (Test-QtReadyField $probe @("menu_bar", "menuBar"))) {
        throw ("Qt shell readiness marker did not report menu_bar=true.`nmarker:`n{0}" -f $probe)
    }
    if (-not (Test-QtReadyField $probe @("status_bar", "statusBar"))) {
        throw ("Qt shell readiness marker did not report status_bar=true.`nmarker:`n{0}" -f $probe)
    }

    if ($probe -notmatch '(?i)dock(?:_|-|\s)?count[^0-9]*(\d+)') {
        throw ("Qt shell readiness marker did not report dock_count.`nmarker:`n{0}" -f $probe)
    }
    $dockCount = [uint32]$Matches[1]
    if ($dockCount -lt $MinimumQtDockCount) {
        throw ("Qt shell readiness marker reported dock_count={0}; expected at least {1}.`nmarker:`n{2}" -f
               $dockCount, $MinimumQtDockCount, $probe)
    }

    foreach ($dockId in $RequiredQtDockIds) {
        if ([string]::IsNullOrWhiteSpace($dockId)) { continue }
        if ($probe -notmatch [regex]::Escape($dockId)) {
            throw ("Qt shell readiness marker is missing required dock '{0}'.`nmarker:`n{1}" -f
                   $dockId, $probe)
        }
    }

    Write-Host ("    shell-surfaces: menu_bar=true status_bar=true dock_count={0}" -f $dockCount)
}

function Test-QtShellSurfaceAvailabilityFromLogs {
    if ($SkipQtShellProbe) {
        Skip-Step "qt-shell-surfaces" "-SkipQtShellProbe"
        return
    }
    [void](Step "qt-shell-surfaces" {
        Assert-QtShellSurfaceAvailabilityFromLogs
    })
}

function Invoke-BoundedQtWindowSmoke([string]$Exe, [bool]$PassUiPresentHz) {
    New-Item -ItemType Directory -Force -Path $SmokeDir | Out-Null
    if (Test-Path $QtStdout) { Remove-Item -LiteralPath $QtStdout -Force }
    if (Test-Path $QtStderr) { Remove-Item -LiteralPath $QtStderr -Force }
    $windowArgs = @(
        "--window",
        "--platform", "qt",
        "--backend", "cpu"
    )
    if ($PassUiPresentHz -and $UiPresentHz -gt 0) {
        $windowArgs += @("--ui-present-hz", [string]$UiPresentHz)
    }
    $windowArgs += @(
        "--frames", [string]$WindowFrames,
        "--exit",
        "--window-width", [string]$Width,
        "--window-height", [string]$Height,
        "--scene", (Join-Path (Join-Path (Join-Path $RepoRoot "assets") "scenes") "cornell_native.json")
    )

    $oldQtQpaPlatform = $env:QT_QPA_PLATFORM
    if (-not [string]::IsNullOrWhiteSpace($QtQpaPlatform)) {
        $env:QT_QPA_PLATFORM = $QtQpaPlatform
    }

    $ready = $false
    $readyReason = ""
    $process = $null
    try {
        $process = Start-Process -FilePath $Exe `
            -ArgumentList (Join-ProcessArguments $windowArgs) `
            -WorkingDirectory $RepoRoot `
            -RedirectStandardOutput $QtStdout `
            -RedirectStandardError $QtStderr `
            -NoNewWindow `
            -PassThru

        $deadline = [DateTime]::UtcNow.AddSeconds([double]$WindowSeconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            $stdoutTail = if (Test-Path $QtStdout) { Get-Content -LiteralPath $QtStdout -Raw -ErrorAction SilentlyContinue } else { "" }
            $stderrTail = if (Test-Path $QtStderr) { Get-Content -LiteralPath $QtStderr -Raw -ErrorAction SilentlyContinue } else { "" }
            $combined = $stdoutTail + [Environment]::NewLine + $stderrTail
            if ($combined -match 'qt window open') {
                $ready = $true
                $readyReason = "stdout"
                break
            }
            if (Test-IsWindowsHost) {
                $process.Refresh()
                if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                    $ready = $true
                    $readyReason = "main-window-handle"
                    break
                }
            }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds 200
        }

        if (-not $ready -and $process.HasExited -and $process.ExitCode -eq 0) {
            $ready = $true
            $readyReason = "bounded-exit-zero"
        }

        if ($ready) {
            Start-Sleep -Milliseconds 500
        }

        $exitBeforeKill = $false
        $exitCode = $null
        if ($process.HasExited) {
            [void]$process.WaitForExit(5000)
            $exitBeforeKill = $true
            $exitCode = $process.ExitCode
        } else {
            Stop-SmokeProcess $process
        }

        $stdoutText = if (Test-Path $QtStdout) { Get-Content -LiteralPath $QtStdout -Raw -ErrorAction SilentlyContinue } else { "" }
        $stderrText = if (Test-Path $QtStderr) { Get-Content -LiteralPath $QtStderr -Raw -ErrorAction SilentlyContinue } else { "" }

        if (-not $ready) {
            throw ("Qt window did not become ready within {0}s`nstdout:`n{1}`nstderr:`n{2}" -f
                   $WindowSeconds, (Get-OutputTail $stdoutText), (Get-OutputTail $stderrText))
        }
        if ($exitBeforeKill -and $null -ne $exitCode -and $exitCode -ne 0) {
            throw ("Qt window process exited before bound with {0}`nstdout:`n{1}`nstderr:`n{2}" -f
                   $exitCode, (Get-OutputTail $stdoutText), (Get-OutputTail $stderrText))
        }
        Write-Host ("    ready: {0}" -f $readyReason)
        if ($PassUiPresentHz -and $UiPresentHz -gt 0) {
            Write-Host ("    ui-present-hz: {0}" -f $UiPresentHz)
        }
    } finally {
        Stop-SmokeProcess $process
        if ([string]::IsNullOrWhiteSpace($oldQtQpaPlatform)) {
            Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
        } else {
            $env:QT_QPA_PLATFORM = $oldQtQpaPlatform
        }
    }
}

function Invoke-QtSelectedHeadlessRender([string]$Exe) {
    if (Test-Path $QtRenderPng) { Remove-Item -LiteralPath $QtRenderPng -Force }
    if (Test-Path $QtRenderExr) { Remove-Item -LiteralPath $QtRenderExr -Force }
    $output = Assert-ToolOk "ptapp --render --platform qt" $Exe @(
        "--render",
        "--platform", "qt",
        "--backend", "cpu",
        "--width", [string]$Width,
        "--height", [string]$Height,
        "--spp", [string]$Spp,
        "--output", $QtRenderPng,
        "--exr-output", $QtRenderExr
    ) 'render complete'

    if ($output -notmatch 'ui shell:\s*headless\s*\(Qt requested; GUI not initialized for render\)') {
        throw ("Qt-selected render did not report the headless shell`n{0}" -f (Get-OutputTail $output))
    }
    if ($output -match 'qt window open') {
        throw ("Qt-selected render unexpectedly opened a Qt window`n{0}" -f (Get-OutputTail $output))
    }
}

function Test-QtRenderHeadlessOutput {
    if (-not (Test-Path $QtRenderExr)) {
        Skip-Step "qt-render-headless-output" "EXR was not produced by this binary"
        return
    }
    Step "qt-render-headless-output" {
        $size = (Get-Item $QtRenderExr).Length
        if ($size -lt 100) { throw "render output is suspiciously small: $size bytes" }
    } | Out-Null
}

New-Item -ItemType Directory -Force -Path $SmokeDir | Out-Null

$NoQtPtapp = Get-PtappPath $DisabledBuildDir
$QtPtapp = Get-PtappPath $QtBuildDir

Push-Location $RepoRoot
try {
    Write-Host ""
    Write-Host "=== vkPathTracer UI/Qt smoke ==="
    Write-Host ("  no-Qt preset : {0}" -f $DisabledPreset)
    Write-Host ("  no-Qt build  : {0}" -f $DisabledBuildDir)
    Write-Host ("  Qt preset    : {0}" -f $QtPreset)
    Write-Host ("  Qt build     : {0}" -f $QtBuildDir)
    Write-Host ""

    $noQtBuildReady = [bool]$NoBuild
    if ($NoBuild) {
        Skip-Step "noqt-configure" "-NoBuild"
        Skip-Step "noqt-build" "-NoBuild"
    } else {
        $noQtConfigured = Step "noqt-configure" {
            [void](Assert-ToolOk "cmake configure no-Qt" $CmakeExe @(
                "--preset", $DisabledPreset,
                "-B", $DisabledBuildDir,
                "-DPT_ENABLE_QT=OFF",
                "-DPT_ENABLE_QT_EDITOR=OFF"
            ))
        }
        if ($noQtConfigured) {
            $noQtBuildReady = Step "noqt-build" {
                [void](Assert-ToolOk "cmake build no-Qt" $CmakeExe @(
                    "--build", $DisabledBuildDir,
                    "--target", "ptapp"
                ))
            }
        } else {
            Skip-Step "noqt-build" "configure failed"
        }
    }

    $noQtCanRun = $false
    if ($noQtBuildReady) {
        $noQtCanRun = Step "noqt-binary" {
            if (-not (Test-Path $NoQtPtapp)) { throw "ptapp not found at $NoQtPtapp" }
        }
    } else {
        Skip-Step "noqt-binary" "build did not complete"
    }
    if ($noQtCanRun) {
        [void](Step "noqt-version-json" {
            [void](Assert-ToolOk "ptapp --version --json" $NoQtPtapp @("--version", "--json") '"version"')
        })
        [void](Step "noqt-doctor" {
            [void](Assert-ToolOk "ptapp --doctor" $NoQtPtapp @("--doctor") 'doctor:\s*ok')
        })
        [void](Step "noqt-ui-model-smoke" {
            [void](Assert-ToolOk "ptapp --ui-model-smoke" $NoQtPtapp @("--ui-model-smoke") 'ui model smoke:\s*ok')
        })
        [void](Step "noqt-ui-release-gate" {
            [void](Assert-ToolOk "ptapp --ui-release-gate --json" $NoQtPtapp @("--ui-release-gate", "--json") '"pending_count"\s*:\s*0')
        })
        [void](Step "noqt-headless" {
            [void](Assert-ToolOk "ptapp --headless" $NoQtPtapp @("--headless") 'headless platform initialized')
        })
        $renderOk = Step "noqt-render" {
            if (Test-Path $NoQtExr) { Remove-Item -LiteralPath $NoQtExr -Force }
            [void](Assert-ToolOk "ptapp --render" $NoQtPtapp @(
                "--render",
                "--backend", "cpu",
                "--width", [string]$Width,
                "--height", [string]$Height,
                "--spp", [string]$Spp,
                "--exr-output", $NoQtExr
            ) 'render complete')
        }
        if ($renderOk -and (Test-Path $NoQtExr)) {
            [void](Step "noqt-render-output" {
                $size = (Get-Item $NoQtExr).Length
                if ($size -lt 100) { throw "render output is suspiciously small: $size bytes" }
            })
        } elseif ($renderOk) {
            Skip-Step "noqt-render-output" "EXR was not produced by this binary"
        } else {
            Skip-Step "noqt-render-output" "render command failed"
        }
    } else {
        Skip-Step "noqt-version-json" "no runnable no-Qt binary"
        Skip-Step "noqt-doctor" "no runnable no-Qt binary"
        Skip-Step "noqt-ui-model-smoke" "no runnable no-Qt binary"
        Skip-Step "noqt-ui-release-gate" "no runnable no-Qt binary"
        Skip-Step "noqt-headless" "no runnable no-Qt binary"
        Skip-Step "noqt-render" "no runnable no-Qt binary"
        Skip-Step "noqt-render-output" "no runnable no-Qt binary"
    }

    if ($SkipQt) {
        Skip-Step "qt-configure" "-SkipQt"
        Skip-Step "qt-build" "-SkipQt"
        Skip-Step "qt-ui-present-hz-option" "-SkipQt"
        Skip-Step "qt-window-bounded" "-SkipQt"
        Skip-Step "qt-shell-surfaces" "-SkipQt"
        Skip-Step "qt-render-headless" "-SkipQt"
        Skip-Step "qt-render-headless-output" "-SkipQt"
    } elseif ($NoBuild) {
        Skip-Step "qt-configure" "-NoBuild"
        Skip-Step "qt-build" "-NoBuild"
        if (Test-Path $QtPtapp) {
            $qtPassUiPresentHz = Test-UiPresentHzCoverage $QtPtapp
            $qtWindowOk = Step "qt-window-bounded" {
                Invoke-BoundedQtWindowSmoke $QtPtapp $qtPassUiPresentHz
            }
            if ($qtWindowOk) {
                Test-QtShellSurfaceAvailabilityFromLogs
            } else {
                Skip-Step "qt-shell-surfaces" "bounded Qt window smoke failed"
            }
            $qtRenderOk = Step "qt-render-headless" {
                Invoke-QtSelectedHeadlessRender $QtPtapp
            }
            if ($qtRenderOk) {
                Test-QtRenderHeadlessOutput
            } else {
                Skip-Step "qt-render-headless-output" "render command failed"
            }
        } else {
            Skip-Step "qt-ui-present-hz-option" "Qt binary not found at $QtPtapp"
            Skip-Step "qt-window-bounded" "Qt binary not found at $QtPtapp"
            Skip-Step "qt-shell-surfaces" "Qt binary not found at $QtPtapp"
            Skip-Step "qt-render-headless" "Qt binary not found at $QtPtapp"
            Skip-Step "qt-render-headless-output" "Qt binary not found at $QtPtapp"
        }
    } else {
        $qtConfigured = $false
        $qtConfigure = Invoke-Tool $CmakeExe @(
            "--preset", $QtPreset,
            "-B", $QtBuildDir,
            "-DPT_ENABLE_QT=ON",
            "-DPT_ENABLE_QT_EDITOR=ON"
        )
        if ($qtConfigure.ExitCode -eq 0) {
            $qtConfigured = $true
            Pass-Step "qt-configure"
        } elseif (Test-QtUnavailable $qtConfigure.Output) {
            Skip-Step "qt-configure" "Qt6 not available for preset $QtPreset"
        } else {
            Fail-Step "qt-configure" ("cmake exited {0}`n{1}" -f $qtConfigure.ExitCode, (Get-OutputTail $qtConfigure.Output))
        }

        if ($qtConfigured) {
            $qtBuilt = Step "qt-build" {
                [void](Assert-ToolOk "cmake build Qt" $CmakeExe @(
                    "--build", $QtBuildDir,
                    "--target", "ptapp"
                ))
            }
            if ($qtBuilt) {
                $qtBinaryReady = Step "qt-binary" {
                    if (-not (Test-Path $QtPtapp)) { throw "ptapp not found at $QtPtapp" }
                }
                if ($qtBinaryReady) {
                    $qtPassUiPresentHz = Test-UiPresentHzCoverage $QtPtapp
                    $qtWindowOk = Step "qt-window-bounded" {
                        Invoke-BoundedQtWindowSmoke $QtPtapp $qtPassUiPresentHz
                    }
                    if ($qtWindowOk) {
                        Test-QtShellSurfaceAvailabilityFromLogs
                    } else {
                        Skip-Step "qt-shell-surfaces" "bounded Qt window smoke failed"
                    }
                    $qtRenderOk = Step "qt-render-headless" {
                        Invoke-QtSelectedHeadlessRender $QtPtapp
                    }
                    if ($qtRenderOk) {
                        Test-QtRenderHeadlessOutput
                    } else {
                        Skip-Step "qt-render-headless-output" "render command failed"
                    }
                } else {
                    Skip-Step "qt-ui-present-hz-option" "Qt binary not built"
                    Skip-Step "qt-window-bounded" "Qt binary not built"
                    Skip-Step "qt-shell-surfaces" "Qt binary not built"
                    Skip-Step "qt-render-headless" "Qt binary not built"
                    Skip-Step "qt-render-headless-output" "Qt binary not built"
                }
            } else {
                Skip-Step "qt-binary" "Qt build failed"
                Skip-Step "qt-ui-present-hz-option" "Qt build failed"
                Skip-Step "qt-window-bounded" "Qt build failed"
                Skip-Step "qt-shell-surfaces" "Qt build failed"
                Skip-Step "qt-render-headless" "Qt build failed"
                Skip-Step "qt-render-headless-output" "Qt build failed"
            }
        } else {
            Skip-Step "qt-build" "Qt configure did not complete"
            Skip-Step "qt-ui-present-hz-option" "Qt configure did not complete"
            Skip-Step "qt-window-bounded" "Qt configure did not complete"
            Skip-Step "qt-shell-surfaces" "Qt configure did not complete"
            Skip-Step "qt-render-headless" "Qt configure did not complete"
            Skip-Step "qt-render-headless-output" "Qt configure did not complete"
        }
    }

    Write-Host ""
    if ($script:Fail -eq 0) {
        Write-Host ("PASSED ({0} passed, {1} skipped)" -f $script:Pass, $script:Skip) -ForegroundColor Green
        exit 0
    }

    Write-Host ("FAILED ({0} failed, {1} passed, {2} skipped)" -f $script:Fail, $script:Pass, $script:Skip) -ForegroundColor Red
    exit 1
} finally {
    Pop-Location
}
