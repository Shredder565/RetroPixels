# Supermodel rewrite: batch SF (inputs and util)

Every file in `supermodel/Src/Inputs/` and `supermodel/Src/Util/` was rewritten as original RetroPixels code. Each file starts with the RetroPixels original-code line, has no Supermodel licence header, and keeps its file name. All files use LF line endings. The debugger hooks are gone from these files: `uiEnterDebugger` and every `#ifdef SUPERMODEL_DEBUGGER` block. `Src/Debugger` itself was left alone.

## Rewritten

| File | Lines before | Lines after |
|---|---:|---:|
| Inputs/Input.cpp | 217 | 174 |
| Inputs/Input.h | 228 | 133 |
| Inputs/InputSource.cpp | 140 | 73 |
| Inputs/InputSource.h | 111 | 72 |
| Inputs/InputSystem.cpp | 2755 | 1966 |
| Inputs/InputSystem.h | 1039 | 418 |
| Inputs/InputTypes.cpp | 235 | 200 |
| Inputs/InputTypes.h | 172 | 127 |
| Inputs/Inputs.cpp | 805 | 656 |
| Inputs/Inputs.h | 311 | 227 |
| Inputs/MultiInputSource.cpp | 222 | 152 |
| Inputs/MultiInputSource.h | 109 | 65 |
| Util/BMPFile.h | 426 | 213 |
| Util/BitCast.h | 57 | 36 |
| Util/ByteSwap.cpp | 71 | 24 |
| Util/ByteSwap.h | 34 | 23 |
| Util/ConfigBuilders.cpp | 331 | 250 |
| Util/ConfigBuilders.h | 41 | 39 |
| Util/Format.cpp | 110 | 73 |
| Util/Format.h | 129 | 139 |
| Util/GenericValue.h | 299 | 269 |
| Util/NewConfig.cpp | 414 | 216 |
| Util/NewConfig.h | 284 | 247 |
| Util/Test_BitRegister.cpp | 338 | 229 |
| Util/Test_Config.cpp | 228 | 191 |
| **Total** | **9106** | **6212** |

The table above covers Inputs (6344 → 4263 lines) and Util (2762 → 1949 lines).

### What stayed the same

- **Public interface.** Class names, the methods other files call, the macros, the enums and the settings/details structs are unchanged.
- **Ini names.** `Input<id>`, `InputDigitalSensitivity` and `InputKey|Mouse|Joy[n]...` are unchanged.
- **Mapping-string grammar.** Keys, bare key names, `MOUSE[n]_` and `JOY[n]_` parts and their aliases, `_INV`/`_POS`/`_NEG`, POVs, `!` negation, `+` and `,`, and `NONE` all work as before, including the tokenizer's quirks with quotes and spaces.
- **Console texts.** The texts printed by Configure, Calibrate and Detect are unchanged, typos included.

### What changed in the structure

- The source classes nested in `CInputSystem` are now only declared in the header.
- The source cache is a vector per device kind.
- The key, mouse-button, POV and joystick-button sources share one ramp base class.
- The `CInputs` table is built by helpers and loops.
- `ConfigureInputs` is a small state machine.

### Shared headers touched

None outside the batch. Inside the batch, `Util/Format.h` still includes `<iomanip>` and `Util/GenericValue.h` still includes `<cctype>`, as the old headers did. Other files rely on them: `Main.cpp` uses `std::setw` and `std::setfill` through them.

## Fixes and deliberate differences

- **RP_FLIGHTSTICK smoothing state.** The state now lives in each joystick-axis source. Before, it was a global map keyed by the source's address. The numbers are identical: the cached sources are the same objects in both versions. The only difference arises when a new source reuses the address of a freed one within 0.25 s; the old code would then inherit the stale state.
- **BMP file-size field.** `BMPFile.h` now writes the real file size. The old code wrote header + w·h·3, ignoring row padding.
- **INI "File read error" log.** The log call in `ConfigBuilders.cpp` now gets the arguments its format string needs.
- **Uninitialised values.** These were undefined behaviour in the old code; they never change results:
  - `ValueAs<number/bool>` on an empty or whitespace-only string now reads a value-initialised result instead of an uninitialised one.
  - `CInputSource::IsActive()` now passes an initialised `bool`. In the old code, UBSan reports "load of value 24/56/128, not a valid value for type bool" from `CNegInputSource`.
- **`Util::Format`.**
  - `Write` no longer empties the stream.
  - `Format(str) << x` appends to `str` instead of overwriting it. No caller relies on the old behaviour.
- **`Test_BitRegister.cpp`.** It is self-contained, with a small local bit-string class. The `Util/BitRegister.h` it tested does not exist in this tree or upstream, so the file was never built. All 40 checks pass.
- **`Test_Config.cpp`.** Rewritten with its own checks. They pass, and the old Test_Config also passes against the new code.
- **Leaks.** The old code leaks some "any device" multi sources at shutdown; the new code leaks the same objects (ASan shows the same allocation sites in both). This was not changed, because the leak comes from the reference-count rules that ini mappings depend on.

## Checks (old vs new, side by side)

The harnesses build the old and new files into separate binaries and diff their outputs. They live in the session scratchpad (`sf/h/`), not in the repo.

### Util

Three seeds each. Every case was identical:

| Test | Cases per seed |
|---|---|
| Format, Hex, Join, Split, ToLower, Trim, Stricmp | 2000 |
| ByteSwap | 2000 |
| GenericValue conversions, including hex, bool words and ranges | 3000 |
| Node operation scripts (40 steps each, every Node method, including placeholders, copy/move, iterators and `PrintConfigTree`) | 400 |
| Generated INI files: parse (with sections, errors and quotes), write and merge | 1500 |
| Generated XML configs | 1500 |
| BMP surfaces × 14 pixel formats, plus all 65536 texels of each format | 150 |

The BMP comparison excluded the file-size field.

### Inputs

The harness uses a mock `CInputSystem` driven by scripted device state:
- 1–4 keyboards (or "any"), 0–3 mice (or "any") and 0–5 joysticks, with random axes, POVs, buttons and force-feedback details;
- a back end that lacks some keys, sometimes with `SHIFT` and sometimes without `RIGHTCTRL`;
- a fake `steady_clock`, advanced in steps of 0 to 260 ms.

Each run was repeated under seven RP_FLIGHTSTICK settings:
- unset;
- `curve=60,dead=5,smooth=60`;
- `curve=100,dead=20,smooth=0`;
- `on`;
- `off`;
- `curve=-5,dead=80,smooth=900`;
- `smooth=16,curve=30`.

There are three modes:
- **parse**
  1. Random settings ini: load, print and store.
  2. `PrintDevices`.
  3. Per run, 200–400 generated mapping strings, mixing:
     - valid and invalid parts, device numbers (including 0, 01 and huge numbers) and case;
     - `!`, `+` and `,`;
     - quotes and spaces, `NONE` and junk.
  4. Each string is parsed for axis and non-axis inputs, and the resulting source type is printed.
  5. Every source is then read over 150 steps of changing device state: switch value, `IsActive`, and analog values in 6 ranges (including reversed ranges). Sources are skipped at random on some steps, to test time-based smoothing.
  6. Force-feedback calls.
- **inputs**
  1. A full `CInputs` with random `Input<id>` mappings and auto-trigger settings.
  2. `PrintInputs`, `GetGameInputs`, and lookup by id and by label.
  3. Store, then 300 `Poll`s for random games, printing every input value and `Changed`, with occasional `DumpState`.
  4. Random `SetMapping`, `AppendMapping`, `ResetToDefaultMapping` and `ClearMapping` calls.
- **read**, on scripted presses, sweeps and releases:
  - 120 `ReadMapping` calls (all read flags, short buffers, other escape keys);
  - 40 `CInput::Configure` calls;
  - 30 `DetectJoystickAxis` calls;
  - 30 `CalibrateJoystickAxis` calls;
  - 12 menu-driven `ConfigureInputs` sessions;
  - 12 `CalibrateJoysticks`/`CalibrateJoystick` sessions.

Results:
- **Plain build:** 504 runs (24 seeds × 3 modes × 7 RP_FLIGHTSTICK settings), about 260 MB of output, all identical. About 50,000 parsed mapping strings.
- **ASan + UBSan build:** 63 runs, all identical. No findings in the new code.

The harness found one bug in the rewrite, which was fixed before the runs above: with a device count of "any" (-1), the shared keyboard settings were not loaded.

### Build

`sh supermodel/build.sh windows` (MinGW-w64) builds `supermodel.exe` from a clean work folder with no warnings in `Src/Inputs` or `Src/Util`.

## Left

Nothing in this batch.
