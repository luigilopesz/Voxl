<#
.SYNOPSIS
    Take one reproducible screenshot (and optionally a per-frame benchmark CSV) of the engine.

.DESCRIPTION
    This is a thin wrapper over the engine's own command line (src/application/cli.hpp). It
    exists so that every image under docs/images/ has exactly one command written next to it
    that reproduces it, byte for byte, from a cold start.

    WHY THIS REPLACES THE KEYSTROKE HARNESS. tools/run.ps1 and tools/bench.ps1 drive the app by
    sending synthetic F3/ESC/W keystrokes at its window and then BitBlt-ing the window contents.
    That works, but it has four failure modes that each cost a wasted measurement run:
      - F3 *toggles* a persisted setting, so a blind F3 turns the overlay off half the time;
      - keystrokes sent during shader compilation are dropped, and compilation time varies;
      - the capture is of the *window*, so anything overlapping it lands in the image;
      - the camera ends up wherever the keys left it, which is not the same twice.
    The engine now takes --overlay, --pos/--rot, --screenshot and --exit-after, so none of those
    apply here: no keys are sent, no window is read, and the image comes out of the swapchain.

    Poses are given in ABSOLUTE world metres -- the sum the debug overlay shows as
    `Player Unit Offset` + `Player Pos`. docs/SCENE.md works in scene-local metres, where
    local = absolute + (183, 110, 52.5); -Local converts for you.

.EXAMPLE
    pwsh -File tools\shot.ps1 -Name 10-tree -Local "8,3,7" -Rot "1.1,1.45" -Overlay
#>
[CmdletBinding()]
param(
    # Output file stem. Written to docs/images/<Name>.png unless -Out is given.
    [Parameter(Mandatory = $true)][string]$Name,

    # Camera position, absolute world metres, "X,Y,Z".
    [string]$Pos,
    # Camera position in docs/SCENE.md's scene-local frame, "X,Y,Z". Converted to absolute.
    [string]$Local,
    # "YAW,PITCH" in radians. Pitch 1.571 is level, smaller looks up.
    [string]$Rot,

    # Seconds to let the renderer converge before the shot. The irradiance cache fires one bounce
    # per probe per frame, so a dark interior needs noticeably longer than a sunlit meadow.
    [double]$ConvergeSec = 18,
    # Force the F3 debug overlay on (text only -- the frame-time graphs stay collapsed).
    [switch]$Overlay,
    # Overlay plus both frame-time graphs expanded. Covers about a fifth of the frame, so it is
    # separate from -Overlay: scene shots want neither, and one dedicated shot wants both.
    [switch]$Graphs,
    # Also write a per-frame CSV to docs/benchmarks/<Name>.csv.
    [switch]$Bench,
    # Sample whole-device VRAM at this interval while the run is up. 0 disables.
    [int]$VramIntervalMs = 250,
    # Circle the pose: "RADIUS,PERIOD". Implies a moving camera, so pair it with -Bench.
    [string]$Patrol,
    # Total run length. Defaults to ConvergeSec + 2.
    [double]$Seconds = 0,
    [string]$Out,
    [uint32]$Width = 1280,
    [uint32]$Height = 720
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo '.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe'
if (-not (Test-Path $exe)) { throw "engine not built: $exe" }

# docs/SCENE.md sec 1: local = absolute + (183, 110, 52.5).
#
# WHY InvariantCulture APPEARS THREE TIMES BELOW. This machine's locale is pt-BR, where the
# decimal separator is a comma. PowerShell's -f operator and [double]::ToString() both honour it,
# so "$($l[2] - 52.5)" renders -26.5 as "-26,5" -- and the engine, which splits --pos on commas,
# then sees FOUR fields instead of three. It cost one wasted pair of capture runs: --rot was
# accepted (its values happened to be positive and were passed as literals) while --pos was
# rejected, so the images came out framed from the default spawn with the requested rotation, and
# looked entirely plausible. The engine now treats a malformed pose as fatal for the same reason.
$inv = [Globalization.CultureInfo]::InvariantCulture
function Fmt3([double]$a, [double]$b, [double]$c) {
    '{0},{1},{2}' -f $a.ToString('0.####', $inv), $b.ToString('0.####', $inv), $c.ToString('0.####', $inv)
}
if ($Local) {
    $l = $Local -split ',' | ForEach-Object { [double]::Parse($_, $inv) }
    if ($l.Count -ne 3) { throw "-Local needs X,Y,Z" }
    $Pos = Fmt3 ($l[0] - 183.0) ($l[1] - 110.0) ($l[2] - 52.5)
}

if ($Seconds -le 0) { $Seconds = $ConvergeSec + 2 }
if (-not $Out) { $Out = Join-Path $repo "docs\images\$Name.png" }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Out) | Out-Null
if (Test-Path $Out) { Remove-Item $Out -Force }

$engineArgs = @(
    '--unpause'
    '--exit-after', $Seconds.ToString('0.###', $inv)
    '--screenshot', $Out
    '--screenshot-after', $ConvergeSec.ToString('0.###', $inv)
    '--width', $Width
    '--height', $Height
)
if ($Pos) { $engineArgs += @('--pos', $Pos) }
if ($Rot) { $engineArgs += @('--rot', $Rot) }
if ($Patrol) { $engineArgs += @('--patrol', $Patrol) }
if ($Overlay -or $Graphs) { $engineArgs += '--overlay' } else { $engineArgs += '--no-overlay' }
if ($Graphs) { $engineArgs += '--expand-graphs' }

$csv = $null
if ($Bench) {
    $csv = Join-Path $repo "docs\benchmarks\$Name.csv"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $csv) | Out-Null
    $engineArgs += @('--bench-csv', $csv)
}

Write-Host "==> $exe $($engineArgs -join ' ')" -ForegroundColor Cyan

# nvidia-smi is streamed by ONE long-lived process rather than polled in a loop: polling costs a
# process launch per sample (~40 ms here), which both perturbs the machine under measurement and
# puts a floor under the sample rate well above the sub-second transients that matter. The heap
# reallocates by holding the old and new buffers at once, and that spike is invisible at 1 Hz.
$vramFile = Join-Path $env:TEMP "voxl2-vram-$([guid]::NewGuid().ToString('N')).txt"
$smi = $null
if ($VramIntervalMs -gt 0) {
    $smi = Start-Process -FilePath 'nvidia-smi' -PassThru -NoNewWindow -RedirectStandardOutput $vramFile `
        -ArgumentList @('--query-gpu=memory.used', '--format=csv,noheader,nounits', "-lms", $VramIntervalMs)
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$proc = Start-Process -FilePath $exe -ArgumentList $engineArgs -WorkingDirectory (Split-Path -Parent $exe) `
    -PassThru -NoNewWindow
# Touching .Handle caches the native handle in the PSObject. Without it, .NET closes the handle
# when the process ends and .ExitCode comes back as $null -- which reads as "clean" to anything
# checking `-ne 0`. Long-standing PowerShell behaviour, not a bug in this script.
$null = $proc.Handle
# Generous: a cold SPIR-V cache adds 20-40 s of shader compilation before frame 0, and
# --exit-after is measured from the first frame, not from launch.
$null = $proc.WaitForExit(([int]$Seconds + 180) * 1000)
if (-not $proc.HasExited) {
    Write-Warning "engine did not exit on its own; killing"
    $proc.Kill(); $proc.WaitForExit()
    $hung = $true
}
$sw.Stop()

if ($smi) { try { $smi.Kill() } catch {} }
Start-Sleep -Milliseconds 200

$vram = @()
if (Test-Path $vramFile) {
    $vram = Get-Content $vramFile | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int]$_ }
    Remove-Item $vramFile -Force -ErrorAction SilentlyContinue
}

$result = [ordered]@{
    name        = $Name
    exit_code   = $proc.ExitCode
    hung        = [bool]$hung
    wall_s      = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    image       = if (Test-Path $Out) { $Out } else { $null }
    image_bytes = if (Test-Path $Out) { (Get-Item $Out).Length } else { 0 }
    csv         = $csv
}
if ($vram.Count -gt 0) {
    $result.vram_first_mib = $vram[0]
    $result.vram_peak_mib = ($vram | Measure-Object -Maximum).Maximum
    $result.vram_last_mib = $vram[-1]
    $result.vram_samples = $vram.Count
}

# Frame-time summary straight from the CSV. This is the number that used to have to be read off a
# screenshot by eye; percentiles were simply unavailable. The first 25% of rows are dropped
# because they include world generation and the renderer's temporal accumulation warming up.
if ($csv -and (Test-Path $csv)) {
    $rows = Import-Csv $csv
    if ($rows.Count -gt 8) {
        $settled = $rows | Select-Object -Skip ([int]($rows.Count * 0.25))
        $full = $settled | ForEach-Object { [double]$_.full_ms } | Sort-Object
        $result.frames = $rows.Count
        $result.full_ms_mean = [math]::Round(($full | Measure-Object -Average).Average, 2)
        $result.full_ms_p50 = [math]::Round($full[[int]($full.Count * 0.50)], 2)
        $result.full_ms_p99 = [math]::Round($full[[int]($full.Count * 0.99)], 2)
        $result.full_ms_max = [math]::Round($full[-1], 2)
        $result.fps_mean = [math]::Round(1000.0 / $result.full_ms_mean, 1)
        $result.cpu_ms_mean = [math]::Round((($settled | ForEach-Object { [double]$_.cpu_ms }) | Measure-Object -Average).Average, 2)
        $result.heap_pages = [int]$rows[-1].heap_pages
        $result.heap_capacity_mb = [double]$rows[-1].heap_capacity_mb
        $result.heap_used_mb_max = ($rows | ForEach-Object { [double]$_.heap_used_mb } | Measure-Object -Maximum).Maximum
        $result.heap_cap_pages = [int]$rows[-1].heap_cap_pages
    }
}

[pscustomobject]$result | Format-List

# Loud, because a silent "no image" is the one outcome a capture script must never produce.
if ($proc.ExitCode -ne 0) { Write-Warning "engine exited with code $($proc.ExitCode)" }
if (-not (Test-Path $Out)) { throw "no screenshot was written to $Out" }
