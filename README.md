<h1 align="center">RetroPixels</h1>

<p align="center">
  Classic consoles and arcade games for Windows, on emulators written from scratch.
</p>

<p align="center">
  <a href="../../releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/Shredder565/RetroPixels?label=release&sort=semver"></a>
  <img alt="Platform: Windows 10/11 x64" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="Go 1.24" src="https://img.shields.io/badge/go-1.24-00add8">
  <a href="../../actions/workflows/ci.yml"><img alt="Build" src="https://github.com/Shredder565/RetroPixels/actions/workflows/ci.yml/badge.svg"></a>
</p>

<p align="center">
  <a href="../../releases/latest"><b>Download</b></a> ·
  <a href="#features">Features</a> ·
  <a href="#supported-systems">Systems</a> ·
  <a href="#building-from-source">Build</a> ·
  <a href="#support-development">Donate</a> ·
  <a href="CHANGELOG.md">Changelog</a>
</p>

<p align="center">
  <img src="docs/images/playing.png" alt="RetroPixels playing a Genesis homebrew game" width="820">
</p>

RetroPixels is a single Windows program that plays games from eleven classic systems and a set of
arcade boards. Every emulator in it (the CPUs, graphics and sound chips, cartridge hardware and the
arcade boards) is RetroPixels' own code. It runs offline and needs no BIOS files for the built-in
systems. GameCube and Wii games play through [Dolphin](https://dolphin-emu.org/), shown inside the
RetroPixels window.

> RetroPixels does not include any games, ROMs, BIOS or firmware. Use copies of games you own.

## Features

- **One window for everything.** Drop a game file on the window; zipped games open directly and the
  system is detected from the file.
- **Xbox controllers work out of the box**, with per-system button remapping, rumble and analog
  sticks where the game uses them. Up to four players.
- **Player picker** for 4-player games: choose whether your controller plays as player 1, 2, 3 or 4.
- **Recording that captures only the game.** Videos contain just the game picture and sound, with
  no window borders, cursor, black bars or other apps.
- **Saves handled for you.** Battery saves are kept automatically; save states with undo; import
  `.sav` / `.srm` files from other emulators.
- **Cartridge hardware** emulated in software: SNES Super FX, SA-1, DSP-1/2, Cx4, S-DD1, SPC7110;
  about 120 NES mappers and the Famicom sound chips; Game Boy MBC1–7, HuC1/3, TAMA5; GBA sensor
  carts; Atari 2600 bank-switching including DPC+ and the Supercharger.
- **GameCube and Wii** through Dolphin, downloaded from inside the app, with a library, volume
  control and automatic recording.

## Supported systems

| System | Files | Notes |
| --- | --- | --- |
| NES / Famicom | `.nes` `.unf` | ~120 mappers, VRC6, VRC7, N163, 5B, MMC5 audio |
| Super Nintendo | `.sfc` `.smc` `.fig` `.swc` | Super FX, SA-1, DSP-1–4, Cx4, S-DD1, SPC7110, ST010/011/018 ¹ |
| Game Boy / Color | `.gb` `.gbc` | MBC1–7, HuC1/3, MMM01, TAMA5, real-time clock |
| Game Boy Advance | `.gba` | Solar, gyro, tilt and rumble cartridges |
| Nintendo 64 | `.z64` `.n64` `.v64` | Runs natively inside `RetroPixels.exe` |
| Sega Genesis / Mega Drive | `.md` `.gen` `.smd` `.bin` | EEPROM saves, J-Cart, Realtec, SSF2 |
| Sega Master System | `.sms` | |
| Atari 2600 | `.a26` `.bin` | Nearly all bank-switching schemes, paddles |
| Atari 7800 | `.a78` `.bin` | SuperGame, POKEY, banksets |
| Arcade | `.zip` ROM sets | Pac-Man, Galaxian, Space Invaders, Donkey Kong, TMNT, TMNT: Turtles in Time, The Simpsons |
| GameCube / Wii | `.iso` `.gcm` `.rvz` `.wbfs` `.ciso` `.gcz` `.wia` `.wad` | Through Dolphin |

¹ DSP-3, DSP-4, ST010, ST011 and ST018 run a program stored inside the chip. RetroPixels asks for
your own dump of it the first time and remembers it.

## Getting started

1. Download `RetroPixels-x.y.z.zip` from the [latest release](../../releases/latest) and unzip it.
2. Run `RetroPixels.exe`.
3. Drop a game onto the window, or click **Choose game file…**.

**Requirements:** Windows 10 or 11, 64-bit, with Microsoft Edge (built into Windows; RetroPixels
uses it to draw its window). For recording, RetroPixels downloads ffmpeg the first time it is needed.

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show *"Windows protected your PC"*.
> Click **More info**, then **Run anyway**.

<p align="center">
  <img src="docs/images/home.png" alt="The RetroPixels home screen" width="820">
</p>

## Building from source

RetroPixels is a Go program with an embedded web front end; no C toolchain is required.

```sh
git clone https://github.com/Shredder565/RetroPixels.git
cd RetroPixels
go test ./...
GOOS=windows GOARCH=amd64 CGO_ENABLED=0 \
  go build -trimpath -ldflags "-s -w -H windowsgui" -o RetroPixels.exe .
```

Requires Go 1.24 or newer. When the version in `main.go` changes on `main` (or a tag such as `v2.7.0` is pushed), GitHub Actions builds the program and publishes the release with the .exe, the zip and the source code.

### Project layout

| Path | What it is |
| --- | --- |
| `main.go`, `*.go` | Windows app: app window, Dolphin integration, recording, settings |
| `web/index.html` | The app's interface |
| `web/emu/` | The emulators: CPUs (6502, 65C816, Z80, 68000, ARM, Konami-2, 8080…), video and sound chips, arcade boards |
| `n64/` | Nintendo 64 emulator (CPU, RSP, RDP), run natively |
| `sevenzip/` | `.7z` extractor (LZMA, LZMA2, BCJ, BCJ2) |
| `docs/` | Project website (GitHub Pages) |

## Support development

RetroPixels is free to download and built by one developer. If you enjoy it, a donation helps fund
new systems, more arcade boards, accuracy work and code signing (so Windows stops warning about the
download).

<p>
  <a href="https://github.com/sponsors/Shredder565"><img alt="Sponsor on GitHub" src="https://img.shields.io/badge/Sponsor-%E2%9D%A4-db61a2?logo=githubsponsors&logoColor=white&style=for-the-badge"></a>
</p>

You can also use the **Sponsor** button at the top of this repository. Thank you!

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for every release.

## Legal

RetroPixels contains no copyrighted game code, BIOS or firmware. Its emulators are written from
scratch. Game and system names are trademarks of their respective owners and are used only to
describe compatibility. RetroPixels is not affiliated with or endorsed by Nintendo, Sega, Atari,
Konami, Namco, Microsoft or the Dolphin project.

Screenshot: *Shattering Jaws*, a Genesis homebrew game by Mun (2016).

Copyright © 2026 RetroPixels. All rights reserved.
