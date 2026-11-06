# Changelog

All notable changes to RetroPixels. Newest first.

## [2.36.0] - 2026-11-06

- PixelDS has save states for 3DS games too (DS games had them already): F5 saves, F8 loads, F6 / F7 pick one of ten slots. A 3DS state holds the whole machine (processor, memory, threads, services, graphics and sound state) and loads into a freshly opened copy of the game, so play carries on exactly as it was; files the game had open are opened again.
- PixelDS's window has a menu bar: Game (Pause, Restart, Fullscreen, Exit) and Save states (Save, Load, and the slot, ticked). It hides in fullscreen.
- Pause: P (or the Pause key, the menu, or a controller's left-stick click) pauses and resumes. While paused nothing runs: no sound, and the recording leaves the pause out, so the video and its sound stay together.
- This version also has 2.35.0's texture packs and 2.34.2's high-quality recordings together (2.35.0 was built without 2.34.2's recording change).
- Recordings hold only the game's own sound: PixelDS writes the sound it makes itself, never other sounds from the PC.

## [2.35.0] - 2026-11-05

- Texture packs for DS games in PixelDS. A pack is a folder of pictures that replace the game's 3D textures, found by a hash of each texture, like other emulators' packs: put it in %APPDATA%\RetroPixels\PixelDS\textures\<game code> (New Super Mario Bros. is A2DE) and it loads when the game starts. F10 turns it on and off.
- PixelDS can make a texture pack by itself: press F9 while playing (or run `PixelDS.exe -make-texpack game.nds`). It reads every 3D model and texture file in the game, enlarges each texture 4× with smooth filtering, and restyles Mario's textures in the colours of today's Mario games (a deeper red with softer shadows, a brighter royal blue, gold buttons, softer browns and more natural skin, with a faint cloth weave). For New Super Mario Bros. that is 912 pictures, 66 of them Mario's, in about 20 seconds. `-chars` picks other characters, `-style hd` only enlarges.
- F3 dumps the textures a game draws into the pack's dump folder, to paint your own; pictures you put in the pack folder win over the automatic ones. Pressing F9 again after dumping also restyles Mario's textures in colours the game sets while playing (a power-up's), found by matching the picture.
- DS 3D can be drawn at 2×, 3× or 4× the DS's resolution: F2 changes it (remembered), or `-scale`. The 3D picture gets the extra detail (and the texture packs' bigger pictures show), the 2D layers stay the DS's own pixels, and the game itself still sees the DS's picture, so it plays the same.
- Fixed in PixelDS's DS 3D: models drew washed out over the background in games that blend the 3D layer (New Super Mario Bros.' Mario and Peach were half see-through); 3D commands sent to the geometry FIFO's mirror addresses were lost (games send them with STM), which broke models; and VTX_DIFF moved vertices 8 times too far.
- What a texture pack can't do: change a model's shape (Mario keeps the DS model; a "newer model" look is only its paint and colours), or touch 2D graphics (New Super Mario Bros.' levels, blocks and most enemies are 2D sprites and tiles). Mario on screen is about 20 pixels tall at 1×, so the enlarged textures show best at 3× or 4×.

## [2.34.2] - 2026-11-05

- PixelDS recordings are now high-quality .mp4 files by default. PixelDS saves the DS / 3DS screens losslessly (full colour) while you play, and the .mp4 is made once, scaled up with crisp square pixels to 1080p or more (DS games 768×1152, 3DS games 1200×1440), H.264 at near-lossless quality (CRF 14 or the graphics card's equivalent) with 256 kbps AAC sound. Before, the picture was compressed twice and kept at only twice the screens' size.

## [2.34.1] - 2026-11-05

- PixelDS is now the only DS and 3DS emulator in RetroPixels: Azahar and melonDS are no longer in the download, and their choices are gone from the 3DS / DS tab and RetroPixels Native's 3DS / DS menu. Every DS and 3DS game plays in PixelDS. A setting from an older version that picked Azahar or melonDS is ignored.
- The release no longer attaches Azahar's or melonDS's source, since neither is included.

## [2.34.0] - 2026-11-05

- 3DS games now play in PixelDS by default, like DS games: one RetroPixels emulator for both. Azahar (3DS) and melonDS (DS) are still in the download if you pick them in the 3DS / DS tab; a player who chose Azahar keeps it.
- 3DS and DS games in PixelDS are recorded automatically (with "Record games automatically" on, the default), in RetroPixels.exe and RetroPixels Native: PixelDS saves the game's own picture and sound as it runs, and the .mp4 is made when the game ends.
- The Xbox controller is ready for DS and 3DS games out of the box: A / B / X / Y by position, LB / RB = L / R, Back = Select, Menu = Start; on the 3DS the left stick is the circle pad (no longer the D-pad as well) and LT / RT are ZL / ZR; on the DS LT is L too. The right stick moves the stylus; RT (DS) or R3 touches.
- Checked that PixelDS is all original code: no code from melonDS, Azahar or any other emulator. The 3DS sound processor's shared memory is now laid out PixelDS's own way.

## [2.33.0] - 2026-11-05

- PixelDS 3DS games now have sound: the 3DS sound processor's 24 voices (8-bit, 16-bit and ADPCM samples, looping and streamed), their filters and the three mixers, played through RetroPixels' sound output.
- 3DS games that load code modules (CRO files) while running now start in PixelDS: the ldr:ro service loads, links and unloads them.
- PixelDS's 3DS graphics gained geometry shaders, cube maps, shadow maps, projected textures and procedural textures (and texture unit 0's kind is now read correctly).
- Still to come for 3DS games in PixelDS: save states, encrypted games, and fixes from real games. Azahar stays the default until then.

## [2.32.0] - 2026-11-05

- PixelDS now plays 3DS games, as an experiment: decrypted .3ds, .cci, .cxi and .cia files, with 3D graphics. Choose it in the 3DS / DS tab under "3DS games play in: PixelDS (experimental)" (RetroPixels Native: 3DS / DS menu). Azahar stays the default for 3DS games until PixelDS plays them as well, so nothing changes unless you pick it. A .cia plays straight away in PixelDS (no installing).
- New in PixelDS's 3DS side: the PICA200 graphics chip (vertex shaders, clipping, textures in every format including ETC1, the six colour combiners, per-pixel lighting with its lookup tables, fog, blending, stencil and depth), drawn on several processor cores; loading games (game card images, CIAs, compressed code, the game's RomFS); a system font for games' text (built from DejaVu Sans, as PixelDS can't include Nintendo's); the sound processor's start-up so games' sound code runs; and more of the system calls the official SDK uses.
- Not yet in PixelDS for 3DS games: sound, games that load code modules (CROs), geometry shaders, procedural textures, save states, and games still need decrypting. If a 3DS game doesn't start, %APPDATA%\RetroPixels\PixelDS\3DS\log.txt says where it stopped; switch the tab back to Azahar to play it.

## [2.31.0] - 2026-11-05

- PixelDS takes its first step into the 3DS: it now runs 3DS homebrew programs (.3dsx) that draw straight to the screen. Drop a .3dsx file onto PixelDS.exe (or start `PixelDS.exe game.3dsx`). Like the rest of PixelDS it's RetroPixels' own new code.
- What's in: the 3DS's ARM11 processor with its media instructions and floating-point unit (checked against 20,000 reference cases), the 3DS's memory, and its system software done at a high level: threads with priorities, events, mutexes, semaphores, timers, the address arbiter, shared memory, and the system services programs talk to (srv, APT, GSP for the screens, HID for buttons, the circle pad and the touch screen, FS for the SD card and save data, CFG, PTM).
- 3DS controls in PixelDS: arrows (or a controller's left stick) move the circle pad, I / J / K / L are the D-pad, E = ZL, R = ZR (LT / RT on a controller), and the rest as for DS games. The 3DS's SD card and save data are kept in %APPDATA%\RetroPixels\PixelDS\3DS.
- Not yet: 3DS 3D graphics (the PICA200), sound, and game cards and installed titles (.3ds, .cci, .cxi, .cia). Those come next, so the 3DS / DS tab still plays 3DS games in Azahar for now.

## [2.30.0] - 2026-11-05

- PixelDS, RetroPixels' own DS emulator, now draws 3D graphics, so 3D DS games play in it too. It's RetroPixels' own new code, like the rest of PixelDS.
- What's in: all the DS's 3D commands (matrices and their stacks, vertices, normals and four lights with the shininess table, texture coordinate transforms, box, position and vector tests), clipping, front and back face culling, all seven texture formats (including 4×4 compressed), repeat and flip, modulation, decal, toon and highlight shading, translucency with blending and sorting, alpha test, shadow polygons, wireframe, edge marking, fog, the rear-plane bitmap, and Z or W depth.
- Display capture works too, which games use to show 3D on both screens at once and for motion blur.
- The 3D picture is drawn on several processor cores at once to keep busy scenes at full speed.
- Not yet: anti-aliasing (edges are drawn sharp).

## [2.29.0] - 2026-11-05

- New: PixelDS, RetroPixels' own Nintendo DS emulator, in the PixelDS folder next to the programs. It's new code written for RetroPixels (no melonDS or other emulator code in it), and the 3DS / DS tab now plays DS games in it. It's the first step towards one RetroPixels emulator for both the DS and the 3DS.
- What PixelDS does now: both DS processors (ARM9 and ARM7, checked against 20,000 reference cases), its own built-in BIOS (no BIOS or firmware files needed), both 2D graphics engines (backgrounds, sprites, windows, blending, brightness), the sound chip, buttons, the touch screen, the clock, game saves (EEPROM, flash and FRAM, found automatically, kept as `game.sav` next to the game like melonDS does) and save states.
- Not yet: DS 3D graphics. 3D games start, but their 3D parts are blank for now. Pick melonDS for those: the tab has "DS games play in: PixelDS / melonDS", and RetroPixels Native's 3DS / DS menu has the same choice. 3D comes next, then 3DS games in the same emulator.
- PixelDS controls: arrows, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Shift = Select; the mouse is the stylus on the bottom screen. Xbox controllers work (the right stick moves a stylus pointer, RT or R3 touches). F5 saves a state, F8 loads it, F6 / F7 pick the slot, F11 or Alt+Enter toggles fullscreen, Ctrl+R restarts.
- melonDS stays in the download as the other choice for DS games, and Azahar still plays 3DS games.

## [2.28.0] - 2026-11-05

- New: the 3DS / DS tab, for Nintendo 3DS and Nintendo DS games in one library. Add the folder with your games and click one to play. 3DS games play in Azahar (the open-source 3DS emulator, GPL-2.0-or-later) and DS games play in melonDS (the open-source DS emulator, GPL-3.0-or-later). Azahar plays only 3DS games, so RetroPixels sends each game to the emulator for its system: you just pick the game.
- Both emulators come in the download, in the Azahar and melonDS folders next to the programs (their official Windows releases, unchanged: Azahar 2126.2 and melonDS 1.1). Nothing else to install. Their source code is attached to the release.
- The tab lists .3ds, .cci, .cxi, .3dsx (and their compressed .z versions) and .cia files for the 3DS, and .nds, .dsi and .srl files for the DS. 3DS games must be decrypted, as Azahar needs. Clicking a .cia installs it into Azahar; then play it from Azahar's game list. DS games play without a BIOS; DSi games need your own DSi BIOS and NAND files in melonDS.
- Games start fullscreen in the emulator's own window (untick "Start games fullscreen" to play in a window). Set up controls in each emulator: the tab's "Open Azahar" and "Open melonDS" buttons open them.
- RetroPixels Native: the new 3DS / DS menu opens a 3DS or DS game in the right emulator, and 3DS and DS files opened or dropped on its window start there too.
- Recording isn't available for 3DS and DS games yet.

## [2.27.0] - 2026-11-04

- GameCube and Wii: games now play at 3× resolution (1K / 1080p) by default instead of the native 480p, and the resolution you choose stays: RetroPixels keeps it in %APPDATA%\RetroPixels\gamecube-resolution.txt (shared by RetroPixels.exe and RetroPixels Native) and puts it into PixelU's GameCube / Wii settings before every game, so it's no longer lost when you unzip a new version into a new folder. A higher resolution you had already set is kept. Change it in the GameCube / Wii graphics settings (RetroPixels Native: GameCube / Wii > PixelU settings).

## [2.26.0] - 2026-10-28

- New: the RetroPixels SNES Debugger (RetroPixels-SNES-Debugger.exe, in the download next to the programs). It opens alongside RetroPixels.exe and RetroPixels Native when a SNES game starts (turn that off below the game in RetroPixels.exe, or in RetroPixels Native's Options menu).
- Capture: press "Capture now" (or "Capture in 5 s") at the moment you want, the first level say. It saves VRAM, CGRAM (palettes), OAM, WRAM, the PPU registers, and every DMA transfer the game made since it started, with its source address, size, destination, and the code that set it up. For every byte of VRAM, CGRAM and WRAM it also knows which transfer (or which CPU instruction) wrote it, and where from. Both SNES emulators (RetroPixels.exe's and RetroPixels Native's) make the same captures.
- Analysis: for each background layer it shows the whole tilemap drawn as the game has it, the layer's tile size, colour depth, tilemap and tile addresses, and where its tilemap, tiles and palettes are in the ROM: file offset and SNES address, through WRAM when the game copies them there first, or found by searching the ROM when the game's code wrote them. Graphics the game decompresses are pointed out with the code that unpacks them.
- Background swap: choose a PNG and the debugger puts it in the layer: scaled to the tilemap, reduced to the SNES's palette limits (new palettes for the picture, written to the ROM, or the game's own colours; optional dithering), cut into tiles with repeats and flipped repeats shared, and written back to the ROM addresses the tiles, tilemap and palettes came from. When the picture needs more tiles than the game had, the tiles move to free ROM space with room for more and the game's DMA transfer is repointed (its source address, bank and size, in the code or in a pointer table), or similar tiles are merged. The checksum is updated; your original ROM file isn't changed.
- It also works from a terminal: RetroPixels-SNES-Debugger.exe capture / analyze / export / swap. docs/snes-debugger.md explains the capture files and how the swap works.
- Limits: compressed graphics and scrolling level maps can't be swapped in place (title screens and fixed backdrops can); Mode 7 layers are shown but not swapped yet.

## [2.25.0] - 2026-10-28

- Star Wars Trilogy Arcade: the space background no longer glitches in the Return of the Jedi Death Star battle. Pieces of the planet Endor flashed across the stars for a frame at a time whenever its huge surface passed close by the camera; the 3D renderer now clips polygons at a margin around the screen, so they stay where they belong.
- Star Wars Trilogy Arcade: no more thin dotted line across the screen during explosion flashes and other full-screen effects. Neighbouring triangles now meet exactly, with no gap between them.

## [2.24.0] - 2026-10-28

- Star Wars Trilogy Arcade: C-3PO shows up again in Echo Base, and the big blue shape that covered the Millennium Falcon hangar is gone. The game copies some of its 3D data (C-3PO among it) straight from its program ROM, and the emulator was reading those copies from the wrong part of the ROM. Checked frame by frame against Supermodel.

## [2.23.0] - 2026-10-28

- Star Wars Trilogy Arcade: the mouse aims and fires by default, alongside the Xbox controller, in RetroPixels.exe and RetroPixels Native. Whichever you moved last aims: the mouse points where it is over the picture, the left button fires and the right button is the yoke's thumb button.
- Star Wars Trilogy Arcade: the flight stick feel from RetroPixels' Supermodel build is back, now in RetroPixels' own emulator and on by default: a round dead zone, the thumbstick's round gate stretched so the corners are reachable, a response curve for steady aim near the centre, and smoothing like the yoke's weight and spring. The game's machine settings have "Flight stick feel" (On / Off), "Flight stick smoothness" (Off, Light, Medium, Heavy) and "Stick up / down" (up climbs, or up dives like a flight yoke).
- RetroPixels.exe: Star Wars Trilogy Arcade now reads the controller's stick as an analog stick (2.22.0 only had the stick's directions there).

## [2.22.0] - 2026-10-27

- Star Wars Trilogy Arcade (Sega Model 3) now plays on RetroPixels' own Sega Model 3 emulator, built into RetroPixels.exe and RetroPixels Native, like the other arcade games. Supermodel is no longer needed or included: nothing to install on your PC or keep in the RetroPixels folder. Open swtrilgy.zip or swtrilgya.zip like any arcade game.
- The emulator has the Model 3 main board (PowerPC 603e), the Real3D 3D board drawn on the processor's cores, the tile layers, the sound board (two SCSP sound chips with their 68000) and the MPEG music board. Pictures and sound were checked against Supermodel frame by frame.
- Controls: the Xbox controller's left stick aims like the arcade's flight yoke; the d-pad and arrow keys aim too, further the longer you hold them. Fire, Button 1 (the yoke's thumb button), Coin, Start and Service can be changed in Controls. High scores and settings are kept like a battery save. Save states aren't available for this game yet.
- Licences: the Model 3 sound chips are a Go port of Supermodel's code and the music board follows Supermodel's, both GPL-3.0-or-later, so RetroPixels' programs are now distributed under GPL-3.0-or-later as a whole, with their full source attached to each release. cores/model3/NOTICE.txt lists each third-party part.
- The Supermodel tab, RetroPixels Native's Supermodel menu and settings, and the Supermodel folder are gone.
- Two downloads, one per program: RetroPixels-2.22.0.zip (RetroPixels.exe) and RetroPixels-Native-2.22.0.zip (RetroPixels-Native.exe), each with the engines and VERSION.txt.

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
