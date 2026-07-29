<#
.SYNOPSIS
    Configures and builds Voxl with the Visual Studio 2022 toolchain.

.DESCRIPTION
    Locates MSVC via vswhere, prefers the CMake and Ninja bundled with Visual
    Studio when they are not already on PATH, and drives a Ninja build. Running
    this script is the documented, reproducible way to build the project; see
    docs/BUILDING.md.

.EXAMPLE
    ./tools/build.ps1 -Config Release -Target voxl
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Config = 'RelWithDebInfo',

    [string]$Target = 'all',

    [string]$BuildDir = '',

    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($BuildDir)) {
    $BuildDir = Join-Path $repoRoot "build/$Config"
}

# ------------------------------------------------------------- toolchain --
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio 2022 (or the Build Tools) with the 'Desktop development with C++' workload."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsPath)) {
    throw "No Visual Studio installation with the MSVC v143 toolset was found."
}
Write-Host "Visual Studio: $vsPath" -ForegroundColor DarkGray

# Fall back to the copies shipped inside Visual Studio when the user has no
# standalone CMake/Ninja, so a stock VS install is sufficient to build.
function Resolve-Tool {
    param([string]$Name, [string]$BundledPath)
    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    if (Test-Path $BundledPath) { return $BundledPath }
    throw "$Name not found on PATH and not bundled at $BundledPath."
}

$cmakeExe = Resolve-Tool 'cmake' (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
$ninjaExe = Resolve-Tool 'ninja' (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')
$ctestExe = Join-Path (Split-Path -Parent $cmakeExe) 'ctest.exe'
Write-Host "CMake: $cmakeExe" -ForegroundColor DarkGray
Write-Host "Ninja: $ninjaExe" -ForegroundColor DarkGray

# ------------------------------------------- import the MSVC environment --
# Ninja needs cl.exe, the Windows SDK headers and the linker on PATH/INCLUDE/LIB.
# vcvars64.bat is the only supported way to obtain them, so run it and import
# the resulting environment into this session.
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

& cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe is still unavailable after importing the vcvars64 environment."
}

# ------------------------------------------------------------------ build --
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Removing $BuildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $BuildDir
}

$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if ($Reconfigure -or -not (Test-Path $cacheFile)) {
    Write-Host "Configuring ($Config)..." -ForegroundColor Cyan
    & $cmakeExe -S $repoRoot -B $BuildDir -G Ninja `
        "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
        '-DCMAKE_C_COMPILER=cl' `
        '-DCMAKE_CXX_COMPILER=cl'
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }
}

Write-Host "Building $Target ($Config)..." -ForegroundColor Cyan
$buildArgs = @('--build', $BuildDir)
if ($Target -ne 'all') { $buildArgs += @('--target', $Target) }
& $cmakeExe @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

if ($RunTests) {
    Write-Host "Running tests..." -ForegroundColor Cyan
    & $ctestExe --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Tests failed with exit code $LASTEXITCODE." }
}

Write-Host "Done. Binaries in $BuildDir/bin" -ForegroundColor Green
