RetroPixels
============

Double-click RetroPixels.exe to start. It opens in its own window.

Used RetroConsole before? Your settings, save states, recordings and save
backups move over to RetroPixels automatically the first time it starts.
Put RetroPixels.exe in the same folder as the old RetroConsole.exe so it can
reuse the "ffmpeg" folder there (otherwise it will offer to download it again).

Systems: NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64,
Sega Genesis, Atari 2600, Atari 7800, plus GameCube and Wii through Dolphin.
Your Xbox controller works automatically.

First run
- Windows may show "Windows protected your PC" because this program isn't
  signed by a publisher. Click "More info", then "Run anyway".
- NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64,
  Genesis, Atari 2600 and Atari 7800 run on RetroPixels' own emulators, built
  into the program, so they work offline. No BIOS files are needed.
- Nintendo 64 runs inside RetroPixels.exe itself (it's too heavy for the
  page), so N64 games need the desktop app. Its 3D graphics are still being
  finished, so many games don't show their picture yet.
- Super Nintendo games with extra chips in the cartridge (DSP-1: Super Mario
  Kart, Pilotwings; Super FX: Star Fox, Yoshi's Island; SA-1: Super Mario RPG,
  Kirby Super Star; and a few others) aren't supported yet and show a message
  saying so.

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
