# Building PixelU

RetroPixels ships PixelU for 64-bit Windows and Linux. The builds are made by
`.github/workflows/pixelu.yml` in the repository root. It runs on every push
to `pixelu/` and produces the `PixelU-windows-x64` and `PixelU-linux-x64`
workflow artifacts, then starts `bundle-test.yml` and `bundle-linux.yml`,
which put them into the RetroPixels downloads.

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

## Linux (what CI does)

The `build-linux` job of `.github/workflows/pixelu.yml` builds on Ubuntu 22.04
(glibc 2.35, the oldest base RetroPixels supports) with GCC 12 and vcpkg's
`x64-linux` triplet, so the libraries are linked statically as on Windows.
GTK 3, X11/XCB, Wayland, libGL and the Vulkan loader come from the system
(the empty ports in `dependencies/vcpkg_overlay_ports_linux`); the job lists
the packages it installs.

```
cd retropixels/pixelu        # a recursive clone, as for Windows
./dependencies/vcpkg/bootstrap-vcpkg.sh -disableMetrics
CC=gcc-12 CXX=g++-12 cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The program is `bin/PixelU_release`. CI packs it as `PixelU/PixelU` with
`resources`, `gameProfiles` and an empty `portable` folder into
`PixelU-linux.tar.gz` (artifact `PixelU-linux-x64`), checks that it needs no
newer glibc or libstdc++ than Ubuntu 22.04's, and runs RetroPixels' own demo
(`wiiu/testdata/demo.rpx`) under Xvfb, with Vulkan (lavapipe) and with OpenGL
(llvmpipe), through the docking tests in `internal/pixelu/screen_linux_test.go`
(`dist/linux/demo-test.sh`). Building without vcpkg (`-DENABLE_VCPKG=OFF`)
needs the same libraries installed system-wide.

Docked into RetroPixels (`--rp-embed`), PixelU runs on X11, which Wayland
desktops provide through XWayland; see `src/gui/wxgui/RPEmbed.h` and
`RPEmbedX11.h`.

## macOS

The CMake files still configure on macOS (Vulkan through MoltenVK,
`-DMACOS_BUNDLE=ON` for an .app). That build is not tested and RetroPixels
does not ship it.

## Checking a change

- `python3 tools/pixelu-original.py` (from the repository root) updates the
  table in `docs/pixelu-original.md`.
- To syntax-check a single file on Linux, run this from `pixelu/src`:
  `g++ -std=c++20 -fsyntax-only -include Common/precompiled.h -I. -ICafe -I../dependencies -I../dependencies/Vulkan-Headers/include -DENABLE_VULKAN -DVK_NO_PROTOTYPES <file>`.
  The Windows CI build is the real test.
