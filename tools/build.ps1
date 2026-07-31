<#
.SYNOPSIS
    One-command build for voxl2 (path-traced voxel engine, forked from GabeRundlett/gvox_engine).

.DESCRIPTION
    Every environment trap this project has is encoded here. Read the WHY comments before
    changing anything -- each one cost an hour to find.

    Usage:
        pwsh -File tools\build.ps1                  # incremental Release build
        pwsh -File tools\build.ps1 -Config Debug
        pwsh -File tools\build.ps1 -Reconfigure     # force a fresh CMake configure
        pwsh -File tools\build.ps1 -Clean           # delete .out entirely, then full build

.NOTES
    A full cold build takes ~25-30 min and peaks around 14 GB of disk. Almost all of that is
    vcpkg compiling 46 packages from a 2023-era baseline; this project's own 16 .cpp files
    link in about 3 seconds.
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    # Force CMake to re-run configure even when the cache looks current.
    [switch]$Reconfigure,

    # Delete .out and build from scratch. Costs ~25-30 min. See -WhatIf note above.
    [switch]$Clean,

    # Skip the build step; configure only.
    [switch]$ConfigureOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$preset = 'cl-x86_64-windows-msvc'
$buildDir = Join-Path $repo ".out\$preset"

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Fact($k, $v) { Write-Host ("    {0,-22} {1}" -f $k, $v) -ForegroundColor DarkGray }

# ---------------------------------------------------------------------------
# 1. Locate the MSVC toolchain.
# ---------------------------------------------------------------------------
# WHY: only Build Tools 2022 is installed on this machine, but vswhere keeps the script
# working if a full VS edition is ever installed alongside it.
$vsRoot = $null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null | Select-Object -First 1
}
if (-not $vsRoot) { $vsRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools" }

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# ---------------------------------------------------------------------------
# 2. Import the vcvars64 environment into this PowerShell session.
# ---------------------------------------------------------------------------
# WHY THIS IS NOT OPTIONAL: cmake/toolchains/cl-x86_64-windows-msvc.cmake lines 4-7 do
#   find_program(CMAKE_C_COMPILER cl REQUIRED HINTS ${MSVC_ENV_Path})
# and MSVC_ENV_Path is populated by cmake/vcvars.cmake, which only special-cases the exact
# variable name "Path". cmd.exe exports it as uppercase PATH, so the cache ends up holding
# MSVC_ENV_PATH and the HINTS list is empty:
#       Could not find CMAKE_C_COMPILER using the following names: cl
# find_program still falls back to the process PATH, so the fix is simply to have cl.exe
# already on PATH before CMake starts -- i.e. to run inside a vcvars environment.
Write-Step "Importing MSVC environment from $vcvars"
$vcvarsOut = & "$env:ComSpec" /c "call `"$vcvars`" >nul 2>&1 && set"
foreach ($line in $vcvarsOut) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

# VS ships its own CMake and Ninja; neither is on PATH by default.
$vsCMake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:Path = "$vsCMake\CMake\bin;$vsCMake\Ninja;$env:Path"

# ---------------------------------------------------------------------------
# 3. Project-specific environment.
# ---------------------------------------------------------------------------

# WHY: cmake/vcpkg.cmake clones vcpkg *inside* the source tree. Point at that clone
# explicitly, or the VS-bundled vcpkg under Program Files gets picked up instead and
# installs to the wrong root.
$env:VCPKG_ROOT = Join-Path $repo 'vcpkg'

# WHY 2 AND NOT MORE: ninja defaults to -j21 on this 14C/20T box, and vcpkg additionally
# passes /MP4 to each cl.exe it spawns -- up to 84 concurrent compilers against 16 GB of
# RAM. The failure is silent and looks like a compiler bug:
#       FAILED: [code=4294967295]
# with no diagnostic at all, on spirv-tools, while the same TU compiles fine alone.
# 6 was still not enough. 2 works.
$env:VCPKG_MAX_CONCURRENCY = '2'

# WHY A LOCAL SDK: the LunarG Vulkan SDK is deliberately NOT installed on this machine and
# is not needed. vksdk/ is a minimal synthesised SDK -- Vulkan-Headers v1.3.260 plus an
# import library generated from the NVIDIA driver's own vulkan-1.dll. find_package(Vulkan)
# takes only Vulkan_LIBRARY from here; Vulkan_INCLUDE_DIR resolves to vcpkg's vulkan-headers
# port because the vcpkg toolchain prepends its own prefix path. At *runtime* nothing from
# vksdk is needed at all -- the driver's loader (1.4.341.0) is sufficient.
$env:VULKAN_SDK = Join-Path $repo 'vksdk'
if (-not (Test-Path (Join-Path $env:VULKAN_SDK 'Lib\vulkan-1.lib'))) {
    throw "vksdk is missing its import library at $env:VULKAN_SDK\Lib\vulkan-1.lib"
}

Write-Fact 'repo'        $repo
Write-Fact 'cl.exe'      (Get-Command cl -ErrorAction SilentlyContinue).Source
Write-Fact 'cmake'       (Get-Command cmake -ErrorAction SilentlyContinue).Source
Write-Fact 'ninja'       (Get-Command ninja -ErrorAction SilentlyContinue).Source
Write-Fact 'VCPKG_ROOT'  $env:VCPKG_ROOT
Write-Fact 'VULKAN_SDK'  $env:VULKAN_SDK
Write-Fact 'concurrency' $env:VCPKG_MAX_CONCURRENCY

# NOTE ON PATH LENGTH: this repo must live at a SHORT path. cmake/vcpkg.cmake clones vcpkg
# into the source tree, and from a deep root the resulting paths exceed MAX_PATH and git
# aborts with "Filename too long". LongPathsEnabled is 0 on this machine and `subst` does
# not help -- the kernel resolves the substituted drive back to the real path. C:\voxl2 is
# chosen for exactly this reason. Do not move the repo under C:\Users\...
if ($repo.Length -gt 24) {
    Write-Warning "Repo root '$repo' is $($repo.Length) chars. vcpkg clones inside the tree and can blow MAX_PATH from a deep root."
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Step "Removing $buildDir (full rebuild requested)"
    Remove-Item -Recurse -Force $buildDir
}

# ---------------------------------------------------------------------------
# 4. Sample free disk space while the build runs, so peak usage is a measurement.
# ---------------------------------------------------------------------------
$drive = (Get-Item $repo).PSDrive.Name
$freeBefore = (Get-PSDrive $drive).Free
$sampler = Start-Job -ScriptBlock {
    param($d)
    $min = [long]::MaxValue
    while ($true) {
        $f = (Get-PSDrive $d).Free
        if ($f -lt $min) { $min = $f }
        Write-Output $min
        Start-Sleep -Seconds 2
    }
} -ArgumentList $drive

$sw = [Diagnostics.Stopwatch]::StartNew()
try {
    if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Write-Step "cmake --preset=$preset"
        Push-Location $repo
        try { & cmake --preset=$preset } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "configure failed with exit code $LASTEXITCODE" }
    }
    else {
        Write-Step 'Using existing CMake cache (pass -Reconfigure to force a fresh configure)'
    }

    if (-not $ConfigureOnly) {
        $buildPreset = "$preset-" + $Config.ToLower()
        Write-Step "cmake --build --preset=$buildPreset"
        Push-Location $repo
        try { & cmake --build --preset=$buildPreset } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
    }
}
finally {
    $sw.Stop()
    $minFree = (Receive-Job $sampler | Select-Object -Last 1)
    Stop-Job $sampler -ErrorAction SilentlyContinue
    Remove-Job $sampler -Force -ErrorAction SilentlyContinue
    $freeAfter = (Get-PSDrive $drive).Free

    Write-Host ''
    Write-Step 'Build report'
    Write-Fact 'elapsed'        ('{0:hh\:mm\:ss}' -f $sw.Elapsed)
    Write-Fact 'free before'    ('{0:N2} GB' -f ($freeBefore / 1GB))
    Write-Fact 'free after'     ('{0:N2} GB' -f ($freeAfter / 1GB))
    if ($minFree -and $minFree -ne [long]::MaxValue) {
        Write-Fact 'peak disk used' ('{0:N2} GB (transient, min free {1:N2} GB)' -f (($freeBefore - $minFree) / 1GB), ($minFree / 1GB))
    }
    if (Test-Path $buildDir) {
        $outSize = (Get-ChildItem $buildDir -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        Write-Fact '.out size' ('{0:N2} GB' -f ($outSize / 1GB))
    }
    $exe = Join-Path $buildDir "$Config\gvox_engine.exe"
    if (Test-Path $exe) {
        $i = Get-Item $exe
        Write-Fact 'exe' ('{0}  ({1:N2} MB, {2})' -f $i.FullName, ($i.Length / 1MB), $i.LastWriteTime)
    }
}
