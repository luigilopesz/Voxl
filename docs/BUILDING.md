# Building Voxl on Windows

## Prerequisites

| Requirement | Minimum | Notes |
|---|---|---|
| Windows | 10 (1809) / 11 | 64-bit only. |
| Visual Studio 2022 | 17.10+ | The **Build Tools** edition is sufficient. Install the *Desktop development with C++* workload, which provides MSVC v143, the Windows SDK, CMake and Ninja. |
| Windows SDK | 10.0.19041 | Newer is fine; 10.0.26100 is what CI uses. |
| GPU driver | OpenGL 4.5 core | Any GPU from roughly 2014 onward. Verified on an NVIDIA RTX 3050 reporting `OpenGL 4.5.0 NVIDIA`. |
| Git | any recent | Required: dependencies are fetched from GitHub at configure time. |

You do **not** need a standalone CMake, Ninja or Python installation. `tools/build.ps1`
falls back to the copies bundled inside Visual Studio, and the OpenGL loader is
pre-generated and committed under `external/glad/`.

An internet connection is required for the **first** configure only. After that,
sources are cached in `.deps/` and subsequent configures work offline.

## Build

From the repository root, in PowerShell:

```powershell
./tools/build.ps1
```

That configures and builds `RelWithDebInfo` into `build/RelWithDebInfo/`, with
binaries in `build/RelWithDebInfo/bin/`.

The script locates Visual Studio via `vswhere`, imports the MSVC environment from
`vcvars64.bat`, and drives a Ninja build. Importing `vcvars64.bat` is not optional:
Ninja invokes `cl.exe` directly and needs `INCLUDE`, `LIB` and `PATH` populated.

### Options

```powershell
./tools/build.ps1 -Config Debug          # Debug | RelWithDebInfo | Release
./tools/build.ps1 -Target voxl           # build a single target
./tools/build.ps1 -RunTests              # build, then run the suite via ctest
./tools/build.ps1 -Reconfigure           # re-run CMake configure
./tools/build.ps1 -Clean                 # delete the build directory first
```

Configuration differences that matter:

- **Debug** enables `VOXL_DEBUG`, so `VOXL_ASSERT` is active and a GL debug context
  is requested. Expect a large frame-time penalty — this is not a performance
  configuration.
- **RelWithDebInfo** is the default and the one to profile and play with.
- **Release** links with `/SUBSYSTEM:WINDOWS`, so no console window appears and
  logging goes to `voxl.log` only.

### CMake options

| Option | Default | Effect |
|---|---|---|
| `VOXL_BUILD_TESTS` | `ON` | Builds `voxl_tests` and registers it with CTest. |
| `VOXL_BUILD_BENCHMARKS` | `ON` | Builds `voxl_bench`. |
| `VOXL_WARNINGS_AS_ERRORS` | `OFF` | Adds `/WX`. Enabled in CI. |
| `VOXL_ENABLE_ASAN` | `OFF` | Adds `/fsanitize=address`. Incompatible with `/RTC`, so use with `RelWithDebInfo`. |

## Run

```powershell
./build/RelWithDebInfo/bin/voxl.exe
```

The executable expects `assets/` beside it; CMake copies the directory on every
build via the `voxl_copy_assets` target, so running from `bin/` always works.

Logs are written to `voxl.log` in the working directory and mirrored to stdout.
Every record is flushed immediately, so the log is complete even after a crash.

## Test

```powershell
./tools/build.ps1 -RunTests
```

Or directly, which is faster while iterating on one suite:

```powershell
./build/RelWithDebInfo/bin/voxl_tests.exe "[mesher]"
```

Tests are headless by contract — nothing in `tests/` may create an OpenGL context,
so the suite runs on a machine with no GPU. GPU-dependent measurements belong in
`benchmarks/`.

## Checking a single file

A full build is unnecessary when iterating on one translation unit:

```powershell
./tools/syntax_check.ps1 src/mesh/GreedyMesher.cpp
```

This runs `cl /Zs` (syntax only, no object file) with the project's include paths.
It requires that `.deps/` has been populated by at least one successful configure.

## Troubleshooting

**`vswhere.exe not found`** — Visual Studio 2022 is not installed, or only the IDE
without the C++ workload. Install *Desktop development with C++*.

**`cl.exe is still unavailable after importing the vcvars64 environment`** — the
MSVC toolset component is missing from an otherwise valid VS installation. Add
*MSVC v143 - VS 2022 C++ x64/x86 build tools* in the VS Installer.

**Configure fails cloning a dependency** — the first configure needs network access
to GitHub. Behind a proxy, set `HTTPS_PROXY` before running the script.

**`Failed to create a GLFW window with an OpenGL 4.5 core context`** — the GPU
driver does not expose 4.5 core. Update the driver. On laptops with switchable
graphics, force `voxl.exe` onto the discrete GPU: integrated Intel parts often
advertise a lower core-profile version than the discrete GPU beside them.

**Stale build after switching branches** — `./tools/build.ps1 -Reconfigure`. A full
`-Clean` is rarely needed and re-downloads nothing, since `.deps/` sits outside the
build directory.
