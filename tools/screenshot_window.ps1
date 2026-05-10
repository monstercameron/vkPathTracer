param(
    [Parameter(Mandatory=$true)][string]$ProcessName,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [int]$DelaySeconds = 0
)

if ($DelaySeconds -gt 0) { Start-Sleep -Seconds $DelaySeconds }

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$signature = @'
[DllImport("user32.dll")]
public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", SetLastError=true)]
public static extern bool GetWindowRect(System.IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll")]
public static extern bool IsWindow(System.IntPtr hWnd);
[DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr hWnd);
[DllImport("user32.dll", SetLastError=true)]
public static extern System.IntPtr FindWindow(string lpClassName, string lpWindowName);
[DllImport("user32.dll")]
public static extern bool SetForegroundWindow(System.IntPtr hWnd);
[DllImport("user32.dll")]
public static extern bool ShowWindow(System.IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")]
public static extern bool BringWindowToTop(System.IntPtr hWnd);
[DllImport("user32.dll")]
public static extern bool PrintWindow(System.IntPtr hWnd, System.IntPtr hDC, uint nFlags);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
Add-Type -MemberDefinition $signature -Name WinApi -Namespace ScreenshotPS

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero -and [ScreenshotPS.WinApi]::IsWindowVisible($_.MainWindowHandle) } | Sort-Object { ($_.MainWindowTitle.Length) } -Descending | Select-Object -First 1
if (-not $proc) {
    # fallback: find by class name
    $hwnd = [ScreenshotPS.WinApi]::FindWindow("Qt6QWindowIcon", $null)
    if ($hwnd -eq [IntPtr]::Zero) { Write-Error "no window found for $ProcessName"; exit 1 }
} else {
    $hwnd = $proc.MainWindowHandle
}

[ScreenshotPS.WinApi]::ShowWindow($hwnd, 9) | Out-Null  # SW_RESTORE
[ScreenshotPS.WinApi]::SetForegroundWindow($hwnd) | Out-Null
[ScreenshotPS.WinApi]::BringWindowToTop($hwnd) | Out-Null
Start-Sleep -Milliseconds 1500

$rect = New-Object ScreenshotPS.WinApi+RECT
[ScreenshotPS.WinApi]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { Write-Error "invalid window rect: ${w}x${h}"; exit 1 }

$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)

# Try PrintWindow first (works with offscreen content), fall back to screen capture
$hdc = $g.GetHdc()
$ok = [ScreenshotPS.WinApi]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT=2
$g.ReleaseHdc($hdc)
if (-not $ok) {
    $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, [System.Drawing.Size]::new($w, $h))
}

$bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $OutputPath ($w x $h) hwnd=$hwnd"
