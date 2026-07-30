# Third-Party Notices

Voxl links the following third-party components. All are permissively licensed and
are fetched from their official upstream repositories at configure time (see
`cmake/Dependencies.cmake`), except GLAD, which is pre-generated and committed under
`external/glad/`.

No source code, assets, audio, textures, branding or world-generation parameters
were taken from any commercial voxel game. All game content in this repository is
original.

| Component | Version | License | Purpose |
|---|---|---|---|
| [GLFW](https://github.com/glfw/glfw) | 3.4 | Zlib/libpng | Window creation, OpenGL context, input |
| [GLM](https://github.com/g-truc/glm) | 1.0.3 | MIT (Happy Bunny alt.) | Vector and matrix math |
| [GLAD 2](https://github.com/Dav1dde/glad) | generated, GL 4.6 core | MIT (generator); generated loader is public-domain-equivalent | OpenGL function pointer loading |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9-docking | MIT | Debug overlay and settings UI |
| [stb_image](https://github.com/nothings/stb) | `f58f558` | MIT / Public Domain (dual) | PNG and texture decoding |
| [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) | 1.1.1 | MIT | Coherent noise for terrain generation |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | MIT-0 / Public Domain (dual) | Audio playback and spatialisation |
| [Catch2](https://github.com/catchorg/Catch2) | 3.11.0 | BSL-1.0 | Test framework (test binary only; not linked into the game) |

The Khronos OpenGL and EGL registry XML used to generate the GLAD loader, and the
resulting `KHR/khrplatform.h`, are © The Khronos Group Inc. and distributed under
the Apache 2.0 / MIT-style Khronos license.

## License texts

Full license texts are not reproduced here. Each dependency's license file is
present in its checkout under `.deps/<name>-src/` after a configure, and at the
upstream URLs listed above.

## Assets

All textures, sounds and shaders shipped in `assets/` are original work created for
this project. Block textures are currently generated procedurally at runtime by
`src/render/TextureGen.cpp` rather than authored as image files; that generator is
original code and produces original imagery.

## Obligations

Every component above is permissive and requires only attribution — this file, plus
retention of the copyright notices inside the sources themselves, satisfies them.
None require source disclosure or impose copyleft on Voxl. Redistributing a built
binary requires shipping this file alongside it.
