<#
.SYNOPSIS
    Files the flat validation screenshots into named, ordered folders.

.DESCRIPTION
    Screenshots accumulate during development with terse capture-time names
    (ml_tod_dusk, vr_lod_seam_hard). This maps every one onto a descriptive path
    under docs/images/, grouped by the milestone that produced it and numbered so
    a directory listing reads in the order the work happened.

    COPIES rather than moves by default: a capture agent may still hold the old
    path, and git stores blobs by content hash, so the duplicate costs nothing in
    the object database until the flat originals are removed. Pass -Move once no
    process is using them.
#>
[CmdletBinding()]
param(
    [switch]$Move,
    [string]$Root = (Join-Path (Split-Path -Parent $PSScriptRoot) 'docs/images')
)

$ErrorActionPreference = 'Stop'

# source file name -> destination path relative to $Root
$map = [ordered]@{
    # ---------------------------------------------- M1: the vertical slice --
    'm1_spawn.png'                         = '01-milestone1-vertical-slice/01-first-successful-render-forest-and-mountains.png'
    'm1_uvfix.png'                         = '01-milestone1-vertical-slice/02-after-side-texture-rotation-fix.png'

    # ------------------------------------------------- M3: level of detail --
    'm3_lod_view.png'                      = '02-milestone3-lod/01-lod-long-view-radius-20.png'
    'm3_zoom_notch.png'                    = '02-milestone3-lod/02-zoom-dark-recess-confirmed-not-a-crack.png'
    'vr_default_spawn.png'                 = '02-milestone3-lod/03-default-spawn-reference.png'
    'vr_lod_rings_on.png'                  = '02-milestone3-lod/04-lod-rings-enabled-aerial.png'
    'vr_lod_rings_off.png'                 = '02-milestone3-lod/05-lod-rings-disabled-comparison.png'
    'vr_lod_overlay.png'                   = '02-milestone3-lod/06-lod-debug-overlay.png'
    'vr_lod_seam_hard.png'                 = '02-milestone3-lod/07-lod-seam-at-hard-band-edge.png'
    'vr_lod_seam_hard_zoom.png'            = '02-milestone3-lod/08-lod-seam-at-hard-band-edge-zoom.png'
    'vr_lod_seam_tight.png'                = '02-milestone3-lod/09-lod-seam-with-tight-bands.png'
    'vr_lod_ocean_before_fix.png'          = '02-milestone3-lod/10-lod-ocean-before-fix.png'
    'vr_lod_ocean_before_fix_zoom.png'     = '02-milestone3-lod/11-lod-ocean-before-fix-zoom.png'
    'vr_lod_ocean_on.png'                  = '02-milestone3-lod/12-lod-ocean-after-fix.png'
    'vr_lod_ocean_on_zoom.png'             = '02-milestone3-lod/13-lod-ocean-after-fix-zoom.png'
    'vr_lod_ocean_off.png'                 = '02-milestone3-lod/14-lod-ocean-with-lod-disabled.png'
    'ml_lod_stress.png'                    = '02-milestone3-lod/15-lod-stress-many-visible-chunks.png'
    'ml_lod_stress_off.png'                = '02-milestone3-lod/16-lod-stress-with-lod-disabled.png'

    # ------------------------------------------ M3: destructible sub-voxels --
    'm3_subvoxel_tunnel.png'               = '03-milestone3-subvoxels/01-first-carve-attempt-out-of-frame.png'
    'm3_tunnel_bore.png'                   = '03-milestone3-subvoxels/02-bore-camera-aimed-wrong-way.png'
    'm3_tunnel_bore2.png'                  = '03-milestone3-subvoxels/03-bore-anchor-moved-with-player.png'
    'm3_tunnel_bore3.png'                  = '03-milestone3-subvoxels/04-bore-through-slab-CONFIRMED-subvoxel-resolution.png'
    'vr_subvoxel_bore.png'                 = '03-milestone3-subvoxels/05-bore-visual-review.png'
    'vr_subvoxel_bore_zoom.png'            = '03-milestone3-subvoxels/06-bore-visual-review-zoom.png'
    'vr_subvoxel_bore_before_fix.png'      = '03-milestone3-subvoxels/07-bore-before-uv-phase-fix.png'
    'vr_subvoxel_bore_before_fix_zoom.png' = '03-milestone3-subvoxels/08-bore-before-uv-phase-fix-zoom.png'
    'vr_subvoxel_crater.png'               = '03-milestone3-subvoxels/09-crater-rig-on-terrain-surface.png'
    'vr_subvoxel_crater_zoom.png'          = '03-milestone3-subvoxels/10-crater-rig-rim-zoom.png'
    'vr_subvoxel_slab.png'                 = '03-milestone3-subvoxels/11-carved-slab.png'
    'vr_subvoxel_tunnel_inside.png'        = '03-milestone3-subvoxels/12-tunnel-interior.png'
    'ml_sub_bore.png'                      = '03-milestone3-subvoxels/13-bore-with-light-propagation.png'
    'ml_sub_bore_nolight.png'              = '03-milestone3-subvoxels/14-bore-with-lighting-disabled.png'
    'ml_sub_bore_zoom.png'                 = '03-milestone3-subvoxels/15-bore-lit-surface-zoom.png'
    'ml_sub_slab.png'                      = '03-milestone3-subvoxels/16-carved-slab-with-lighting.png'

    # ------------------------------------------- M4: light propagation --
    'ml_day_ground.png'                    = '04-milestone4-lighting/01-daylight-open-ground-uniformly-lit.png'
    'ml_day_stand.png'                     = '04-milestone4-lighting/02-daylight-standing-view.png'
    'ml_day_aerial.png'                    = '04-milestone4-lighting/03-daylight-aerial.png'
    'ml_day_ground_zoom_horizon.png'       = '04-milestone4-lighting/04-daylight-horizon-zoom.png'
    'ml_day_ground_zoom_water.png'         = '04-milestone4-lighting/05-daylight-water-zoom.png'
    'ml_cave_dark.png'                     = '04-milestone4-lighting/06-cave-interior-is-dark.png'
    'ml_cave_mouth.png'                    = '04-milestone4-lighting/07-cave-mouth-light-gradient.png'
    'ml_cave_mouth_lines.png'              = '04-milestone4-lighting/08-cave-mouth-scanline-analysis.png'
    'ml_cave_mouth_lodoff_zoom.png'        = '04-milestone4-lighting/09-cave-mouth-lod-disabled-zoom.png'
    'ml_cave_night_check.png'              = '04-milestone4-lighting/10-cave-at-night.png'
    'ml_cavefog_noon.png'                  = '04-milestone4-lighting/11-cave-fog-at-noon.png'
    'ml_cavefog_midnight.png'              = '04-milestone4-lighting/12-cave-fog-at-midnight.png'
    'ml_cavefog_nolight.png'               = '04-milestone4-lighting/13-cave-fog-lighting-disabled.png'
    'ml_cavefog_mid_zoom.png'              = '04-milestone4-lighting/14-cave-fog-zoom.png'
    'ml_glow_slab_before.png'              = '04-milestone4-lighting/15-glowstone-before-emission.png'
    'ml_glow_slab_after.png'               = '04-milestone4-lighting/16-glowstone-lighting-its-surroundings.png'
    'ml_white_down.png'                    = '04-milestone4-lighting/17-white-artifact-looking-down.png'
    'ml_white_night.png'                   = '04-milestone4-lighting/18-white-artifact-at-night.png'
    'ml_white_nohud.png'                   = '04-milestone4-lighting/19-white-artifact-hud-hidden.png'
    'ml_fix_before_zoom.png'               = '04-milestone4-lighting/20-white-artifact-before-fix-zoom.png'
    'ml_fix_after_zoom.png'                = '04-milestone4-lighting/21-white-artifact-after-fix-zoom.png'
    'ml_fix_white_nohud.png'               = '04-milestone4-lighting/22-white-artifact-fixed.png'
    'ml_shoreline_zoom.png'                = '04-milestone4-lighting/23-water-shoreline-zoom.png'

    # ----------------------------------------------- M4: day/night and sky --
    'ml_tod_dawn.png'                      = '05-milestone4-daynight-sky/01-time-of-day-dawn.png'
    'ml_tod_noon.png'                      = '05-milestone4-daynight-sky/02-time-of-day-noon.png'
    'ml_tod_dusk.png'                      = '05-milestone4-daynight-sky/03-time-of-day-dusk.png'
    'ml_tod_night.png'                     = '05-milestone4-daynight-sky/04-time-of-day-night.png'
    'ml_tod_dusk_horizon.png'              = '05-milestone4-daynight-sky/05-dusk-horizon-fog-sky-match.png'
    'ml_tod_night_horizon.png'             = '05-milestone4-daynight-sky/06-night-horizon-fog-sky-match.png'
    'sky_dawn.png'                         = '05-milestone4-daynight-sky/07-sky-dome-dawn.png'
    'sky_noon.png'                         = '05-milestone4-daynight-sky/08-sky-dome-noon.png'
    'night_zenith.png'                     = '05-milestone4-daynight-sky/09-night-zenith-star-field.png'
    'night_moon.png'                       = '05-milestone4-daynight-sky/10-night-moon.png'
    'night_horizon_BEFORE.png'             = '05-milestone4-daynight-sky/11-night-horizon-before-fix.png'
    'night_horizon.png'                    = '05-milestone4-daynight-sky/12-night-horizon-after-fix.png'
    'crop_dawn_horizon.png'                = '05-milestone4-daynight-sky/13-crop-dawn-horizon.png'
    'crop_before_horizon.png'              = '05-milestone4-daynight-sky/14-crop-horizon-before-fix.png'
    'crop_after_horizon.png'               = '05-milestone4-daynight-sky/15-crop-horizon-after-fix.png'
    'crop_moon.png'                        = '05-milestone4-daynight-sky/16-crop-moon.png'
    'diff_stars.png'                       = '05-milestone4-daynight-sky/17-difference-star-field.png'
    'diff_terrain_band.png'                = '05-milestone4-daynight-sky/18-difference-terrain-band.png'
    'sky_dusk.png'                         = '05-milestone4-daynight-sky/19-sky-dome-dusk.png'
    'crop_dawn_moon_horizon.png'           = '05-milestone4-daynight-sky/20-crop-dawn-moon-at-horizon.png'
    'crop_night_seam.png'                  = '05-milestone4-daynight-sky/21-crop-night-horizon-seam-check.png'
    'water_night.png'                      = '05-milestone4-daynight-sky/22-water-at-night.png'

    # ---------------------------- M5: water-at-LOD artefact, found and fixed --
    'water_lod_artifact_off.png'           = '07-milestone5-water-lod-artifact/01-water-with-lod-disabled-control.png'
    'water_lod_artifact_on.png'            = '07-milestone5-water-lod-artifact/02-water-with-lod-enabled-artefact.png'
    'crop_water_lod_off.png'               = '07-milestone5-water-lod-artifact/03-crop-lod-disabled.png'
    'crop_water_lod_on.png'                = '07-milestone5-water-lod-artifact/04-crop-lod-enabled.png'

    # ------------------------------------------------ M4: menus and settings --
    'ml_menu_title.png'                    = '06-milestone4-ui/01-main-menu-title-screen.png'
    'ml_ui_pause.png'                      = '06-milestone4-ui/02-pause-menu.png'
    'ml_ui_pause_zoom.png'                 = '06-milestone4-ui/03-pause-menu-zoom.png'
    'ml_ui_settings.png'                   = '06-milestone4-ui/04-settings-panel.png'
    'ml_ui_settings_zoom.png'              = '06-milestone4-ui/05-settings-panel-zoom.png'
    'ml_ui_settings_zoom2.png'             = '06-milestone4-ui/06-settings-panel-zoom-detail.png'
}

$copied = 0
$absent = @()
foreach ($entry in $map.GetEnumerator()) {
    $source = Join-Path $Root $entry.Key
    if (-not (Test-Path $source)) { $absent += $entry.Key; continue }

    $destination = Join-Path $Root $entry.Value
    $folder = Split-Path -Parent $destination
    if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }

    if ($Move) { Move-Item -Force $source $destination } else { Copy-Item -Force $source $destination }
    $copied++
}

# Anything not in the map is a capture taken after this script was last updated.
$unmapped = Get-ChildItem $Root -Filter *.png -File |
            Where-Object { -not $map.Contains($_.Name) } |
            Select-Object -ExpandProperty Name

Write-Host "$(if ($Move) { 'Moved' } else { 'Copied' }) $copied image(s)." -ForegroundColor Green
if ($absent.Count)   { Write-Host "Listed but not on disk: $($absent -join ', ')" -ForegroundColor Yellow }
if ($unmapped.Count) { Write-Host "On disk but unmapped ($($unmapped.Count)): $($unmapped -join ', ')" -ForegroundColor Yellow }
