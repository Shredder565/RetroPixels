# PlayStation 3 in RetroPixels (RPCS3)

RetroPixels plays PlayStation 3 games in RPCS3 (https://rpcs3.net,
GPL-2.0): the player's own, or the official build that comes in the
Windows download (`RPCS3\`, below). RetroPixels starts RPCS3 as a separate
program, the way it starts MAME; it never links with or changes it. `internal/rpcs3` has everything both programs share; RetroPixels.exe's
PS3 tab is `rpcs3.go` + `web/index.html`, RetroPixels Native's PS3 menu is
`cmd/retropixels-native/ps3_windows.go`. Settings are kept in
`%APPDATA%\RetroPixels\rpcs3.json` for both.

RetroPixels never downloads, includes or makes PS3 system software
(firmware), games, keys or licences, and neither does its bundled RPCS3. Those are the player's own files, installed with RPCS3's
own installers or copied into RPCS3's folder as RPCS3's own UI does.

## Finding RPCS3

In this order (`rpcs3.WhichExe` / `FindExe`, and `rpcs3.Search`):

1. The folder chosen with **Choose RPCS3 folder…** (it overrides the rest:
   if `rpcs3.exe` isn't in it, RPCS3 isn't used from anywhere else).
2. The player's own RPCS3: the one found on the drives before
   (remembered); `E:\Roms All Systems\008 - PS3\- PS3\rpcs3-v0.0.21-13400-e53bbd66_win64`,
   then `D:\Roms All Systems\PS3\- PS3\rpcs3-v0.0.21-13400-e53bbd66_win64`;
   an `RPCS3` folder of their own next to RetroPixels (one without
   `RETROPIXELS-BUNDLED.txt`).
3. The bundled RPCS3, run from `%APPDATA%\RetroPixels\RPCS3` (below).

When nothing is found, or only the bundled RPCS3, the drives are searched
once (Downloads, Desktop, Documents and each drive, up to five folders
down, only into folders whose names look like games or emulators: rom,
game, emu, ps3, rpcs3, playstation, retro…; `rpcs3.json`'s `detected`
remembers it was done) and an RPCS3 found there is remembered and used
from then on. The search never picks a folder with `RETROPIXELS-BUNDLED.txt`
(a RetroPixels download's own RPCS3). The PS3 tab ("Emulator") and Native's
PS3 > Play menu say which RPCS3 is in use (`Status.Source`: chosen, yours,
bundled; `Status.Version` for the bundled build).

## The bundled RPCS3

`bundle-test.yml` (the Windows download, test and release) downloads one
official RPCS3 Windows build from the RPCS3 team's own releases
(`RPCS3/rpcs3-binaries-win`), pinned by release tag and checked against its
SHA-256 (`RPCS3_*` in the workflow), and unpacks it unchanged into
`RPCS3\` next to `Supermodel\`, with RPCS3's `LICENSE.txt` (GPL-2.0, from
the same RPCS3 commit, also checked by SHA-256), `SOURCE.txt` (the matching
commit of github.com/RPCS3/rpcs3) and `RETROPIXELS-BUNDLED.txt` (which
build; it marks the folder as RetroPixels' copy). The workflow checks that
no `dev_hdd0`, `dev_flash` or `dev_usb000` is in it. To move to a newer
build, change the tag, file name and both hashes there.

A download folder is replaced on every update, and RPCS3 keeps the system
software, installed games, saves, licences and settings inside its own
folder, so the bundled copy is never run in place (`internal/rpcs3/bundled.go`):

- On first use it is copied to `%APPDATA%\RetroPixels\RPCS3` (next to
  RetroPixels' Supermodel folder; `RETRO_RPCS3_HOME` overrides it for
  tests) and run from there.
- When a newer RetroPixels brings a different build (its
  `RETROPIXELS-BUNDLED.txt` differs), only RPCS3's program files are
  replaced: `rpcs3.exe`, the DLLs, `qt6\`, themes, icons, fonts, test
  programs. Anything under `dev_*` (dev_hdd0, dev_flash, dev_usb000…),
  `config\`, `cache\`, `patches\`, `savestates\` and other data folders, and
  any `.yml`, `.ini`, `.log` or `.json` file anywhere (`games.yml`,
  `GuiConfigs\CurrentSettings.ini`…) is never replaced or deleted
  (`rpcs3.Protected`); it is only put there when missing. Program files of
  the old build that the new one doesn't have are removed only if
  RetroPixels put them there (they are listed in `RETROPIXELS-FILES.txt`).
  The marker is written last, so a copy that was cut off is finished
  next time. If RPCS3 is open and its files can't be replaced, the copy
  already there is used.
- Choosing the download's own `RPCS3` folder with **Choose RPCS3 folder…**
  also runs the lasting copy, not the download's.

The Linux download doesn't include RPCS3 (its AppImage could be added to
`tools/linux/package.sh` later); Linux players use their own.

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
`rpcs3` script and made-up game folders, SFO files and EBOOT headers. The
copy and update of the bundled RPCS3 are tested with temporary folders
(`internal/rpcs3/bundled_test.go`: the player's dev_hdd0, dev_flash,
dev_usb000, config, games.yml and other files survive an update); that the
pinned build starts and plays on Windows is only checked by hand.
