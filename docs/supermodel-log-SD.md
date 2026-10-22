# Supermodel rewrite: batch SD (sound and drive boards)

Every file in `supermodel/Src/Sound/` and `supermodel/Src/Model3/DriveBoard/` was rewritten as original RetroPixels code, and so were `Model3/SoundBoard.cpp/.h` and `Model3/DSB.cpp/.h`. Each file starts with the RetroPixels original-code line, has no Supermodel licence header, uses LF line endings and keeps its file name. `Sound/MPEG/MpegAudio.cpp` still uses minimp3 from `Pkgs/`.

None of these files had debugger hooks. The `DebugLog` trace calls in the DSB and drive-board code were dropped. They only wrote to the log at debug level.

## Rewritten

| File | Lines before | Lines after |
|---|---:|---:|
| Sound/SCSP.cpp | 2134 | 1362 |
| Sound/SCSP.h | 89 | 67 |
| Sound/SCSPDSP.cpp | 497 | 249 |
| Sound/SCSPDSP.h | 85 | 59 |
| Sound/SCSPLFO.cpp | 181 | 145 |
| Sound/MPEG/MpegAudio.cpp | 393 | 277 |
| Sound/MPEG/MpegAudio.h | 21 | 33 |
| Model3/SoundBoard.cpp | 589 | 295 |
| Model3/SoundBoard.h | 210 | 69 |
| Model3/DSB.cpp | 1277 | 735 |
| Model3/DSB.h | 335 | 184 |
| Model3/DriveBoard/DriveBoard.cpp | 482 | 339 |
| Model3/DriveBoard/DriveBoard.h | 317 | 123 |
| Model3/DriveBoard/WheelBoard.cpp | 707 | 404 |
| Model3/DriveBoard/WheelBoard.h | 215 | 72 |
| Model3/DriveBoard/JoystickBoard.cpp | 724 | 390 |
| Model3/DriveBoard/JoystickBoard.h | 220 | 73 |
| Model3/DriveBoard/SkiBoard.cpp | 267 | 159 |
| Model3/DriveBoard/SkiBoard.h | 159 | 48 |
| Model3/DriveBoard/BillBoard.cpp | 193 | 96 |
| Model3/DriveBoard/BillBoard.h | 118 | 36 |
| Model3/DriveBoard/Z80CTC.cpp | 99 | 72 |
| Model3/DriveBoard/Z80CTC.h | 33 | 46 |
| **Total** | **9345** | **5333** |

### What stayed the same

These are kept as the old build had them:

- **Interface.** The `SCSP_*` functions, `MpegDec::*`, `CSoundBoard`, `CDSB`/`CDSB1`/`CDSB2`, `CDriveBoard` and the four board classes are unchanged. The calls `Model3.cpp`, `Main.cpp` and the debugger make still compile.
- **Config names:** `EmulateSound`, `EmulateDSB`, `SoundVolume`, `MusicVolume`, `FlipStereo`, `LegacySoundDSP`, `ForceFeedback` and `ForceFeedbackStrength`.
- **`Music.xml` format** for replacement tracks.
- **Log texts.**
- **What the games see:**
  - the sound 68000's memory map;
  - SCSP registers, timers, interrupts and DSP;
  - the DSB1 Z80 ports and DSB2 68000 map and MPEG command bytes;
  - the drive boards' Z80 memory, ports and CTC;
  - every force-feedback command each board sends;
  - the billboard outputs.
- **Save states.** The block names, field order, sizes and types are all kept:
  - "SCSP x 2" and the "Sound Board" blocks;
  - "DSB1"/"DSB1 Z80" and "DSB2"/"DSB2 68K";
  - "DriveBoard.2", "DriveBoard Z80", "WheelBoard", "JoystickBoard" and "BillBoard";
  - the CTC's raw 48-byte channel array.

  Old states load into the new build, and new states into the old one (checked, see below).

### Old behaviour kept on purpose

Each of these is commented in the code.

- **SCSP LFO tables.** The amplitude and pitch LFO tables form one contiguous array, in the old row order, because some lookups read past their own row into the next one. Voices start with the same default LFO pointers.
- **Sound board memory.** The two chips' RAM and the four output buffers stay one block, in the old order, because a voice can read past the end of its chip's RAM.
- **DSB memory.** RAM and the MPEG decode buffers are one block in each board. A DSB2 long read at the end of RAM or ROM takes its second word from what follows. DSB2 byte reads see RAM only up to F0FFFF.
- **DSB1 reset leaves a stale loop length.** `CDSB1::Reset` clears the saved loop start but not the loop length. A state saved after a reset therefore resumes a loop at MPEG ROM offset 0.
- **A loop with no MPEG frame never ends.** If a looped stream holds no MPEG frame (a negative or tiny length, or such a reset loop over data that is not MPEG), `DecodeAudio` loops forever, in the old build and the new.
- **Replacement track looping to a ROM offset.** Suppose a Music.xml replacement file is playing and a loop update names a plain ROM offset. The position is then moved by the 32-bit distance between the ROM and the file's buffer, as before. The result depends on where the buffers sit in memory.
- **Z80 CTC.**
  - A time constant of 0 means 256 but is stored in 8 bits as 0, so computing the NMI rate then divides by zero.
  - A channel in counter mode counts the previous channel, so channel 0 would read channel −1.
  - The NMI period uses unsigned division.
- **Drive boards.**
  - The ROM window is 0000–8FFF.
  - The wheel board loads its own block first and loads nothing if it is missing. The joystick board loads the common block first.
  - The wheel board counts as started on reply 0xCC, the joystick board on 0xCB.
  - The joystick board stops effects without checking that inputs are attached.
  - The ski pad's status byte counts up on every read.
  - The unused simulation code of the wheel and joystick boards is kept.

### Structure

**SCSP**
- Voices, envelopes and the LFO oscillator are small structs.
- The register map is decoded through tables of word pointers.
- The DSP is one interpreter function over the microcode.

**Sound board**
- The 68000 map is decoded on address bits 23–20.

**DSB**
- One upsampler (`DsbUpsampler`) is shared by both boards.
- The DSB2's MPEG command parser is one switch. Its state values are kept, because they are saved.

**MpegAudio**
- Replacement tracks are kept as shared byte buffers keyed by ROM offset. Each file is read once.

**Drive boards**
- The base class holds the common Z80, RAM, CTC and NMI timing.
- Each board decodes its own ports.
- The ADC, the 7-segment displays and the motor ports are small tables or helpers.

### Shared headers touched

None outside the batch. Inside it, the frame-size macro in `DSB.h` is now `DSB_SAMPLES_PER_FRAME`. Its only other user is `SoundBoard.cpp`.

## Fixes and deliberate differences

- **SCSP: no crash through the LFO alias.** In the old build, a byte write to voice 0's LFO register through the 0x30–0x4F alias of the common registers crashed when the voice's LFO had never been set up. In the new build every voice's LFO is valid from reset.
- **Zero instead of indeterminate values.**
  - CDSB1 never wrote its command latch, which is in its save state, and some of its other registers started unset.
  - CJoyBoard's displays, ADC and motor fields were unset until its first Reset.

  All of these now start at 0. The old build's value there was whatever was in memory.
- **Z80 CTC padding byte.** The padding byte at the end of each 12-byte CTC channel is an explicit member, kept zero. The layout is unchanged.

## What was checked

Each old file and its new version were compiled side by side into old and new harness programs. They got the same generated inputs, and their outputs were compared bit for bit. The harnesses are kept in the session scratchpad, not in the repo.

**SCSP** (`SCSP.cpp`, `SCSPDSP.cpp`, `SCSPLFO.cpp`). Two chips with RAM were driven by random register and memory write sequences:
- every voice register, including loop modes, 8- and 16-bit PCM, noise, envelopes, LFO waves and depths, FM sources and the direct, DSP and effect sends;
- the common registers: timers, interrupt enable, pending and acknowledge, MIDI in and out, and the slot monitor;
- random DSP microprogrammes;
- both values of `LegacySoundDSP` and several balance settings.

Each seed ran 300 frames, with a save state at the middle frame. The checks:
- each frame's output for all 4 channels;
- the save state, compared field by field;
- the new build loading the old state and running on;
- the old build loading the new state.

Result: 400 seeds, 120,000 frames, 0 differences. Coverage was 97.4% of `SCSP.cpp` and 100% of `SCSPDSP.cpp`; the lines not reached are error paths.

**Sound board** (`SoundBoard.cpp`). The SCSP, 68000, DSB and audio output were stubs that record every call. Each seed ran 400 rounds of random reads and writes of 8, 16 and 32 bits over the whole 24-bit map, mixed with:
- MIDI bytes and the bank register;
- changes to `EmulateSound`, `SoundVolume` (−30 to 230) and `FlipStereo`;
- resets, save/load and frames.

Result: 400 seeds with identical call logs, mixed output, save states and final RAM.

**DSB and MpegAudio** (`DSB.cpp`, `MpegAudio.cpp`). The Z80 and 68000 were stubs whose `Run` drives the board's own bus: ports, memory, IRQ and the MPEG command bytes. Decoding was real, through minimp3, over an MPEG ROM built from real MP2 tracks (32 kHz stereo and mono). Seeds alternated DSB1 and DSB2. Half the seeds also loaded a Music.xml with:
- MP3 and MP2 replacement tracks;
- duplicate, missing and out-of-range entries;
- a file mapped at two offsets.

Each seed ran 600 frames, with random command FIFO bytes (including overflow), `EmulateDSB` and `MusicVolume` changes, resets, and save, run and load-back.

The comparison covered the mixed 44.1 kHz output of every frame, the stream position, the save states and every bus read.

Result: 2000 seeds × 600 frames before the final rename and 1000 seeds × 600 frames after it, 0 differences. Coverage was 97.5% of `DSB.cpp` and 95.6% of `MpegAudio.cpp`; the lines not reached are error paths and one parser state that is never entered.

The harness avoids two old-code dead ends that make both builds spin or read wild memory:
- loops over regions with no MPEG frames;
- ROM-offset loop updates while a replacement file plays (see above).

**Drive boards** (all of `DriveBoard/`). Seeds rotated over six cases: the wheel, joystick, ski and billboard boards, an attached base board, and the dummy unattached board. The Z80 stub's `Run` drives:
- memory and ports;
- the ADC bit reads;
- motor data and control bytes, including every control value the boards decode;
- replies to the PowerPC;
- CTC programming, kept to settings that do not divide by zero, with NMI timing through `RunFrame`.

Each seed also used the PowerPC side (commands including 0xCB, reads), DIP switches, force feedback strength, `ForceFeedback` on and off, resets, save/run/load, and loading a state without the board's blocks. The comparison covered every force-feedback command (axis, effect, strength), the billboard outputs, every read, the save states and the RAM.

Result: 3000 seeds × 600 frames, 0 differences, both before and after the final rename. Coverage:

| File | Coverage |
|---|---:|
| DriveBoard.cpp | 97% |
| SkiBoard.cpp | 98% |
| BillBoard.cpp | 96% |
| Z80CTC.cpp | 97% |
| WheelBoard.cpp | 76% |
| JoystickBoard.cpp | 74% |

The wheel and joystick lines not reached are the simulation code, which is never enabled, and a saved state claiming simulation.

**Build.** All rewritten files and their users (`Model3.cpp`, `Main.cpp`, `SupermodelDebugger.cpp`) compile with g++ `-Wall` without warnings. The Windows cross-build (`build.sh windows`) succeeds.

## Left in this batch

Nothing.
