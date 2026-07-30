<#
.SYNOPSIS
    Launches voxl with a scripted camera, captures one screenshot, kills it.

.DESCRIPTION
    The visual review of LOD seams and sub-voxel shading needs the SAME framing
    captured under different settings, which a hand-driven first-person camera
    cannot reproduce. This drives the debug command line in src/app/Main.cpp
    instead: one process per shot, positioned by argument, captured by
    tools/capture.ps1, then terminated so the next shot starts from a clean world.

    A fresh process per shot is deliberate rather than wasteful. Chunk residency,
    LOD hysteresis and the sub-voxel store all carry state forward, so reusing one
    process would make each shot depend on the ones before it.

.PARAMETER GameArgs
    One whitespace-separated string, split here and passed through to voxl.exe.
    A single string rather than a string[] because `powershell -File script.ps1
    -GameArgs a,b,c` collapses an array parameter into one comma-joined token,
    which silently produces an unrecognised option instead of a shot.
    See `voxl --help`.

.PARAMETER SettleSeconds
    Wall-clock time between launch and capture. Must cover streaming the whole
    load radius plus the carve rig; 12 s is comfortable at radius 20.

.EXAMPLE
    ./tools/visual_review.ps1 -Out docs/images/lod_long.png `
        -GameArgs '--pos','0,140,0','--look','40,-8','--freeze','--overlay'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$GameArgs = '',
    [int]$SettleSeconds = 12,
    [string]$Config = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

$root    = Split-Path -Parent $PSScriptRoot
$binDir  = Join-Path $root "build/$Config/bin"
$exePath = Join-Path $binDir 'voxl.exe'
if (-not (Test-Path $exePath)) { throw "No voxl.exe at $exePath - build first." }

# A stale instance would be captured instead of the one about to start.
Get-Process -Name voxl -ErrorAction SilentlyContinue | ForEach-Object {
    $_.Kill(); $_.WaitForExit(5000)
}

$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$logPath = Join-Path $binDir 'voxl.log'
if (Test-Path $logPath) { Remove-Item $logPath -Force }

$argumentList = @($GameArgs -split '\s+' | Where-Object { $_ -ne '' })

Write-Host "Launching: voxl.exe $($argumentList -join ' ')" -ForegroundColor Cyan
$process = if ($argumentList.Count -gt 0) {
    Start-Process -FilePath $exePath -ArgumentList $argumentList -WorkingDirectory $binDir -PassThru
} else {
    Start-Process -FilePath $exePath -WorkingDirectory $binDir -PassThru
}

# GLFW places the window with CW_USEDEFAULT, which cascades it down the desktop
# until its lower rows sit behind the taskbar - and capture.ps1 grabs the client
# rectangle from the SCREEN, so the taskbar lands in the bottom of every shot.
# Pinning the window to the top-left corner before capturing is the whole fix.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Win32Move {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public const uint SwpNoSize = 0x0001, SwpNoZOrder = 0x0004, SwpShowWindow = 0x0040;
}
'@

try {
    Start-Sleep -Seconds $SettleSeconds
    if ($process.HasExited) { throw "voxl exited early with code $($process.ExitCode); see $logPath" }

    $process.Refresh()
    if ($process.MainWindowHandle -ne 0) {
        [void][Win32Move]::SetWindowPos($process.MainWindowHandle, [IntPtr]::Zero, 0, 0, 0, 0,
            [Win32Move]::SwpNoSize -bor [Win32Move]::SwpNoZOrder -bor [Win32Move]::SwpShowWindow)
        Start-Sleep -Milliseconds 400
    }

    & (Join-Path $PSScriptRoot 'capture.ps1') -ProcessName voxl -Out $outPath
} finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(4000)) { $process.Kill() }
    }
    # The log is per-run and the next shot deletes it, so keep a copy beside the
    # image: the LOD residency line in it is the only record of what was actually
    # on screen at each level.
    if (Test-Path $logPath) {
        Copy-Item $logPath ([System.IO.Path]::ChangeExtension($outPath, '.log')) -Force
    }
}
