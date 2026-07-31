<#
.SYNOPSIS
    Launch voxl2 and, optionally, drive it: show the debug overlay, walk around, sample
    VRAM and capture a screenshot.

.DESCRIPTION
    This is the measurement harness as much as the launcher. Every performance number in
    docs/BASELINE.md was produced by this script, so that any later claim can be reproduced
    with the same command rather than "by feel".

    Usage:
        pwsh -File tools\run.ps1                                  # launch and leave running
        pwsh -File tools\run.ps1 -Overlay -Seconds 30 -Quit       # 30 s stationary, VRAM sampled
        pwsh -File tools\run.ps1 -Overlay -ExpandGraphs -Soak -Seconds 30 `
             -Screenshot docs\images\00-baseline-demo-world.png -Quit

.NOTES
    -Soak holds W and sweeps the mouse, which is what actually grows the voxel heap; a
    stationary sample understates VRAM by roughly 1.2 GB.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    # Press F3 to toggle the debug overlay on after the window appears.
    [switch]$Overlay,

    # Click open the "Full frame-time" and "CPU-only frame-time" tree nodes. ImGui does not
    # persist tree-node open state in imgui.ini, so this has to be done by clicking, every run.
    [switch]$ExpandGraphs,

    # Hold W and sweep the view, so new chunks are generated and the heap actually grows.
    [switch]$Soak,

    # How long to keep the app up and sampling. 0 means launch and return immediately.
    [int]$Seconds = 0,

    # Capture the window to this PNG at the end of the sampling window.
    [string]$Screenshot,

    # Close the window when sampling finishes and verify VRAM returns to baseline.
    [switch]$Quit
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo ".out\cl-x86_64-windows-msvc\$Config\gvox_engine.exe"
if (-not (Test-Path $exe)) { throw "Not built: $exe  (run tools\build.ps1 -Config $Config)" }

$nvidiaSmi = "$env:SystemRoot\System32\nvidia-smi.exe"
function Get-VramMiB {
    if (-not (Test-Path $nvidiaSmi)) { return $null }
    [int](& $nvidiaSmi --query-gpu=memory.used --format=csv,noheader,nounits | Select-Object -First 1)
}

Add-Type @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class Win {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public const byte VK_ESCAPE = 0x1B, VK_F3 = 0x72, VK_W = 0x57;
    public const uint KEYUP = 0x0002, MOVE = 0x0001, LDOWN = 0x0002, LUP = 0x0004;

    public static void Tap(byte vk) {
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        keybd_event(vk, 0, KEYUP, UIntPtr.Zero);
    }
    public static void ClickClient(IntPtr h, int x, int y) {
        POINT p; p.X = x; p.Y = y;
        ClientToScreen(h, ref p);
        SetCursorPos(p.X, p.Y);
        System.Threading.Thread.Sleep(200);
        mouse_event(LDOWN, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        mouse_event(LUP, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(250);
    }
    // BitBlt from the screen, not PrintWindow: a Vulkan swapchain is presented by the
    // compositor and PrintWindow returns a black or stale frame for it.
    public static void Capture(IntPtr h, string path) {
        RECT r; GetClientRect(h, out r);
        POINT o; o.X = r.Left; o.Y = r.Top; ClientToScreen(h, ref o);
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        using (var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp)) {
            g.CopyFromScreen(o.X, o.Y, 0, 0, new Size(w, ht), CopyPixelOperation.SourceCopy);
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
'@ -ReferencedAssemblies System.Drawing, System.Windows.Forms

# WHY: without per-monitor DPI awareness, GetClientRect/ClientToScreen return logical
# coordinates and both the clicks and the screenshot land in the wrong place on a scaled display.
[void][Win]::SetProcessDpiAwarenessContext([IntPtr](-4))

# WHY THIS IS SET AT ALL: nothing in vksdk is needed at runtime -- the NVIDIA driver ships
# its own Vulkan loader. It is exported only so a run launched from a clean shell sees the
# same environment the build saw, which removes one variable when something misbehaves.
$env:VULKAN_SDK = Join-Path $repo 'vksdk'

$vramBefore = Get-VramMiB
Write-Host ("==> VRAM before launch: {0} MiB" -f $vramBefore) -ForegroundColor Cyan

$stdout = Join-Path $env:TEMP 'voxl2_run_stdout.txt'
$stderr = Join-Path $env:TEMP 'voxl2_run_stderr.txt'
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath $exe -WorkingDirectory $repo -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
Write-Host ("==> started pid={0}" -f $p.Id) -ForegroundColor Cyan

# Cold start compiles ~127 GLSL shaders; .out/spirv_cache makes subsequent starts fast.
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $p.Refresh()
    if ($p.HasExited) { throw "process exited early with code $($p.ExitCode); see $stderr" }
    if ($p.MainWindowHandle -ne 0) { break }
}
$sw.Stop()
$hwnd = $p.MainWindowHandle
if ($hwnd -eq 0) { throw 'no window appeared within 120 s' }
Write-Host ("==> window up after {0:N2} s" -f $sw.Elapsed.TotalSeconds) -ForegroundColor Cyan

[void][Win]::ShowWindow($hwnd, 9)
[void][Win]::SetForegroundWindow($hwnd)
Start-Sleep -Milliseconds 800

if ($Overlay) { [Win]::Tap([Win]::VK_F3); Start-Sleep -Milliseconds 500; Write-Host '==> F3 sent (debug overlay)' -ForegroundColor Cyan }

if ($ExpandGraphs) {
    # ESC releases the mouse capture so ImGui can receive clicks.
    [Win]::Tap([Win]::VK_ESCAPE); Start-Sleep -Milliseconds 700
    $r = New-Object Win+RECT
    [void][Win]::GetClientRect($hwnd, [ref]$r)
    $cw = $r.Right - $r.Left
    # The Debug Menu is pinned to the right edge, 290 px wide (see src/application/ui.cpp:657-661).
    # The two tree arrows are the first two rows inside it.
    $x = $cw - 290 + 14
    [Win]::ClickClient($hwnd, $x, 27)   # "Full frame-time"
    [Win]::ClickClient($hwnd, $x, 176)  # "CPU-only frame-time" (below the 120 px plot)
    Write-Host '==> expanded frame-time graphs' -ForegroundColor Cyan
}

$peak = 0; $samples = @()
if ($Seconds -gt 0) {
    if ($Soak) {
        [void][Win]::SetForegroundWindow($hwnd)
        [Win]::mouse_event([Win]::MOVE, 0, [uint32]260, 0, [UIntPtr]::Zero)  # pitch down at terrain
        Start-Sleep -Milliseconds 400
        [Win]::keybd_event([Win]::VK_W, 0, 0, [UIntPtr]::Zero)
    }
    for ($i = 1; $i -le $Seconds; $i++) {
        if ($Soak) { [Win]::mouse_event([Win]::MOVE, [uint32]25, 0, 0, [UIntPtr]::Zero) }
        Start-Sleep -Seconds 1
        if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { Write-Warning "process died at t=${i}s"; break }
        $u = Get-VramMiB
        $samples += $u
        if ($u -gt $peak) { $peak = $u }
        if ($i % 5 -eq 0) { Write-Host ("    t={0,3}s  vram={1} MiB  peak={2} MiB" -f $i, $u, $peak) -ForegroundColor DarkGray }
    }
    if ($Soak) { [Win]::keybd_event([Win]::VK_W, 0, [Win]::KEYUP, [UIntPtr]::Zero) }
}

if ($Screenshot) {
    $full = if ([IO.Path]::IsPathRooted($Screenshot)) { $Screenshot } else { Join-Path $repo $Screenshot }
    New-Item -ItemType Directory -Force -Path (Split-Path $full -Parent) | Out-Null
    [void][Win]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 600
    [Win]::Capture($hwnd, $full)
    Write-Host ("==> screenshot -> {0}" -f $full) -ForegroundColor Cyan
}

Write-Host ''
Write-Host '==> Run report' -ForegroundColor Cyan
Write-Host ("    {0,-22} {1}" -f 'startup to window', ('{0:N2} s' -f $sw.Elapsed.TotalSeconds))
Write-Host ("    {0,-22} {1} MiB" -f 'VRAM before', $vramBefore)
if ($samples.Count) {
    Write-Host ("    {0,-22} {1} MiB" -f 'VRAM steady (last)', $samples[-1])
    Write-Host ("    {0,-22} {1} MiB of 6144" -f 'VRAM peak', $peak)
}

if ($Quit) {
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(15000)) { Write-Warning 'did not exit within 15 s; killing'; $p.Kill() }
    $p.Refresh()
    Start-Sleep -Seconds 3   # let the driver release the allocations
    Write-Host ("    {0,-22} {1}" -f 'exit code', $p.ExitCode)
    Write-Host ("    {0,-22} {1} MiB" -f 'VRAM after exit', (Get-VramMiB))
}
else {
    Write-Host ("    {0,-22} pid {1} still running" -f 'state', $p.Id)
}
