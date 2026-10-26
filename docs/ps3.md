# PlayStation 3 in RetroPixels (RPCS3)

RetroPixels plays PlayStation 3 games in the player's own RPCS3
(https://rpcs3.net, GPL-2.0), the same way it plays arcade games in the
player's own MAME: RetroPixels starts RPCS3, it doesn't include or change
it. `internal/rpcs3` has everything both programs share; RetroPixels.exe's
PS3 tab is `rpcs3.go` + `web/index.html`, RetroPixels Native's PS3 menu is
`cmd/retropixels-native/ps3_windows.go`. Settings are kept in
`%APPDATA%\RetroPixels\rpcs3.json` for both.

RetroPixels never downloads, includes or makes PS3 system software, games,
keys or licences. Those are the player's own files, installed with RPCS3's
own installers or copied into RPCS3's folder as RPCS3's own UI does.

## Finding RPCS3

In this order (`rpcs3.FindExe`, then `rpcs3.Search`):

1. The folder chosen with **Choose RPCS3 folder…** (it overrides the rest:
   if `rpcs3.exe` isn't in it, RPCS3 isn't used from anywhere else).
2. The RPCS3 found before (remembered).
3. `E:\Roms All Systems\008 - PS3\- PS3\rpcs3-v0.0.21-13400-e53bbd66_win64`,
   then `D:\Roms All Systems\PS3\- PS3\rpcs3-v0.0.21-13400-e53bbd66_win64`.
4. An `RPCS3` folder next to RetroPixels.
5. A search of Downloads, Desktop, Documents and each drive, up to five
   folders down, only into folders whose names look like games or emulators
   (rom, game, emu, ps3, rpcs3, playstation, retro…).

## Games

- Installed games: each `dev_hdd0\game\<folder>` with a `PARAM.SFO` and
  `USRDIR\EBOOT.BIN`. The SFO gives the title, title ID, version and
  category; only bootable categories are listed (HG installed game, DG disc
  game, 1P, 2P, MN, PE). Game data and updates (GD) are left out.
- Disc games: the folders in RPCS3's `games.yml` (`config\games.yml`, or
  next to the program in older builds), each with `PS3_GAME\PARAM.SFO`.

## What a game needs

- **The PS3 system software.** RPCS3 needs `PS3UPDAT.PUP` installed
  (`dev_flash\vsh\module\vsh.self` is there afterwards). Without it the tab
  says so and **Install PS3 system software…** runs
  `rpcs3.exe --installfw <file>`.
- **The game.** **Install game (.pkg)…** runs `rpcs3.exe --installpkg <file>`.
- **A licence, for PSN games.** The EBOOT.BIN's NPDRM block names the
  game's content ID and licence kind. Kinds 1 and 2 need
  `<content ID>.rap` or `.edat` in `dev_hdd0\home\<user>\exdata`.
  **Add licence file…** copies the player's own file there (for user
  00000001), unchanged, after checking it is named after a content ID and
  looks like a .rap (16 bytes) or .edat (starts with `NPD`).

## Starting a game

`rpcs3.exe --no-gui [--fullscreen] <...\USRDIR\EBOOT.BIN>`, run from
RPCS3's folder with no console window. `--fullscreen` only when the game
isn't docked. Docked ("Play inside RetroPixels"), RPCS3's game window is
kept over the page the way PixelU's is. A game that closes within 20
seconds is explained from what RPCS3 printed and the `·F` / `·E` lines of
`RPCS3.log`.

RPCS3 command-line options relied on: `--no-gui`, `--fullscreen` (with
`--no-gui`), `--installpkg`, `--installfw`. All are in RPCS3 0.0.21 builds
(early 2022) and later. If an older build can't install from RetroPixels,
RPCS3's own File > Install Packages/Raps/Edats and File > Install Firmware
do the same.

## Controller

When RPCS3 has no controller settings yet (`config\input_configs\global\Default.yml`),
RetroPixels writes player 1 on the first Xbox controller (XInput handler,
PS3 buttons where they are on the Xbox pad, rumble on). If the player has
set player 1 up themselves, it's left alone; **Use the Xbox controller for
player 1** replaces only player 1's part and keeps the old file as
`Default.yml.bak`.

## Recording

PS3 games aren't recorded. Every engine RetroPixels records saves its own
picture and sound (Dolphin's frame dump, PixelU, MAME's AVI, RetroPixels'
Supermodel); RPCS3 0.0.21 has nothing like that. OBS Studio can record
RPCS3's window.

## Not verified here

RPCS3 can't run in RetroPixels' test machines: the tests use a stand-in
`rpcs3` script and made-up game folders, SFO files and EBOOT headers.
