<#
.SYNOPSIS
    Composites two captures into one labelled side-by-side comparison image.

.DESCRIPTION
    A pair of screenshots in a folder is hard to judge - you cannot hold one in
    your head while looking at the other, and the differences that matter are
    often small. This stitches them into a single image with a caption strip, so
    the comparison is the thing you look at rather than something you have to
    reconstruct.

    Both images are scaled to a common height and drawn at their own aspect
    ratio, so a full-frame capture and a zoomed crop can still be compared
    without either being distorted.

.EXAMPLE
    ./tools/montage.ps1 -Left before.png -Right after.png `
        -LeftLabel "LOD disabled" -RightLabel "LOD enabled" `
        -Title "Distant terrain" -Out docs/comparisons/01-lod.png
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Left,
    [Parameter(Mandatory = $true)][string]$Right,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$LeftLabel  = 'BEFORE',
    [string]$RightLabel = 'AFTER',
    [string]$Title      = '',
    [string]$Note       = '',
    [int]$Height        = 720,
    [int]$Gap           = 12
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

foreach ($p in @($Left, $Right)) {
    if (-not (Test-Path $p)) { throw "Missing image: $p" }
}

$titleH   = if ($Title) { 46 } else { 0 }
$labelH   = 34
$noteH    = if ($Note) { 30 } else { 0 }
$margin   = 12

$a = [System.Drawing.Image]::FromFile((Resolve-Path $Left).Path)
$b = [System.Drawing.Image]::FromFile((Resolve-Path $Right).Path)

try {
    # Common height, native aspect each: a 1280x720 frame and a 260x150 crop can
    # sit side by side without either being stretched.
    $aW = [int]([math]::Round($a.Width * ($Height / $a.Height)))
    $bW = [int]([math]::Round($b.Width * ($Height / $b.Height)))

    $canvasW = $margin * 2 + $aW + $Gap + $bW
    $canvasH = $margin + $titleH + $labelH + $Height + $noteH + $margin

    $canvas = New-Object System.Drawing.Bitmap $canvasW, $canvasH
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    try {
        $g.InterpolationMode  = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.SmoothingMode      = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.TextRenderingHint  = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
        $g.Clear([System.Drawing.Color]::FromArgb(24, 24, 27))

        $y = $margin

        if ($Title) {
            $titleFont = New-Object System.Drawing.Font 'Segoe UI Semibold', 16
            $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(240, 240, 245))
            $g.DrawString($Title, $titleFont, $white, $margin, $y)
            $titleFont.Dispose(); $white.Dispose()
            $y += $titleH
        }

        $labelFont = New-Object System.Drawing.Font 'Segoe UI Semibold', 11
        $dim = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(170, 170, 178))
        $g.DrawString($LeftLabel,  $labelFont, $dim, $margin, $y + 8)
        $g.DrawString($RightLabel, $labelFont, $dim, $margin + $aW + $Gap, $y + 8)
        $labelFont.Dispose(); $dim.Dispose()
        $y += $labelH

        $g.DrawImage($a, $margin, $y, $aW, $Height)
        $g.DrawImage($b, ($margin + $aW + $Gap), $y, $bW, $Height)

        # A hairline between the panels, so the eye does not read a dark region at
        # the edge of one image as part of the other.
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(90, 90, 96)), 1
        $x = $margin + $aW + [int]($Gap / 2)
        $g.DrawLine($pen, $x, $y, $x, ($y + $Height))
        $pen.Dispose()
        $y += $Height

        if ($Note) {
            $noteFont = New-Object System.Drawing.Font 'Segoe UI', 10
            $noteBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 150, 158))
            $g.DrawString($Note, $noteFont, $noteBrush, $margin, $y + 7)
            $noteFont.Dispose(); $noteBrush.Dispose()
        }
    } finally { $g.Dispose() }

    $outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location) $Out }
    $dir = Split-Path -Parent $outPath
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

    $canvas.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose()
    Write-Host ("  {0}  ({1}x{2})" -f (Split-Path -Leaf $outPath), $canvasW, $canvasH) -ForegroundColor Green
} finally {
    $a.Dispose()
    $b.Dispose()
}
