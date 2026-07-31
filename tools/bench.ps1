<#
.SYNOPSIS
    Runs a fixed, repeatable measurement window against voxl2 and writes a CSV pair
    under docs\benchmarks\ so runs can be diffed against each other.

.DESCRIPTION
    Two files come out of every run:

      docs\benchmarks\<run-id>.csv    one row per GPU sample, ~2 Hz by default
      docs\benchmarks\index.csv       one row per run, appended -- the diffable ledger

    plus, when the overlay is on, a PNG of the debug panel so the frame-time and heap
    figures are preserved as evidence rather than as a claim.

    WHAT THIS MEASURES, AND WHAT IT DOES NOT
    ----------------------------------------
    It measures the GPU, over time, with timestamps: memory used, utilisation, SM clock,
    board power and temperature, streamed from nvidia-smi. On a 6 GB card whose voxel
    heap allocator has no cap, no OOM check and no shrink path
    (src\utilities\allocator.inl:264), memory-over-time is the number most likely to end
    the project, so it is the number this script exists to capture.

    It does NOT read the in-app frame time programmatically, and that is a real gap
    rather than an oversight. See the block comment below.

    WHY FRAME TIME IS NOT READ PROGRAMMATICALLY
    -------------------------------------------
    The engine has no way to emit it. Verified, not assumed:

      * The frame-time history lives only in CPU memory as two 200-element ring buffers
        (src\application\ui.hpp:28-30, written at src\application\ui.cpp:602-605) and is
        consumed exactly once, by ImGui::PlotLines inside a collapsed tree node
        (src\application\ui.cpp:678-685). Nothing else ever reads it.
      * The only thing the process writes to stdout is debug_utils::Console::add_log
        (src\utilities\debug.cpp:33, std::cout). Across a whole session that produces one
        line -- "startup: 0.675 s" (src\voxel_app.cpp:69) -- plus shader-compile
        diagnostics. No per-frame or per-second output of any kind.
      * There is no telemetry file, no CSV writer, no socket, and no IPC. grep for
        ofstream in src\ finds only the settings serialiser.

    So the number is on screen and nowhere else. The options were OCR of the overlay
    strip (fragile, and it would need a dependency this project does not have) or a
    source change. The source change is small and specific and is written up as an
    integration note for whoever owns src\ -- see "INTEGRATION NOTE" at the foot of this
    file. Until it lands, this script captures the overlay to a PNG, records the PNG's
    path in index.csv, and leaves frametime_ms / fps / gpu_heap_pages / heap_usage_mb
    empty for a human to fill in from the image. An empty cell that is honestly empty is
    worth more than a number nobody can reproduce.

    GPU memory is whole-device, not per-process. Windows WDDM does not report per-process
    VRAM for a graphics application through nvidia-smi, so vram_used_mib includes the
    desktop. vram_before_mib in index.csv is there to be subtracted; on this machine idle
    sits at 273-288 MiB (docs\BASELINE.md §3).

.EXAMPLE
    # The standard run: 30 s walking forward, which is what actually grows the heap.
    pwsh -File tools\bench.ps1 -Label baseline -Soak -Seconds 30

.EXAMPLE
    # Stationary at spawn, for comparison. Understates VRAM by about 1.2 GB.
    pwsh -File tools\bench.ps1 -Label stationary -Seconds 30

.EXAMPLE
    # Sample an instance somebody has already launched and framed by hand.
    pwsh -File tools\bench.ps1 -Label handheld -Attach -Seconds 60

.NOTES
    This script drives the window with the same constants tools\run.ps1 uses -- an 8 s
    settle, the persisted-F3 check, the two ImGui click coordinates. That duplication is
    deliberate and not accidental: run.ps1 is owned by another agent, and a benchmark that
    silently changed behaviour when the launcher changed would be worse than a benchmark
    that repeats sixty lines. If the two ever disagree, run.ps1 is the source of truth for
    those constants and this file should be corrected to match.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    # Goes in the filename and in index.csv. Keep it short and stable across a series
    # ("baseline", "small-scene", "cave") so rows sort together and diff cleanly.
    [string]$Label = 'run',

    # Length of the sampling window, in seconds. Everything before it -- launch, settle,
    # overlay, unpause -- is excluded, so two runs with the same -Seconds are comparable.
    [int]$Seconds = 30,

    # GPU sample period. nvidia-smi streams these itself (-lms), so the cost to the
    # measured process is one extra process, not one process spawn per sample.
    [int]$IntervalMs = 500,

    # Seconds to wait after the window appears before sending any input. The window is
    # created early but ~127 GLSL shaders are still compiling behind it, and keystrokes
    # sent during that period are silently dropped -- F3 in particular.
    [int]$SettleSec = 8,

    # Hold W and sweep the view. Without this the heap barely grows and the run measures
    # a state the game is never actually in.
    [switch]$Soak,
    [int]$SoakPitch = 120,
    [int]$SoakYaw = 12,

    # Turn the debug overlay on and click open the two frame-time tree nodes, so the
    # end-of-run PNG actually contains the numbers.
    [switch]$NoOverlay,
    [int]$ExpandY1 = 15,
    [int]$ExpandY2 = 175,

    # Sample a process that is already running instead of launching one. -Seconds still
    # applies; the script will not quit a process it did not start.
    [switch]$Attach,

    # Leave the app running at the end (implied by -Attach).
    [switch]$NoQuit,

    # Also save a full-window PNG. Off by default: at ~375 KB a run it is 8x the overlay
    # crop, and visual review belongs in docs\images\ via tools\capture.ps1, not here.
    [switch]$FrameShot,

    # Free-text column in index.csv. Use it for the thing you will want to know in a
    # month: what changed since the previous row.
    [string]$Note = '',

    # Fill in the four hand-read columns of an existing index.csv row and exit without
    # running anything. The values come off <run-id>-overlay.png; this exists so that
    # reading them off is a recorded edit rather than someone typing into a CSV.
    #   pwsh -File tools\bench.ps1 -Amend 20260731-004431-smoke -FrameTimeMs 19.81 `
    #        -Fps 50.48 -HeapPages 1376256 -HeapUsageMb 1625.74
    [string]$Amend,
    [string]$FrameTimeMs,
    [string]$Fps,
    [string]$HeapPages,
    [string]$HeapUsageMb
)

$ErrorActionPreference = 'Stop'

# Numbers in these CSVs must not depend on the operator's locale. This machine formats
# doubles with a comma decimal separator, which Export-Csv would faithfully write as
# "81,6" -- still valid CSV because the field gets quoted, but unparseable by anything
# that expects a number and silently different from a run on another machine.
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo ".out\cl-x86_64-windows-msvc\$Config\gvox_engine.exe"
$outDir = Join-Path $repo 'docs\benchmarks'
$nvidiaSmi = "$env:SystemRoot\System32\nvidia-smi.exe"
$processName = 'gvox_engine'
$settingsPath = Join-Path $env:APPDATA 'GabeVoxelGame\user_settings.json'

if ($Amend) {
    $indexCsv = Join-Path $outDir 'index.csv'
    if (-not (Test-Path $indexCsv)) { throw "no $indexCsv to amend." }
    $all = @(Import-Csv $indexCsv)
    $row = $all | Where-Object { $_.run_id -eq $Amend }
    if (-not $row) { throw "no row with run_id '$Amend' in $indexCsv." }
    if ($FrameTimeMs) { $row.frametime_ms = $FrameTimeMs }
    if ($Fps) { $row.fps = $Fps }
    if ($HeapPages) { $row.gpu_heap_pages = $HeapPages }
    if ($HeapUsageMb) { $row.heap_usage_mb = $HeapUsageMb }
    if ($Note) { $row.note = $Note }
    $all | Export-Csv -Path $indexCsv -NoTypeInformation -Encoding utf8
    Write-Host "==> amended $Amend in $indexCsv" -ForegroundColor Green
    $row | Format-List
    exit 0
}

if (-not (Test-Path $nvidiaSmi)) { throw "nvidia-smi not found at $nvidiaSmi; this script has nothing to sample." }
if (-not $Attach -and -not (Test-Path $exe)) { throw "Not built: $exe  (run tools\build.ps1 -Config $Config)" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$runId = '{0}-{1}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $Label
$samplesCsv = Join-Path $outDir "$runId.csv"
$indexCsv = Join-Path $outDir 'index.csv'
$overlayPng = Join-Path $repo "docs\benchmarks\$runId-overlay.png"
$framePng = Join-Path $repo "docs\benchmarks\$runId-frame.png"
$nvRaw = Join-Path $env:TEMP "voxl2_bench_$runId.txt"

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Bench {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    // dx/dy are LONG and genuinely signed: the soak pitch is undone with a negative dy.
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    // A hung app is not a dead app: the process is still alive, nvidia-smi still reports
    // its allocations, and BitBlt still succeeds -- it just grabs the blank white ghost
    // window DWM substitutes for a window that has stopped pumping messages. Without this
    // check a hang is recorded as a plausible-looking low-VRAM run with an unreadable
    // screenshot. Observed twice in six launches while writing this script.
    [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
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
}
'@

# Per-monitor DPI awareness must be declared before any window is measured; without it
# GetClientRect and ClientToScreen disagree on a scaled display and the ImGui clicks miss.
[void][Bench]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-VramMiB {
    [int](& $nvidiaSmi --query-gpu=memory.used --format=csv,noheader,nounits | Select-Object -First 1)
}

# ---------------------------------------------------------------- launch or attach ----

# Nothing in vksdk is needed at runtime -- the NVIDIA driver ships its own loader. It is
# exported only so a benchmark run sees the same environment the build saw.
$env:VULKAN_SDK = Join-Path $repo 'vksdk'

$vramBefore = Get-VramMiB
Write-Host ("==> VRAM before: {0} MiB" -f $vramBefore) -ForegroundColor Cyan

$startupSec = ''
if ($Attach) {
    $p = Get-Process -Name $processName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { throw "-Attach was given but no '$processName' process with a window is running." }
    Write-Host ("==> attached to pid={0}" -f $p.Id) -ForegroundColor Cyan
    $NoQuit = $true
}
else {
    $stale = Get-Process -Name $processName -ErrorAction SilentlyContinue
    if ($stale) { Write-Warning "$($stale.Count) '$processName' process(es) already running; VRAM figures will include them." }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -WorkingDirectory $repo -PassThru `
        -RedirectStandardOutput (Join-Path $env:TEMP "voxl2_bench_${runId}_stdout.txt") `
        -RedirectStandardError (Join-Path $env:TEMP "voxl2_bench_${runId}_stderr.txt")
    # Touching .Handle caches the native handle. Without it .NET closes the handle on exit
    # and .ExitCode later comes back empty even after a perfectly clean shutdown.
    $null = $p.Handle

    $deadline = (Get-Date).AddSeconds(120)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $p.Refresh()
        if ($p.HasExited) { throw "process exited early with code $($p.ExitCode)" }
        if ($p.MainWindowHandle -ne 0) { break }
    }
    $sw.Stop()
    if ($p.MainWindowHandle -eq 0) { throw 'no window appeared within 120 s' }
    $startupSec = '{0:N2}' -f $sw.Elapsed.TotalSeconds
    Write-Host ("==> pid={0}, window up after {1} s" -f $p.Id, $startupSec) -ForegroundColor Cyan
}

$hwnd = $p.MainWindowHandle
[void][Bench]::ShowWindow($hwnd, 9)
[void][Bench]::SetForegroundWindow($hwnd)

$r = New-Object Bench+RECT
[void][Bench]::GetClientRect($hwnd, [ref]$r)
$clientW = $r.Right - $r.Left
$clientH = $r.Bottom - $r.Top

if (-not $Attach) {
    Write-Host ("==> settling {0} s (shaders still compiling behind the window)" -f $SettleSec) -ForegroundColor Cyan
    Start-Sleep -Seconds $SettleSec
    [void][Bench]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 500
}

# ------------------------------------------------------------------------ overlay ----

if (-not $NoOverlay -and -not $Attach) {
    # F3 TOGGLES, and "UI"/"show_debug_info" is PERSISTED to user_settings.json
    # (src\application\settings.cpp:130, autosaved on exit). A blind F3 therefore turns the
    # overlay OFF whenever the previous run left it on. Read the persisted value first.
    $already = $false
    if (Test-Path $settingsPath) {
        try {
            $cfg = Get-Content $settingsPath -Raw | ConvertFrom-Json
            $already = [bool]$cfg.categories.UI.show_debug_info.data.setting.value
        }
        catch { Write-Warning "could not parse $settingsPath; assuming the overlay is off" }
    }
    if ($already) { Write-Host '==> overlay already on; F3 not sent' -ForegroundColor Cyan }
    else { [Bench]::Tap([Bench]::VK_F3); Start-Sleep -Milliseconds 800; Write-Host '==> F3 sent (overlay on)' -ForegroundColor Cyan }

    # The app boots paused (src\application\ui.hpp:44), so the cursor is already free and
    # ImGui is already taking clicks. Do NOT press ESC first -- that unpauses and captures
    # the mouse, and the clicks would go to the world instead of the panel.
    # The Debug Menu is pinned to the right edge, 290 px wide (imgui.ini, and
    # src\application\ui.cpp:657-660). The two tree arrows are its first two rows; the
    # second sits below the first one's 120 px plot once that expands.
    $x = $clientW - 290 + 14
    [Bench]::ClickClient($hwnd, $x, $ExpandY1)
    [Bench]::ClickClient($hwnd, $x, $ExpandY2)
    Write-Host "==> frame-time graphs expanded (x=$x, y=$ExpandY1 and y=$ExpandY2)" -ForegroundColor Cyan
}

# ------------------------------------------------------------------------ sampling ----

# nvidia-smi streams at -lms rather than being spawned once per sample: one extra process
# for the whole run instead of ~60, and its own timestamps rather than the harness's.
$nvArgs = @(
    '--query-gpu=timestamp,memory.used,memory.total,utilization.gpu,clocks.sm,power.draw,temperature.gpu',
    '--format=csv,noheader,nounits', '-lms', "$IntervalMs")
$nv = Start-Process -FilePath $nvidiaSmi -ArgumentList $nvArgs -NoNewWindow -PassThru -RedirectStandardOutput $nvRaw

# Wall-clock boundaries of the measured window. Samples are classified against these
# afterwards using nvidia-smi's own timestamps, so a slow harness cannot skew the phase
# labels the way a harness-side clock would.
$t0 = Get-Date
$tEnd = $t0

try {
    # ESC unpauses and captures the mouse (src\voxel_app.cpp:228-232). While paused the
    # main-menu overlay covers the view and the player does not move, so measuring paused
    # measures nothing.
    if (-not $Attach) { [Bench]::Tap([Bench]::VK_ESCAPE); Start-Sleep -Milliseconds 700 }

    $pitched = 0
    if ($Soak) {
        [void][Bench]::SetForegroundWindow($hwnd)
        [Bench]::mouse_event([Bench]::MOVE, 0, $SoakPitch, 0, [UIntPtr]::Zero)
        $pitched = $SoakPitch
        Start-Sleep -Milliseconds 400
        [Bench]::keybd_event([Bench]::VK_W, 0, 0, [UIntPtr]::Zero)
    }

    $t0 = Get-Date
    for ($i = 1; $i -le $Seconds; $i++) {
        if ($Soak) { [Bench]::mouse_event([Bench]::MOVE, $SoakYaw, 0, 0, [UIntPtr]::Zero) }
        Start-Sleep -Seconds 1
        if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { Write-Warning "process died at t=${i}s"; break }
        # Progress only. The authoritative samples come from the nvidia-smi stream, not
        # from these; this is one extra reading every five seconds so a long run shows
        # signs of life.
        if ($i % 5 -eq 0) { Write-Host ("    t={0,3}s  vram={1} MiB" -f $i, (Get-VramMiB)) -ForegroundColor DarkGray }
    }
    if ($Soak) {
        [Bench]::keybd_event([Bench]::VK_W, 0, [Bench]::KEYUP, [UIntPtr]::Zero)
        # Level the view before the capture, or the frame PNG is a picture of the ground.
        [Bench]::mouse_event([Bench]::MOVE, 0, -$pitched, 0, [UIntPtr]::Zero)
    }
    $tEnd = Get-Date
}
finally {
    # Always stop the sampler, even if the app died or the operator interrupted the run;
    # a stranded nvidia-smi -lms would keep writing to TEMP forever.
    Start-Sleep -Milliseconds ($IntervalMs + 200)   # let the last sample land
    if (-not $nv.HasExited) { $nv.Kill() }
    Start-Sleep -Milliseconds 300
}

# --------------------------------------------------------------------- screenshots ----

# Captured through tools\capture.ps1 rather than a second copy of the BitBlt code, so
# there is exactly one place where the "PrintWindow does not work on a Vulkan swapchain"
# lesson is written down.
$capture = Join-Path $PSScriptRoot 'capture.ps1'
# NOT $overlayShot / $frameShot: PowerShell variable names are case-insensitive, so a
# local named $frameShot IS the -FrameShot parameter, still carrying its [switch] type
# constraint, and assigning a path to it throws "Cannot convert System.String to
# SwitchParameter" after the measurement has already been taken. Cost the first 30 s run.
$overlayShotPath = ''
$frameShotPath = ''
$appHung = 0
$alive = [bool](Get-Process -Id $p.Id -ErrorAction SilentlyContinue)
if ($alive -and [Bench]::IsHungAppWindow($hwnd)) {
    $appHung = 1
    Write-Warning 'the window is HUNG: it stopped pumping messages. The samples above describe a stalled process, and any screenshot would be the blank ghost window. Skipping the capture and flagging the row.'
}
if ($alive -and -not $appHung) {
    try {
        if ($FrameShot) {
            & $capture -Out $framePng -SettleMs 400 | Out-Null
            $frameShotPath = "docs/benchmarks/$runId-frame.png"
        }
        if (-not $NoOverlay) {
            & $capture -Out $overlayPng -DebugPanel -SettleMs 400 | Out-Null
            $overlayShotPath = "docs/benchmarks/$runId-overlay.png"
        }
    }
    catch { Write-Warning "screenshot failed: $_" }
}

# --------------------------------------------------------------------------- parse ----

$rows = [System.Collections.Generic.List[object]]::new()
foreach ($line in (Get-Content $nvRaw -ErrorAction SilentlyContinue)) {
    $f = $line -split '\s*,\s*'
    if ($f.Count -lt 7) { continue }
    $ts = try { [datetime]::ParseExact($f[0], 'yyyy/MM/dd HH:mm:ss.fff', $null) } catch { continue }
    $rows.Add([pscustomobject]@{
            t_s           = [math]::Round(($ts - $t0).TotalSeconds, 3)
            phase         = if ($ts -lt $t0) { 'pre' } elseif ($ts -le $tEnd) { if ($Soak) { 'soak' } else { 'idle' } } else { 'post' }
            vram_used_mib = [int]$f[1]
            vram_total_mib= [int]$f[2]
            gpu_util_pct  = [int]$f[3]
            sm_clock_mhz  = [int]$f[4]
            power_w       = [double]$f[5]
            temp_c        = [int]$f[6]
        })
}
$rows | Export-Csv -Path $samplesCsv -NoTypeInformation -Encoding utf8
Write-Host ("==> {0} samples -> {1}" -f $rows.Count, $samplesCsv) -ForegroundColor Cyan

# ---------------------------------------------------------------------- shut down ----

$exitCode = ''
$vramAfter = ''
if (-not $NoQuit) {
    [void]$p.CloseMainWindow()
    if (-not $p.WaitForExit(15000)) { Write-Warning 'did not exit within 15 s; killing'; $p.Kill() }
    # Read ExitCode before anything else touches the object; Refresh() on an exited
    # process can invalidate it and leave the field blank.
    $exitCode = try { $p.ExitCode } catch { 'unavailable' }
    Start-Sleep -Seconds 3       # let the driver release the allocations
    $vramAfter = Get-VramMiB
}

# ------------------------------------------------------------------------ summarise ----

$window = $rows | Where-Object { $_.phase -ne 'pre' }
if (-not $window) { $window = $rows }
$vramPeak = if ($window) { ($window | Measure-Object vram_used_mib -Maximum).Maximum } else { '' }
$vramMean = if ($window) { [int]($window | Measure-Object vram_used_mib -Average).Average } else { '' }
$utilMean = if ($window) { [int]($window | Measure-Object gpu_util_pct -Average).Average } else { '' }
$powerMean = if ($window) { [math]::Round(($window | Measure-Object power_w -Average).Average, 1) } else { '' }
$commit = try { (git -C $repo rev-parse --short HEAD 2>$null) } catch { '' }
$dirty = try { if (git -C $repo status --porcelain 2>$null) { '+dirty' } else { '' } } catch { '' }

# The commit alone does not identify what was measured. Several agents edit this tree in
# parallel, so "972c9a5+dirty" can mean a dozen different binaries, and the exe can even be
# replaced between two runs of this script. The build timestamp and size do pin it: two rows
# with the same exe_mtime and exe_bytes measured the same binary, whatever git says.
# With -Attach the running process may not be the exe this script would have launched, so
# ask the process itself which binary it is rather than assuming.
$measuredExe = if ($Attach) { try { $p.Path } catch { $exe } } else { $exe }
$exeInfo = try { Get-Item $measuredExe -ErrorAction Stop } catch { $null }

$summary = [pscustomobject][ordered]@{
    run_id            = $runId
    utc               = (Get-Date).ToUniversalTime().ToString('s') + 'Z'
    label             = $Label
    config            = $Config
    commit            = "$commit$dirty"
    exe_mtime         = if ($exeInfo) { $exeInfo.LastWriteTime.ToString('s') } else { '' }
    exe_bytes         = if ($exeInfo) { $exeInfo.Length } else { '' }
    resolution        = "${clientW}x${clientH}"
    seconds           = $Seconds
    soak              = [int][bool]$Soak
    startup_s         = $startupSec
    vram_before_mib   = $vramBefore
    vram_peak_mib     = $vramPeak
    vram_mean_mib     = $vramMean
    vram_after_mib    = $vramAfter
    vram_total_mib    = if ($rows) { $rows[0].vram_total_mib } else { '' }
    gpu_util_mean_pct = $utilMean
    power_mean_w      = $powerMean
    samples           = $rows.Count
    exit_code         = $exitCode
    # 1 means the process stopped responding during the window. Treat every other number
    # in the row as describing a stalled process, not a running one.
    app_hung          = $appHung
    # Read off the overlay PNG by hand and fill these in. They stay empty until the
    # integration note at the foot of this script is implemented in src\.
    frametime_ms      = ''
    fps               = ''
    gpu_heap_pages    = ''
    heap_usage_mb     = ''
    overlay_png       = $overlayShotPath
    frame_png         = $frameShotPath
    samples_csv       = "docs/benchmarks/$runId.csv"
    note              = $Note
}

# Export-Csv -Append silently drops any property the existing header does not have, so
# the first run after a column is added would lose that column and every run after it
# would look like it never existed. Rewrite the file instead, padding older rows with
# empty cells. The ledger is a few hundred rows at worst; rewriting it costs nothing.
$cols = $summary.PSObject.Properties.Name
if (Test-Path $indexCsv) {
    $existing = @(Import-Csv $indexCsv)
    $lost = @($existing | Select-Object -First 1 | ForEach-Object { $_.PSObject.Properties.Name } | Where-Object { $cols -notcontains $_ })
    if ($lost.Count) { Write-Warning "index.csv has column(s) this version does not write and they will be dropped: $($lost -join ', ')" }
    foreach ($row in $existing) {
        foreach ($c in $cols) {
            if (-not $row.PSObject.Properties[$c]) { $row | Add-Member -NotePropertyName $c -NotePropertyValue '' }
        }
    }
    (@($existing) + $summary) | Select-Object $cols | Export-Csv -Path $indexCsv -NoTypeInformation -Encoding utf8
}
else { $summary | Export-Csv -Path $indexCsv -NoTypeInformation -Encoding utf8 }

Remove-Item $nvRaw -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '==> Bench report' -ForegroundColor Cyan
$summary | Format-List
Write-Host "Frame time is NOT in this CSV. Read it off $overlayShotPath, then:" -ForegroundColor Yellow
Write-Host "  pwsh -File tools\bench.ps1 -Amend $runId -FrameTimeMs .. -Fps .. -HeapPages .. -HeapUsageMb .." -ForegroundColor Yellow

# nvidia-smi and git leave $LASTEXITCODE set; without this the script reports their exit
# code as its own and a clean run looks like a failure.
exit 0

<#
INTEGRATION NOTE -- for whoever owns src\ . This script does not make these changes.

The engine takes no command-line arguments at all: src\main.cpp:22 is
`auto main() -> int` with no argc/argv, and grep for argc|argv|GetCommandLine across
src\ and deps\Daxa\src returns nothing. Four small additions would make every visual and
performance claim in this repository reproducible from one command. In rough order of
value per line changed:

 1. --bench-csv <path>  (the one this script is blocked on)
    src\application\ui.cpp:602, AppUi::update already receives delta_time and
    cpu_delta_time every frame. Accumulate them and append one line per second to an
    ofstream: frame_index, t_s, full_ms, cpu_ms, and the two figures currently only
    formatted for the overlay at src\voxels\impl\voxel_world.cpp:213-214
    (voxel_malloc.current_element_count, and gpu_output.voxel_malloc_output.current_element_count).
    Fix the u32 overflow at that same line while you are there -- current_element_count *
    VOXEL_MALLOC_PAGE_SIZE_BYTES is computed in daxa_u32 before the cast to double, so the
    MB figure silently wraps above 2,033,601 pages.
    ~25 lines. It removes the only manual step left in this script.

 2. --seed <u64>
    src\voxel_app.cpp:125 hardcodes `auto seed = 15512089755474631791ull;` with the
    std::hash(world_seed_str) call commented out beside it. The UI field already exists
    (src\application\ui.cpp:346). Plumbing an argument to ui.settings.world_seed_str
    before the first upload makes a scene addressable by name.
    ~5 lines.

 3. --pos x,y,z  --rot yaw,pitch  --time <seconds>  --exit-after <seconds>
    player_startup (src\application\player.cpp:28-48) sets PLAYER.pos unconditionally at
    line 46; an override applied there is the whole change for position and rotation.
    --exit-after belongs in the loop at src\voxel_app.cpp:79-96, next to the existing
    glfwWindowShouldClose test. Together these turn "walk forward for 30 s and hope" --
    which is what -Soak in this script is -- into a deterministic camera.
    ~20 lines.

 4. --screenshot <path>
    The swapchain image is already TRANSFER_DST (src\voxel_app.cpp:40); a readback into a
    host buffer plus stbi_write_png is the standard Daxa pattern, and stb is already a
    declared vcpkg dependency. Combined with --exit-after it gives a screenshot-and-exit
    mode, which is what the old engine had and what made its visual review reproducible
    (see C:\Users\luigi\projects\Voxl\docs\images\README.md).
    ~40 lines, the largest of the four.

 5. --width / --height
    src\voxel_app.cpp:29, AppWindow(APPNAME, {1280, 720}). Every number in
    docs\BASELINE.md is at 720p because that literal is the only resolution reachable
    without dragging the window. ~3 lines.

An argument parser is not needed for any of this; a loop over argv with string compares is
proportionate, and adding a dependency for it would be the wrong trade in a project whose
vcpkg manifest is already 32 packages.
#>
