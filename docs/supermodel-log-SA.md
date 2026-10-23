# Supermodel rewrite, batch SA: CPU cores

This batch covers everything under `supermodel/Src/CPU/` except
`CPU/68K/Musashi/`, which is third-party and was left alone.

Every file in the batch has been rewritten as RetroPixels' own code. Each
one starts with the line
`// Original code for RetroPixels (Supermodel). Not derived from Supermodel.`
Supermodel was read only for its interface: the names other files call,
the save-state layout and what a game sees. The debugger hooks and every
`#ifdef SUPERMODEL_DEBUGGER` block are gone. `Src/Debugger` itself was
not touched.

## What was rewritten

| File | Lines before | Lines after |
|---|---:|---:|
| CPU/Bus.h | 110 | 34 |
| CPU/PowerPC/ppc.h | 388 | 113 |
| CPU/PowerPC/ppc.cpp | 1163 | 615 |
| CPU/PowerPC/ppc603.c | 356 | 186 |
| CPU/PowerPC/ppc_ops.h | 170 | 172 |
| CPU/PowerPC/ppc_ops.c | 2868 | 1037 |
| CPU/PowerPC/PPCDisasm.h | 60 | 24 |
| CPU/PowerPC/PPCDisasm.cpp | 1259 | 1043 |
| CPU/Z80/Z80.h | 339 | 134 |
| CPU/Z80/Z80.cpp | 4014 | 1170 |
| CPU/68K/68K.h | 392 | 85 |
| CPU/68K/68K.cpp | 454 | 187 |
| **Total** | **11573** | **4800** |

- **Bus.h**: the `IBus` interface. Method names and defaults are
  unchanged.
- **PowerPC 603e interpreter** (`ppc.h`, `ppc.cpp`, `ppc603.c`,
  `ppc_ops.c`, `ppc_ops.h`):
  - Decoding is table-driven.
  - The FPU, CR and XER arithmetic, SPRs, the MMU stubs, exceptions, the
    decrementer and time base, the fetch regions and the
    `PPCSaveState`/`PPCLoadState` block ("PowerPC") all keep the old
    public API.
  - The save-block byte layout is unchanged.
- **PPCDisasm**: a new keyed-table disassembler with the same entry point
  and the same text output. The standalone `-DSTANDALONE` build has a new
  `main`.
- **Z80**:
  - Decoding now follows the x/y/z/p/q fields of the opcode.
  - Cycle costs are worked out by rule instead of from big tables, and
    reproduce the old costs exactly.
  - The flag helpers model the same undocumented X/Y and quirk behaviour
    as before.
  - The public `CZ80` API is unchanged, and so is the save-state field
    order.
- **68K wrapper**:
  - It still drives Musashi.
  - `M68KCtx`, all `M68K*` functions and the `extern "C"` handlers that
    Musashi calls are unchanged.
  - The 34-word save block keeps its order. The order now lives in one
    table, with a static_assert that the block stays 34 words.
  - `M68KSetARegister` and `M68KSetDRegister` were declared before but
    never defined. They are now defined.

No file outside the batch was changed.

## Deliberate fixes (a game can see these; all are hardware-correct)

1. **PPC FPU NaN propagation.**
   - When two or three inputs are NaN, the result is now the first NaN in
     architected order: frA, then frB, then frC. It is quieted, and single
     precision truncates it.
   - The old build left this to the host compiler's operand order. As a
     result, for `inf*0 + NaN(frB)` it returned the host default NaN.
   - In `fnmadd`/`fnmsub`, the negation now happens before rounding to
     single precision. It also flips the sign of a propagated NaN.
2. **PPC `neg`/`nego` of 0x80000000.**
   - The new build sets OV and SO and gives CR0 = LT, as the hardware does.
   - The old build used a signed-overflow negation, which is undefined in
     C. At -O3 it never set OV and gave CR0 GT.
3. **Disassembler:**
   - SPR 978 is now printed as `hash1`. The old build said `hash2`, which
     is SPR 979's name.
   - The standalone tool's return-code logic was inverted; that is fixed.
4. **Z80 block instructions with a zero count.**
   - LDIR, LDDR, CPIR and CPDR with BC=0 now run 65536 iterations.
   - INIR, OTIR, INDR and OTDR with B=0 now run 256.
   - The old build counted in 32 bits and so ran 2^32 iterations. In
     effect, the emulator hung.

## What was checked

All harnesses build the old and the new source as separate shared
libraries. Each library is loaded with `RTLD_LOCAL|RTLD_DEEPBIND`, and
both run the same inputs in lockstep.

### PowerPC

**What the harness generates:**
- Random instruction streams of integer, FPU (single and double, all
  rounding modes, NaN, infinity and denormal operands), CR, SPR, load and
  store (all widths, update and indexed forms, string and multiple), and
  branch instructions, plus `sc`, `rfi`, `mtmsr`, `tw` and `twi`.
- A random machine state and random PVR, multiplier and frequency
  configurations.
- External interrupt line changes, random SPR writes, PC changes and
  timer-ratio changes between slices.

**What it compares after every slice:**
- the full save block
- the bus access trace, by hash and count
- memory
- the return value, which is the cycle count
- the time base and decrementer
- the error count

On a mismatch, the harness replays the slice one instruction at a time to
find the first differing instruction.

**Results:**

| Run | Seeds | Slices | Instructions | Mismatches |
|---|---|---:|---:|---:|
| big1 | 0–24999 | | 261M | 0 |
| big2 | 100000–124999 | | 261M | 1, harness noise (see below) |
| big3 | 200000–239999 | 606,162 | 419M | 1, harness noise (see below) |

- big3 also had 21,172 stopped cases, 26,557 rearms and 18,340 NaN-choice
  resyncs, 175 of them nego.
- The cases where fixes 1 and 2 apply are recognised by a classifier. For
  each one, the harness checks that only the result register changed and
  then resyncs the new core from the old.

**The two leftover mismatches are not core differences:**
- Both are fix 1, a `fnmadds` with two NaN inputs.
- In each case the classifier could not run, because the step-by-step
  replay diverged from the full run.
- The cause: a pending decrementer exception, raised while MSR[EE]=0, is
  not part of the save block. It never was, in either build. So a replay
  that starts from a saved state loses it.
- I traced seed 207757 in full to confirm this.

**Benchmark (200M instructions):** old 0.85 s, new 0.90 s.

### Disassembler

- The harness covers every 32-bit word, in both modes (simplified on and
  off), across 2^32 × 2 calls. It compares the return code, the mnemonic
  and the operand text.
- Result over all 4,294,967,296 words × 2 modes:
  - 5,755,255,390 calls matched with valid output.
  - 62,456,576 matched as invalid forms.
  - 2,772,222,434 matched as unknown opcodes.
  - 192 differ, all the SPR 978 name (fix 3).
  - 0 other differences.

### Z80

**What the harness generates:**
- Random code with every prefix (CB, DD, FD, ED, DD-CB, FD-CB, repeated
  prefixes and unused ED opcodes).
- Random register and flag state, loaded through the save block.
- In random slices: NMI, INT assert and release, IM 0, IM 1 and IM 2 with
  and without an acknowledge callback, Reset, and step replay.
- The acknowledge callback returns RST opcodes, vector bytes, junk or -1,
  and sometimes releases the line from inside the callback.

**What it compares:**
- memory and I/O access traces, by hash
- cycle counts returned by `Run`
- PC
- the save block
- the acknowledge count
- errors

**Results:**
- 30,000 cases, 693,839 runs, 760M cycles: 0 mismatches.
- 2,222 cases stopped early, where the old core ran away on a zero-count
  block repeat (fix 4).
- Benchmark (200M cycles): old 0.60 s, new 0.89 s. The Z80 on the DSB only
  needs a few MHz, so this is plenty.

### 68K wrapper

**What the harness generates:**
- Two contexts, set up the way the sound board and DSB do it. One has an
  acknowledge callback, which returns autovector, spurious or vector
  numbers and sometimes drops the line itself. The other has none.
- Random 68000 code and vector tables.
- Random runs, IRQ level changes, context swaps and resets.
- Saves from the old build loaded into both builds, and saves from the new
  build loaded into both.
- Random save blobs with valid run-state words.
- A missing save block.

**What it compares after every step:**
- D and A registers through the getters, including out-of-range indexes
- PC
- the bus trace and memory of both buses
- the save block's bytes, name and comment
- the acknowledge count
- errors

**Results:**
- 20,000 cases, 539,952 runs, 1.48G cycles, 204,348 context swaps,
  180,063 saves, 204,169 loads: 0 mismatches.
- A deliberately broken wrapper (the default acknowledge path no longer
  drops the line) was caught in 145 of 200 cases.

### Save states and NVRAM

- **PPC:** the "PowerPC" block has the same bytes. Every harness case
  loads a state, then checks that saving it again gives the same bytes and
  that both builds produce identical blocks.
- **Z80:** the block keeps the same field order and size, checked byte for
  byte after every slice.
- **68K:** the block is the same 34 words in the same order, with the same
  name and comment. The harness loads old-build saves into the new build
  and the other way round.
- The CPU cores keep no NVRAM.

### Windows build

- `supermodel/build.sh windows` builds `supermodel.exe`.
- There are no warnings in any file of this batch. The 7 warnings in the
  log are in Gui.cpp, FileSystemPath.cpp, GameLoader.cpp and zconf.h.
- The new files also build clean on Linux with `-Wall -Wextra`.

## What is left

- Nothing in the batch is still Supermodel code. `CPU/68K/Musashi/` is
  third-party and is out of scope.
- `Src/Debugger/CPU/*` still refers to the debugger hooks that were
  removed. That code only builds with `SUPERMODEL_DEBUGGER`, which
  RetroPixels does not use, and the brief says to leave `Src/Debugger`.
- Musashi's `m68kconf.h` still turns on its instruction hook under
  `SUPERMODEL_DEBUGGER`. The hook's target, `M68KDebugCallback`, no longer
  exists, so a debugger build would not link. Non-debugger builds are
  unaffected.
