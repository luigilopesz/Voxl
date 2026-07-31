<#
.SYNOPSIS
    Captures the voxl2 window to a PNG for visual review.

.DESCRIPTION
    Finds the main window of a running process, brings it to the foreground and grabs
    its client area via BitBlt from the screen.

    Screen capture rather than PrintWindow is deliberate, and the reason is stronger
    here than it was in the old OpenGL engine. PrintWindow asks the window to redraw
    itself into a device context. A Vulkan swapchain is presented by the compositor
    and never rendered through the window's DC at all, so PrintWindow returns a black
    or stale rectangle. BitBlt from the screen reads what is actually composited, which
    is the only thing that is true for both APIs.

    Ported from C:\Users\luigi\projects\Voxl\tools\capture.ps1 (the rasterised engine),
    which this project keeps as a reference. Three things changed in the port:
      * the default process name is gvox_engine, not voxl;
      * -Region / -DebugPanel crop the grab, because the frame-time numbers live in a
        290 px strip on the right edge and a full 1280x720 shot of them is unreadable
        once it has been scaled into a review;
      * -Stage / -Name build the docs/images path so callers do not hand-assemble it.

    The output convention is the old project's, unchanged, because the review habit is
    the same one: docs/images/NN-lowercase-hyphenated-name.png while a stage is in
    progress, moving to docs/images/NN-stage-name/NN-shot-name.png once the stage ends
    and the shots are worth grouping. Numbers make a directory listing read in the order
    the work happened; the name says what the image proves, not what it depicts.

.EXAMPLE
    # Explicit path, whole window.
    pwsh -File tools\capture.ps1 -Out docs\images\02-first-tree.png

.EXAMPLE
    # Same thing, path assembled from the convention.
    pwsh -File tools\capture.ps1 -Stage 02-small-scene -Name 03-cave-interior-is-dark

.EXAMPLE
    # Just the debug overlay strip, which is where frame time and heap size are.
    pwsh -File tools\capture.ps1 -Out docs\images\02-heap-after-soak.png -DebugPanel

.NOTES
    The app must already be running. tools\bench.ps1 calls this script rather than
    duplicating the capture code; tools\run.ps1 has its own inlined copy because it
    captures at moments only it knows about (mid-soak), and it is owned separately.

    The window is 1280x720 by default (src/voxel_app.cpp:29, AppWindow(APPNAME, {1280, 720})).
#>
[CmdletBinding(DefaultParameterSetName = 'Path')]
param(
    [string]$ProcessName = 'gvox_engine',

    # Where to write. Relative paths resolve against the repository root, not the
    # caller's working directory, so the same command works from anywhere.
    [Parameter(Mandatory = $true, ParameterSetName = 'Path')]
    [string]$Out,

    # docs/images/<Stage>/<Name>.png . Stage is optional; without it the file lands
    # flat in docs/images/, which is where shots live while a stage is still open.
    [Parameter(Mandatory = $true, ParameterSetName = 'Convention')]
    [string]$Name,
    [Parameter(ParameterSetName = 'Convention')]
    [string]$Stage,

    # Let the window repaint and the compositor settle before reading pixels.
    [int]$SettleMs = 700,

    # "Left,Top,Width,Height" in CLIENT coordinates. Crops the grab.
    # A string rather than [int[]] on purpose: powershell.exe -File passes every argument
    # as one string and does NOT split on commas, so an [int[]] parameter silently receives
    # a single element and the count check fires. Parsing it here makes the documented
    # invocation and an interactive call behave the same way.
    [string]$Region,

    # Crop to the debug overlay: a strip of $PanelWidth px down the right-hand edge.
    # The Debug Menu is pinned there (src/application/ui.cpp:657-660) and its width is
    # persisted in imgui.ini as 290. It auto-resizes (ImGuiWindowFlags_AlwaysAutoResize),
    # so the height is not fixed and the crop takes the full client height instead.
    [switch]$DebugPanel,
    [int]$PanelWidth = 290,

    # Do not raise the window first. Use when the caller has already framed it and a
    # focus change would disturb what is on screen.
    [switch]$NoActivate
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win32Capture {
    // Without this, Windows virtualises coordinates for a non-DPI-aware process:
    // GetClientRect reports physical pixels while CopyFromScreen addresses scaled
    // logical ones, so the grab lands offset and includes the window frame. It must
    // be called before any window is measured, and it fails harmlessly if the host
    // PowerShell process already committed to a DPI awareness mode.
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

[void][Win32Capture]::SetProcessDpiAwarenessContext([Win32Capture]::DpiAwarePerMonitorV2)

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "No running '$ProcessName' process with a window was found." }

$handle = $process.MainWindowHandle
if (-not $NoActivate) {
    [void][Win32Capture]::ShowWindow($handle, 9)      # SW_RESTORE
    [void][Win32Capture]::SetForegroundWindow($handle)
}
Start-Sleep -Milliseconds $SettleMs

$rect = New-Object Win32Capture+RECT
if (-not [Win32Capture]::GetClientRect($handle, [ref]$rect)) { throw 'GetClientRect failed.' }

$origin = New-Object Win32Capture+POINT
if (-not [Win32Capture]::ClientToScreen($handle, [ref]$origin)) { throw 'ClientToScreen failed.' }

$clientW = $rect.Right - $rect.Left
$clientH = $rect.Bottom - $rect.Top
if ($clientW -le 0 -or $clientH -le 0) { throw "Window client area is empty ($clientW x $clientH)." }

# Work out the sub-rectangle to grab, in client coordinates.
$srcX = 0; $srcY = 0; $width = $clientW; $height = $clientH
if ($DebugPanel) {
    $srcX = [Math]::Max(0, $clientW - $PanelWidth)
    $width = $clientW - $srcX
}
if ($Region) {
    $r = @($Region -split '[,\s]+' | Where-Object { $_ -ne '' })
    if ($r.Count -ne 4) { throw "-Region needs exactly four values: Left,Top,Width,Height (got '$Region')." }
    $srcX = [int]$r[0]; $srcY = [int]$r[1]; $width = [int]$r[2]; $height = [int]$r[3]
    if ($width -le 0 -or $height -le 0) { throw "-Region $Region has a non-positive width or height." }
    if ($srcX -lt 0 -or $srcY -lt 0 -or $srcX + $width -gt $clientW -or $srcY + $height -gt $clientH) {
        throw "-Region $Region falls outside the ${clientW}x${clientH} client area."
    }
}

# Resolve the destination.
if ($PSCmdlet.ParameterSetName -eq 'Convention') {
    if ($Name -notmatch '\.png$') { $Name = "$Name.png" }
    $Out = if ($Stage) { Join-Path (Join-Path 'docs\images' $Stage) $Name } else { Join-Path 'docs\images' $Name }
}
$outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $repo $Out }
$outDir = Split-Path -Parent $outPath
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$bitmap = New-Object System.Drawing.Bitmap $width, $height
try {
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen(
            $origin.X + $srcX, $origin.Y + $srcY, 0, 0,
            (New-Object System.Drawing.Size $width, $height),
            [System.Drawing.CopyPixelOperation]::SourceCopy)
    }
    finally { $graphics.Dispose() }
    $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally { $bitmap.Dispose() }

Write-Host "Captured ${width}x${height} -> $outPath" -ForegroundColor Green
# Emit the path so a calling script can record it without re-deriving it.
$outPath
