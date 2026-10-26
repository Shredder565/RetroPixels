# Changelog

All notable changes to RetroPixels. Newest first.

## [Unreleased]

- Star Wars Trilogy Arcade no longer slows down every few seconds while it records (most noticeable in wide screen on a big screen): the recording is scaled down to 1080p on the graphics card and read back without making the game wait, and it's encoded with a faster setting.
- Star Wars Trilogy Arcade opens on its title: the start-up ("WAIT SETUP THE FEEDBACK LEVER" and the Sega logo) now runs at full speed behind a black screen, silently and unrecorded, so the game is ready for coins and Start when the picture appears, past the point where adding coins early could crash it. In RetroPixels.exe and RetroPixels Native, with RetroPixels' own Supermodel.
- Star Wars Trilogy Arcade: the mouse now works too, at the same time as the Xbox controller, with nothing to switch on: point to aim, left button fires, right button is the event button (force / lock-on), middle button the second trigger. Whichever you moved last steers, so a mouse left off to one side doesn't pull the aim while you play with the controller. In RetroPixels.exe and RetroPixels Native.
- RetroPixels for Android records games too: Settings > Record games automatically, on by default, as on Windows. Each game is recorded from start to end, only the game's picture (scaled up sharp, without the touch controls) and its sound, made into an .mp4 by the phone's own encoder (Android 7 to 9 asks once for storage access; newer phones ask nothing). The videos go to Movies/RetroPixels, named "<game> <date time>.mp4"; going to the background or closing the game ends the video, and the game plays on if the phone can't record.

## [2.21.0] - 2026-10-21

- The first release since 2.19.0. 2.20.0 was never published as a release, so 2.21.0 includes its changes too (its notes are under 2.20.0 in the full change log), with one big difference: Wii U games now play in PixelU.
- One download for everything: RetroPixels-2.21.0.zip has RetroPixels.exe and RetroPixels-Native.exe and, in folders next to them, the engines they play through: PixelU\PixelU.exe (Wii U), PixelU\wii\PixelU-Wii.exe (GameCube and Wii), MAME (built in, as since 2.17.0) and Supermodel\supermodel.exe (Star Wars Trilogy Arcade). Unzip it and keep the folders next to the programs. No games, keys, BIOS or firmware are included.
- New: Wii U games, in PixelU, RetroPixels' Wii U emulator. It started from Cemu's code and has been rewritten as RetroPixels' own code; it is under the Mozilla Public License 2.0 (LICENSE-MPL-2.0.txt). It plays the Wii U games you dump from your own console, with your own keys: disc keys go in keys.txt in the PixelU\portable folder. The Wii U tab lists .wua, .wud and .wux files, Wii U .iso discs and unpacked games (a folder with code\*.rpx) from your games folders.
- Wii U games play inside the RetroPixels window (or in PixelU's own window, and fullscreen if you like). The GamePad screen / TV screen button in the bar above the game shows the GamePad's screen, which you touch with the mouse or a finger. An Xbox controller and the keyboard work right away (keyboard: WASD left stick, arrow keys D-pad, L / K / I / J = A / B / X / Y, Q / O = L / R, E / U = ZL / ZR, Enter / Backspace = Plus / Minus); change them in PixelU's Options > Input settings. If Vulkan doesn't work with your graphics driver, RetroPixels says how to update the driver and offers to play with OpenGL instead.
- GameCube and Wii now play in PixelU's GameCube / Wii engine (PixelU\wii\PixelU-Wii.exe), which comes in the download, so there's no Dolphin to download any more. It started from Dolphin and has been rewritten as RetroPixels' own code; it is under GPL-2.0-or-later (its licence is in PixelU\wii). Your Dolphin settings, memory cards and Wii saves are copied over the first time a game starts (nothing is overwritten).
- Nintendo's keys aren't included: Wii discs, WiiWare and Wii SD card saves need the keys file from your own Wii (the keys.bin BootMii saves with its NAND backup, or the console's otp.bin). RetroPixels Native asks for it before the first Wii game and can look for it on your PC; for RetroPixels.exe, put it in PixelU\wii\User\Wii. GameCube games don't need it. WiiWare add-on (DLC) .wad files are left out of the list, and opening one says how to install it.
- New: Star Wars Trilogy Arcade (Sega Model 3), in Supermodel by The Supermodel Team, modified for RetroPixels (GPL-3.0-or-later; the exact source and the list of changes are in the Supermodel folder). The new Supermodel tab finds your swtrilgy.zip or swtrilgya.zip in your ROM folders, Downloads, Desktop and Documents, then in the game folders on each drive, and remembers it; or choose the file. Click the tile and the game starts playable, fullscreen, with the Xbox controller. "Flight stick feel" (on by default) makes the thumbstick aim like the arcade's yoke, with a Smoothness setting (Off, Light, Medium, Heavy). Tick "Play inside RetroPixels" to play it in the RetroPixels window.
- RetroPixels Native plays Star Wars Trilogy Arcade too: Arcade (MAME) > Star Wars Trilogy Arcade (Supermodel), or open swtrilgy.zip / swtrilgya.zip, which now start in Supermodel instead of MAME (MAME can't play it). Its settings are under Supermodel settings in the same menu.
- RetroPixels Native: Wii U games (Wii U > Open a Wii U game...) and GameCube / Wii games play in PixelU, docked inside its window, so OBS captures them. While one plays, the controller no longer works the start screen behind it.
- RetroPixels Native registers itself for the game file types it plays, so it shows in File Explorer's "Open with". Windows 11 still asks once per file type: pick RetroPixels Native and "Always" (Options > Open game files with RetroPixels Native explains it). A game opened while RetroPixels Native is already running goes to that window.
- Recording: one "Record games automatically" setting for every system, on by default (turn it off in the Recordings tab, or in RetroPixels Native's Record menu). Built-in systems, Wii U, GameCube / Wii, MAME and Supermodel games are recorded from start to end, and the videos go to your recordings folder (Videos\RetroPixels).
- Sega Genesis / Mega Drive: the games from Sonic Classic Collection no longer stay on a black screen, in RetroPixels.exe, RetroPixels Native and RetroPixels for Android.
- Linux: a Linux version is on the way and comes in a later version; 2.21.0 is for Windows (and Android). RetroPixels Native stays Windows-only.

## [2.20.0] - 2026-10-17

- Never published as a release: these changes come with 2.21.0. Since 2.21.0, Wii U games play in PixelU instead (see 2.21.0); RetroPixels.exe's Wii U tab and RetroPixels Native no longer use the emulator described here, which stays in RetroPixels' source.
- New: the Wii U, on RetroPixels' own Wii U emulator, written in Go. It began as code written from scratch (Cemu's design was the guide); some of its files were later rewritten from Cemu's code and are under the Mozilla Public License 2.0, as each of them says. This first version played Wii U homebrew programs (.rpx) that draw with the OS's own screens (OSScreen), on the TV and the GamePad, in RetroPixels.exe and RetroPixels Native.
- Inside: a PowerPC 750CL ("Espresso") processor with its paired-single maths, checked against the same test program built for a real CPU; the .rpx / .rpl program loader; and versions of the Wii U's system libraries: threads on three cores, locks, timers, memory heaps, files (the SD card is /vol/external01), the GamePad's buttons and sticks, and the app lifecycle.
- The download has "RetroPixels Wii U demo.rpx", a small homebrew program made for RetroPixels, to try it with (since 2.21.0 it opens in PixelU). No Nintendo keys, firmware or games are included or needed for it.
- RetroPixels for Android can open .rpx files with this emulator, but that hasn't been tried on a phone.

## [2.19.0] - 2026-10-17

- Eleven more arcade games, each first played in MAME at fast speed and then checked against it frame by frame and sound by sound: Double Dragon, Double Dragon II: The Revenge, Double Dragon 3: The Rosetta Stone, Galaga, Dig Dug, Donkey Kong Jr., Donkey Kong 3, The Real Ghostbusters and Dragon Blaze are new, and Donkey Kong and Galaxian were rechecked. The pictures match MAME pixel for pixel in nearly every frame compared, with the same pitch and loudness within a few percent. RetroPixels' code is its own: MAME was only watched and listened to.
- New processors and chips, written from scratch: the Motorola 6809 and Hitachi HD6309, the HD63701, the Hitachi SH-2, the Yamaha YM3812 (OPL2) and YMF278B (OPL4), the OKI MSM5205, and Namco's Galaga and Dig Dug boards with their wavetable sound. Donkey Kong 3 uses RetroPixels' NES sound chip.
- Galaga, Dig Dug and The Real Ghostbusters don't need their boards' custom chips, which most sets don't include: RetroPixels plays their part from what each game expects. Dragon Blaze makes its own settings chip when the set has none.
- Donkey Kong and Galaxian: colours and sound effects now match MAME more closely, in RetroPixels.exe, RetroPixels Native and Android.
- Fixes for 2.18.0's games: Berzerk's robots now talk when your set has the two speech chip files (RetroPixels' own speech chip; without the files the game plays silently as before). Altered Beast's voices and music now match MAME's sound. Asteroids Deluxe was checked against MAME's picture and its screen now has the arcade monitor's tint and brightness.

## [2.18.0] - 2026-10-17

- Nine more arcade games on RetroPixels' own emulation: 1941: Counter Attack, 1942, 1943: The Battle of Midway, 1943 Kai, Altered Beast, Asteroids Deluxe, Berzerk, Centipede and Vs. Castlevania. Each was first played in MAME at fast speed, then RetroPixels was checked against it frame by frame and sound by sound: the picture matches MAME pixel for pixel in the frames compared (1941, 1942, 1943, 1943 Kai, Altered Beast, Berzerk, Vs. Castlevania, Centipede), with the same pitch and similar loudness. RetroPixels' code is its own: MAME was only watched and listened to.
- New boards and chips, written from scratch: Capcom CP System (1941) with the OKI MSM6295; Capcom's 1942 and 1943 boards with the AY-3-8910 and YM2203; Sega System 16B (Altered Beast) with its memory mapper; Atari's Centipede board and the Asteroids Deluxe vector generator, drawn as smooth lines; Stern's Berzerk with its bitmap and sound timers; Nintendo's Vs. System on RetroPixels' NES emulator.
- 1943 and 1943 Kai don't need the board's protection chip: RetroPixels answers the game's questions from the table in the game's own program. Centipede, Asteroids Deluxe and Berzerk keep their high scores like a battery save. Centipede's trackball moves with the stick or arrow keys.
- Your ROM sets can use the older file names or MAME's current ones. Altered Beast works with the set that has the unencrypted sound program (11671). Berzerk's speech isn't emulated yet (it needs the two speech chip files, which most older sets don't have), so the robots are silent. Asteroids Deluxe's explosion and thrust sounds are RetroPixels' own recreation.
- RetroPixels.exe runs these new boards inside the program, like the Nintendo 64, so they play in the desktop app. RetroPixels Native and RetroPixels for Android play them too.

## [2.17.0] - 2026-10-16

- MAME now comes with RetroPixels. The RetroPixels-2.17.0.zip download has the MAME team's own official Windows release of MAME inside, in its MAME folder, unchanged: it is downloaded from MAME's GitHub, checked against the SHA-256 checksum GitHub lists for it, and not rebuilt or patched. Its source code is attached to the release. Unzip, put your ROM sets in MAME\roms (or add your own ROM folder) and play: there's nothing else to download. Both RetroPixels.exe and RetroPixels Native list it as "MAME 0.xxx (built in)" and use it as the default when you have no other MAME. Your other MAME versions still work alongside it.
- RetroPixels now checks each game in every MAME version you have (with MAME's own -verifyroms, once per game and version) and shows whether it plays: "Plays in MAME 0.xxx", or "Won't start" and the versions it does play in. A game whose default version can't play it starts in a version that can, unless you chose one for it.

## [2.16.0] - 2026-10-16

- New: RetroPixels for Android. Each release now includes RetroPixels-2.16.0.apk, which plays the same systems as RetroPixels Native (NES, Super Nintendo, Game Boy / Color, Game Boy Advance, Nintendo 64, Genesis, Master System, Atari 2600, Atari 7800 and the arcade games) with on-screen touch controls, Bluetooth and USB controllers, four save-state slots, battery save import and export, and the SNES chip and Archipelago patch prompts. Not yet tried on a real phone.
- Also included: RetroPixels-Android-Studio-2.16.0.zip, the Android app as an Android Studio project with its emulator engine already built, so File > Open and Build APK is all it needs. The full source has it in the android folder.

## [2.15.0] - 2026-10-16

- New: arcade games in MAME, any version of it. The new Arcade (MAME) tab in RetroPixels.exe (and the Arcade (MAME) menu in RetroPixels Native) lets you add as many MAME versions as you like: point it at a mame.exe you already have, let it find the ones on your PC, or download any official Windows release of MAME (from the MAME team's GitHub releases, checked against GitHub's checksum), each into its own folder so versions sit side by side.
- Pick a default MAME, and pick a different version for any game: ROM sets have to match the MAME version they were made for, so an old set can keep playing in the old MAME it needs. Add your ROM folders and the library lists your games by their full names; the game plays inside the RetroPixels window (or in MAME's own window if you untick "Play inside RetroPixels"), with Stop game to come back.
- When a game doesn't start (missing or mismatched ROM files, a version that doesn't have the game), RetroPixels shows MAME's reason and suggests trying another version. In RetroPixels Native, a zipped arcade game its own emulators don't have is offered to MAME.

## [2.14.0] - 2026-10-15

- RetroPixels Native test build 6: Super Nintendo, with every cartridge chip RetroPixels.exe supports (SA-1, Super FX, DSP-1 to DSP-4, ST010, ST011, ST018, Cx4, S-DD1, SPC7110, OBC1, S-RTC), matching RetroPixels.exe on every frame compared. Chip programs you supply are asked for once and kept. RetroPixels Native now plays every system RetroPixels.exe plays.
- Archipelago in RetroPixels Native: open patch files, and connect the SNI Client (Super Nintendo) or the BizHawk Client (Game Boy / Color, Game Boy Advance, NES, Nintendo 64) directly, as in RetroPixels.exe. Tested with Archipelago's real server and clients.

## [2.13.0] - 2026-10-15

- RetroPixels Native test build 5: GameCube and Wii games through Dolphin, shown inside the RetroPixels Native window (so OBS captures them), with the library, Dolphin download, game volume, Dolphin settings, Xbox controller setup and automatic recording, using the Dolphin RetroPixels.exe already has.
- RetroPixels Native now has a start screen with your recent games and a controller check, four save-state slots with pictures (F3 picks the slot, F6 shows them all), importing and exporting battery saves and save states, screenshots of just the game (F12), recording quality and folder choices, recent recordings, a rumble switch, "Open .bin files as" and a keys-and-shortcuts list (F1).

## [2.12.0] - 2026-10-15

- RetroPixels Native test build 4: RetroPixels-Native.exe now plays the arcade games too: Pac-Man / Puck Man, Galaxian, Space Invaders, Donkey Kong, Teenage Mutant Ninja Turtles, TMNT: Turtles in Time and The Simpsons. They give the same picture and sound as RetroPixels.exe on every frame compared (15,000 frames each for the three Konami games). Game > Player picks your turtle or Simpson in the 4-player games, and Game > Machine settings changes the DIP switches.

## [2.11.0] - 2026-10-15

- RetroPixels Native test build 3: RetroPixels-Native.exe now also plays Sega Genesis / Mega Drive, Game Boy Advance and Nintendo 64 games. Genesis and Game Boy Advance give the same picture and sound as RetroPixels.exe on every frame compared, with battery saves, 6-button pads, J-Cart, and the Game Boy Advance clock, sunlight, gyro, tilt and rumble cartridges. Nintendo 64 uses the same emulator as RetroPixels.exe, with the same controls, battery saves and save states.

## [2.10.0] - 2026-10-15

- RetroPixels Native test build 2: RetroPixels-Native.exe now also plays Master System, Game Boy / Game Boy Color and Atari 7800 games, alongside NES and Atari 2600. Each new system gives the same picture and sound as RetroPixels.exe on every frame compared (hundreds of test programs and homebrew games), with battery saves (including Game Boy clocks), save states, rumble and Game Boy tilt cartridges.

## [2.9.0] - 2026-10-15

- New: RetroPixels-Native.exe, the first test build of RetroPixels rebuilt as a native Windows program with no web browser inside, so OBS Studio's Window Capture records it with any capture method. It plays NES and Atari 2600 games, with the same emulation as RetroPixels.exe (checked frame by frame and sample by sample against it), Xbox controllers, save states, battery saves and game-only recording. The other systems follow in later test builds; RetroPixels.exe is unchanged and still plays everything.

## [2.8.0] - 2026-10-15

- New: Archipelago (archipelago.gg) multiworld randomizers. Open an Archipelago patch file (.apsm, .aplttp, .apred, .apemerald and others) like a game; RetroPixels asks once for the original ROM, checks it, keeps it for that game and makes the randomized game, with its own saves. Patched ROMs made with the Archipelago Launcher open too.
- The Archipelago SNI Client (Super Nintendo) and BizHawk Client (Game Boy / Color, Game Boy Advance, NES, Nintendo 64) connect to RetroPixels directly; SNI, BizHawk and Lua scripts aren't needed. An "Archipelago" button under the game shows whether the client is connected and how to connect it. RetroPixels only answers programs on this computer.

## [2.7.1] - 2026-10-14

- RetroPixels has its own icon: the warrior picture now shows on the desktop, in Explorer, on the taskbar and on the window.
- 4-player arcade games (TMNT, Turtles in Time, The Simpsons) show "Choose your player" buttons above the game: 1 Leonardo, 2 Michelangelo, 3 Donatello, 4 Raphael, or 1 Marge, 2 Homer, 3 Bart, 4 Lisa. Your keyboard and first controller play as the one you pick; it's remembered per game.
- The program's Properties in Windows show its name and version.

## [2.7.0] - 2026-10-14

- Arcade: TMNT, Turtles in Time (TMNT2) and The Simpsons were each played start to finish in MAME and in RetroPixels and compared frame by frame and sound by sound. The picture now matches MAME pixel for pixel in almost every frame checked, and the sound balance matches. RetroPixels' code is still all its own: MAME was only watched and listened to.
- New: 4-player arcade games (and Nintendo 64 and Genesis, which have four controller ports) have a "Player 1" button above the game. Click it to choose which player (1 to 4) your keyboard and first controller play as. The choice is remembered for each game.
- TMNT: a stray dark mark on the title screen is gone; flip-screen mode now flips the sprites too; drum and effect volumes were swapped between the two sample channels; speech is no longer cut off by the next line; music noise (cymbals, hits) was far too loud; loud chords no longer distort; the music is no longer too quiet next to effects and speech.
- Turtles in Time: the picture is the board's real 304 pixels wide and the backgrounds line up; sprites overlap in the right order (the turtle in the intro circle is no longer hidden); zoomed sprites such as the "Evening News" letters are exact; shadows only darken the background, at the right strength; the news-scene fade and the prehistoric night darkening work, with bright light beams; Krang's legs and other big sprites use the right pieces; sampled sounds and voices were too quiet next to the music.
- The Simpsons: sprites were 16 pixels too high; big sprites (the jewellery store door) had wrong pieces; enemies blinked or showed where they shouldn't; shadows (cage bars, bosses, Krusty) were wrong and the title and score screens were darkened by mistake; sprites line up during the final boss's screen shake; sampled sounds were too quiet next to the music.
- Music chip (all three games): noise level and fade fixed, and loud notes no longer distort.
- New CHANGELOG.txt lists the changes in every version.

## [2.6.2] - 2026-10-14

- TMNT: fire no longer shows in front of the doors in the burning building. Sprites now stack in the right order, hidden duplicate sprites (like an extra turtle in the demo) are gone, the backgrounds sit 6 pixels further right where they belong, and shadows are a little lighter. The picture now matches the arcade board's own layering chip.

## [2.6.1] - 2026-10-14

- Recording on every system now captures only the game picture. NES, SNES, Game Boy, GBA, N64, Genesis, Master System, Atari and arcade recordings no longer include black bars, buttons, messages or the rest of the window, and are saved at a sharp, even size. (GameCube / Wii already recorded only the game since 2.6.0.)
- TMNT: the game's own ROM self-test now passes, speech sounds cleaner, and the service buttons work for players 2 to 4.
- The Simpsons: sprites were drawn too far to the right of the backgrounds (character select, intro, bonus stage); they now line up. Ground shadows draw as full ovals instead of thin slivers.

## [2.6.0] - 2026-10-14

- Arcade: TMNT, Turtles in Time (TMNT2) and The Simpsons now boot and play with correct graphics and sound. Fixed: TMNT's graphics and sprite layout, sound effects and speech; TMNT2's chip list, settings memory, interrupts and sprites; The Simpsons' processor and settings memory. Sets without their .nv file set themselves up on first start.
- GameCube / Wii recording rebuilt: Dolphin now saves the game's own frames and sound while you play, and RetroPixels turns them into an .mp4 when the game ends. The video is smooth and shows only the game: no mouse cursor, no other windows, no black bars. "Saving recording…" shows while it's made. With automatic recording off, the Record button records the next game.

## [2.5.2] - 2026-10-14

- GameCube / Wii recording: black bars are now always trimmed from the finished video (before, only with Aspect ratio on "Auto"). If ffmpeg isn't installed, the first game you start downloads it and recording starts when it's ready, instead of not recording.

## [2.5.1] - 2026-10-14

- GameCube / Wii: a volume slider and mute button in the bar above the game while it plays, and under "Sound" in the GameCube / Wii settings. Windows remembers Dolphin's volume, so it may also apply if you open Dolphin by itself.
- Arcade: ROM sets are recognised even when the zip has a different name (for example "Teenage Mutant Ninja Turtles (World).zip" or "tmnt (1).zip"), by the chip files inside. If a zip still isn't recognised, the message now lists the files inside it.

## [2.5.0] - 2026-10-14

- Arcade: Konami's Teenage Mutant Ninja Turtles, Turtles in Time (TMNT2) and The Simpsons, with up to four players. New from-scratch chips: Konami-2 processor, YM2151, K052109, K051960, K007232, uPD7759, K053260, K053245 / K053247, K053251 and the 93C46 EEPROM.
- Arcade sets whose chip files are named differently are matched by size.
- GameCube / Wii: "Add games folder…" now opens the folder picker in front of RetroPixels (it could open hidden), looks deeper inside the folder, and says how many games it found.

## [2.4.0] - 2026-10-14

- New Arcade system, written from scratch: Puck Man / Pac-Man, Galaxian, Space Invaders and Donkey Kong. Load the game's .zip ROM set. Includes RetroPixels' own Intel 8080 and 8035 processors, and a "Machine settings" panel for each game's DIP switches.

## [2.3.0] - 2026-10-13

- Super Nintendo: DSP-3, DSP-4, ST010, ST011 and ST018 games now play. These chips need their built-in program, which RetroPixels can't include: the first time, it asks you to choose your own dump of it (dsp3.rom, dsp4.rom, st010.rom, st011.rom or st018.rom) and remembers it.
- Originality check: every file was reviewed and the parts that followed other emulators too closely were rewritten in RetroPixels' own way (DSP-1, Cx4, S-DD1 and SPC7110 chips, SNES sound filter, 68000 divide timing, Atari 2600 cartridge detection and DPC music, Game Boy Color colours, Genesis and Master System colours, GBA BIOS ArcTan, N64 6105 boot check and Controller Pak checksum, the 7-Zip ARM64 filter).
- Things you may notice: Konami VRC7 games (Lagrange Point) use RetroPixels' own instrument sounds; Game Boy Color colours look slightly different; DSP-1 games (Pilotwings, Super Mario Kart) compute 3D maths slightly differently from the real chip.

## [2.2.0] - 2026-10-13

- Super Nintendo: games with cartridge chips now play: Super FX (Star Fox, Yoshi's Island), SA-1 (Super Mario RPG, Kirby Super Star), DSP-1 (Super Mario Kart, Pilotwings), DSP-2, Cx4 (Mega Man X2 / X3), S-DD1 (Star Ocean), SPC7110, OBC1 and S-RTC.
- NES: about 120 cartridge types (up from 18) and the Famicom sound chips (VRC6, VRC7, Namco 163, Sunsoft 5B, MMC5); many accuracy fixes.
- Game Boy / Color: more cartridge chips (MBC6, MBC7 tilt, HuC3, TAMA5, rumble and others) and accuracy fixes (passes the blargg, mooneye and acid2 tests).
- Game Boy Advance: sensor cartridges (Boktai sun sensor, WarioWare: Twisted! gyro, Yoshi Topsy-Turvy tilt, rumble) and BIOS call fixes.
- Genesis: EEPROM saves, Realtec mapper, J-Cart 4-player games, automatic 3-button pad mode.
- New: Sega Master System / Mark III.
- Atari 2600: nearly every bank-switching scheme, DPC+ and the Supercharger. Atari 7800: more cartridge types and POKEY sound.
- The version number shows in the app.

## [2.1.0] - 2026-10-13

- Nintendo 64 runs on RetroPixels' own emulator inside RetroPixels.exe (EmulatorJS removed; no internet needed).
- "Download Dolphin" button in the GameCube tab.
- GameCube / Wii games play inside the RetroPixels window; Stop game closes Dolphin completely.

## Before 2.1.0 - 2026-10-13

- RetroConsole became RetroPixels. Settings, save states, recordings and save backups move over from RetroConsole automatically the first time it starts.
- Recordings follow the game's own volume.
