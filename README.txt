RetroPixels
============

Double-click RetroPixels.exe to start. It opens in its own window.

Used RetroConsole before? Your settings, save states, recordings and save
backups move over to RetroPixels automatically the first time it starts.
Put RetroPixels.exe in the same folder as the old RetroConsole.exe so it can
reuse the "ffmpeg" folder there (otherwise it will offer to download it again).

Version 2.13.0 (the number also shows next to the RetroPixels name in the app).

Systems: NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64,
Sega Genesis, Sega Master System, Atari 2600, Atari 7800, classic arcade
games (Pac-Man, Galaxian, Space Invaders, Donkey Kong, TMNT, TMNT: Turtles in
Time, The Simpsons), plus GameCube and
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
  DSP-3 (SD Gundam GX), DSP-4 (Top Gear 3000), ST010 (F1 ROC II), ST011
  (Hayazashi Nidan Morita Shougi) and ST018 (Morita Shougi 2) run on
  RetroPixels' own processor emulation, but these chips run a program stored
  inside the chip, which can't come with RetroPixels. The first time you
  start such a game, RetroPixels asks for your own dump of it (dsp3.rom /
  dsp4.rom 8 KB, st010.rom / st011.rom 52 KB, st018.rom 160 KB) and keeps it,
  so you're only asked once. A game file with the chip program attached at
  its end works with no question. F1 ROC II's records are its battery save.
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
- Arcade (new in 2.4.0): RetroPixels' own emulation of four classic arcade
  boards. Pick Arcade (or just drop the file) and load the game's .zip ROM
  set, kept zipped and named like the set:
  * Puck Man / Pac-Man (puckman.zip, pacman.zip), with its wavetable sound.
  * Galaxian (galaxian.zip, galmidw.zip).
  * Space Invaders (invaders.zip), on RetroPixels' own Intel 8080 processor,
    with the cabinet's coloured screen strips (can be switched off).
  * Donkey Kong (dkong.zip), whose sound program runs on RetroPixels' own
    8035 sound processor.
  Some sounds on the real machines come from analogue circuits (Galaxian's
  sounds, Space Invaders' shots and marching beat, Donkey Kong's walk, jump
  and stomp); RetroPixels makes its own versions of those. Vertical games are
  shown upright. Coin = View button or 5, Start = Menu button or 1 (2 for two
  players), Fire / Jump = A or Z. The machine's DIP switches (coins, lives,
  bonus, difficulty) are under "Machine settings" below the game; press
  Restart after changing one. A set with missing chip files gets a list of
  what's missing; a "split" clone set needs a full (non-merged) set instead.
  RetroPixels includes no game ROMs; use sets you own.
- Arcade, Konami (new in 2.5.0), up to four players, one per controller:
  * Teenage Mutant Ninja Turtles (tmnt.zip; also tmntu, tmntua, tmht, tmntj,
    tmht2p, tmnt2pj).
  * Teenage Mutant Ninja Turtles - Turtles in Time (tmnt2.zip; also tmnt2a,
    tmnt22pu, tmht22pe, tmht24pe, tmnt22pj, tmnt24pj).
  * The Simpsons (simpsons.zip; also simpsons4pe, simpsons2p, simpsons2pa,
    simpsons2pj).
  These run on RetroPixels' own 68000, Konami-2 and Z80 processors and its own
  versions of Konami's graphics and sound chips (YM2151, K007232, uPD7759,
  K053260...). If your set names its chip files differently, keep the zip
  named after the game (for example tmnt.zip) and RetroPixels matches the
  chips by name and size. Turtles in Time and The Simpsons keep their settings
  and high scores in the board's EEPROM (saved like a battery save) and have
  no DIP switches: hold Service + Start on the first controller to open the
  game's own test menu. In TMNT (4-player cabinet) a player joins by pressing
  Attack; Start does the same.
  4-player arcade games show "Choose your player" buttons above the game
  (1 Leonardo, 2 Michelangelo, 3 Donatello, 4 Raphael; or 1 Marge, 2 Homer,
  3 Bart, 4 Lisa): click one to play as that character with your keyboard
  and first controller. Other controllers move round with it. The choice is
  remembered for each game. Nintendo 64 and Genesis games have the same
  choice as a "👤 Player 1" button in the bar under the game.

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

Archipelago (multiworld randomizers, new)
- RetroPixels works with Archipelago (archipelago.gg). It takes the place of
  the emulator and its connector: the SNI Client and the BizHawk Client from
  the Archipelago Launcher talk to RetroPixels directly, so you don't need
  SNI, BizHawk or a Lua script.
- 1. Open your patch file (for example AP_..._P1_Name.apsm, .aplttp, .apred,
     .apemerald) like any game: drop it on the window or click "Choose game
     file...".
  2. The first time, RetroPixels asks for the original ROM that the game's
     Archipelago setup guide names (a zipped ROM works too). It checks that
     it's the right one and keeps it, so the next patch for that game opens
     straight away. The patched game has its own saves, separate from the
     game without the patch.
  3. In the Archipelago Launcher start the "SNI Client" (Super Nintendo
     games) or the "BizHawk Client" (Game Boy / Color, Game Boy Advance, NES,
     Nintendo 64) and connect it to the server (for example
     /connect archipelago.gg:38281).
  4. Play. The "Archipelago" button in the bar under the game shows whether
     the client is connected; click it to see these steps.
- Already made the ROM with the Archipelago Launcher? Open the patched ROM
  instead; that works too.
- Some games need extra steps from Archipelago's own code to build their ROM
  (Kirby's Dream Land 3, Castlevania 64, Castlevania: Circle of the Moon,
  EarthBound, Mario & Luigi: Superstar Saga, Link's Awakening DX, SMZ3,
  Gauntlet Legends). RetroPixels tells you when a patch is one of them: make
  the ROM with "Open Patch" in the Archipelago Launcher, then open that ROM.
  Games that use their own connector script (The Legend of Zelda, Link's
  Awakening DX, Ocarina of Time...) aren't supported.
- RetroPixels only answers clients on this computer (127.0.0.1: port 23074
  for SNI, 43055-43059 for BizHawk) and never connects to the Archipelago
  server itself.

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
- GameCube and Wii games are recorded automatically as .mp4, from the start
  of the game to the end (turn it off in the Recordings tab, or untick
  "Record games automatically").
- Dolphin itself saves the game's picture and sound while you play, and
  RetroPixels makes the .mp4 from them when the game ends. So the video shows
  only the game - no mouse pointer, no other windows, no black bars - with
  only the game's sound (not Discord, music, etc.), and it plays back smoothly
  even if the game slowed down for a moment.
- Making the video takes a little while after a long game; "Saving the
  recording..." shows meanwhile. Keep RetroPixels open until it's done.
  Dolphin's raw files (in its Dump folder) are big - about 15 GB an hour -
  and are deleted once the .mp4 is saved, so keep some free disk space.
- Stop recording in the bar at the top stops the video at that point; Record
  starts a new video from then on. Dolphin can only start saving a game as it
  starts, so with automatic recording off the button says "Record next game".
- While recording, the bar at the top shows REC.
- Videos are saved to Videos\RetroPixels. If something goes wrong, details
  are in Videos\RetroPixels\logs\last-recording.log. Recording needs
  Dolphin 5.0 or newer.
- The first time, RetroPixels asks to download ffmpeg (a free video encoder,
  about 190 MB, from github.com/BtbN/FFmpeg-Builds). Click "Download ffmpeg"
  (if you don't, it's downloaded when a recorded game ends);
  it's checked against its published checksum and saved in an "ffmpeg" folder
  next to RetroPixels.exe. It uses your NVIDIA / AMD / Intel graphics card for
  encoding when it can. ffmpeg is GPL-licensed; its licence is saved with it.
- Classic-console and arcade games: press ● Record above the game (or hold
  the right stick down for a second). The video holds only the game's
  picture at its own shape (no black bars, buttons or messages) and only the
  game's sound. Those videos go to your Downloads folder.

RetroPixels Native (test build)
RetroPixels-Native.exe is a new version of RetroPixels being rebuilt without
a web browser inside, so OBS Studio's Window Capture works with any capture
method (or use Display Capture). This test build plays NES, Atari 2600,
Master System, Game Boy / Game Boy Color, Atari 7800, Sega Genesis / Mega
Drive, Game Boy Advance and Nintendo 64 games, and the arcade games (Pac-Man,
Galaxian, Space Invaders, Donkey Kong, TMNT, TMNT: Turtles in Time and The
Simpsons), and GameCube / Wii games through Dolphin; Super Nintendo will
follow in a later test build.
Keep using RetroPixels.exe for Super Nintendo for now. The two programs keep separate settings
and saves, so trying the test build changes nothing in RetroPixels.exe.
- Starting a game: drag the game file onto the window, or File > Open game...
  (Ctrl+O). .nes, .a26, .sms, .gb, .gbc, .a78, .md, .gen, .smd, .gba, .agb,
  .z64, .n64, .v64 and .bin files work, also inside .zip and .7z files.
  Arcade games: open the game's .zip ROM set and keep it zipped (a .7z works
  too); the game is found from the names of the files inside, so the zip can
  have any name. If chip files are missing or the wrong size, it says which;
  clone sets need a full (non-merged) set. Upright-cabinet games are shown
  upright.
  File > Recent games lists the last ten.
- Keys (the same as in RetroPixels.exe): arrow keys move. NES: X = A, Z = B,
  Enter = Start, Shift = Select. Atari 2600: Z, X or Space = Fire, Enter =
  Reset, Shift = Select, 1 / 2 = left / right difficulty switch, 3 = Colour /
  B&W switch. Master System: Z = button 1, X = button 2, Enter = Pause,
  R = console Reset. Game Boy / Color: X = A, Z = B, Enter = Start, Shift =
  Select. Atari 7800: Z = button 1, X = button 2 (one-button games take
  either), Enter = Pause, Shift = Select, R = Reset, 1 / 2 = difficulty
  switches. Genesis: A = A, S = B, D = C, Q / W / E = X / Y / Z, Enter =
  Start, Shift = Mode. Game Boy Advance: X = A, Z = B, Q / W = L / R, Enter =
  Start, Shift = Select. Nintendo 64: X = A, Z = B, A = Z, Enter = Start,
  Q / W = L / R, arrow keys = analog stick (pushed all the way), T F G H =
  D-pad, I J K L = C buttons. Change them in Options > Controls... (one set
  per system).
- Xbox controllers work automatically, up to four (one per player). Xbox A /
  B are the NES's B / A, as on the original pad; the left stick moves too.
  Atari paddle games: turn the paddle with the left stick, or tick Options >
  "Mouse turns the paddle" and move the mouse across the picture (the left
  mouse button is Fire).
- P pauses, M mutes, hold Tab to fast-forward, Ctrl+R resets, F11 or
  Alt+Enter switches fullscreen (Esc leaves it). Window size (1x to 5x),
  volume and smooth or sharp pixels are in the Options menu.
- Game Boy: games with a clock keep counting real time between sessions;
  rumble cartridges shake the Xbox controller; tilt cartridges (Kirby Tilt 'n'
  Tumble, Command Master) tilt with the right stick or the I J K L keys.
- Genesis: pads are 6-button; if an older game misbehaves, hold Mode while it
  starts to use a 3-button pad (known games switch automatically). J-Cart
  games: controllers 3 and 4 play as players 3 and 4. European games run at
  50 fps, as on the console. Virtua Racing isn't supported (as in
  RetroPixels.exe). Genesis save states don't carry over from RetroPixels.exe.
- Game Boy Advance: no BIOS file is needed. Games with a clock read your
  computer's clock. Boktai (sunlight): I / K or the right stick set the sun.
  WarioWare: Twisted! (gyro): right stick or J / L, and it rumbles. Yoshi
  Topsy-Turvy and Koro Koro Puzzle (tilt): right stick or I J K L. Drill Dozer
  rumbles.
- Nintendo 64: on an Xbox controller A = A, X = B, LT = Z, LB / RB = L / R,
  left stick = analog stick, right stick = C buttons; up to 4 controllers.
  Because the right stick is the C buttons, recording from the controller is
  View + D-pad down. Battery saves use the same .sav format as
  RetroPixels.exe (plain .eep / .sra / .fla / .mpk files work too); save
  states made in RetroPixels.exe load here. No Rumble Pak (as in
  RetroPixels.exe).
- Arcade keys: Coin = 5 or C (controller: View), Start = 1 or Enter
  (controller: Menu), Start 2P = 2, Coin 2 = 6, Service = 9, Button 1 /
  Attack = Z (controller A), Button 2 / Jump = X (B), Button 3 = A (X),
  Button 4 = S (Y); arrows or the left stick move.
- Arcade Player menu: in TMNT, Turtles in Time and The Simpsons, Game > Player
  picks who you play: Player 1 (Leonardo) to Player 4 (Raphael), or Marge,
  Homer, Bart and Lisa. The keyboard and first controller play as that player
  and the other controllers move round with it. It is remembered per game.
- Arcade machine settings: Game > Machine settings changes the board's DIP
  switches (coins, lives, difficulty and so on). They are kept per game; press
  Reset (Ctrl+R) after changing one. Turtles in Time and The Simpsons keep
  their settings and high scores in the board's EEPROM, saved like a battery
  save.
- Not in the test build yet (as in RetroPixels.exe, or still to come): Game
  Gear, the Atari 7800's SOUPER cartridges, YM2151 sound and high-score
  cartridge saves.
- Start screen: with no game running, the window shows a drop target, an
  Open game button, your recent games (click one, or use the arrow keys and
  Enter, or the controller's D-pad and A), and a controller check that lights
  up the buttons you press.
- Save states: four slots per game. F2 saves to the chosen slot, F4 loads it,
  F3 / Shift+F3 picks the next / previous slot (Game > State slot shows
  them with their times); Ctrl+Z undoes the last load. Game > Save states
  (F6) shows all four with a picture of each, and exports, deletes or
  imports state files. Controller: click the right stick to save, the left
  stick to load, flick the right stick left or right to change slot (Nintendo
  64: View + D-pad left / right). Save states from earlier test builds are
  slot 1.
- Battery saves: Game > Battery save imports a .sav / .srm from another
  emulator (or one exported from RetroPixels.exe with "Download battery
  save"); the game restarts, and the old one is kept as a .bak file. Export
  saves the game's battery save as a .sav. Dropping a .sav or .state file on
  a running game imports it.
- Screenshots: F12 (Game > Screenshot) saves only the game's picture to
  Pictures\RetroPixels.
- .bin files: File > "Open .bin files as" picks the system instead of
  guessing.
- Options > Controller rumble turns rumble off. Record > Quality, Change
  recordings folder and Recent recordings work as in RetroPixels.exe's
  Recordings tab. Help > Keys and shortcuts (F1) lists every key.
- GameCube and Wii (through Dolphin): .iso .gcm .rvz .wbfs .ciso .gcz .wia
  and WiiWare .wad. It uses the Dolphin RetroPixels.exe already found or
  downloaded; if there is none, choose GameCube / Wii > Download the latest
  Dolphin (from dolphin-emu.org), or Change Dolphin... to pick a Dolphin.exe.
  GameCube / Wii > Library lists the games in your games folders (search,
  filter, sort; double-click, Enter or controller A plays). Games inside a
  .zip or .7z must be unpacked first. The game plays inside the RetroPixels
  Native window, so OBS can capture it. F11 or Alt+Enter: fullscreen; Esc:
  leave fullscreen; F9: start or stop recording; Alt+F4: stop the game. Games
  are recorded automatically (turn it off in the GameCube / Wii menu); the
  mp4 is saved when the game ends. The menu also has Game volume, Dolphin
  settings (close Dolphin first), Set up Xbox controller and Controller
  layout. Save states are Dolphin's: Shift+F1 saves, F1 loads.
- Recording: F9 (or Record > Start recording, or hold the right stick down for
  a second) starts and stops it. The video is only the game's picture and
  sound - no window, menus or mouse pointer - saved as .mp4 in
  Videos\RetroPixels (Record > Open recordings folder). It uses the same
  ffmpeg as RetroPixels.exe; if there isn't one yet, it offers to download it.
- OBS Studio: add a Window Capture of RetroPixels Native (Game Capture is for
  3D games and doesn't apply here).
  Its title also shows the game, so set Window Match Priority to "Match
  title, otherwise find window of same executable". Messages like "State
  saved" show in the title bar only, never on the picture.
- Kept in %APPDATA%\RetroPixels Native: settings.json, and saves\<system>\
  with each game's battery save (.sav) and save states (.state, slots 2-4 as
  .slotN.state) with a .png picture of each.
- The version shows in the title bar and in Help > About.

Where things are kept
- Save states and battery saves: inside RetroPixels's own browser profile
                 (%LOCALAPPDATA%\RetroPixels\BrowserProfile)
- Settings:      %APPDATA%\RetroPixels\config.json
- Recordings:    Videos\RetroPixels (GameCube / Wii), Downloads (classic)


Credits & licences
- RetroPixels' own code was written for this project and uses no copied code:
  the program, its screens, and its NES, Super Nintendo, Game Boy / Game Boy
  Color, Game Boy Advance, Nintendo 64, Sega Genesis, Atari 2600, Atari
  7800 and arcade emulators - including the 6502, 65C816, SPC700, Game Boy,
  ARM7TDMI, 68000, Z80, Intel 8080, 8035, Konami-2 and N64 (VR4300 and RSP) processors, the graphics and sound
  chips (S-DSP, YM2612, PSG...), the cartridge hardware, and replacements
  for the Game Boy Advance BIOS, the SNES sound start-up program and the N64
  boot program, so no Nintendo or Sega code is used. It uses only Go's
  standard library. Version 2.3.0 was checked file by file for code that
  followed other emulators too closely, and those parts were rewritten. Lists
  of facts about particular games (which save chip or controller a game uses)
  and measured hardware values are still facts others have published too.
  The DSP-3, DSP-4, ST010, ST011 and ST018 chip programs are never included;
  you supply your own.
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
