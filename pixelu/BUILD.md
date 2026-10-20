# Building PixelU

RetroPixels ships PixelU for 64-bit Windows only. The release build is made by
`.github/workflows/pixelu.yml` in the repository root. It runs on every push
to `pixelu/` and produces the `PixelU-windows-x64` workflow artifact, then
starts `bundle-test.yml`, which puts that build into the RetroPixels download.

## Windows (what CI does)

You need Visual Studio 2022 with the C++ desktop workload, CMake 3.21 or
newer (CI uses 3.29), and Git.

```
git clone --recursive https://github.com/shredder565/retropixels
cd retropixels/pixelu
dependencies\vcpkg\bootstrap-vcpkg.bat
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=release
cmake --build . --config release
```

The first configure builds every library in `vcpkg.json` from source with
vcpkg, which takes a while. Later builds reuse vcpkg's cache.

The program is written to `bin/PixelU_release.exe` (or `PixelU_debug.exe`).
It needs the `resources` and `gameProfiles` folders next to it, which `bin/`
already has. CI copies all three into `PixelU/`, renames the program to
`PixelU.exe`, and adds an empty `portable` folder, so PixelU keeps its
settings, keys and saves next to the program.

## Options

The CMake options in `CMakeLists.txt` select backends and features, for
example `ENABLE_VULKAN`, `ENABLE_OPENGL`, `ENABLE_SDL`, `ENABLE_CUBEB`,
`ENABLE_DISCORD_RPC` and `ALLOW_PORTABLE`. The defaults are what CI builds.
To stamp a version, pass `EMULATOR_VERSION_MAJOR`, `EMULATOR_VERSION_MINOR`
and `EMULATOR_VERSION_PATCH`. Without them, PixelU shows the commit hash as
its version.

## Linux and macOS

The CMake files still configure on Linux (GTK and, optionally, Wayland) and
on macOS (Vulkan through MoltenVK, `-DMACOS_BUNDLE=ON` for an .app). These
builds are not tested and RetroPixels does not ship them. On Linux, building
without vcpkg (`-DENABLE_VCPKG=OFF`) needs the same libraries installed
system-wide.

## Checking a change

- `python3 tools/pixelu-original.py` (from the repository root) updates the
  table in `docs/pixelu-original.md`.
- To syntax-check a single file on Linux, run this from `pixelu/src`:
  `g++ -std=c++20 -fsyntax-only -include Common/precompiled.h -I. -ICafe -I../dependencies -I../dependencies/Vulkan-Headers/include -DENABLE_VULKAN -DVK_NO_PROTOTYPES <file>`.
  The Windows CI build is the real test.
