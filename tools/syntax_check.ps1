<#
.SYNOPSIS
    Compiles one or more translation units in isolation, without linking.

.DESCRIPTION
    Lets a single source file be validated without a full project build, which
    is useful while a module is still being written and its siblings do not
    compile yet. Object files go to a scratch directory and are discarded, so
    this never disturbs the real build tree.

.EXAMPLE
    ./tools/syntax_check.ps1 src/world/Chunk.cpp
    ./tools/syntax_check.ps1 src/world/*.cpp
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]]$Sources
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# ------------------------------------------------------------- toolchain --
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsPath)) { throw 'No MSVC installation found.' }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'cl.exe unavailable.' }

# Dependency headers come from the FetchContent cache populated by a configure.
$deps = Join-Path $repoRoot '.deps'
if (-not (Test-Path $deps)) {
    throw "No .deps directory. Run ./tools/build.ps1 once so dependencies are fetched."
}

$includes = @(
    (Join-Path $repoRoot 'src'),
    (Join-Path $repoRoot 'external/glad/include'),
    (Join-Path $deps 'glm-src'),
    (Join-Path $deps 'glfw-src/include'),
    (Join-Path $deps 'imgui-src'),
    (Join-Path $deps 'imgui-src/backends'),
    (Join-Path $deps 'stb-src'),
    (Join-Path $deps 'fastnoiselite-src/Cpp'),
    (Join-Path $deps 'miniaudio-src'),
    # Catch2, so that files under tests/ can be checked too.
    (Join-Path $deps 'catch2-src/src'),
    (Join-Path $deps 'catch2-build/generated-includes')
) | Where-Object { Test-Path $_ } | ForEach-Object { "/I`"$_`"" }

$scratch = Join-Path $env:TEMP 'voxl_syntax_check'
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$resolved = @()
foreach ($pattern in $Sources) {
    $matched = Get-ChildItem -Path $pattern -ErrorAction SilentlyContinue
    if ($matched) { $resolved += $matched.FullName } else { $resolved += $pattern }
}

$failed = 0
foreach ($source in $resolved) {
    if (-not (Test-Path $source)) { Write-Host "SKIP (missing): $source" -ForegroundColor Yellow; continue }
    Write-Host "Checking $source" -ForegroundColor Cyan

    # /Zs is syntax-only; it is faster than /c and emits no object file.
    $arguments = @(
        '/nologo', '/std:c++20', '/permissive-', '/Zc:preprocessor', '/Zc:__cplusplus',
        '/EHsc', '/W4', '/utf-8', '/external:W0', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
        '/D_CRT_SECURE_NO_WARNINGS', '/DVOXL_DEBUG=0', '/Zs'
    ) + $includes + @("`"$source`"")

    & cl.exe @arguments
    if ($LASTEXITCODE -ne 0) { $failed++; Write-Host "  FAILED" -ForegroundColor Red }
}

if ($failed -gt 0) {
    Write-Host "$failed file(s) failed to compile." -ForegroundColor Red
    exit 1
}
Write-Host "All files compiled cleanly." -ForegroundColor Green
