<#
.SYNOPSIS
    Crop and optionally magnify a region of a PNG, for looking at voxel-scale detail.

.DESCRIPTION
    Reading a 1280x720 frame whole is not enough to judge whether needles resolve as individual
    voxels or whether a dark patch is a hole or a shading artefact. This crops a region and scales
    it with NEAREST-NEIGHBOUR interpolation, which is the only correct choice here: any smoothing
    invents intermediate colours between voxel faces and makes a hard artefact look soft.

.EXAMPLE
    pwsh -File tools\crop.ps1 -In docs\images\13-cave-mouth.png -Region "520,60,220,140" -Scale 4 -Out out.png
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Region, # "LEFT,TOP,WIDTH,HEIGHT" in source pixels
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Scale = 3
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$inv = [Globalization.CultureInfo]::InvariantCulture
$r = $Region -split ',' | ForEach-Object { [int]::Parse($_.Trim(), $inv) }
if ($r.Count -ne 4) { throw "-Region needs LEFT,TOP,WIDTH,HEIGHT" }

$src = [System.Drawing.Image]::FromFile((Resolve-Path $In).Path)
try {
    $dst = New-Object System.Drawing.Bitmap(($r[2] * $Scale), ($r[3] * $Scale))
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, ($r[2] * $Scale), ($r[3] * $Scale))),
        (New-Object System.Drawing.Rectangle($r[0], $r[1], $r[2], $r[3])), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $outPath = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outPath) | Out-Null
    $dst.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $dst.Dispose()
    Write-Host $outPath
}
finally { $src.Dispose() }
