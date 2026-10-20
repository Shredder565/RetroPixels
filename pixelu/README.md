# PixelU

PixelU is the Wii U emulator inside RetroPixels. RetroPixels starts it as
`PixelU\PixelU.exe` when you open a Wii U game.

PixelU began as a copy of Cemu (MPL-2.0). Since then the C and C++ code under
`src/` has been rewritten as RetroPixels' own code, file by file, apart from
the bundled third-party libraries and data listed in
[THIRD_PARTY.md](THIRD_PARTY.md). [docs/pixelu-original.md](../docs/pixelu-original.md)
records how that was done and what was checked at each step. `wii/` holds
PixelU-Wii, a separate program built from Dolphin with a few patches.

PixelU comes with no Nintendo keys, firmware or games. You need dumps of your
own.

- [BUILD.md](BUILD.md): how to build it
- [CONTRIBUTING.md](CONTRIBUTING.md): how to change it
- [CODING_STYLE.md](CODING_STYLE.md): the code style
- [LICENSE.txt](LICENSE.txt): the licence file the project carries for now
