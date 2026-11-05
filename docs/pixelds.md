# PixelDS

PixelDS is RetroPixels' own Nintendo DS emulator, and the start of one
emulator for both the DS and the 3DS. It is new code written for RetroPixels:
no code from melonDS, Azahar, DeSmuME or any other emulator is in it. Public
hardware documentation (GBATEK and the ARM Architecture Reference Manual) and
the behaviour of other emulators were used as references only.

## Where things are

- `cores/pixelds/arm`: the ARM interpreter (ARMv4T for the ARM7, ARMv5TE for
  the ARM9, and the start of ARMv6K for the 3DS's ARM11).
  `arm946_test.go` runs 20,000 instruction cases generated with
  `tools/pixelds/armcases.py` against Unicorn's ARM946 model.
- `cores/pixelds/nds`: the DS itself: memory and VRAM banks, timers, DMA,
  IPC, interrupts, the cartridge and its save chip, SPI (firmware, touch
  screen, power), the clock, both 2D engines, the sound chip, a built-in BIOS
  (`bios.go`, plus small ARM stubs from `tools/pixelds/biosstub.py`) and save
  states. `gpu3d.go` is a placeholder until the 3D engine lands.
- `cores/pixelds/pixelds.go`: the front shared by DS and (later) 3DS games.
- `cmd/pixelds`: PixelDS.exe, the window, keyboard, controller and sound.
- `tools/pixelds/selftest`: a small homebrew DS program (our own code, built
  with `gcc-arm-none-eabi`) that the tests boot to check the CPU, the BIOS,
  the IPC, the timers, DMA, the sound and the screens.

## Command line

    PixelDS.exe [-f] game.nds              play (fullscreen with -f)
    PixelDS.exe -w -frames 600 -shot a.png game.nds   run without a window

Saves are `game.sav` next to the game (the same file melonDS uses), or under
`%APPDATA%\RetroPixels\PixelDS\saves` when that folder can't be written.
Save states are in `%APPDATA%\RetroPixels\PixelDS\states`.

## Plan

1. 2.29.0: DS games with 2D graphics, sound, touch, saves (done).
2. DS 3D graphics: the geometry engine and the rasterizer, then display
   capture.
3. 3DS games in the same emulator: the ARM11, the 3DS system software
   (high-level), and the PICA200 GPU. Then Azahar leaves the download.
