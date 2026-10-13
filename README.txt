RetroPixels
============

Double-click RetroPixels.exe to start. It opens in its own window.

Used RetroConsole before? Your settings, save states, recordings and save
backups move over to RetroPixels automatically the first time it starts.
Put RetroPixels.exe in the same folder as the old RetroConsole.exe so it can
reuse the "ffmpeg" folder there (otherwise it will offer to download it again).

Version 2.2.0 (the number also shows next to the RetroPixels name in the app).

Systems: NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64,
Sega Genesis, Sega Master System, Atari 2600, Atari 7800, plus GameCube and
Wii through Dolphin.
Your Xbox controller works automatically.

First run
- Windows may show "Windows protected your PC" because this program isn't
  signed by a publisher. Click "More info", then "Run anyway".
- NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64,
  Genesis, Master System, Atari 2600 and Atari 7800 run on RetroPixels' own emulators, built
  into the program, so they work offline. No BIOS files are needed.
- Nintendo 64 runs inside RetroPixels.exe itself (it's too heavy for the
  page), so N64 games need the desktop app.

What each system supports
- NES: about 120 cartridge types (mappers), covering nearly all licensed
  Nintendo, Konami, Namco, Bandai, Jaleco, Irem, Taito and Sunsoft games plus
  common multicarts and unlicensed boards. Famicom games with extra sound chips
  play their full soundtrack (Konami VRC6 and VRC7, Namco 163, Sunsoft 5B,
  MMC5). NES 2.0 and UNIF (.unf) files work. Famicom Disk System (.fds) games
  can't be played: the Disk System needs Nintendo's own BIOS.
- Super Nintendo: games with chips in the cartridge work, recreated by
  RetroPixels' own code (no chip firmware needed): Super FX (Star Fox, Stunt
  Race FX, Yoshi's Island, Doom, Vortex, Winter Gold, Dirt Trax FX), SA-1
  (Super Mario RPG, Kirby Super Star, Kirby's Dream Land 3, PGA Tour 96 and
  others), DSP-1 (Super Mario Kart, Pilotwings and others), DSP-2 (Dungeon
  Master), Cx4 (Mega Man X2 / X3), S-DD1 (Star Ocean, Street Fighter Alpha 2),
  SPC7110 (Far East of Eden Zero, Momotarou Dentetsu Happy, Super Power League
  4, with its clock), OBC1 (Metal Combat) and S-RTC (Daikaijuu Monogatari II).
  Games with the DSP-3, DSP-4, ST010, ST011 or ST018 chips aren't supported
  yet and show a message saying so.
- Game Boy / Color: MBC1 (including multicarts), MBC2, MBC3 with clock, MBC30,
  MBC5 with rumble (your controller vibrates), MBC6, MBC7 (Kirby Tilt 'n'
  Tumble: tilt with the right stick or I / J / K / L), HuC1, HuC3, MMM01, TAMA5
  and the Pocket Camera (shows a test picture). Clocks keep time while the
  game is closed.
- Game Boy Advance: sensor cartridges work. Boktai's sunlight sensor: More Sun
  / Less Sun (I / K), or push the right stick up. WarioWare: Twisted!: turn with
  the right stick or J / L. Yoshi Topsy-Turvy and Koro Koro Puzzle: tilt with
  the right stick or I / J / K / L. Drill Dozer and WarioWare rumble through
  your controller.
- Genesis / Mega Drive: battery and EEPROM saves, Realtec and SSF2 mappers,
  J-Cart 4-player games, and an automatic 3-button pad for games that need one
  (or hold Mode while the game starts). Virtua Racing (SVP chip) isn't
  supported.
- Master System / Mark III (new): .sms files (NTSC). 1 = Z, 2 = X,
  Pause = Enter, Reset = R.
- Atari 2600: nearly every bank-switching scheme, including Superchip RAM,
  3E / 3E+, DPC (Pitfall II), DPC+ (including the game's ARM code) and the
  Starpath Supercharger (no BIOS needed). The scheme is detected automatically;
  you can force one by putting it in the file name, e.g. "Game (F8SC).bin".
  CDF / CDFJ and 4A50 cartridges aren't supported yet.
- Atari 7800: .a78 headers (v3 and v4), SuperGame with extra RAM, Absolute,
  Activision, banksets and POKEY sound.

Playing classic games
- Drop a game file on the window or click "Choose game file…". Zipped games
  open directly. A .bin file is guessed (Atari 2600, Atari 7800 or Genesis);
  click the system first if it's guessed wrong.
- The bar at the bottom of the game (move the mouse over it) has Pause,
  Restart, Fast-forward (or hold Space), volume, 🎮 Controls to change buttons,
  and 📷 screenshot.
- Atari 2600 paddle games (Kaboom!, Warlords, Super Breakout…) are detected
  automatically: steer with the left stick, fire with A.
- Battery saves (Pokémon, Zelda…) are kept automatically. Under Save states
  you can import a .sav / .srm from another emulator or download yours.
- Save states made with an older RetroPixels (or with the previous Super
  Nintendo / Game Boy Advance / Genesis engine) can't be loaded by the new
  emulators; in-game saves can be brought over with "Import battery save…"
  (.srm / .sav files from other emulators work too).
- Nintendo 64: the left stick is the analog stick and the right stick works
  the C buttons; Xbox A / X = A / B, LT = Z, LB / RB = L / R, Menu = Start.
  On the keyboard: arrows = stick, X / Z = A / B, A = Z, IJKL = C buttons,
  TFGH = D-pad. Since the right stick is busy, change save slot with View +
  D-pad left / right and start or stop recording with View + D-pad down.
- Atari 7800 light-gun games (Alien Brigade, Barnyard Blaster, Sentinel) start,
  but can't be aimed yet.

GameCube & Wii (Dolphin)
- No Dolphin yet? Click "Download Dolphin" in the GameCube tab: RetroPixels
  downloads the latest official version from dolphin-emu.org into a "Dolphin"
  folder next to RetroPixels.exe, and uses it from then on.
- The "GameCube" and "Wii" tabs (the Wii tab includes WiiWare) find Dolphin
  and your games automatically (it reads each disc, so PC and PS2 .iso files are skipped).
- Click a game (or D-pad + A) to play it. It plays right inside the RetroPixels
  window (no separate Dolphin window), and with "Start games fullscreen" ticked
  the RetroPixels window goes fullscreen. Use "Stop game" in the bar at the top
  to come back to your library. Untick "Play inside RetroPixels" to have Dolphin
  open in its own window as before. The first time,
  your Xbox controller is set up as the GameCube controller and as a Wii Remote
  + Nunchuk (Wii pointer = right stick, shake = RB). Your own Dolphin controller
  settings are never replaced unless you click "Reset to Xbox layout".
- Dolphin's main graphics settings are on the right of both tabs (they're shared).

Recording games
- GameCube and Wii games are recorded automatically as .mp4 (turn it off in
  the Recordings tab, or untick "Record games automatically").
- Only the game is recorded: in fullscreen the black bars are cropped off, and
  only Dolphin's own sound is captured (not Discord, music, etc.). With Aspect
  ratio on "Auto", a 4:3 game's black side bars are trimmed off right after
  the recording ends.
- While playing, the bar at the top shows REC and a Stop / Record button.
- Videos are saved to Videos\RetroPixels. If something goes wrong, details
  are in Videos\RetroPixels\logs\last-recording.log.
- The first time, RetroPixels asks to download ffmpeg (a free video encoder,
  about 190 MB, from github.com/BtbN/FFmpeg-Builds). Click "Download ffmpeg";
  it's checked against its published checksum and saved in an "ffmpeg" folder
  next to RetroPixels.exe. It uses your NVIDIA / AMD / Intel graphics card for
  encoding when it can. ffmpeg is GPL-licensed; its licence is saved with it.
- Classic-console games: press ● Record above the game (or hold the right
  stick down for a second). Those videos go to your Downloads folder.

Where things are kept
- Save states and battery saves: inside RetroPixels's own browser profile
                 (%LOCALAPPDATA%\RetroPixels\BrowserProfile)
- Settings:      %APPDATA%\RetroPixels\config.json
- Recordings:    Videos\RetroPixels (GameCube / Wii), Downloads (classic)


Credits & licences
- RetroPixels' own code was written for this project and uses no copied code:
  the program, its screens, and its NES, Super Nintendo, Game Boy / Game Boy
  Color, Game Boy Advance, Nintendo 64, Sega Genesis, Atari 2600 and Atari
  7800 emulators - including the 6502, 65C816, SPC700, Game Boy, ARM7TDMI,
  68000, Z80 and N64 (VR4300 and RSP) processors, the graphics and sound
  chips (S-DSP, YM2612, PSG...), the cartridge hardware, and replacements
  for the Game Boy Advance BIOS, the SNES sound start-up program and the N64
  boot program, so no Nintendo or Sega code is used. It uses only Go's
  standard library.
- It works together with separate programs that keep their own licences and
  are not part of RetroPixels:
  * ffmpeg (GPL): video encoding for recordings; downloaded on request from
    github.com/BtbN/FFmpeg-Builds. Its licence is saved next to it.
  * Dolphin (GPL-2.0+): your own install, or the official release RetroPixels
    downloads for you from dolphin-emu.org (unchanged; a note with its licence
    and where to get its source is saved in its folder).
    Launched by RetroPixels and shown inside its window. Dolphin's source code:
    https://github.com/dolphin-emu/dolphin
- Nintendo, GameCube, Wii, Xbox, Sega, Genesis and Atari are trademarks of
  their owners, named only to describe what the program works with.
  RetroPixels isn't connected with or endorsed by any of them.
