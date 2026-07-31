<#
.SYNOPSIS
    Builds the whole side-by-side comparison set from captures in both projects.

.DESCRIPTION
    Every meaningful change in this project was captured as a pair - a control and
    a treatment, framed identically. This assembles all of them into one numbered,
    readable folder so progress can be reviewed in one pass instead of by opening
    forty files and remembering what each one was for.

    Pairs are declared rather than discovered, because only a human knows which
    two images are meant to be compared and what the comparison is supposed to
    show. A missing source is reported and skipped, so this stays runnable while
    captures are still being produced.
#>
[CmdletBinding()]
param(
    [string]$Out = 'C:/voxl2/docs/comparisons'
)

$ErrorActionPreference = 'Stop'
$montage = Join-Path $PSScriptRoot 'montage.ps1'
$V2  = 'C:/voxl2/docs/images'                       # the current engine
$OLD = 'C:/Users/luigi/projects/Voxl/docs/images'   # the previous OpenGL engine

# out-name | left | right | left label | right label | title | note
$pairs = @(
  @{ out='01-gi-cave-lit-vs-dark.png'
     l="$V2/14-cave-interior-lit.png";  r="$V2/15-cave-interior-dark-control.png"
     ll='Emissive crystal present'; rl='Same chamber, light removed'
     t='Path-traced GI - the proof'
     n='The control is the evidence: the cave is genuinely enclosed, so sunlight cannot reach it. All the light on the left is the crystal bouncing off the walls.' }

  @{ out='02-gi-cave-lookback.png'
     l="$V2/16-cave-lookback-gi.png";   r="$V2/17-cave-lookback-dark-control.png"
     ll='Lit'; rl='Control'
     t='Cave looking back toward the entrance'
     n='Same pair from the opposite direction. The faint cool light in the control is skylight bleeding down the tunnel.' }

  @{ out='03-scene-vs-demo-world.png'
     l="$V2/10-scene-wide-sunlit.png";  r="$V2/20-ab-demo-world-same-build.png"
     ll='Our test island'; rl="gvox_engine's own demo world"
     t='Our scene against the reference engine, same build'
     n='Both at 16 voxels/m. Ours runs at 10.93 ms / 830 MB heap; the demo at 17.97 ms / 2906 MB.' }

  @{ out='04-subvoxel-bore.png'
     l="$OLD/03-milestone3-subvoxels/07-bore-before-uv-phase-fix.png"
     r="$OLD/03-milestone3-subvoxels/04-bore-through-slab-CONFIRMED-subvoxel-resolution.png"
     ll='Before the UV phase fix'; rl='After'
     t='Destructible sub-voxels (previous engine)'
     n='Wall stepping far finer than one block is what proves the carve is genuinely sub-voxel rather than block removal.' }

  @{ out='05-lod-rings.png'
     l="$OLD/02-milestone3-lod/05-lod-rings-disabled-comparison.png"
     r="$OLD/02-milestone3-lod/04-lod-rings-enabled-aerial.png"
     ll='LOD disabled'; rl='LOD enabled'
     t='Chunk level of detail (previous engine)'
     n='124x fewer triangles per chunk at distance; 6.4x more world for 1.5x the geometry.' }

  @{ out='06-lod-ocean-fix.png'
     l="$OLD/02-milestone3-lod/10-lod-ocean-before-fix.png"
     r="$OLD/02-milestone3-lod/12-lod-ocean-after-fix.png"
     ll='Before'; rl='After'
     t='LOD ocean-horizon artefact, fixed' }

  @{ out='07-glowstone-emission.png'
     l="$OLD/04-milestone4-lighting/15-glowstone-before-emission.png"
     r="$OLD/04-milestone4-lighting/16-glowstone-lighting-its-surroundings.png"
     ll='Before'; rl='After'
     t='Block light propagation (previous engine)'
     n='Emissive blocks lighting their surroundings independently of sunlight.' }

  @{ out='08-night-horizon-fix.png'
     l="$OLD/05-milestone4-daynight-sky/11-night-horizon-before-fix.png"
     r="$OLD/05-milestone4-daynight-sky/12-night-horizon-after-fix.png"
     ll='Before'; rl='After'
     t='Night horizon, fog matched to sky' }

  @{ out='09-white-artifact-fix.png'
     l="$OLD/04-milestone4-lighting/20-white-artifact-before-fix-zoom.png"
     r="$OLD/04-milestone4-lighting/21-white-artifact-after-fix-zoom.png"
     ll='Before'; rl='After'
     t='Washed-out white artefact, diagnosed and fixed' }

  @{ out='10-engine-generations.png'
     l="$OLD/01-milestone1-vertical-slice/01-first-successful-render-forest-and-mountains.png"
     r="$V2/11-tree-full-height.png"
     ll='Previous engine - 1 m blocks, rasterised'; rl='Current - 16 voxels/m, path traced'
     t='Where this started and where it is now'
     n='4096x more voxels per unit volume, and per-voxel colour instead of texture mapping.' }
)

if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force -Path $Out | Out-Null }

Write-Host "Building comparisons into $Out" -ForegroundColor Cyan
$made = 0; $skipped = @()
foreach ($p in $pairs) {
    if (-not (Test-Path $p.l) -or -not (Test-Path $p.r)) {
        $skipped += $p.out
        continue
    }
    $args = @{
        Left = $p.l; Right = $p.r; Out = (Join-Path $Out $p.out)
        LeftLabel = $p.ll; RightLabel = $p.rl
    }
    if ($p.t) { $args.Title = $p.t }
    if ($p.n) { $args.Note  = $p.n }
    & $montage @args
    $made++
}

Write-Host "$made comparison(s) written." -ForegroundColor Green
if ($skipped.Count) {
    Write-Host "Skipped (source not present yet): $($skipped -join ', ')" -ForegroundColor Yellow
}
