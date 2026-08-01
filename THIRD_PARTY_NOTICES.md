# Third-Party Notices

This project is a fork of [gvox_engine](https://github.com/GabeRundlett/gvox_engine)
by Gabe Rundlett, used under the MIT License. What follows is everything it
carries, with the licence each is used under.

Two of these were **not** attributed by upstream, and are corrected here — see
"Corrections to upstream" at the end.

## Engine and libraries

| Component | Licence | Notes |
|---|---|---|
| [gvox_engine](https://github.com/GabeRundlett/gvox_engine) | MIT — © 2023 Gabe Rundlett | The engine this forks |
| [Daxa](https://github.com/Ipotrick/Daxa) | MIT — © 2021 Ipotrick (Patrick Ahrens) | Vulkan abstraction |
| [gvox](https://github.com/GabeRundlett/gvox) | MIT — © 2022 Gabe Rundlett | Voxel format library |
| [kajiya](https://github.com/EmbarkStudios/kajiya) | MIT — © 2019 Embark Studios | **See below** |
| AMD FidelityFX (FSR 2.2) | MIT — © 2021 Advanced Micro Devices, Inc. | Upscaling |
| blue-noise-sampler | MIT (upstream Jasper-Bekkers) | Fork ships no LICENSE file |
| assimp | BSD-3-Clause | |
| GLFW, minizip | Zlib | |
| Dear ImGui, GLM, fmt, nlohmann-json, stb | MIT | |

### On kajiya, which deserves more than a table row

**Roughly 47% of `src/` is a C++/GLSL port of Embark Studios' kajiya renderer** —
the irradiance cache, ReSTIR diffuse, ReSTIR reflections and the denoising stack.
The path-traced global illumination that gives this engine its look is Embark's
algorithm, ported by Gabe Rundlett, not original to either this fork or upstream.
It is MIT licensed and its use here is entirely legitimate, but it should be
credited plainly rather than buried.

## Assets

| Asset | Licence |
|---|---|
| `assets/STBN.zip` — spatiotemporal blue noise | **NVIDIA** — see below |
| Inter Tight | SIL Open Font License 1.1 |
| Roboto Mono | Apache License 2.0 |

### NVIDIA spatiotemporal blue noise

`assets/STBN.zip` contains spatiotemporal blue noise textures from
[NVIDIA-RTX/STBN](https://github.com/NVIDIA-RTX/STBN). Upstream ships these under
a blanket MIT licence with no NVIDIA notice, which is incorrect: NVIDIA licenses
them under a **dual** licence, whose default branch restricts use to "research or
evaluation purposes only".

This project elects NVIDIA's **Commercial Use License** branch, and accordingly
reproduces the required notice:

> This software contains source code provided by NVIDIA Corporation.

That branch additionally requires the application to have material functionality
beyond the included portions of the SDK (it does — this is a voxel game engine),
and excludes avionics, military, medical and other life-critical use.

If you would rather not depend on NVIDIA's terms at all, the textures can be
regenerated: NVIDIA publishes the generator, and the engine only uses them for
sampling-noise decorrelation.

## Corrections to upstream

Recorded so the reasoning is not lost:

1. **`assets/STBN.zip` was shipped with no licence and no attribution**, under a
   blanket MIT that does not apply to it. Corrected above by electing NVIDIA's
   commercial branch and reproducing the required notice.
2. **`packaging/infos/license.txt` read "Gabe Rundlett / All Rights Reserved"** and
   is wired into CPack as the installer EULA, so any installer built from upstream
   presents an All-Rights-Reserved agreement over MIT-licensed code. Replaced with
   the actual MIT terms. This was reported upstream as issue #12 in 2024 and closed
   without the file being changed.

Neither correction changes what anyone may do with the code. Both make the
repository say what is actually true.
