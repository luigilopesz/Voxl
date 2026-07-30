<#
.SYNOPSIS
    Captures the Voxl window to a PNG for visual review.

.DESCRIPTION
    Finds the main window of a running process, brings it to the foreground and
    grabs its client area via BitBlt from the screen. Screen capture rather than
    PrintWindow is deliberate: PrintWindow asks the window to redraw itself into
    a DC, which an OpenGL window backed by a swap chain cannot do, and returns a
    black or garbage rectangle.

.EXAMPLE
    ./tools/capture.ps1 -ProcessName voxl -Out docs/images/forest.png
#>
[CmdletBinding()]
param(
    [string]$ProcessName = 'voxl',
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$SettleMs = 700
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win32Capture {
    // Without this, Windows virtualises coordinates for a non-DPI-aware process:
    // GetClientRect reports physical pixels while CopyFromScreen addresses scaled
    // logical ones, so the grab lands offset and includes the window frame.
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    public static readonly IntPtr DpiAwarePerMonitorV2 = new IntPtr(-4);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@

# Must happen before any window is measured. Fails harmlessly if the host process
# already committed to a DPI awareness mode.
[void][Win32Capture]::SetProcessDpiAwarenessContext([Win32Capture]::DpiAwarePerMonitorV2)

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
           Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "No running '$ProcessName' process with a window was found." }

$handle = $process.MainWindowHandle
[void][Win32Capture]::ShowWindow($handle, 9)      # SW_RESTORE
[void][Win32Capture]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds $SettleMs                # let it repaint and settle

$rect = New-Object Win32Capture+RECT
if (-not [Win32Capture]::GetClientRect($handle, [ref]$rect)) { throw 'GetClientRect failed.' }

$origin = New-Object Win32Capture+POINT
if (-not [Win32Capture]::ClientToScreen($handle, [ref]$origin)) { throw 'ClientToScreen failed.' }

$width  = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
if ($width -le 0 -or $height -le 0) { throw "Window client area is empty ($width x $height)." }

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }
$outDir = Split-Path -Parent $outPath
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$bitmap = New-Object System.Drawing.Bitmap $width, $height
try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size $width, $height))
    } finally { $graphics.Dispose() }
    $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
} finally { $bitmap.Dispose() }

Write-Host "Captured ${width}x${height} -> $outPath" -ForegroundColor Green
