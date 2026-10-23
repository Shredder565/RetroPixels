# Supermodel rewrite log: SB2 (Model 3 board and game loader)

Batch SB, second part, on top of b1ebe6a (the board devices). Brief:
`docs/supermodel-original.md`. I read each Supermodel file only for its
interface, then wrote it again. Each file starts with the
original-code line and keeps its name.

## Files rewritten

| File | Lines | What it is |
|---|---:|---|
| `Src/Model3/Model3.h` | 221 | `CModel3`: the board, its memory and devices |
| `Src/Model3/Model3.cpp` | 2063 | PowerPC memory map, I/O and security boards, system registers, frame loop and threads, save states, game loading |
| `Src/Model3/Model3GraphicsState.h` | 125 | the Real3D/tile generator state used by the graphics viewer |
| `Src/GameLoader.h` | 90 | `GameLoader` interface |
| `Src/GameLoader.cpp` | 799 | reads `Games.xml`, finds a game's files in zips, builds the `ROMSet` |

The public interfaces stay the same, so callers didn't change. Some private
members of `CModel3` were never used, and I dropped them: the synced sound
thread flag, its semaphore and its thread function.

## How I checked them

Each file was compared old against new: the old code is b1ebe6a, the new
code is this worktree. Both builds are fed the same random inputs, and their
traces must match byte for byte.

### `CModel3` (`Model3.cpp`, `Model3.h`)

The harness builds both versions headless. Fakes stand in for the OSD,
renderers and net boards, and they record every call they get. For each seed
it picks at random:
- the game name;
- the MPEG board type;
- the encryption key;
- whether a net board is present and attached;
- the input flags;
- the drive board type;
- the ROM sizes (CROM, banked CROM, VROM, samples, MPEG, drive);
- config settings (PowerPC frequency, LegacySoundDSP, Network, SimulateNet).

Then it runs 20000 random operations:
- reads and writes of 8, 16, 32 and 64 bits over the whole PowerPC map. The
  addresses are 1 in 20 fully random, the rest picked from a list of every
  area of the map (RAM, CROM, banked CROM, Real3D, tile generator, SCSI, the
  C0/C1/C2 areas, the MPC105/106 config ports, I/O, backup RAM, RTC,
  security, the sound board);
- random inputs;
- SaveState, then RAM changes, then LoadState;
- NVRAM save, clear and load;
- RunFrame and Reset.

Every read result is traced, along with:
- each memory area's hash after load and at the end;
- the final save-state hash;
- the audio sample hash;
- a PCI config read;
- pause and resume.

Model 3 steps 1.0, 1.5, 2.0 and 2.1 were all tested (MPC105 versus MPC106,
and the net board versus SCSI at C0). The tests ran under `setarch -R`, with
`time()` pinned. The SCSP's save state holds host pointers, and the RTC reads
the clock.

Before my changes, the harness itself made both versions crash or run away
in the same way. I fixed this in the harness only:
- The Real3D buffers polygon RAM and high culling RAM writes in an update
  list while a ping-pong flip waits. The list has no bound, so random writes
  run past `cullingRAMLo` and segfault in `CReal3D::WritePolygonRAM`. That
  happens in both versions and is a Real3D issue. The harness now writes
  those areas in runs, and moves the write elsewhere while a flip waits.
- Random sound programs drove the SCSP into a crash in `SCSP_UpdateSlot`.
  The sound 68K now idles on `BRA.S *`.
- Random CROM code could spin, and the PowerPC now idles at FFF00100 on
  `b .`. Random lengths in the Real3D DMA registers made one DMA read
  gigabytes through the bus, filling the trace with "unknown R32". Those
  registers now get short lengths. Each trace is also capped at 20 MB.

Results: 44 runs, every one SAME, none hit the cap. Each run is 20000
operations and about 27000 to 30000 trace lines. The runs were seeds 2 to 10
at steps 1.0, 1.5, 2.0 and 2.1, plus seeds 101 to 103 with
`MultiThreaded` on and `GPUMultiThreaded` random. Between them they covered
these games: getbass, getbassur, lostwsgo, scud, spikeout, magtruck and xyz.
They also covered:
- DSB1, DSB2, DSB3 and no MPEG board;
- encrypted and plain sets;
- net board attached and absent;
- every drive board type.

Every run finished: frames, save and load, delete.

### `GameLoader`

The generator writes fake zips for every game in `Games.xml` (63 games).
CRC-32s are forged with a GF(2) solve, so each file carries the CRC the XML
asks for. It makes good sets, sets with files missing, wrong sizes, wrong
CRCs and wrong names. It also makes clone-only zips, parent-plus-clone zips
and zips holding several games. Then it writes 13 XML variants: the real file
plus mutated ones with missing attributes, bad values, unknown parents and
patches. Both loaders load every zip under each variant. I compared:
- the games list;
- each loaded ROM region's size and hash;
- the patches;
- the messages.

Seeds 1 to 4 (about 520 zips each): every variant is the same.

## Behaviour kept on purpose

- `LoadGame` copies only 32 × 100000 bytes (3,200,000) of a VROM of up to
  32 MB, as before. The same goes for `Model3GraphicsState.h`.
- The MIDI IRQ loop in a frame runs at most 129 times.
- A Read16 or Write16 at C0 always goes to the net board.
- GameLoader still gives a default entry to a parent that is missing from
  `Games.xml`. An unknown audio type still means mono.

## One change

GameLoader now names the zip file, not the file inside it, in four messages:
"not sized in chunks", "Unable to locate", "Unable to read" and "CRC error".
The old code printed the inner name where it meant the zip.

## Builds

- `build.sh windows` built `supermodel.exe`.
- `build.sh linux` built `supermodel`. These all worked under Xvfb:
  - `-help`;
  - `-print-games` (63 games);
  - `-print-gl-info`;
  - a missing ROM set (error message and log);
  - a generated fake `scud` zip, which loaded and printed its game info.
- `tools/supermodel-original.py`: 6390 of 83337 Supermodel lines are now
  original (7.67%).
