# W43 behavioural specification: Triforce, magnetic card readers, modem, AGP

This is phase A of batch W43 of the PixelU-Wii rewrite (see
`docs/pixelu-wii-original.md`). W11, W12 and W13 left these files as Dolphin
code because the hardware is only described by Dolphin's own code. W43 uses a
two-team clean room: the phase A worker read the old files and wrote this
specification; the phase B worker, who never sees the old `.cpp` files,
writes new files from this document alone.

Files covered (all under `pixelu/wii/Source/Core/Core/HW`):

| File | Section |
|---|---|
| `DVD/AMMediaboard.cpp/.h` | 2 |
| `SI/SI_DeviceAMBaseboard.cpp/.h` | 3 |
| `Triforce/IOPorts.cpp/.h` | 4 |
| `Triforce/JVSIO.cpp/.h` | 5 |
| `Triforce/FZeroAX.cpp/.h`, `MarioKartGP.cpp/.h`, `Touchscreen.cpp/.h` | 6 |
| `Triforce/ICCardReader.cpp/.h` | 7 |
| `Triforce/DeckReader.cpp/.h` | 8 |
| `MagCard/MagneticCardReader.cpp/.h`, `C1231BR.cpp/.h`, `C1231LR.cpp/.h` | 9 |
| `EXI/EXI_DeviceBaseboard.cpp/.h` | 10 |
| `EXI/EXI_DeviceAGP.cpp/.h` | 11 |
| `EXI/EXI_DeviceModem.cpp/.h` | 12 |

`Triforce/SerialDevice.cpp/.h` is already original (W13) and is used as is.

Sections 13 to 16 cover undefined behaviour in the old code, every
user-visible and logged string, the key/credential findings, and the test
plan with the old-vs-new harness in `tools/w43-harness/`.

## 0. Rules for the phase B implementer

- Write every file from this document. Do not open the old `.cpp` files, the
  old headers beyond the declarations repeated here, Dolphin upstream, or
  YACardEmu (the old `MagCard` files say they are based on YACardEmu, which
  is GPL too; its code is just as off limits).
- The header-level declarations in each section (class names, public and
  protected member functions, enums and their values, struct layouts,
  constants other files use) may be kept as they are. Private members may be
  renamed or restructured unless the section says other files depend on them.
- Start each file with `// Original code for RetroPixels (PixelU-Wii). Not
  derived from Dolphin.` and no Dolphin copyright or SPDX line.
- Keep behaviour byte for byte as specified, including the quirks marked
  **Quirk**. Where the old code had undefined behaviour (section 13), this
  document says what to do instead.
- Log messages: section 14 lists every old log format string. Keeping them
  verbatim lets the harness's log hash match; rewording debug/log text that
  users don't see is allowed by the method (as in W11-W13) but then only the
  behaviour hash is expected to match. Panic alerts, OSD messages, on-screen
  display messages and translatable strings (also section 14) must stay
  verbatim.
- MSVC builds with /W4 and warnings as errors: write narrowing and sign
  changes as explicit casts with the same values.
- The harness builds on Linux, so the media board's socket code needs a
  POSIX branch next to the Windows one (section 2.10).

## 1. Conventions used below

- **Byte order.** "BE" = big-endian, "LE" = little-endian. The emulated
  console is big-endian. `Memory::MemoryManager::Write_U16/Write_U32` store BE;
  `Write_U32_Swap(v, a)` stores `v` byte-swapped, i.e. the bytes of `v` in LE
  order. `Read_U8/Read_U32` read BE. "Host" means the x86-64 host (LE).
- **Savestates.** `PointerWrap::Do` stores trivially copyable values as raw
  host bytes (so a `u32` is 4 LE bytes, a `bool` 1 byte, a `std::array` its
  raw bytes with no count), `std::vector`/`std::string`/`std::deque` as a
  `u32` element count followed by the elements, and `std::optional<T>` as a
  1-byte presence flag followed by `T` if present. Sections give the exact
  `DoState` order; sizes are in bytes.
- **`Common::ExpandValue<T>(v, n)`** (in `Common/BitUtils.h`): shifts `v`
  left by `n` and fills the `n` low bits with copies of bit 0 of `v`. So an
  8-bit stick value `0x80` becomes `0x8000`, `0x81` becomes `0x81FF`.
- **SerialDevice** (original, W13): `WriteRxBytes` appends to the inbound
  byte vector, `GetRxByteSpan` views it, `ConsumeRxBytes(n)` drops the first
  `n`, `WriteTxByte(s)` append to the outbound deque, `TakeTxBytes(span)`
  removes the first `span.size()` outbound bytes into `span`,
  `PassThroughTxBytes(other)` moves all of `other`'s outbound bytes to the end
  of ours. `SerialDevice::DoState` saves inbound (vector) then outbound
  (deque), each as a `u32` count plus bytes.
- **Triforce user folder.** `File::GetUserPath(D_TRIUSER_IDX)` is the
  `Triforce/` folder in the user directory, with a trailing separator. All
  file names below are relative to it unless stated. `<GameID>` is
  `SConfig::GetInstance().GetGameID()` (6 characters for discs).
- **File primitives.** `File::IOFile` wraps C stdio (`fopen` modes as given;
  short reads fail and leave the rest of the destination untouched; a failure
  clears `IsGood()` until the next open). `File::DirectIOFile` keeps its own
  position; `AccessMode::Write` with the default `OpenMode` truncates,
  `OpenMode::Always` creates if missing without truncating;
  `OffsetWrite(off, data)` writes at `off`, extending the file with zeros if
  needed.
- **Log categories** are the existing `Common::Log::LogType` values
  `AMMEDIABOARD`, `AMMEDIABOARD_NET`, `SERIALINTERFACE`,
  `SERIALINTERFACE_AMBB`, `SERIALINTERFACE_CARD`, `SERIALINTERFACE_JVSIO`,
  `SP1` and `EXPANSIONINTERFACE`.

## 2. AM media board (`DVD/AMMediaboard.cpp/.h`)

### 2.1 Role

On a Triforce the GameCube's DVD interface talks to the AM media board (a
DIMM board holding the game, or a GD-ROM/NAND board) instead of a disc
drive. `DVDInterface` (original) calls into this module when
`Core::System::IsTriforce()`:

- `DVDInterface::Init` calls `AMMediaboard::Init()`; `Shutdown` calls
  `AMMediaboard::Shutdown()`; `DoState` calls `AMMediaboard::DoState(p)` and
  then `p.DoMarker("AMMediaboard")`.
- `DVDInterface::ExecuteCommand` calls
  `AMMediaboard::ExecuteCommand(m_DICMDBUF, &m_DIIMMBUF, m_DIMAR, m_DILENGTH)`.
  A return value of 0 means "handled": the DI transfer completes at once (TC
  interrupt, DIMAR advanced by the length). Non-zero means "not handled": DI
  shifts `DICMDBUF[0]` left by 24 and runs it as a plain drive command.
  `ExecuteCommand` modifies `dicmd_buf` in place (it de-obfuscates it).
- `Boot.cpp` calls `FirmwareMap(true)` when booting the Triforce IPL;
  `Boot_BS2Emu.cpp` calls `InitDIMM(volume)`, then
  `InitKeys(RAM[0], RAM[4], RAM[8])` (BE words), then `FirmwareMap(false)`.
- `SI_DeviceAMBaseboard`, `ICCardReader`, `EXI_DeviceBaseboard` call
  `GetGameType()`; `IOPorts` calls `GetTestMenu()`.
- DolphinQt's `TriforcePane` calls `ParseIPRedirection`; the debugger's
  `NetworkWidget` calls `GetIPRedirections`, `IPRedirection::Apply/Reverse/
  ToString`, `SOCKET_FD_MAX` and `DebuggerGetSocket`.

### 2.2 Public interface (`AMMediaboard.h`, may be kept)

```cpp
namespace AMMediaboard {
enum GameType { FZeroAX = 1, FZeroAXMonster, MarioKartGP, MarioKartGP2, VirtuaStriker3,
                VirtuaStriker4, VirtuaStriker4_2006, GekitouProYakyuu, KeyOfAvalon,
                FirmwareUpdate };                                   // 1..10
enum MediaType { GDROM = 1, NAND };
enum MediaBoardType { NANDMaskBoardHDD = 0, NANDMaskBoardMask = 1, NANDMaskBoardNAND = 2,
                      DIMMBoardType3 = 4 };
enum MediaBoardStatus { Initializing = 0, CheckingNetwork = 1, SystemDisc = 2,
                        TestingGameProgram = 3, LoadingGameProgram = 4, LoadedGameProgram = 5,
                        Error = 6 };
enum InquiryType { Version1 = 0x21484100, Version2 = 0x29484100 };
struct MediaBoardRange { u32 start; u32 end; u8* buffer; std::size_t buffer_size;
                         MediaBoardRange(u32 start_, u32 size_, std::span<u8> buffer_); };
enum class AMMBDICommand : u16 { Inquiry = 0x12, Read = 0xA8, Write = 0xAA, Execute = 0xAB };
enum class AMMBCommand : u16 { /* values in 2.6 */ };
enum MediaBoardAddress : u32 { /* values in 2.4 */ };
enum SocketStatusCodes { /* values in 2.9 */ };
static constexpr std::size_t SOCKET_FD_MAX = 64;
void Init();
void FirmwareMap(bool on);
void InitDIMM(const DiscIO::Volume& volume);
void InitKeys(u32 key_a, u32 key_b, u32 key_c);
u32 ExecuteCommand(std::array<u32, 3>& dicmd_buf, u32* diimm_buf, u32 address, u32 length);
u32 GetGameType();
u32 GetMediaType();
bool GetTestMenu();
void Shutdown();
void DoState(PointerWrap& p);
struct ParsedIPRedirection { std::string_view original, replacement, description; };
std::optional<ParsedIPRedirection> ParseIPRedirection(std::string_view str);
struct IPRedirection {
  Common::IPv4PortRange original; Common::IPv4PortRange replacement;
  Common::IPv4Port Apply(Common::IPv4Port subject) const;
  Common::IPv4Port Reverse(Common::IPv4Port subject) const;
  std::string ToString() const; };
using IPRedirections = std::vector<IPRedirection>;
IPRedirections GetIPRedirections();
s32 DebuggerGetSocket(u32 triforce_fd);
}
```

The header includes `Common/ChunkFile.h`, `Common/CommonTypes.h`,
`Common/Network.h` and `DiscIO/Volume.h` (other files rely on these
transitive includes only for the types above).

`AMMBCommand` values (several names share a value; both names must exist):

| Name | Value | Name | Value |
|---|---|---|---|
| Unknown_000 | 0x000 | Listen | 0x408 |
| GetDIMMSize | 0x001 | Recv | 0x409 |
| GetMediaBoardStatus | 0x100 | Send | 0x40A |
| GetSegaBootVersion | 0x101 | Socket | 0x40B |
| GetSystemFlags | 0x102 | Select | 0x40C |
| GetMediaBoardSerial | 0x103 | Shutdown | 0x40D |
| GetNetworkConfig | 0x104 | SetSockOpt | 0x40E |
| NetworkReInit | 0x204 | GetSockOpt | 0x40F |
| TestHardware | 0x301 | SetTimeOuts | 0x410 |
| Accept | 0x401 | GetLastError | 0x411 |
| Bind | 0x402 | RouteAdd | 0x412 |
| Closesocket | 0x403 | RouteDelete | 0x413 |
| Connect | 0x404 | GetParambyDHCPExec | 0x414 |
| GetIPbyDNS | 0x405 | ModifyMyIPaddr | 0x415 |
| InetAddr | 0x406 | Recvfrom | 0x416 |
| Ioctl | 0x407 | Sendto | 0x417 |
| InitLink | 0x601 | RecvDimmImage | 0x418 |
| Unknown_605 | 0x605 | SendDimmImage | 0x419 |
| SetupLink | 0x606 | AllNetInit | 0x700 |
| SearchDevices | 0x607 | Unknown_701..704 | 0x701..0x704 |
| Unknown_608 | 0x608 | Unknown_001 | 0x001 |
| Unknown_614 | 0x614 | GetNetworkFirmVersion | 0x101 |
| | | Unknown_103 | 0x103 |

`MediaBoardAddress` values:

| Name | Value | Name | Value |
|---|---|---|---|
| MediaBoardStatus1 | 0x80000000 | DIMMCommandVersion2_2 | 0x89000000 |
| MediaBoardStatus2 | 0x80000020 | DIMMCommandExecute1 | 0x84000040 |
| MediaBoardStatus3 | 0x80000040 | DIMMCommandExecute2 | 0x88000000 |
| FirmwareStatus1 | 0x80000120 | NetworkCommandAddress1 | 0x1F800200 |
| FirmwareStatus2 | 0x80000140 | NetworkCommandAddress2 | 0x89040200 |
| BackupMemory | 0x000006A0 | NetworkBufferAddress1 | 0x1FA00000 |
| DIMMMemory | 0x1F000000 | NetworkBufferAddress2 | 0x1FD00000 |
| DIMMMemory2 | 0xFF000000 | NetworkBufferAddress3 | 0x89100000 |
| DIMMExtraSettings | 0x1FFEFFE0 | NetworkBufferAddress4 | 0x89180000 |
| AllNetSettings | 0x1EFF8000 | NetworkBufferAddress5 | 0x1FB00000 |
| NetworkControl | 0xFFFF0000 | AllNetBuffer | 0x89010000 |
| DIMMCommandVersion1 | 0x1F900000 | FirmwareAddress | 0x84800000 |
| DIMMCommandVersion2 | 0x84000000 | FirmwareMagicWrite1 | 0x00600000 |
| | | FirmwareMagicWrite2 | 0x00700000 |

`MediaBoardRange(start, size, buffer)`: `end = start + min(size,
buffer.size())`, `buffer`/`buffer_size` from the span. If `size >
buffer.size()` it logs the warning `Invalid MediaBoardRange: ...` (section
14). The module's own range table (2.4) contains one such range, so this
warning is logged once during static initialisation, before `main`.

### 2.3 State

All state is module-global (namespace-scope statics):

| State | Type / size | Initial (static) | Reset by `Init` |
|---|---|---|---|
| firmware_map | bool | false | false |
| test_menu | bool | false | false, then true if SegaBoot loaded |
| timeouts | u32[3] | {20000, 20000, 20000} | **not reset** |
| last_error | u32 | 70 (SSC_SUCCESS) | 70 |
| key_a, key_b, key_c | u32 | 0 | 0 |
| firmware | u8[0x200000] | zero | filled with 0xFF, then SegaBoot |
| media buffer | 0xC0 u32 = 0x300 bytes (byte view and u32 view of the same memory; the u32 view is host LE) | zero | zero |
| exec1_response, exec2_response | 8 u32 each | zero | zero |
| network_command_buffer | u8[0x4FFE00] | zero | zero |
| network_buffer | u8[0x80000] | zero | zero |
| allnet_buffer | u8[0x1000] | zero | zero |
| allnet_settings | u8[0x8500] | zero | zero |
| game_modified_ip | `Common::IPAddress` (4 bytes) | 0.0.0.0 | 0.0.0.0 |
| netconfig_read_pending | bool | false | false |
| board_status | u32 | 4 (LoadingGameProgram) | 4 |
| load_progress | u32 | 80 | 80 |
| sockets | host socket handle [64] | undefined until Init | all -1 |
| next_valid_fd | u32 | 1 | 1 |
| files netcfg, netctrl, extra, dimm, backup | `File::IOFile` | closed | opened (2.5) |
| dimm_disc | `std::unique_ptr<DiscIO::BlobReader>` | null | **not reset** |
| TestHwPhase2 event | `CoreTiming::EventType*` | null | registered (2.5) |

In the tables below "word N" means the u32 at byte offset 4N of the media
buffer, read and written as host LE; "byte N" is the byte at offset N.

### 2.4 Media board address ranges

Addresses in DI commands (the "offset", 2.6) can point into the board's own
buffers. The table, in lookup order (first match wins):

| Start | Size | Buffer |
|---|---|---|
| 0x1F900000 | 0x40 | media buffer |
| 0x84000000 | 0x60 | media buffer |
| 0x89000000 | 0x220 | media buffer |
| 0x1F800200 | 0x1040 | network_command_buffer |
| 0x89040200 | 0x40000 | network_command_buffer |
| 0x1FA00000 | 0x10000 | network_buffer |
| 0x1FD00000 | 0x10000 | network_buffer |
| 0x89100000 | 0x50000 | network_buffer |
| 0x89180000 | 0xC0000 → clamped to 0x80000 (warning logged) | network_buffer |
| 0x1FB00000 | 0x10000 | network_buffer |
| 0x1EFF8000 | 0x8000 | allnet_settings |
| 0x89010000 | 0x1000 | allnet_buffer |

`span(address)`: for the first range with `start <= address < end`, the
buffer from byte `address - start` **to the end of the whole buffer** (not to
the end of the range); empty if no range matches. **Quirk:** so a range only
decides where an access may start; its length can run on past the range.

Helpers used below:

- `SafeCopyToEmu(addr, src, src_size, off, len)`: fails with the error
  `GC-AM: Read overflow: ...` if `off > src_size` or `len > src_size - off`;
  fails with `GC-AM: Memory buffer too small: ...` if the emulated memory span
  at `addr` is shorter than `len`; otherwise copies `src[off..off+len)` to
  emulated memory at `addr`.
- `SafeCopyFromEmu(dst, addr, dst_size, off, len)`: the same checks (message
  `GC-AM: Write overflow: ...` for the first) and copies emulated memory into
  `dst[off..)`.
- `FileReadData(file, pos, addr, len)`: if the emulated span at `addr` is at
  least `len`: seek to `pos` and read `len` bytes into memory (a short read
  leaves the rest untouched); otherwise error `GC-AM: Read overflow:
  address=...`. `FileWriteData(file, pos, addr, len)`: same check (error
  `GC-AM: Write overflow: address=...`), seek, write, flush.
- `GetSafeString(off, max)`: the span at `off`; empty if none; otherwise the
  bytes up to the first NUL, at most `min(max, span size)` bytes.

### 2.5 Init, Shutdown, FirmwareMap, InitKeys, InitDIMM

`Init()`:
1. Zero the media buffer, both saved responses, network buffers, allnet
   buffers; fill firmware with 0xFF; set every socket slot to -1.
2. Reset game_modified_ip, netconfig_read_pending, board_status (4),
   load_progress (80), firmware_map, test_menu, last_error (70),
   next_valid_fd (1), the three keys (0).
3. Register the CoreTiming event named **`AMMediaboardTestHwPhase2`**
   (callback in 2.8).
4. Open, in this order, each with "open existing as `rb+`, else create as
   `wb+`": `trinetcfg.bin`, `trinetctrl.bin`, `triextra.bin`,
   `tridimm_<GameID>.bin`, `backup_<GameID>.bin`. Then for each one that is
   not open, in the same order, `PanicAlertFmt("Failed to open/create: {}",
   path)` where path is the Triforce folder plus `trinetcfg.bin`,
   `trinetctrl.bin`, `triextra.bin`, **`tridimm.bin`**, **`backup.bin`**
   (**Quirk:** the last two messages name the file without the game ID).
5. SegaBoot: if `segaboot.gcm` does not exist in the Triforce folder:
   `PanicAlertFmt("Failed to open segaboot.gcm({}), which is required for
   test menus.", full_path)` and return. Open it `rb+`; if that fails,
   `PanicAlertFmt("Failed to read: {}", full_path)` and return. Read
   `min(file size, 0x200000)` bytes into the start of firmware, set
   test_menu = true.

`Shutdown()`: close the five files, reset dimm_disc, close every socket slot
1..63 that is not -1 and set it to -1.

`FirmwareMap(on)`: firmware_map = on. `InitKeys(a, b, c)`: set the three keys.
`InitDIMM(volume)`: dimm_disc =
`DiscIO::CreateCachedBlobReader(volume.GetBlobReader().CopyReader())`.

`GetTestMenu()`: test_menu.

`GetGameType()`: take `GetGameID()`; if it has exactly 6 characters, the id
is `(char[1] << 8) | char[2]` (characters as `char`), else `0x454A`. Look it
up:

| id | type | id | type |
|---|---|---|---|
| 0x4747 | FZeroAX | 0x484A, 0x484E, 0x485A, 0x4A41, 0x4A4A | VirtuaStriker4 |
| 0x4841 | FZeroAXMonster | 0x4C4A, 0x4C4B, 0x4C4C | VirtuaStriker4_2006 |
| 0x4B50, 0x4B5A | MarioKartGP | 0x4658, 0x4A4E | KeyOfAvalon |
| 0x4E4A, 0x4E4C | MarioKartGP2 | 0x4758 | GekitouProYakyuu |
| 0x454A, 0x4559, 0x5342, 0x3132, 0x454C | VirtuaStriker3 | 0x3030 | FirmwareUpdate |

Not found: `PanicAlertFmtT("Unknown game ID:{0:08x}, using default
controls.", id)` (every call) and return VirtuaStriker3.

`GetMediaType()`: NAND (2) for MarioKartGP, MarioKartGP2, FZeroAXMonster;
GDROM (1) for everything else.

### 2.6 ExecuteCommand

`ExecuteCommand(dicmd, diimm, address, length)`:

1. **De-obfuscation.** `dicmd[i] ^= key[i]` for i = 0..2 (in place). Let
   `seed = dicmd[0] >> 16`; then `key_a *= seed`, `key_b *= seed`,
   `key_c *= seed` (u32 wrap-around).
2. If key_a is now 0 and `Read_U32(0) != 0`: `HLE::Patch(system,
   0x813048B8, "OSReport")`, `HLE::Patch(system, 0x8130095C, "OSReport")`,
   then `InitKeys(Read_U32(0), Read_U32(4), Read_U32(8))`. (This repeats on
   every command while key_a stays 0.)
3. `command = dicmd[0] << 24` (so the DI opcode is the low byte of the
   de-obfuscated word 0), `offset = dicmd[1] << 2`.
4. **Test mode check.** If `offset == 0x2440` and `Read_U32(0x811FFF00) ==
   1` and `Read_U32(0x8006BF70) != 0x0A536567`: firmware_map = true.
5. Dispatch on the opcode (`command >> 24`):

**Inquiry (0x12).** firmware_map = false. `*diimm = Version2` (0x29484100)
for VirtuaStriker4_2006, KeyOfAvalon, MarioKartGP, MarioKartGP2 and
FirmwareUpdate, else `Version1` (0x21484100). Return 0.

**Read (0xA8).** Checks in order; the first that applies is the only one
done. All return 0 unless stated.

1. `(offset & 0x8FFF0000) == 0x80000000` (status area): by exact offset:

   | offset | effect at `address` |
   |---|---|
   | 0x80000000 | `Write_U16(1)` |
   | 0x80000020 | fill `length` bytes with 0x00 |
   | 0x80000040 | fill `length` bytes with 0xFF, then `Write_U32_Swap(0x20000000)` at +0, `Write_U32(0x4743414D)` ("GCAM") at +4 |
   | 0x80000100 | `Write_U32_Swap(0x001F1F1F)` |
   | 0x80000120 | `Write_U32_Swap(0x000001FA)` |
   | 0x80000140 | `Write_U32_Swap(1)` |
   | 0x80000160 | `Write_U32(0x00001E00)` |
   | 0x80000180 | `Write_U32(0)` |
   | 0x800001A0 | `Write_U32(0xFFFFFFFF)` |
   | other | error log + `PanicAlertFmtT("Unhandled Media Board Read: offset={0:08x} length={0:08x}", offset, length)` |

   (The mask matches any offset with bit 31 set and bits 16-27 clear.)
2. `offset == 0 && length == 0x80`: FileReadData(netcfg, 0).
3. `offset == 0x1FFEFFE0 && length == 0x20`: FileReadData(extra, 0).
4. `0x1F000000 <= offset < 0x1F800000`: FileReadData(dimm, offset -
   0x1F000000).
5. `0xFF000000 <= offset < 0xFF800000`: FileReadData(dimm, offset -
   0xFF000000).
6. `offset == 0xFFFF0000 && length == 0x20`: FileReadData(netctrl, 0).
7. `0x89010000 <= offset < 0x89011000`: info log, then
   `SafeCopyToEmu(address, allnet_reply, sizeof, offset - 0x89010000,
   sizeof)` where `allnet_reply` is the string in 2.11 including its NUL
   (sizeof = 252 bytes). **Quirk:** the DI length is ignored and the copy
   length is the whole reply, so the copy only happens for offset
   0x89010000 exactly; other offsets log `GC-AM: Read overflow`.
8. netconfig_read_pending and `length == 0x80` and `span(offset)` is not
   empty: clear the flag; `config` = 0x80 zero bytes; if netcfg is open, seek
   it to 0 and read 0x80 bytes into `config` (short read keeps zeros); if
   `config[0] == 0` set it to 2; copy `config` to `address`.
9. `offset == 0x84000000 && length == 0x20`: copy exec1_response (32 bytes,
   host LE words) to `address`.
10. `offset == 0x89000000 && length == 0x20`: copy exec2_response likewise.
11. `span(offset)` not empty: `SafeCopyToEmu(address, span, span size, 0,
    length)` (on failure only the error is logged).
12. `offset == 0x88000000`: **Execute2** (2.7).
13. `offset >= 0x57058000`: error log + the "Unhandled Media Board Read"
    panic alert above.
14. firmware_map: `SafeCopyToEmu(address, firmware, 0x200000, offset,
    length)`; on failure also log `GC-AM: Invalid firmware buffer range: ...`.
15. Otherwise read the game: if the emulated span at `address` is shorter
    than `length`, log `GC-AM: Invalid DIMM Disc read from: ...` and return
    **1**; else `dimm_disc->Read(offset, length, ptr)`: true → return 0,
    false → return **1**.

**Write (0xAA).** Checks in order, all return 0:

1. `offset == 0x00600000 && length == 0x20` → firmware_map = true.
2. `offset == 0x00700000 && length == 0x20` → firmware_map = true.
3. firmware_map and `0x00400000 <= offset <= 0x00600000`:
   `SafeCopyFromEmu(firmware, address, 0x200000, offset - 0x400000, length)`;
   on failure also log `GC-AM: Invalid firmware write: ...`.
4. `offset == 0 && length == 0x80` → FileWriteData(netcfg, 0).
5. `offset == 0x1FFEFFE0 && length == 0x20` → FileWriteData(extra, 0).
6. `0x6A0 <= offset < 0x00800000` → FileWriteData(backup, **0**).
   **Quirk:** always at file offset 0, whatever the offset.
7. `0x1F000000 <= offset < 0x1F800000` → FileWriteData(dimm, offset -
   0x1F000000).
8. `0x84800000 <= offset < 0x84818000` → info log `GC-AM: Write Firmware
   ({:08x})` only.
9. `0xFF000000 <= offset < 0xFF800000` → FileWriteData(dimm, offset -
   0xFF000000).
10. `offset == 0xFFFF0000 && length == 0x20` → FileWriteData(netctrl, 0).
11. `offset == 0x84000040 && length == 0x20 && Read_U8(address) == 1` →
    **Execute1** (2.7). If the first byte is not 1, fall through to 12.
12. `span(offset)` not empty: if `length == 0x80` and `Read_U8(address) !=
    0` and netcfg is open, first FileWriteData(netcfg, 0, address, 0x80);
    then `SafeCopyFromEmu(span, address, span size, 0, length)`.
13. `offset >= 0x57058000`: error log +
    `PanicAlertFmtT("Unhandled Media Board Write: offset={0:08x}
    length={0:08x}", offset, length)`.
14. Otherwise nothing.

**Execute (0xAB)** (the "DI Execute" path): `cmd = word 8 >> 16`. If cmd is
0x000, return 0 with no effect. If `offset == 0 && length == 0`: run 2.7's
"DI Execute" handling and return 0. Otherwise error log +
`PanicAlertFmtT("Unhandled Media Board Execute:{0:04x}", cmd)`, return 0.

**Any other opcode:** error log + `PanicAlertFmtT("Unhandled Media Board
Command:{0:02x}", command)` where `command` is the full `dicmd[0] << 24`
value (so it prints 8 hex digits), return 0.

Note the format strings `offset={0:08x} length={0:08x}` print the offset
twice (**Quirk**, keep verbatim).

### 2.7 Board commands (Execute1, Execute2, DI Execute)

The three paths share the media buffer. The command code is a u16.

**Execute1** (Write of 0x20 bytes to 0x84000040 whose first byte is 1):
`cmd` = u16 LE at byte 0x22. Zero bytes 0x00-0x1F. word 0 = word 8 |
0x80000000. Then by `cmd`:

| cmd | effect |
|---|---|
| 0x000 | word 1 = 1 |
| 0x001 | word 1 = 0x20000000 |
| 0x100 | word 1 = 5, word 2 = 100 |
| 0x101 | byte 4 = 0x03, byte 5 = 0x09, byte 6 = 1, word 2 = 1, word 4 = 0xFF (SegaBoot 3.09) |
| 0x102 | byte 4 = 1, byte 5 = 1, byte 6 = 1, byte 8 = 0 |
| 0x103 | bytes 4-19 = ASCII `A89E-27A50364511` |
| 0x104 | byte 4 = 1 |
| 0x301 | test_type = word 9, ptr = word 10; if ptr != 0: `Write_U32_Swap(0x54455354, ptr)`, `Write_U32_Swap(0x204F4B00, ptr+4)` (the bytes `TSET`, `\0KO ` — i.e. "TEST OK\0" as LE words); word 1 = test_type; RemoveEvent(TestHwPhase2), ScheduleEvent(ticks_per_second / 10000, TestHwPhase2, userdata 0) |
| other with bit 15 set | debug log only |
| other | error log + `PanicAlertFmtT("Unhandled Media Board Command:{0:04x}", cmd)` |

Then zero bytes 0x20-0x3F, copy words 0-7 to exec1_response, and
`ExpansionInterface::GenerateInterrupt(0x04)` (section 10).

**Execute2** (Read at 0x88000000): `cmd` = u16 LE at byte 0x202. Copy bytes
0x200-0x21F to 0x00-0x1F, zero bytes 0x200-0x21F, set byte 0x204 = 1. Then
by `cmd` (parameters start at word 2, "p = 2"):

| cmd | effect |
|---|---|
| 0x001 | word 1 = 1 |
| 0x101 | word 1 = 0x1305, then byte 6 = 1 (network firmware 13.05, VxWorks) |
| 0x102 | byte 4 = 1, byte 6 = 2, byte 7 = 1 |
| 0x103 | nothing |
| 0x104 | byte 4 = 1, netconfig_read_pending = true |
| 0x204 | nothing |
| 0x301 | test_type = word 1, ptr = word 2; if ptr != 0 write "TEST OK" as in Execute1; word 1 = test_type; RemoveEvent, ScheduleEvent(ticks_per_second / 10000, TestHwPhase2, userdata 1) |
| 0x401 Accept | 2.9 Accept(p = 2) |
| 0x402 Bind | 2.9 Bind (always words 2-4) |
| 0x403 Closesocket | 2.9 Closesocket(2) |
| 0x404 Connect | 2.9 Connect(2) |
| 0x406 InetAddr | 2.9 InetAddr |
| 0x408 Listen | 2.9 Listen |
| 0x409 Recv | 2.9 Recv(2) |
| 0x40A Send | 2.9 Send(2) |
| 0x40B Socket | 2.9 Socket(2) |
| 0x40C Select | 2.9 Select(2) |
| 0x40E SetSockOpt | 2.9 SetSockOpt(2) |
| 0x410 SetTimeOuts | 2.9 SetTimeOuts |
| 0x414 GetParambyDHCPExec | notice log with word 2; byte 1 = 0; word 1 = 0 |
| 0x415 ModifyMyIPaddr | 2.9 ModifyMyIPaddr(2) |
| 0x411 GetLastError | 2.9 GetLastError |
| 0x601 InitLink | notice log `GC-AM: InitLink` |
| 0x700 AllNetInit | notice log `GC-AM: AllNetInit` |
| other with bit 15 set | debug log only |
| other | error logs `GC-AM: Command:{0:04x}` and `GC-AM: Command Unhandled!` |

Then byte 3 |= 0x80, copy words 0-7 to exec2_response, fill `length` bytes
at `address` with 0, and `GenerateInterrupt(0x10)`. Return 0.

**DI Execute** (opcode 0xAB with offset 0 and length 0, `cmd` = word 8 >>
16, never 0): zero bytes 0x00-0x1F, word 0 = word 8 | 0x80000000. Then
(parameters start at word 10, "p = 10"):

| cmd | effect |
|---|---|
| 0x001 | word 1 = 0x20000000 |
| 0x100 | word 1 = board_status, word 2 = load_progress; then if load_progress < 100 it is incremented, else board_status = 5 |
| 0x101 | byte 4 = 0x03, byte 5 = 0x11, byte 6 = 1, word 2 = 1, word 4 = 0xFF (SegaBoot 3.11) |
| 0x102 | byte 4 = 1, byte 5 = 1, byte 6 = 1, byte 8 = 0 |
| 0x103 | bytes 4-19 = `A89E-27A50364511` |
| 0x104 | byte 4 = 1 |
| 0x204 | nothing |
| 0x301 | debug logs; **unconditionally** `Write_U32_Swap(0x54455354, word 12)` and `Write_U32_Swap(0x204F4B00, word 12 + 4)` (even when word 12 is 0); word 1 = word 9. No event. |
| 0x403, 0x404, 0x409, 0x40A, 0x40B, 0x40C, 0x40E, 0x415 | as Execute2 but with p = 10 |
| 0x601 | notice `GC-AM: InitLink` |
| 0x605 | notice `GC-AM: 0x605` |
| 0x606 SetupLink | notice logs only (size = bytes 0x24 \| 0x25<<8, port = BE from bytes 0x26/0x27, LinkNum byte 0x28, byte 0x2A, bytes 0x2C/0x2D, IPs from words 12 and 13, words 14/15 byte-swapped); word 1 = 0 |
| 0x607 SearchDevices | notice logs; `off` = byte 0x26 \| byte 0x27<<8, `addr` = word 10; if `span(off + addr)` has fewer than 0x20 bytes: error log, stop (word 1 untouched); else notice-log the 0x20 bytes as 8 host-LE words and set word 1 = 0 |
| 0x608 | notice log only (`ip` = word 10, `port` = byte-swap of bytes 6/7, `flag` = bytes 10/11) |
| 0x614 | notice `GC-AM: 0x614` |
| other (including 0x000 never reached, 0x401, 0x402, 0x408, 0x410, 0x411, 0x414) | error `GC-AM: Execute buffer UNKNOWN:{0:04x}` |

Then zero bytes 0x20-0x3F and copy words 0-7 to exec1_response. No
interrupt.

### 2.8 TestHardware phase 2 event

Callback of `AMMediaboardTestHwPhase2(userdata)`: target = exec2_response if
userdata != 0, else exec1_response. Fill it with 0, set its word 0 =
0x03020000, word 1 = 2, word 2 = 100, debug log, and
`GenerateInterrupt(userdata != 0 ? 0x10 : 0x04)`.

### 2.9 Network commands

The guest uses socket numbers 0-63 ("guest sockets"); slot `i` holds a host
socket or -1. Slot 0 is never handed out. Host error codes are mapped as on
Windows: on POSIX, `EAGAIN`, `EWOULDBLOCK` and `EINPROGRESS` read as 10035
(`WSAEWOULDBLOCK`), anything else is `errno`. `SOCKET_ERROR` is -1 and
`INVALID_SOCKET` is the all-ones handle (-1 on POSIX). Stored into the u32
last_error, -1 becomes 0xFFFFFFFF.

`SocketStatusCodes` (header data): SSC_E_4 = -4, SSC_E_3 = -3, SSC_E_2 = -2,
SSC_E_1 = -1, SSC_EINTR = 4, SSC_EBADF = 9, SSC_E_11 = 11, SSC_EACCES = 13,
SSC_EFAULT = 14, SSC_E_23 = 23, SSC_AEMFILE = 24, SSC_EMSGSIZE = 36,
SSC_EAFNOSUPPORT = 47, SSC_EADDRINUSE = 48, SSC_EADDRNOTAVAIL = 49, SSC_E_50 =
50, SSC_ENETUNREACH = 51, SSC_ENOBUFS = 55, SSC_EISCONN = 56, SSC_ENOTCONN =
57, SSC_ETIMEDOUT = 60, SSC_ECONNREFUSED = 61, SSC_EHOSTUNREACH = 65,
SSC_EHOSTDOWN = 67, SSC_EWOULDBLOCK = 70, SSC_E_69 = 69, SSC_SUCCESS = 70.
**Quirk:** EWOULDBLOCK and SUCCESS are both 70.

Slot allocation (`GetAvailableGuestSocket`): try up to 64 times: `i =
next_valid_fd`, `next_valid_fd = (next_valid_fd + 1) % 64`; skip `i == 0`;
if slot `i` is -1 return `i`. Else return -1 ("invalid guest socket"). The
slot is not reserved until the host call succeeds, but next_valid_fd has
moved on either way.

`host(g)`: slot `g` if `g < 64` (as u32), else warn `GC-AM: Bad GuestSocket
value: {}` and return -1. `guest(h)`: index of the first slot 1..63 equal to
`h`, or error `GuestSocket not found. This should not happen.` and -1.

Guest structures (copied raw from the board buffers, fields host LE unless
noted):

- `GuestSocketAddress` (16 bytes): u8 unknown (struct size in VxWorks, games
  leave it 0 or random), u8 family, u16 port (network order), 4-byte IPv4
  address, 8 bytes padding.
- `GuestFdSet` (32 bytes): 256 bits; bit `n` is bit `n % 8` (LSB first) of
  byte `n / 8`.
- `TimeVal` (16 bytes): u64 seconds at +0, u32 microseconds at +8, 4 bytes
  padding.

Response conventions: "byte 1 = byte 8" copies byte 8 into byte 1 (part of
word 0) before word 1 is written. In the DI Execute path bytes 0-0x1F were
zeroed, so there byte 8 is 0.

| Command | Parameters (words p, p+1, ...) | Behaviour | Response |
|---|---|---|---|
| Socket | domain, type | `GetAvailableGuestSocket`; if none, -1. Host `socket(domain, type, 0)`; on failure error log, -1. Else `Common::SetPlatformSocketOptions(fd)`, store in slot. Notice log. | byte 1 = 0; word 1 = guest socket or 0xFFFFFFFF |
| Closesocket | g | `close(host(g))` (also for -1); if g < 64 slot = -1; last_error = 70 | word 1 = close result |
| Connect | g, addr_off, len | if len != 16: error, return with no response change; if `span(addr_off)` < 16: error, return. Else NetDIMMConnect (below). | byte 1 = byte 8; word 1 = 0 or 0xFFFFFFFF |
| Accept | g, addr_off, addrlen_off | if `span(addrlen_off)` ≥ 4: addrlen_ptr = it, addrlen = its u32; if `span(addr_off)` ≥ addrlen: addr_ptr = it else error log. NetDIMMAccept. | word 1 = guest socket or 0xFFFFFFFF |
| Bind | words 2, 3, 4 = g, addr_off, len | len != 16 or span < 16: error, return with no response change. Else NetDIMMBind; last_error = 70 | word 1 = bind result |
| Listen | words 2, 3 = g, backlog | `listen(host(g), backlog)`; notice log | byte 1 = byte 8; word 1 = result |
| Recv | g, off, len | if `span(off)` < len: error log, len = 0 (pointer = start of the possibly empty span). `recv(host(g), ptr, len, 0)`; logs by result | byte 1 = byte 8; word 1 = result |
| Send | g, off, len | as Recv with `send(..., Common::SEND_FLAGS)` (MSG_NOSIGNAL on POSIX, 0 on Windows) | byte 1 = byte 8; word 1 = result |
| Select | nfds, readfds_off, writefds_off, exceptfds_off, timeout_off | below | byte 1 = 0; word 1 = poll result |
| SetSockOpt | g, level, optname, optval_off, optlen | if `span(optval_off)` < optlen: error, return with no response change. `setsockopt(host(g), level, optname, ptr, optlen)` with the guest's level/optname values unchanged; notice log | byte 1 = byte 8; word 1 = result |
| SetTimeOuts | words 2-5 = g, a, b, c | timeouts = {a, b, c} (global). If host(g) != -1: `setsockopt(SOL_SOCKET, SO_SNDTIMEO, &b, sizeof(int))`; on failure ret = mapped error code; else `setsockopt(SOL_SOCKET, SO_RCVTIMEO, &c, sizeof(int))`, on failure ret = mapped error; info log. Else ret = -1, error log. | byte 1 = byte 8; word 1 = ret |
| GetLastError | word 2 = g | debug log if last_error == 70, else notice | byte 1 = byte 8; word 1 = last_error |
| InetAddr | — | string at the start of network_command_buffer (`strnlen` limit 15, the length check can never fail); `inet_addr` of it; notice log | byte 1 = byte 8; word 1 = the address in network order |
| ModifyMyIPaddr | off | `GetSafeString(off, 15)`; notice log; if `Common::StringToIPv4PortRange` parses it, game_modified_ip = its first IP | none |

**Quirk (SetTimeOuts):** the option value is passed as a 4-byte int, which
Windows reads as milliseconds; on POSIX `SO_SNDTIMEO` needs a `timeval` and
fails, so word 1 becomes the errno value (22). The behaviour on Windows is
what matters; the POSIX branch should do the same calls so the harness
matches.

**NetDIMMConnect(g, addr):** info log. Destination = `{addr.ip, addr.port}`
adjusted by the first IP redirection (2.10) whose `original` range matches,
with an info log; else unchanged. `sockaddr_in` family = addr.family. Then if
the first redirection whose `original` matches `{game_modified_ip, port 0}`
exists, bind the source: `first = that result's port value` (host order);
`any = first == 0`; attempts = any ? 1 : 10; last = any ? 0 : first + 999
(u16 wrap). BindEphemeralPort tries `attempts` random ports uniformly in
[first, last] (random generator seeded from the clock; port 0 means any),
binding to the result IP, info log per attempt; if none binds, last_error =
-1 and return -1. Set the socket non-blocking (`FIONBIO` 1); it is set back
to blocking (`FIONBIO` 0) on every exit from here on. `connect`:

- 0 → last_error = 70, return 0.
- error other than 10035 → warning, last_error = -1, return -1.
- 10035 → `poll` for POLLOUT with timeout = timeouts[0] microseconds
  converted to milliseconds (truncated). poll < 0 → error log, last_error =
  -1, return -1. Neither POLLOUT nor POLLERR in revents → last_error = 70
  ("would block"), return -1. Else `getsockopt(SOL_SOCKET, SO_ERROR)`; if
  that call fails, error log and fall through to failure; if the error is 0
  → info log, last_error = 70, return 0; else error log, last_error = -1,
  return -1.

**NetDIMMAccept(g, addr_ptr, addrlen_ptr):** if exactly one of the two
pointers is null: error `NetDIMMAccept: bad parmeters`, last_error = 14,
return -1. `poll` POLLIN on host(g) with timeout 0. poll < 0 → error,
last_error = -1, return -1. No POLLIN → debug log, last_error = 70, return
-1. Then `GetAvailableGuestSocket` and host `accept(fd, sockaddr_in, len)`;
no slot or accept failure → error log, last_error = -1, return -1. Else
`Common::SetPlatformSocketOptions`, store, last_error = 70, notice log. If
addr_ptr: build a GuestSocketAddress {0, u8(family), port, ip, zero
padding}; if a redirection's `replacement` range matches {ip, port}, replace
them by its `Reverse` (notice log); copy `min(u32 at addrlen_ptr, 16)` bytes
to addr_ptr; then write **one byte** 16 at addrlen_ptr (**Quirk:** only the
low byte of the length word). Return the guest socket.

**NetDIMMBind(g, addr):** notice log. Considered address = {addr.ip,
addr.port}, but if addr.ip is 0.0.0.0 the IP is replaced by game_modified_ip
(info log). If a redirection's original matches the considered address, the
bind address is its `Apply` result (info log); otherwise the bind address is
the guest's own {ip, port} (so 0.0.0.0 stays). `bind` with family =
addr.family. Info log. On failure: message `Failed to bind socket {ip}:{port}`
(port in host order), error log, and `OSD::AddMessage(message,
OSD::Duration::SHORT, OSD::Color::RED)`. Return the bind result.

**Select:** nfds = min(word p, 64). Timeout = -1 ms (infinite) unless
timeout_off != 0 and `span(timeout_off)` ≥ 16: then seconds + microseconds
converted to milliseconds (truncated). A bad timeout_off logs an error and
keeps -1. A negative timeout logs the warning `AMMBCommandSelect: Infinite
timout!`. Build `nfds` poll entries (fd -1, events 0); for each of
readfds/writefds/exceptfds with a non-zero offset and a span ≥ 32 (else
error log `Bad FDSET offset: {:08x}`), for each index < nfds whose bit is
set: entry fd = host(index), events |= POLLIN / POLLOUT / POLLPRI. Remove
entries whose fd is -1. `poll(entries, timeout)`. If the result ≥ 0: for
each fd set with a non-zero offset and a big enough span, write back a fresh
32-byte set containing the bits `guest(fd)` of the entries whose revents
include POLLIN / POLLOUT / POLLPRI respectively (entries not found are
skipped); else error log.

### 2.10 IP redirections

`ParseIPRedirection(str)`: the part before the first space is the pair;
split the pair at `=` into exactly two parts (no `=` or more than one →
nullopt). `description` = everything after the first space (empty view if
there is no space). Returned views point into `str`.

`GetIPRedirections()`: read config `Core.TriforceIPRedirections`
(`Config::MAIN_TRIFORCE_IP_REDIRECTIONS`, defined in MainSettings), split it
at every `,`; for each piece parse it; if parsing succeeds and both sides
convert with `Common::StringToIPv4PortRange`, append `{original,
replacement}`; if parsing succeeds but a side does not convert, error log
`Bad IP redirection string: {}`. Pieces that don't parse are skipped
silently.

`IPRedirection::Apply(s)`: with ip values as host-order u32 and ports as
host-order u16:
`ip_count = 1 + repl.last.ip - repl.first.ip` (as u64);
`result_ip = u32(repl.first.ip + ((s.ip - orig.first.ip) mod ip_count))`
where the subtraction is u32 arithmetic and the sum is taken as u64 then
truncated; `port_count = 1 + repl.last.port - repl.first.port` (u32); if
`port_count != 65536`, `result_port = u16(repl.first.port + ((s.port -
orig.first.port) mod port_count))` (subtraction in int, then converted to u32
for the modulo), else the port is unchanged. `Reverse(s)` = Apply with
original and replacement swapped. `ToString()` = `"{original}={replacement}"`
using `IPv4PortRange::ToString`.

Matching uses `IPv4PortRange::IsMatch` (base tree). Redirections are re-read
from config on every use.

`DebuggerGetSocket(fd)`: slot `fd` as s32 if fd < 64, else warning `GC-AM:
Bad socket fd used by the debugger: {}` and -1.

Platform layer: on Windows use Winsock (`closesocket`, `ioctlsocket`,
`WSAPoll`, `WSAGetLastError`); on POSIX the same names map to `close`,
`ioctl`, `poll` (retried on EINTR) and the errno mapping above. The harness
intercepts the POSIX calls `socket, bind, connect, listen, accept, recv,
send, setsockopt, getsockopt, poll, ioctl, close`, so the POSIX branch must
use exactly these (and `inet_addr`, `inet_ntoa`, `htons`, `ntohs`).

### 2.11 Constant data

- All.Net reply (252 bytes with the NUL):
  `uri=http://sega.com&host=sega.com&nickname=sega&name=sega&year=2025&month=08&day=16&hour=21&minute=10&second=12&place_id=1234&setting=0x123&region0=jap&region_name0=japan&region_name1=usa&region_name2=asia&region_name3=export&end`
- Media board serial: `A89E-27A50364511` (16 bytes, no NUL).
- "TEST OK": words 0x54455354 and 0x204F4B00 written with `Write_U32_Swap`.

### 2.12 DoState

Order and sizes: firmware_map (1), test_menu (1), timeouts (12), last_error
(4), key_a (4), key_b (4), key_c (4), firmware (0x200000), media buffer
(0x300), exec1_response (32), exec2_response (32), network_command_buffer
(0x4FFE00), network_buffer (0x80000), allnet_buffer (0x1000),
allnet_settings (0x8500), next_valid_fd (4), game_modified_ip (4),
netconfig_read_pending (1), board_status (4), load_progress (4), then a
32-byte GuestFdSet of open sockets: in write and verify mode bit `i` is set
for each slot 1..63 that is not -1; in measure mode it is all zero. In read
mode, after loading it: close all open sockets (as in Shutdown) and, for
each set bit `i` in 1..63, slot `i` = `socket(AF_INET, SOCK_STREAM, 0)` (no
platform options). Files, dimm_disc and the event are not saved.

## 3. SI baseboard (`SI/SI_DeviceAMBaseboard.cpp/.h`)

### 3.1 Role and interface

`SerialInterface::CSIDevice_AMBaseboard final : public ISIDevice` is the
Triforce baseboard on an SI port (device type `SIDEVICE_AM_BASEBOARD`). It
owns the cabinet I/O: an `IOPorts` hub with game-specific adapters, a JVS-IO
board, and up to two serial devices (ports A and B). Public interface (may be
kept):

```
CSIDevice_AMBaseboard(Core::System& system, SIDevices device, int device_number);
~CSIDevice_AMBaseboard() override;
int RunBuffer(u8* buffer, int request_length) override;
DataResponse GetData(u32& hi, u32& low) override;   // always DataResponse::NoData, no side effects
void SendCommand(u32 command, u8 poll) override;     // only logs (section 14)
void DoState(PointerWrap&) override;
```

Members other files rely on: none. The header forward-declares
`Triforce::JVSIOBoard` and `Triforce::SerialDevice` and includes
`MagCard/MagneticCardReader.h`, `SI/SI.h`, `SI/SI_Device.h` and
`Triforce/IOPorts.h`. State:

| State | Type | Initial |
|---|---|---|
| response buffers | 2 x 128 bytes (`SerialInterfaceManager::BUFFER_SIZE`) | zero |
| current response buffer index | u8 | 0 |
| I/O ports | `Triforce::IOPorts` | see 4 |
| JVS board | owned `Triforce::JVSIOBoard` | per game |
| magnetic card settings | `MagCard::MagneticCardReader::Settings` | see below |
| serial device A, B | owned `Triforce::SerialDevice`, may be null | per game |

### 3.2 Construction

1. Card settings: `card_path` = Triforce user folder, `card_name` =
   `tricard_<GameID>.bin`; the two flags keep their defaults
   (`insert_card_when_possible = true`, `report_dispenser_empty = false`).
2. Add a `Common_IOAdapter(system)` to the I/O ports (always first).
3. By `AMMediaboard::GetGameType()`:

| Game type | JVS board | Serial A | Serial B | Adapters added after Common, in order |
|---|---|---|---|---|
| FZeroAX | `Sega_837_13844_JVSIOBoard` | `FZeroAXSteeringWheel` | `MagCard::C1231BR(&settings)` | `FZeroAXCommon(wheel A)`, `FZeroAXDeluxe` |
| FZeroAXMonster | (fallback) | `FZeroAXSteeringWheel` | none | `FZeroAXCommon(wheel A)`, `FZeroAXMonster` |
| MarioKartGP, MarioKartGP2 | `Namco_FCA_JVSIOBoard` | `MarioKartGPSteeringWheel` | `MagCard::C1231LR(&settings)` | `MarioKartGPCommon(wheel A)` |
| VirtuaStriker3 | (fallback) | none | none | `VirtuaStriker3` |
| VirtuaStriker4 | (fallback) | `ICCardReader(0)` | `ICCardReader(1)` | `VirtuaStriker4Common(A, B)` |
| VirtuaStriker4_2006 | (fallback) | `ICCardReader(0)` | `ICCardReader(1)` | `VirtuaStriker4Common(A, B)`, `VirtuaStriker4_2006(A, B)` |
| GekitouProYakyuu | (fallback) | `ICCardReader(0)` | `ICCardReader(1)` | `GekitouProYakyuu(A, B)` |
| KeyOfAvalon | (fallback) | `DeckReader` | `Touchscreen` | `KeyOfAvalon(deck reader's internal IC card reader)` |
| anything else | (fallback) | none | none | none |

"Fallback" is `Sega_837_13551_JVSIOBoard`. Every JVS board is constructed
with a pointer to the I/O ports.

### 3.3 RunBuffer(buffer, request_length)

`buffer` always points at 128 bytes. Steps:

1. Call the base `ISIDevice::RunBuffer` (debug only; no effect) and then
   `IOPorts::Update()` (section 4), whatever the command.
2. Dispatch on `buffer[0]`:

**`0x00` (CMD_STATUS):** write the status response for
`SI_AM_BASEBOARD | 0x100` = `0x10110900` with `ISIDevice::CreateStatusResponse`
(bytes `10 11 09` into `buffer[0..2]`) and return 3.

**`0x70` (CMD_AM_BASEBOARD):** `len = buffer[1]`. If `2 + len >
request_length`, log the "Bad command_length" error and return -1 (no other
change). Otherwise parse sub-commands from `buffer[2 .. 2+len)` into the
response buffer selected by the current index (the "out" buffer). Output
starts at offset 2; the usable size is 127 (byte 127 holds the checksum).

A helper validates each step as `(n_in, n_out, name)`: it fails if fewer
than `n_in` input bytes remain, else if `offset + n_out > 127`. On failure
it logs "data_in overflow in {name}" or "data_out overflow in {name}",
then the "Overflow details" error (it holds host pointers; see section 14),
and sets the input position to the end, which ends the loop. The handler
then writes nothing.

Sub-commands (the command byte has already been consumed; "in" counts the
bytes after it):

| Cmd | Validate (in, out) | Consumes | Output appended |
|---|---|---|---|
| `10` status switches | (1, 4) | 1 | `10 02` + the 2 status-switch bytes from the I/O ports |
| `11` serial number | (1, 18) | 1 | `11 10` + ASCII `AADE-01B98394904` (16 bytes) |
| `12`, `14` | (2, 2) | 2 | `<cmd> 00` |
| `15` firmware version | (1, 4) | 1 | `15 02 00 26` |
| `16` FPGA version | (1, 4) | 1 | `16 02 07 06` |
| `1F` region | (5, 22) | 5 | `1F 14` + the 20-byte region table (below) |
| `21` | (4, 0) | 4 | nothing |
| `22` | (7, 0), then (`in[0]+1`, 0) | `in[0]+1` | nothing |
| `23`, `24` | (2, 2) | 2 | `<cmd> 00` |
| `31` serial A | (1, 0), then (`n`, 0) with `n = in[0]` | `1 + n` | nothing; the `n` bytes go to serial A's rx if A exists |
| `32` serial B | same as `31` for B | `1 + n` | nothing |
| `40`, `41` JVS | (1, 0), then (`n`, 0) | `1 + n` | see below |
| `60` | (3, 0), then (`in[0]+1`, 0) | `in[0]+1` | nothing |
| other | (5, 0) | 0 | nothing (logs the unknown command and the next 5 bytes) |

Notes:

- `22` and `60` validate the larger count first, then consume only
  `in[0]+1` bytes (so the count byte is included in what is skipped).
- Unknown commands consume only the command byte when 5 or more bytes
  follow, so parsing resumes at the next byte. With fewer than 5 bytes the
  helper fails ("data_in overflow in Unknown_<decimal cmd>") and the loop
  ends.
- JVS (`40`/`41`, both go to the same board): append the `n` bytes to the
  JVS board's rx, then call its `Update()`. If its tx count `t` is 0,
  output nothing. Otherwise validate (0, `t + 2`) with name `SerialDevice`;
  on success append `<cmd> <u8 t>` and take all `t` tx bytes. On failure the
  bytes stay queued for a later request. JVS output is not capped at 0x1F.

Region table (20 bytes): `00 00 30 00 02 FD 00 00` then twelve `FF`.

After the loop, for serial A and then serial B (each only if it exists):
call its `Update()`; `t = min(tx count, 0x1F)`; if `t > 0`, validate (0,
`t + 2`, `SerialDevice`) and on success append `31` (A) or `32` (B), `u8 t`
and the first `t` tx bytes. This runs even if the loop ended on a validation
failure. (A validation failure here also logs the overflow errors.)

Finish the out buffer: `[0] = 01`, `[1] = offset - 2`, bytes `offset..126`
= 0, `[127] = ~(sum of bytes 0 .. offset-1, mod 256)`.

Then advance the index (0 -> 1 -> 0). If byte 0 of the buffer at the new
index is nonzero, copy all 128 bytes of it into `buffer` and return 128;
otherwise return 0. **Quirk:** so each reply is returned one request late
(the very first 0x70 request returns 0, the second returns the first
request's reply, and so on). The old comment says games' parsers need this.

**Any other command:** log `Unknown SI command (0x{:08x})` (the command byte
as a number), `PanicAlertFmt("SI: Unknown command")`, return -1.

### 3.4 DoState

Both response buffers (256 raw bytes, buffer 0 first), the index (1), the
I/O ports (section 4), the JVS board (5.6), serial A's `DoState` if A
exists, then serial B's if B exists.

## 4. I/O ports and adapters (`Triforce/IOPorts.cpp/.h`)

### 4.1 Interface (may be kept)

Namespace `Triforce`. `IOPorts` holds the cabinet's input and output lines:

| Member | Type | Initial | Notes |
|---|---|---|---|
| status switches | `std::array<u8, 2>` | `FF FF` | baseboard DIP/status bits, active low |
| system inputs | u8 | 0 | first byte of JVS switch data |
| switch inputs | 2 players x 2 bytes | 0 | |
| analog inputs | 8 x u16 | 0 | |
| generic outputs | 4 bytes | 0 | lamps, locks; written by JVS |
| coin inputs | 2 x bool | false | |
| adapters | vector of owned `IOAdapter` | empty | updated in insertion order |

Public: `Update()`; `GetStatusSwitches()` (span of 2, const and non-const);
`GetSystemInputs()` (pointer to the byte); `GetSwitchInputs(player)` (span of
2 bytes for player 0 or 1; `ASSERT(player < 2)`); `GetAnalogInputs()` (span
of 8); `GetGenericOutputs()` (const span of 4); `SetGenericOutputs(span)`;
`ResetGenericOutputs()`; `GetCoinInputs()` (span of 2 bool);
`AddIOAdapter(unique_ptr)`; `DoState`. Constants: `PLAYER_COUNT = 2`,
`COIN_SLOT_COUNT = 2`, `NEUTRAL_ANALOG_VALUE = 0x8000`.

`IOAdapter` (non-copyable, non-movable) has public virtual `Update()` and
`DoState(PointerWrap&)`, a protected `GetIOPorts()`, a protected virtual
`HandleGenericOutputsChanged(span bits_set, span bits_cleared)`, and a
private `SetIOPorts` that `IOPorts` (a friend) calls from `AddIOAdapter`. The
base versions do nothing. Adapter class names and constructors (may be
kept): `Common_IOAdapter(Core::System&)`, `VirtuaStriker3_IOAdapter()`,
`VirtuaStriker4Common_IOAdapter(ICCardReader* a, ICCardReader* b)`,
`VirtuaStriker4_2006_IOAdapter(a, b)`, `GekitouProYakyuu_IOAdapter(a, b)`,
`KeyOfAvalon_IOAdapter(ICCardReader*)`; the F-Zero and Mario Kart ones are in
section 6.

### 4.2 Behaviour

`Update()`: system inputs = 0, switch inputs = 0, all 8 analog inputs =
`0x8000`, coin inputs = false; then each adapter's `Update()` in order. The
status switches and generic outputs are not reset.

`SetGenericOutputs(bytes)`:

1. If `bytes.size() > 4`, warn "SetGenericOutputs: Unexpected byte count".
2. `n = min(size, 4)`. If the first `n` new bytes equal the first `n`
   stored bytes, return (no handlers called).
3. For `i < n`: `set[i] = ~old[i] & new[i]`, `cleared[i] = old[i] & ~new[i]`,
   store `new[i]`. `set`/`cleared` are 4-byte arrays; entries `>= n` are 0.
   Bytes `>= n` of the stored outputs keep their value.
4. Call every adapter's `HandleGenericOutputsChanged(set, cleared)` in order.

`ResetGenericOutputs()` = `SetGenericOutputs` with 4 zero bytes.

`DoState`: status switches (2), generic outputs (4), then every adapter's
`DoState` in order. Inputs are not saved (they are rebuilt by `Update`).

### 4.3 Adapters

All pad reads are `Pad::GetStatus(i)` (`GCPadStatus`; button bits from
`InputCommon/GCPadStatus.h`). "p0[0] |= 0x80" means switch byte 0 of player
0 gets bit 0x80.

**Common** (every game): for `i` in 0, 1, only if
`system.GetSerialInterface().GetDeviceType(i) == SIDEVICE_AM_BASEBOARD`:
read pad `i`;
- `switches & SWITCH_TEST`: if `AMMediaboard::GetTestMenu()`, system inputs
  `|= 0x80`; else
  `OSD::AddMessage("Test menu is disabled due to missing SegaBoot.",
  OSD::Duration::NORMAL, OSD::Color::RED)` (every update while held);
- `switches & SWITCH_SERVICE`: p`i`[0] `|= 0x40`;
- `switches & SWITCH_COIN`: coin `i` = true.

**VirtuaStriker3**, for players 0 and 1 from pads 0 and 1: Start -> [0]
0x80, Up 0x20, Down 0x10, Left 0x08, Right 0x04, X 0x02, B 0x01; A -> [1]
0x80.

**VirtuaStriker4Common** (lock bits in generic output byte 0: A = 0x40,
B = 0x10):
1. If A is locked: if reader B is ready to insert, insert into B. Else: if
   B has a card and B is not locked, eject B.
2. If reader A is ready to insert, insert into A.
3. For players `i` = 0, 1 (reader `i`, pad `i`): set or clear p`i`[0] bit
   0x10 to "reader `i` has a card", and p`i`[1] bit 0x20 to "reader `i` is
   ejecting"; then Start -> [0] 0x80, Left 0x20, Up 0x08, Right 0x04,
   A 0x02, X 0x01; B -> [1] 0x80, Y 0x40; analog `2i` =
   `ExpandValue<u16>(0xFF - stickY, 8)`, analog `2i+1` =
   `ExpandValue<u16>(stickX, 8)`.

Its output handler: with `changed = set[0] | cleared[0]`, if `changed & 0x40`
log info "JVS-IO: Slot 1: Locked" when `set[0] & 0x40` else "...Unlocked";
same for 0x10 and "Slot 2".

**VirtuaStriker4_2006** (no `Update`): output handler ejects reader A if
`set[0] & 0x80`, reader B if `set[0] & 0x20`.

**GekitouProYakyuu**, for `i` = 0, 1: insert into reader `i` if it is ready;
set or clear p`i`[1] bit 0x40 to "reader `i` has a card"; Start -> [0]
0x80, Up 0x20, Down 0x10, Left 0x08, Right 0x04, A 0x02, B 0x01; trigger-L
button bit (`PAD_TRIGGER_L`) -> [1] 0x80. Output handler: eject A if
`cleared[0] & 0x40`, eject B if `cleared[0] & 0x10`.

**KeyOfAvalon** (one reader): if the reader's insert-check count is `>= 10`,
eject; then if ready, insert. Pad 0: Start -> p0[0] 0x80, B 0x08, A 0x04.

"Ready to insert", "has a card", "ejecting" and the insert-check count are
defined in section 7.

## 5. JVS-IO boards (`Triforce/JVSIO.cpp/.h`)

### 5.1 Interface (may be kept)

`class JVSIOBoard : public SerialDevice` with `explicit
JVSIOBoard(IOPorts*)`, `Update() override`, `DoState() override`, a protected
`using HandlerResponse = std::expected<JVSIOReportCode, JVSIOStatusCode>`,
a protected `struct FrameContext { JVSIORequestReader& request;
JVSIOResponseWriter& response; }` and a protected virtual
`HandlerResponse HandleCommand(JVSIOCommand, FrameContext)`. The enums
`JVSIOStatusCode`, `JVSIOReportCode`, `JVSIOCommand` (all `u8`) and the
classes `JVSIORequestReader`, `JVSIOResponseWriter` are forward-declared in
the header and defined in the `.cpp`. Final subclasses that override only
`HandleCommand` and inherit the constructor: `Namco_FCA_JVSIOBoard`,
`Sega_837_13551_JVSIOBoard`, `Sega_837_13844_JVSIOBoard`.

State: client address (u8, 0 = unassigned), last response (byte vector,
without the sync byte), coin counts (2 x u16), previous coin input states
(2 x bool), I/O ports pointer.

Codes: status Okay 1, UnknownCommand 2, ChecksumError 3, ResponseOverflow 4.
Report Okay 1, ParameterSizeError 2, ParameterDataError 3, Busy 4.

Commands (values; only those with a handler below do anything): 10
IOIdentify, 11 CommandRevision, 12 JVSRevision, 13 CommVersion, 14
FeatureCheck, 15 MainID, 20 SwitchInput, 21 CoinInput, 22 AnalogInput, 23
RotaryInput, 24 KeycodeInput, 25 ScreenPositionInput, 26 MiscSwitchInput, 2E
RemainingPayout, 2F DataRetransmit, 30 CoinCounterDec, 31 PayoutCounterInc,
32 GenericOutput, 33 AnalogOutput, 34 CharacterOutput, 35 CoinCounterInc, 36
PayoutCounterDec, 37 GenericOutputByte, 38 GenericOutputBit, 70
NamcoCommand, F0 Reset, F1 SetAddress, F2 CommMethodChange.

### 5.2 Framing

Sync `E0`, escape mark `D0`, broadcast address `FF`, host address `00`. On
the wire every frame body byte equal to `E0` or `D0` is sent as `D0`
followed by the byte minus 1; on receive, `D0 x` decodes to `x + 1` (any
`x`).

A request frame decodes to: destination, `N`, then `N` bytes whose last is
the checksum. The checksum is the 8-bit sum of destination, `N` and the
first `N - 1` payload bytes.

### 5.3 Update()

1. Coins: for each slot, if the I/O ports' coin input is true and the stored
   previous state was false, increment that coin count (u16, wraps). Store
   the input as the new previous state.
2. Loop while rx is not empty:
   - First byte not `E0`: log error "Expected JVSIO_SYNC", drop 1 byte,
     continue.
   - Decode 2 header bytes after the sync, then `N` more bytes. If the rx
     ends before either is complete, stop and wait (nothing consumed).
   - The whole frame (sync + escaped header + escaped payload) is consumed
     at the end of this step whatever happens below.
   - `N == 0`: log error "Empty payload"; next frame.
   - Bad checksum: log error "Bad checksum"; set the last response to the
     frame `[00, 02, 03]` + checksum (`05`) and send it (`E0 00 02 03 05`);
     next frame.
   - The request is the `N - 1` payload bytes. Destination `FF`: broadcast
     (5.4). Else if it equals the client address: unicast (5.5). Else warn
     "Unexpected destination node".
   - If request bytes are left over after processing, warn "Excess request
     bytes: {count}".

Sending a response = append `E0`, then each body byte escaped as in 5.2.

### 5.4 Broadcast requests

Empty request: nothing. Otherwise only the first command is processed:

- `F0` Reset: no parameter byte -> log error "Reset: ParameterSizeError",
  done. Else read it; if it is `D9`: notice "Reset", client address = 0,
  status switch byte 1 `|= 0x01`. In both cases (`D9` or not): reset the
  generic outputs (4.2, which may call adapter handlers) and clear the last
  response. No reply.
- `F1` SetAddress: no parameter -> log error "SetAddress: ParameterSizeError".
  Else read address `a`: info "SetAddress: {a}", client address = `a`,
  status switch byte 1 `&= ~0x01`, last response = `[00, 03, 01, 01]` +
  checksum `05`, send it (`E0 00 03 01 01 05`).
- Other: error "ProcessBroadcastFrame: UnknownCommand: {:02x}".

### 5.5 Unicast requests

If the request is not empty and its first byte is `2F` (DataRetransmit):
consume it, info "DataRetransmit", send the stored last response again
(just `E0` if it is empty), done.

Otherwise build a new last response: start with `[00, cnt, status]`
(placeholders). Then loop:
- No request bytes left: end the frame with status Okay; stop.
- Append a 1-byte report placeholder, read the command byte, call
  `HandleCommand`.
- Handler returned an error status: log error "HandleCommand: cmd:{:02x}
  StatusCode:{}", end the frame with that status (the placeholder report
  byte stays 0 and is part of the frame); stop.
- Else write the report into the placeholder. If it is not Okay: log error
  "HandleCommand: cmd:{:02x} ReportCode:{}", end the frame with status Okay;
  stop.

Ending a frame: `count = size - 1` (bytes after the destination, plus the
checksum to come). If `count > 255`, cut the response back to its first 3
bytes (`00 00 00`), with no checksum and no further change (**Quirk**: the
overflow status is not written). Else byte 1 = count, byte 2 = status, append
the 8-bit sum of all bytes so far. Then send the last response.

### 5.6 Command handlers

Base board (`JVSIOBoard::HandleCommand`):

| Cmd | Request params | Data appended | Report |
|---|---|---|---|
| `11` | - | `11` | Okay (info "CommandRevision") |
| `12` | - | `20` | Okay (info "JVSRevision") |
| `13` | - | `10` | Okay (info "CommVersion") |
| `15` | NUL-terminated string | - | Okay, consuming string + NUL (info "MainID: {string}"); no NUL in the rest of the request: ParameterDataError, nothing consumed |
| `20` | `P`, `B` | system inputs byte, then for each player `0..P-1`: the first `min(B, 2)` switch bytes, then zero bytes up to `B` | fewer than 2 params: ParameterSizeError |
| `21` | `S` | for each slot `i < S`: BE16 of `(cond << 14) \| (count_i & 0x3FFF)`, where `cond` = 0 and `count_i` = coin count for `i < 2`, else `cond` = 2 and count 0 | no param: ParameterSizeError |
| `22` | `C` | for each channel `< min(C, 8)`: BE16 of the analog input; then BE `80 00` for the rest up to `C` | no param: ParameterSizeError |
| `30`, `35` | slot, msb, lsb | - | fewer than 3: ParameterSizeError. `adj = (msb << 8 \| lsb)`, negative for `30`. Notice "CoinCounter: slot:{} adjustment:{}". If slot < 2: count = clamp(count + adj, 0, 0x3FFF). Okay. |
| `32` | `K`, then `K` bytes | - | no `K` or fewer than `K` bytes: ParameterSizeError. Else `SetGenericOutputs` with the `K` bytes, consume them, Okay. |
| other | - | - | error status UnknownCommand |

`P > 2` in `20` hits the assertion in `GetSwitchInputs` (section 13).

Board-specific (each falls back to the base for other commands):

| Board | `10` IOIdentify reply (bytes, including the final NUL) | `14` FeatureCheck reply |
|---|---|---|
| Namco_FCA | `namco ltd.;FCA-1;Ver1.01;JPN,Multipurpose + Rotary Encoder` + `00` | `01 01 10 00`, `02 02 00 00`, `03 07 00 00`, `12 06 00 00`, `13 04 00 00`, `00` |
| Sega_837_13551 | `SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10` + `00` | `01 02 0D 00`, `02 02 00 00`, `03 08 00 00`, `12 06 00 00`, `00` |
| Sega_837_13844 | `SEGA ENTERPRISES,LTD.;837-13844-01 I/O CNTL BD2 ;Ver1.00;99/07` + `00` | `01 02 0C 00`, `02 02 00 00`, `03 08 00 00`, `12 16 00 00`, `00` |

Both report Okay and log info "IOIdentify" / "FeatureCheck". Namco also has
`70`: no sub-command byte -> ParameterSizeError. Sub-command `18`: needs 4
more bytes (else ParameterSizeError); info "NAMCO_COMMAND(18): {4 bytes}",
consume them, append `FF`, Okay. Other sub-commands: error "Unknown
NAMCO_COMMAND: {:02x}", ParameterDataError.

### 5.7 DoState

`SerialDevice::DoState`, client address (1), last response (vector), coin
counts (4), previous coin states (2).

## 6. Wheels and touchscreen (`FZeroAX`, `MarioKartGP`, `Touchscreen`)

### 6.1 F-Zero AX (`Triforce/FZeroAX.cpp/.h`)

Interface (may be kept): `FZeroAXSteeringWheel final : SerialDevice` with
`Update()`, `bool IsInitializing() const`, `s8 GetServoPosition() const`,
`DoState()`; state init state (u8, 0) and servo position (s8, 0).
`FZeroAXCommon_IOAdapter(FZeroAXSteeringWheel*)` with `Update` and the
output handler; `FZeroAXDeluxe_IOAdapter` with `Update`, `DoState` and
state `m_delay` (u32, 0) and `m_rx_reply` (u8, `F0`);
`FZeroAXMonster_IOAdapter` with `Update`.

Wheel `Update()`: walk the rx bytes with a position starting at 0; while at
least 4 bytes remain from the position: if the XOR of the 4 bytes and `80`
is not 0, warn "SteeringWheel: Bad checksum!" and advance the position by 1;
else process the 4-byte request and advance by 4. Then consume everything up
to the position (fewer than 4 trailing bytes stay).

Processing a request `r`: `cmd = r[0] ^ 0x80`.
- `4`: servo = `s8(0x80 - (u8(r[1] << 7) | r[2]))` (8-bit arithmetic).
- `7`: init state = 2.
- `0x7F`: init state = 1.
- anything else: no change.
Every valid request then sends 4 zero bytes.

`IsInitializing()` is `init state == 1`. Wheel `DoState`: init state (1),
servo (1) only; its SerialDevice buffers are not saved (**Quirk**).

Common adapter `Update()` (pad 0 only):
- status switch byte 0 `&= ~0x20`;
- p0[0]: Start 0x80, Right 0x20, Left 0x10, Up 0x08, Down 0x04, A 0x02;
- p1[0]: X 0x20, Y 0x10;
- analog 0 = if the wheel is initializing, `u16(servo * 512 + 0x8000)`
  (servo as a signed int), else `ExpandValue<u16>(stickX, 8)`;
- analog 1 = `ExpandValue(stickY, 8)`, analog 4 =
  `ExpandValue(triggerRight, 8)`, analog 5 = `ExpandValue(triggerLeft, 8)`,
  analog 6 = `0x8000`.

Common output handler: `changed = set[0] | cleared[0]`; for each of the
lamps in this order: `80` "START BUTTON", `20` "VIEW CHANGE 1", `10` "VIEW
CHANGE 2", `08` "VIEW CHANGE 3", `04` "VIEW CHANGE 4", `40` "BOOST": if
changed, info "JVS-IO: {name}: ON" if `set[0]` has the bit, else
"...: OFF".

Deluxe adapter `Update()`: `seat = BE16(generic outputs bytes 1 and 2) >> 2`.
If `seat == 0x70`: `++m_delay`; if `m_delay % 10 == 0`, reply = `FB`. If
`seat == 0xF0`: reply = `F0`. Other values: no change. Then p0[0] `|= 0x01`,
p0[1] = `reply & 0xF0` (assigned), p1[1] = `u8(reply << 4)` (assigned).
`DoState`: `m_delay` (4), reply (1).

Monster adapter `Update()`: p1[0] `|= 0x02`.

### 6.2 Mario Kart Arcade GP (`Triforce/MarioKartGP.cpp/.h`)

Interface (may be kept): `MarioKartGPSteeringWheel final : SerialDevice`
with `Update()`, `Reset()`, `DoState()`, state init state (u8, 0);
`MarioKartGPCommon_IOAdapter(MarioKartGPSteeringWheel*)` with `Update` and
the output handler.

Wheel `Update()`: while 10 or more rx bytes remain, process 10 bytes, then
consume all processed bytes. Processing: if BE32 of bytes 0-3 is not
`FFFFFF01`, warn "SteeringWheel: Unknown command: {:08x}" and send nothing.
Else (bytes 4-9 are force values, debug only) by init state: 0 -> send
ASCII `E00`, state 1; 1 -> send `C06`, state 2; 2 or more -> send `C01`,
state 1.

`Reset()`: info "SteeringWheel: Reset", init state = 0. `DoState`: init
state only (1 byte; buffers not saved).

Adapter `Update()` (pad 0): status switch byte 0 `&= 0xFF` (no change);
p0[0] Start 0x80; p0[1]: A 0x20, B 0x02; analog 0 = `ExpandValue(stickX,
8)`, analog 1 = `ExpandValue(triggerRight, 8)`, analog 2 =
`ExpandValue(triggerLeft, 8)`.

Output handler: first, if `set[0] & 0x80`, reset the wheel. Then with
`changed = set[0] | cleared[0]`: `04` "ITEM BUTTON", `08` "CANCEL BUTTON",
logged as in 6.1.

### 6.3 Touchscreen (`Triforce/Touchscreen.cpp/.h`)

`Touchscreen final : SerialDevice` with `Update()` only; no own state
(`DoState` is SerialDevice's).

`Update()`: if rx is not empty, warn "Unexpected write of {n} bytes to
touchscreen." and consume it all. Then always send one 10-byte packet from
pad 0:

| Offset | Value |
|---|---|
| 0 | `55` |
| 1 | `54` |
| 2 | `FF` |
| 3-4 | LE16 `ExpandValue(u16(substickX), 4)` |
| 5-6 | LE16 `ExpandValue(u16(substickY), 4)` |
| 7 | `triggerRight` |
| 8 | `00` |
| 9 | `AA` + sum of bytes 0-8 (mod 256) |

## 7. IC card reader (`Triforce/ICCardReader.cpp/.h`)

### 7.1 Interface (may be kept)

`ICCardReader final : SerialDevice`: `explicit ICCardReader(u8 slot_index)`,
`Update()`, `bool IsCardPresent() const`, `bool IsEjecting() const`,
`bool IsReadyToInsertCard() const`, `InsertCard()`, `EjectCard()`,
`u8 GetCardPresentInsertCheckCount() const`, `DoState()`.

State: a list of cards ("tags"; several can sit on one physical card),
field on (bool, false), slot index (u8, const), eject timer (u8, 0),
insert-check count (u8, 0). Each card: 2048 data bytes (256 pages of 8), a
file name, an 8-byte UID and a state (Idle 0 / Halted 1, initially Idle).

- `IsCardPresent()` = the card list is not empty.
- `IsEjecting()` = eject timer > 60.
- `IsReadyToInsertCard()` = no card and eject timer == 0.
- `InsertCard()`: if a card is present, nothing. Else notice "InsertCard",
  create 1 card (7.4) and initialise it (7.4).
- `EjectCard()`: if no card, nothing. Else notice "EjectCard", clear the
  list, eject timer = 120, insert-check count = 0.

### 7.2 Request and reply

Request: `00`, command, BE16 payload length `L`, `L` payload bytes, then a
checksum byte making the XOR of all `L + 5` bytes 0.

`Update()`:
1. If the eject timer is nonzero, decrement it.
2. If fewer than 4 rx bytes, or fewer than `L + 5`, wait.
3. If the XOR of the `L + 5` bytes is nonzero: log error "Bad checksum!",
   consume 1 byte, return (only one step per `Update`).
4. Otherwise handle one request; the `L + 5` bytes are consumed afterwards.
   At most one request is handled per `Update`.

Before dispatch: unless the command is `20` (InsertCheck), set the
insert-check count to 0. `status` starts at 0 and the reply payload is
empty.

Size check `need(n)`: if `L < n`, log error "Undersized payload size {L}
for command: {:02x}", status = `0x0080`, skip the handler. If `L > n`, warn
"Oversized payload size ..." and continue.

Session check: a session value `s` selects card `s & 0xFF` if
`(s & 0xFF00) == 0x2300` and that index exists; otherwise log error
"Unexpected card session: {:04x}", status = `0x0080`, and the handler stops.
Sessions are not checked against earlier Selects.

Reply: `10`, command, BE16 (2 + payload length), BE16 status, payload, then
the XOR of all previous reply bytes.

### 7.3 Commands

Payload offsets are within the request payload; numbers are BE16 unless
stated. "Page" values are masked with `0xFF`.

| Cmd | need | Behaviour | Reply payload |
|---|---|---|---|
| `10` GetStatus | 0 | info "GetStatus"; status = `0x30` if the field is on else `0x20` | - |
| `11` SetBaudrate | 8 | info with the payload bytes | - |
| `14` FieldOn | 0 | info "FieldOn"; field on | - |
| `15` FieldOff | 0 | info "FieldOff"; field off; every card Idle | - |
| `16` EjectCard | 0 | `EjectCard()` | - |
| `20` InsertCheck | 8 | info "InsertCheck: {u16 at 0}"; present = some card is not Halted; if present and count < 255, ++count; status 0 if present else `0x0080` | - |
| `21` AntiCollision | 16 | info; if some card is not Halted: status = 1 if more than one is not Halted, else 0 | UID of the first non-Halted card (8) |
| | | if none: status `0x0080` | - |
| `22` SelectCard | 8 | info with the 8 UID bytes; find the card whose UID equals payload 0-7 | BE16 `0x2300 \| index` + 6 zero bytes; status 0 |
| | | not found: warn "SelectCard: Unexpected Card ID.", status `0x0080` | - |
| `24` ReadPage, `33` ReadUseCount | 8 | session @0, page @2; read 1 page | the 8 page bytes |
| `25` WritePage | 16 | session @0, unknown @2, page @4; write the 8 bytes at payload 6-13 | - (status `0x0080` if the write fails) |
| `26` DecreaseUseCount | 8 | session @0, page @2, amount @4; see 7.5 | BE16 new count + 6 zero bytes |
| `27` HaltCard | 8 | session @0; card Halted | - |
| `34` ReadPages | 8 | session @0, page @2, count @4 (not masked); read `count` pages | the bytes |
| `35` WritePages | see text | | - |
| other | - | log error "Unknown command: {:02x}"; status stays 0 | - |

Read failures (7.5) set status `0x0080` and the payload stays empty.

`35` WritePages: if `L < 8`, log error "WritePages: Undersized payload size:
{L}" and stop, **with status 0** (**Quirk**). Else session @0, page @2,
count @4; info log; session check; then `need(8 + count * 8)`; then write
`count * 8` bytes starting at payload offset 6 (**Quirk**: the data start at
offset 6 even though 8 bytes were reserved). A failed write sets `0x0080`.

All the info lines are in section 14.

### 7.4 Cards and files

Creating `n` cards (used by `InsertCard` with 1 and by `DoState`): clear the
list; for card `i`: name `slot<slot_index + 1>`, plus the letter `'a' + i`
if `i > 0` (so the second tag of slot 1 is `slot1b`); file
`tricard_<GameID>_<name>.bin` in the Triforce folder; UID
`00 00 54 4D 50 00 <slot_index> <i>`.

Initialising a card: try to load the file: open it for reading; if
`File::Exists` is false, the load fails. If the file is larger than 2048
bytes, warn "Unexpected size of {size} for file: {name}". Read
`min(size, 2048)` bytes into the start of the data (the rest stays zero); a
read failure logs error "Failed to read from file: {name}" and the load
fails. Success logs info "Loaded {n} bytes from file: {name}".

If the load fails: notice "Creating new IC Card data.", then:
- bytes 32-39 (page 4, the serial number): byte 32 = `95`, 33 = `71`, then
  by game type: KeyOfAvalon: 34 = `26`, 35 = `40 | (s % 10)` where `s` is the
  sum of both nibbles of each of bytes 36-39; VirtuaStriker4 and
  VirtuaStriker4_2006: 34 = `44`, 35 = `00`; GekitouProYakyuu: 34 = `76`,
  35 = `50`; other games leave 34-35 as they are;
- BE16 `FFFF` at byte `0x28` (the use count);
- flush bytes 32-47 to the file (7.5).

### 7.5 Card data operations

- Read(page, count): offset `page * 8`, size `count * 8`. If offset + size >
  2048, warn "ReadData: Attempt to read beyond end of card." and fail. A
  size of 0 gives an empty result, which callers treat as a failure.
- Write(page, data): offset `page * 8`. If the range overlaps bytes 32-39
  (`offset < 40 && offset + size > 32`), warn "WriteData: Read-only page."
  and fail. If offset + size > 2048, warn "WriteData: Attempt to write
  beyond end of card." and fail. Else copy and flush that range.
- DecreaseUseCount(page, amount): at offset `page * 8`, read BE16 `old`;
  `new = max(old - amount, 0)`; notice "DecreaseUseCount: {old} -> {new}";
  store BE16; flush those 2 bytes. No read-only check.
- Flush(offset, size): open the card file with `DirectIOFile`,
  `AccessMode::Write`, `OpenMode::Always`, and `OffsetWrite(offset, bytes)`;
  on failure log error "FlushData: Failed to write to: {file}".

### 7.6 DoState

`SerialDevice::DoState`; card count (u8); if it differs from the current
list size, recreate that many cards (7.4, no file access); for each card:
data (2048), UID (8), state (1); field on (1); eject timer (1); insert-check
count (1).

## 8. Deck reader (`Triforce/DeckReader.cpp/.h`)

### 8.1 Interface (may be kept)

`DeckReader final : SerialDevice` with `Update()`, `DoState()` and
`auto* GetICCardReader()` returning a pointer to its internal
`ICCardReader` (slot 0). Private state: the IC card reader, a firmware
update timeout (u8, 0) and a `File::DirectIOFile` for the firmware dump
(the header includes `Common/DirectIOFile.h`).

### 8.2 Update()

Whatever path `Update()` takes (including early returns), it ends by
calling the internal IC reader's `Update()` and passing its tx bytes
through to our tx (`PassThroughTxBytes`).

1. If the firmware timeout is nonzero: firmware mode (8.3); done.
2. If rx is empty: done.
3. If the first byte is `00` (an IC card command): if fewer than 4 bytes,
   or fewer than `BE16(bytes 2-3) + 5`, wait; else move exactly that many
   bytes to the IC reader's rx. Done.
4. Otherwise it is a one-byte deck command `c`: send `AA c`, then the body
   below, then `00`, then consume 1 byte. Only one command per `Update`.

| `c` | Name | Info log | Body |
|---|---|---|---|
| `74` | SelfTest | "SelfTest" | `00` |
| `63` | SensLock | "SensLock" | `01` |
| `76` | ProgramVersion | "ProgramVersion" | ASCII `Version 1.22,2003/09/19,171-8213B` padded with zeros to 48 bytes |
| `62` | BootVersion | "BootVersion" | ASCII `Version 1.04,2003/06/17,171-8213B` padded to 48 |
| `6B` | ProgramChecksum | "ProgramChecksum" | `89 AB CD EF` |
| `6D` | BootChecksum | "BootChecksum" | `01 23 45 67` |
| `68` | CameraCheck | "CameraCheck" | 10 zero bytes |
| `67` | ShutterGet | "ShutterGet" | BE32 1234567890 = `49 96 02 D2` |
| `61` | ShutterAuto | "ShutterAuto" | `49 96 02 D2` |
| `69`, `6C` | Shutter | "Shutter" | - |
| `6F` | ShutterLoad | "ShutterLoad" | - |
| `73` | ShutterSave | "ShutterSave" | - |
| `65` | SensCard | "SensCard" | `01` |
| `72` | ReadCard | "ReadCard" | `AA 72`, a length byte, the cards (below) |
| `66` | FirmwareUpdate | "FirmwareUpdate" | - (below) |
| other | | error "Unknown command: {:02x}" | - |

So a ReadCard reply is `AA 72 AA 72 <len> <cards> 00` (**Quirk**: the
header appears twice).

ReadCard cards: read the whole file `tricard_<GameID>_deck.json` (a missing
file reads as an empty string) and parse it with picojson. On a parse
error, log error "LoadCardDeckFromFile: {picojson error}"; if the root is
not an object, "LoadCardDeckFromFile: Invalid JSON root object."; if
`"cards"` is not an array, "LoadCardDeckFromFile: Invalid \"cards\"
array."; in these cases the length byte is `00` and no cards follow. Else
for each element of `"cards"`: non-objects are skipped; `"index"` must be a
number (else error "LoadCardDeckFromFile: Invalid \"index\" field." and the
element is skipped), converted to u16; `"table"` is a number converted to u8,
default 0; `"quantity"` is a number converted to u8, default 1 (0 adds
nothing). Each element adds `quantity` copies of the 3-byte entry
`[table, BE16 index]`. The length byte is `u8(entries * 3)` (it wraps above
85 entries; all entries are still sent). Numbers are converted with a plain
C++ cast from double, so they must be in range (section 13).

FirmwareUpdate: timeout = 240; open `card_deck_reader_firmware.bin` with
`AccessMode::Write` (truncating); notice "Writing firmware to: {file}" on
success, else error "Failed to open: {file}".

### 8.3 Firmware mode

If rx is empty: decrement the timeout; when it reaches 0, send `AA 66 00`,
info "Fake FirmwareUpdate done.", close the file. Else: if the file is open,
write all rx bytes to it (on failure error "Failed to write {n} bytes to
firmware."); consume all rx; timeout = 240.

### 8.4 DoState

`SerialDevice::DoState`, the IC reader's `DoState`, the timeout (1). In read
mode, then close the firmware file.

## 9. Magnetic card readers (`MagCard/*`)

### 9.1 Interface (may be kept)

Namespace `MagCard`. `MagneticCardReader : Triforce::SerialDevice`
(abstract, non-copyable, non-movable) with:

- `static constexpr std::size_t TRACK_SIZE = 69, NUM_TRACKS = 3;`
- `struct Settings { std::string card_name; std::string card_path; bool
  insert_card_when_possible = true; bool report_dispenser_empty = false; };`
  (not part of the savestate; owned by the SI baseboard)
- `explicit MagneticCardReader(Settings*)`, virtual destructor, `Update()`,
  `DoState()`.
- Protected enums: `R` (card position, u8) `NO_CARD, READ_WRITE_HEAD,
  THERMAL_HEAD, DISPENSER_THERMAL, ENTRY_SLOT, MAX_POSITIONS` (values 0-5;
  the wire values come from the model); `P` (error, u8) `NO_ERR 30,
  READ_ERR 31, WRITE_ERR 32, CARD_JAM 33, MOTOR_ERR 34, PRINT_ERR 35,
  ILLEGAL_ERR 38, BATTERY_ERR 40, SYSTEM_ERR 41, TRACK_1_READ_ERR 51 ...
  TRACK_2_AND_3_READ_ERR 56` (51 T1, 52 T2, 53 T3, 54 T1&2, 55 T1&3, 56
  T2&3); `S` (job status, u8) `NO_JOB 30, ILLEGAL_COMMAND 32,
  RUNNING_COMMAND 33, WAITING_FOR_CARD 34, DISPENSER_EMPTY 35, NO_DISPENSER
  36, CARD_FULL 37`; `InitMode` (u8) `Standard 30, EjectAfter 31,
  ResetSpecifications 32`.
- Protected `struct Status { R r = NO_CARD; P p = NO_ERR; S s = NO_JOB; void
  SoftReset(); }` where `SoftReset` sets `p = NO_ERR, s = NO_JOB`.
- Protected members used by the models: `virtual u8 GetPositionValue() = 0`,
  `virtual bool IsReadyForCard()`, `virtual void Command_10_Initialize()`,
  `virtual void Command_D0_ControlShutter()`, `SetPError`, `SetSError`,
  `FinishCommand`, `MoveCard(R)`, `IsCardPresent()`, the settings pointer
  and the `Status`, the current packet (byte vector).

The protected helper names in the old header (`ReceivePacket`,
`BuildPacket`, `StepStateMachine(DT)`, `StepStatePerson(DT)`,
`ClearCardInMemory`, `ReadCardFile`, `WriteCardFile`, `IsRunningCommand`,
`GetCurrentCommand`, `IsCardLoaded`, `EjectCard`, `DispenseCard`, and the
`Command_XX_*` functions) may be kept or replaced; only C1231BR and
C1231LR use the ones listed above.

State, in savestate order: status (`r`, `p`, `s`, 3 bytes), current command
(u8, 0), reply payload (byte vector), current packet (byte vector), current
step (s32, 0 = no command running), three tracks (`std::optional<std::array<u8,
69>>` each), time since the machine moved the card (`DT`), time since the
"person" moved the card (`DT`).

Helpers:
- present = `r != NO_CARD`; loaded = present and `r != ENTRY_SLOT`;
- running = step > 0;
- `FinishCommand()`: step = 0; if `s != ILLEGAL_COMMAND`, `s = NO_JOB`;
- `SetPError(e)`: warn "SetPError: {:02x}", `p = e`, finish;
  `SetSError(e)`: warn "SetSError: {:02x}", `s = e`, finish;
- `MoveCard(pos)`: if already there, nothing; else info "MoveCard: {name}"
  (names `NO_CARD`, `READ_WRITE_HEAD`, `THERMAL_HEAD`, `DISPENSER_THERMAL`,
  `ENTRY_SLOT`), set `r`, machine time = 0;
- `EjectCard()`: if loaded: notice "Ejecting card.", clear all tracks, move
  to `ENTRY_SLOT`, `Core::DisplayMessage("Ejected Magnetic Card", 4000)`;
- `DispenseCard()`: if loaded, `SetSError(ILLEGAL_COMMAND)`; else if the
  dispenser is reported empty, `s = DISPENSER_EMPTY`; else notice
  "Dispensing a new card.", `Core::DisplayMessage("Created Magnetic Card",
  4000)`, move to `DISPENSER_THERMAL`;
- base `IsReadyForCard()` = not present.

The destructor warns "Shutdown before ejecting card!" if a card is loaded.

### 9.2 Wire protocol

`Update()` loops while rx is not empty:
- `05` (ENQ): step the command machine by 50 ms (9.3), step the person by
  50 ms (9.5), send the status packet (below), consume 1.
- `02` (STX): needs at least 2 bytes and then `size = rx[1]` more; else
  wait. Check the `size` bytes as a packet (below); send `06` (ACK) if
  accepted, else warn "Writing NACK" and send `15` (NACK). Consume
  `size + 2`.
- any other byte: log error "Process: Unexpected {:02x}", consume 1.

Packet check (`p` = the `size` bytes after the size byte):
1. `size < 6`: warn "ReceivePacket: Bad packet size!", reject.
2. `p[size-2] != 03`: warn "ReceivePacket: Missing END_OF_TEXT!", reject.
3. XOR of `size` and all bytes of `p` must be 0 (the last byte is the
   checksum); else warn "ReceivePacket: Bad checksum!", reject.
4. If a command is running: info "ReceivePacket: Cancelling command: {old}"
   if `p[0] == 40`, else warn "ReceivePacket: Running command: {old}
   interrupted by command: {new}".
5. Current command = `p[0]`; current packet = `p[4 .. size-2)` (skipping
   the command and 3 host status bytes, dropping ETX and the checksum);
   `SoftReset`; `s = RUNNING_COMMAND`; step = 1; clear the reply payload.
   Accept.

Status packet: `02`, then `L = u8(payload size + 6)`, current command,
`GetPositionValue()`, `p`, `s`, the reply payload, `03`, and a checksum
that is the XOR of everything after the `02` (lead-in, payload, `03`).

### 9.3 Command machine

Each ENQ: machine time += 50 ms; if a command is running, run its handler
for the current step; if it is still running afterwards, step += 1. So
step 1 runs at the first ENQ after the command arrived, step 2 at the
second, and so on.

Handlers ("finish" = `FinishCommand`; P/S errors finish too):

| Cmd | Handler |
|---|---|
| `10` Initialize | (base) `EjectCard`, `SoftReset`, finish. C1231BR overrides it (9.6). |
| `20` ReadStatus, `40` Cancel, `78` PrintSettings, `7A` RegisterFont, `C0` ControlLED, `C1` SetPrintRetry | `SoftReset`, finish |
| `33` ReadData | see below |
| `35` GetData | step 2: if not loaded, P `ILLEGAL_ERR`; else for each of the 3 tracks: if empty, P `READ_ERR` and stop; else append its 69 bytes. Finish at step >= 2 if still running. |
| `53` WriteData | see below |
| `7B` PrintImage | if not present, P `PRINT_ERR`; else `SoftReset`, finish |
| `7C` PrintLine | packet < 3 bytes: P `SYSTEM_ERR`. Step 1: if not present, P `PRINT_ERR`, else move to `THERMAL_HEAD`. Step 2: move to `READ_WRITE_HEAD`. Finish at step >= 2. |
| `7D` Erase | as `7C` without the packet-size check |
| `7E` PrintBarcode | step 2: if not present, P `ILLEGAL_ERR`. Finish at step >= 2. |
| `80` EjectCard | at step >= 3: `EjectCard`, finish |
| `A0` Clean | step 2: if not loaded: notice "Inserting cleaning card.", `Core::DisplayMessage("Inserting cleaning card", 4000)`, move to `THERMAL_HEAD`. Step 3: `EjectCard`. Finish at step >= 3. |
| `B0` DispenseCard | mode = packet[0] if the packet is not empty, else `31`. Step 2: mode `32` (check only): `s = DISPENSER_EMPTY` if the dispenser is reported empty, else `s = CARD_FULL`; other modes: `DispenseCard()`. Step 3: if `r == DISPENSER_THERMAL`, move to `READ_WRITE_HEAD`. Finish at step >= 3 if still running. |
| `D0` ControlShutter | (base) S `ILLEGAL_COMMAND`. C1231BR overrides it. |
| `F0` GetVersion | append ASCII `AP:S1234-5678,OS:S9012-3456,0000` (32 bytes), `SoftReset`, finish |
| other | warn "HandlePacket: Unhandled command {:02x}", S `ILLEGAL_COMMAND` |

Note that `s` values set during a step (for example `DISPENSER_EMPTY` or
`CARD_FULL`) are visible in status packets until the finish turns them back
into `NO_JOB`.

`33` ReadData (packet: mode, bit mode, track):
- packet < 3 bytes: P `SYSTEM_ERR` (at every step).
- step 1: if mode is `32` (card capture), info "Command_33_ReadData:
  CardCapture".
- step 2, mode `32`: if not present: `s = WAITING_FOR_CARD`, step -= 1 (so
  step 2 runs again at the next ENQ). Else if `r != READ_WRITE_HEAD`: notice
  "Capturing Card.", move to `READ_WRITE_HEAD`, read the card file (9.4),
  `Core::DisplayMessage("Inserted Magnetic Card", 4000)`.
- step 2, other modes: tracks = the track list (below) for the track byte.
  If not loaded or the list is empty: P `ILLEGAL_ERR`. Else for each listed
  track: if empty, P `READ_ERR`, warn "Command_33_ReadData: Empty track
  {:02x}" (the track index), stop; else append its 69 bytes.
- finish at step >= 2 if still running.

`53` WriteData (packet: mode, bit mode, track, data):
- packet < 3 bytes: P `SYSTEM_ERR`.
- step < 2: nothing.
- tracks = the list for the track byte; data = packet from offset 3. If
  mode is not `30`, or not loaded, or the list is empty, or the data length
  is not `69 * tracks`: warn "Command_53_WriteData: Failed.", P
  `ILLEGAL_ERR`.
- else info "Command_53_WriteData: Writing {n} track(s) to memory."; for
  the `k`-th listed track, store data bytes `69k .. 69k+68` (section 13 on
  why this is positional); `Core::DisplayMessage("Writing Magnetic Card",
  4000)`; write the card file (9.4); finish.

Track lists: `30` -> [0], `31` -> [1], `32` -> [2], `33` -> [0, 1], `34` ->
[0, 2], `35` -> [1, 2], `36` -> [0, 1, 2], other -> empty.

### 9.4 Card file

The card file is `card_path + card_name` (for the SI baseboard:
`<Triforce>/tricard_<GameID>.bin`). It holds up to 3 tracks of 69 bytes back
to back.

Reading: notice "Reading card data from: {card_name}"; clear all tracks;
open with `DirectIOFile` read; if the size is 0, not a multiple of 69, or
more than 3 tracks, log error "ReadCardFile: Incorrect card file size:
{size}" (and continue); for `k < min(size / 69, 3)`: read 69 bytes into
track `k`; on a read failure log error "File read failed.", empty track `k`
and stop. A missing file has size 0.

Writing: notice "Writing card data to: {card_name}"; open with
`DirectIOFile` write (truncating); for each present track `k`,
`OffsetWrite(69k, track)`; on failure log error "File write failed.".

### 9.5 The person

Each ENQ, after the machine step: person time += 50 ms. If a card is
loaded, nothing else. The "file exists" test (`File::Exists(card file)`)
is evaluated at most once per step, only when needed.

- No card present: if `insert_card_when_possible`, `IsReadyForCard()`,
  person time > 500 ms and the file exists: notice "Inserting card.",
  `r = ENTRY_SLOT`, person time = 0. (This does not reset the machine time.)
- Card at the entry slot: let "machine moved last" = machine time < person
  time. If (machine moved last and machine time > 100 ms) or the file does
  not exist: notice "Removing card.", `r = NO_CARD`, person time = 0.

All time comparisons are strict.

### 9.6 Models

**C1231BR** (F-Zero AX, `MagCard/C1231BR.h`): `final`, inherits the
constructor; adds shutter open (bool, initially true).
- Position value = `(shutter open ? 0x80 : 0x40) | (report_dispenser_empty ?
  0 : 0x20) | pos`, with `pos` for `NO_CARD 00, READ_WRITE_HEAD 18,
  THERMAL_HEAD 07, DISPENSER_THERMAL 1C, ENTRY_SLOT 01`.
- `IsReadyForCard()` = not present and shutter open.
- Initialize (`10`): if the packet is not empty and packet[0] is `31`
  (EjectAfter): shutter open, move to `ENTRY_SLOT` (the tracks are not
  cleared). Then finish. No `SoftReset`.
- ControlShutter (`D0`): empty packet: P `SYSTEM_ERR`. Else shutter open =
  (packet[0] == `31`), info "Shutter: Open" / "Shutter: Close", finish.
- `DoState`: base `DoState`, then shutter open (1).

**C1231LR** (Mario Kart GP, `MagCard/C1231LR.h`): `final`, inherits the
constructor; position value `NO_CARD 30, READ_WRITE_HEAD 31, THERMAL_HEAD
32, DISPENSER_THERMAL 33, ENTRY_SLOT 34`. Nothing else is overridden.

### 9.7 DoState

`SerialDevice::DoState`, status (3), current command (1), reply payload
(vector), current packet (vector), step (4), track 0, 1, 2 (optional, 1 +
69 when present), machine time (8), person time (8).

## 10. EXI baseboard (`EXI/EXI_DeviceBaseboard.cpp/.h`)

### 10.1 Interface (may be kept)

```
namespace ExpansionInterface {
void GenerateInterrupt(int flag);
class CEXIBaseboard final : public IEXIDevice {
public:
  explicit CEXIBaseboard(Core::System& system);
  ~CEXIBaseboard() override;
  void SetCS(int cs) override;
  bool IsInterruptSet() override;
  bool IsPresent() const override;      // true
  void DoState(PointerWrap& p) override;
  void DMAWrite(u32 addr, u32 size) override;
  void DMARead(u32 addr, u32 size) override;
protected:
  void TransferByte(u8& byte) override;
};
}
```

`ImmWrite`/`ImmRead` are the base class's (byte by byte through
`TransferByte`, most significant byte first; reads feed `00` into
`TransferByte`). `GenerateInterrupt` is called by the media board (section
2).

State: three file-scope statics shared by all instances: interrupt set
(bool, false), IRQ timer (u32, 0), IRQ status (u32, 0). Per instance:
position (u32, 0), the 4 command bytes (0), backup offset (u16, 0), DMA
offset (u32) and length (u32) (stored, otherwise unused), and the backup
file (`File::IOFile`).

### 10.2 Construction

1. File `tribackup_<GameID>.bin` in the Triforce folder: open with mode
   `rb+`; if that fails, `wb+`.
2. If the file is not good: `PanicAlertFmt("Failed to open {}\nFile might be
   in use.", file name)`; seed `std::srand` with the current time; open
   `tribackup_tmp_<std::rand()><GameID>.bin` with `wb+` instead.
3. If the game type is VirtuaStriker4 or GekitouProYakyuu (not
   VirtuaStriker4_2006) and the file is at least `0x400` bytes: read it all;
   store u16 `0x1703` in host order (bytes `03 17`) at offsets `0x12` and
   `0x212`; store at `0x0A` the BE16 of the low 16 bits of the byte sum of
   `0x1F4` bytes from `0x0C`, and at `0x20A` the same for `0x1F4` bytes from
   `0x20C`; seek to 0, write it all back, flush. (The file position is then
   at the end.)

### 10.3 Interrupts and chip select

- `GenerateInterrupt(flag)`: interrupt set = true, timer = 0, status =
  `flag`; then `System::GetInstance().GetExpansionInterface().UpdateInterrupts()`.
- `IsInterruptSet()`: if set: `++timer`; if timer > 12, clear the flag;
  return true (so it reports true 13 times after each trigger). Else false.
- `SetCS(cs)`: if `cs` is nonzero, position = 0.

### 10.4 TransferByte(byte)

`pos` = position; `cmd` = the 4 command bytes. In order:

1. If `pos < 4`: `cmd[pos] = byte`, `byte = FF`.
2. Then exactly one of:
   - `pos >= 2` and `cmd[0] == 0` and `cmd[1] == 0` (serial ID read):
     `byte = {06, 04, 10, 00}[(pos - 2) & 3]`.
   - `pos == 3`: a check value is computed over cmd 0-2 (debug only; no
     effect); `byte` stays `FF`.
   - `pos == 4`: by `cmd[0]`:

| cmd[0] | Effect | byte |
|---|---|---|
| `01` backup offset set | offset = `cmd[1] << 8 \| cmd[2]`; seek the file there | `01` |
| `02` backup write | write the 1 byte `cmd[1]` at the file position; flush | `01` |
| `03` backup read | - | `01` |
| `05` DMA offset/length set | DMA offset = `cmd[1] << 8 \| cmd[2]`, length = `cmd[3]`; notice log | `01` |
| `82` read ISR | notice log | `04` |
| `83` write ISR | IRQ status `&= ~cmd[2]`; notice log | `04` |
| `86` read IMR, `87` write IMR | notice log | `04` |
| `FF` write LANCNT | notice log; if `cmd[1] == 0 && cmd[2] == 0`: set = true, timer = 0, status = 2; if `cmd[1] == 2 && cmd[2] == 1`: status = 0. No interrupt update call. | `08` |
| other | error "AM-BB: COMMAND: ..." | `04` |

   - `pos > 4`: by `cmd[0]`:

| cmd[0] | byte |
|---|---|
| `03` | the next byte read from the file (unchanged if the read fails, e.g. at end of file) |
| `05` | `01` |
| `82` | at `pos == 6`: the IRQ status (low byte), and the interrupt flag is cleared; other positions: `04` |
| `86` | `pos == 5`: `FF`; `pos == 6`: `81`; else unchanged |
| other | unchanged; error "Unknown AM-BB command: {:02x}" |

   - otherwise (`pos` 0, 1, or 2 without the serial case): `byte = FF`.
3. `++position`.

The file position is shared by reads and writes and advances with each.

### 10.5 DMA

`DMAWrite(addr, size)`: `span = memory.GetSpanForAddress(addr)`; if
`span.size() < size`, log error "AM-BB: Backup DMA Write overflow: ..." and
return. Else notice "AM-BB: COMMAND: Backup DMA Write: {:08x} {:x}", seek
the file to the backup offset (not the DMA offset), write `size` bytes from
memory, flush. `DMARead` is the same with "Read", reading `size` bytes from
the file into memory (a short read leaves the rest of memory unchanged; no
flush).

### 10.6 DoState

Position (4), the static interrupt flag (1), the 4 command bytes. The timer,
status, offsets and file are not saved.

## 11. AGP cartridge reader (`EXI/EXI_DeviceAGP.cpp/.h`)

### 11.1 Interface (may be kept)

```
class CEXIAgp : public IEXIDevice {
public:
  CEXIAgp(Core::System& system, const Slot slot);   // slot must be a memory card slot (ASSERT)
  ~CEXIAgp() override;
  bool IsPresent() const override { return true; }
  void ImmWrite(u32 _uData, u32 _uSize) override;
  u32 ImmRead(u32 _uSize) override;
  void DoState(PointerWrap& p) override;
};
```

`SetCS`, DMA and `IsInterruptSet` are the base class's. State (savestate
order in 11.6): slot (`Slot`, an `int` enum), address (u32, 0, never
changed), current command (u32, 0), EEPROM bytes (vector), EEPROM command
(u32, 0), EEPROM data (u64, 0), EEPROM mask (u32, 0), EEPROM position (u16,
0), EEPROM size (u32, 0), address end (u16, 0), address mask (u16, 0), read
mask (u16, 0), status mask (u32, 0), write status (bool, false), hash (u8,
0), position (u32, 0, never changed), return position (u32, 0), ROM bytes
(vector), ROM mask (u32, 0), ROM size (u32, 0), ROM offset (u32, 0).

Constants: ignore bits 4, data bits 64 (0x40), read false `0A`, read true
`0B`.

### 11.2 Files

`Config::Get(Config::GetInfoForAGPCartPath(slot))` (`MAIN_AGP_CART_A_PATH`
or `_B_PATH`) is split with `SplitPath` into folder (with trailing
separator), name (no extension) and extension (with the dot). `base` =
folder + name.

Construction: load the ROM from `base + extension`, info "Loaded GBA rom:
{base} card: {slot}", load the save from `base + ".sav"`, info "Loaded GBA
sav: {base} card: {slot}".

- ROM: open with `rb`. If it opens: ROM size = file size (low 32 bits),
  ROM mask = size - 1, read the whole file. Else the ROM is 0x2000 zero
  bytes with size and mask left at 0.
- Save: open with `rb`. If it opens: EEPROM size = file size, EEPROM mask =
  size - 1, read the whole file. If the size is 512 or 8192: reinterpret it
  as 64-bit words: for each 8-byte group, the host `u64` at that position
  becomes the big-endian value of the 8 file bytes (so on a LE host each
  group is byte-reversed in memory); address end = 8 (512) or 16 (8192);
  address mask = `3F` or `3FF`; read mask = `80` or `8000`; status mask =
  `1FFFF00` if the ROM size is exactly 32 MiB, else `1000000`. Any other
  size: status mask = 0, the others stay 0. If the save does not open:
  EEPROM size 0, no bytes. **Quirk:** with an empty configured path the
  save file name is `.sav` relative to the current directory.

Destruction: if `base` is not empty, write `base + ".sav"` with `wb`: for
sizes 512 and 8192 the words are converted back to file order (each host
`u64` written as its 8 bytes most significant first); otherwise the bytes
as they are (possibly 0 bytes, which creates an empty file).

### 11.3 Hash

`CRC8(bytes)`: for each byte `b`: `h ^= b`; then `h` = XOR of the table
entries for each set bit of `h`: bit 0 `5E`, 1 `BC`, 2 `61`, 3 `C2`, 4 `9D`,
5 `23`, 6 `46`, 7 `8C`. (This is the Dallas/Maxim 1-Wire CRC-8, reflected
polynomial `8C`.) "Hash `x`" below means apply this to the single byte `x`.

### 11.4 ImmWrite(data, size)

If `size == 1` and the top byte of `data` is 0: ignore. Else by the current
command:

| Current command | Effect |
|---|---|
| `AE020000`, `AE030000` | ROM offset = `(data & FFFFFF00) >> 7`; return position = 0; hash bytes 3, 2, 1 of `data` (top first) |
| `AE040000` | EEPROM position = `u16((data >> 16) & EEPROM mask)`; hash bytes 3, 2 |
| `AE070000` | EEPROM position as above; if EEPROM size != 0, EEPROM byte at that position = byte 1 of `data`; hash bytes 3, 2, 1 |
| `AE0C0000` | one EEPROM bit (below); then position += 1, return position = 0, hash bytes 3, 2 |
| `AE0B0000` | write status = false (the command stays) |
| `AE000000`, `AE010000`, `AE090000` | write status = false, then as `AE0A0000` |
| `AE0A0000` | EEPROM position = 0, then as default |
| anything else (default) | current command = `data`; return position = 0; hash = `FF`; hash byte 2 of `data` |

So only the default rows (and `AE0x` rows that fall into them) change the
current command; while a command such as `AE0C0000` is current, every write
is data for it.

EEPROM bit write, with `pos` = EEPROM position, `end` = address end, `bit`
= `(data >> 16) & 1`, `special = (EEPROM command & read mask) ? end : end +
64`:
- If `pos < end` or `pos == special`: mask = `1 << (end - min(pos, end))`;
  set or clear that bit of the EEPROM command. Then if `pos == end + 64`:
  if EEPROM size != 0, store the EEPROM data as the 64-bit word at index
  `(EEPROM command >> 1) & address mask`; write status = true.
- Else: mask = `1 << (end + 63 - pos)`; set or clear that bit of the EEPROM
  data.

### 11.5 ImmRead(size)

By the current command (then "reset" = current command = 0):

| Current command | Result |
|---|---|
| `AE000000` | `5AAA5517`; reset |
| `AE010000` | `01020304` if the return position is 0, else `F0020304`. If the return position is 1, reset; else set it to 1 (the command stays). |
| `AE020000` | if write status and status mask != 0 and `(ROM offset & status mask) == status mask`: `v1 = 01, v2 = 00`; else `v1 = ROM[offset++ & ROM mask]`, `v2 = ROM[offset++ & ROM mask]`. Hash `v2`, then `v1`. Result `v2 << 24 \| v1 << 16 \| hash << 8`. Reset. |
| `AE030000` | if `size == 1`: `FF000000`, reset. Else read 4 ROM bytes `v1..v4` as above; hash `v2, v1, v4, v3`; result `v2 << 24 \| v1 << 16 \| v4 << 8 \| v3`; the command stays. |
| `AE040000` | `v` = `FF` if EEPROM size is 0, else the EEPROM byte at the EEPROM position; hash `v`; result `v << 24 \| hash << 16`; reset |
| `AE0B0000` | `v` = `0B` if EEPROM size != 0 and position >= 4 and bit `(63 - (position - 4))` of the 64-bit word at index `(EEPROM command >> 1) & address mask` is 1; else `0A`. Hash `00`, then `v`. Result `v << 16 \| hash << 8`. Position += 1. Reset. |
| `AE070000`, `AE0C0000` | `hash << 24`; reset |
| other | 0; reset |

The ROM offset is a u32 and wraps.

### 11.6 DoState

slot (4), address (4), current command (4), EEPROM bytes (vector), EEPROM
command (4), EEPROM data (8), EEPROM mask (4), EEPROM position (2), EEPROM
size (4), address end (2), address mask (2), read mask (2), status mask
(4), write status (1), hash (1), position (4), return position (4), ROM
bytes (vector), ROM mask (4), ROM size (4), ROM offset (4).

## 12. Modem adapter (`EXI/EXI_DeviceModem.cpp/.h`)

### 12.1 Interface (must be kept)

The header is used by the original `EXI/Modem/TAPServerModem.cpp`, so keep
it as is, including the private parts that file uses:

- `static constexpr std::size_t MODEM_RECV_SIZE = 0x800;`
- `enum { EXI_DEVTYPE_MODEM = 0x02020000 };`
- `enum class ModemDeviceType { TAPSERVER };`
- `class CEXIModem : public IEXIDevice` with public `CEXIModem(Core::System&,
  ModemDeviceType)`, destructor, `SetCS`, `IsPresent` (true),
  `IsInterruptSet`, `ImmWrite`, `ImmRead`, `DMAWrite`, `DMARead`, `DoState`;
- private `void SendComplete();` and `void AddToReceiveBuffer(std::string&&
  data);`
- the private nested `class NetworkInterface` (protected `CEXIModem*
  m_modem_ref`, protected explicit constructor taking it, public virtual
  `Activate()` false, `Deactivate()`, `IsActivated()` false,
  `SendAndRemoveAllHDLCFrames(std::string*)` false, `RecvInit()` false,
  `RecvStart()`, `RecvStop()`, virtual destructor) and `class
  TAPServerNetworkInterface : public NetworkInterface` (public constructor
  `(CEXIModem*, const std::string& destination)`, the seven overrides,
  private `TAPServerConnection m_tapserver_if;` and `void
  HandleReceivedFrame(std::string&&)`).

The header includes `Core/HW/EXI/BBA/TAPServerConnection.h` and
`Core/HW/EXI/EXI_Device.h`. Other private members may change.

State: the network interface (owned), transfer descriptor (u32, invalid =
`FFFFFFFF`), AT command bytes, AT reply bytes, send buffer, receive buffer
(strings; the receive buffer is guarded by a mutex because frames arrive on
the tapserver thread), 32 registers (all 0 except below).

### 12.2 Registers, interrupts, descriptors

Registers used: `00` device type, `01` interrupt mask, `02` pending
interrupts, `04` AT command size, `05` AT reply size, `0B`/`0C` bytes
available (high/low), `10`/`11` RX threshold (high/low). The others
(`03`, `06`-`0A`, `0D`-`0F` TX threshold, `12`, `13`) are plain storage.
Interrupt bits: `02` AT reply available, `10` send buffer below threshold,
`20` receive buffer above threshold (`40` exists but is never set).

Construction: create the `TAPServerNetworkInterface` with
`Config::Get(Config::MAIN_MODEM_TAPSERVER_DESTINATION)`, info "Created
tapserver physical network interface.", then register `00` = 2 and `01` = 2.
Destruction: `RecvStop()`, `Deactivate()`.

Setting interrupt flag `f` to `on`: set or clear `f` in register `02`, then
always call `ScheduleUpdateInterrupts(CPU or NON_CPU, 0)` on the system's
EXI manager (`CPU` from CPU-side calls, `NON_CPU` from received frames).

`IsInterruptSet()` = `(reg 01 & reg 02) != 0`. `SetCS(any)` = descriptor
invalid. `DoState` saves nothing.

A descriptor `d`: write if bit 30 is set; modem transfer if bit 29 is set;
for modem transfers the kind is `d & 0F000000` (`03000000` AT buffer,
`08000000` packet buffer) and the remaining size is bits 8-23; for register
transfers the first register is `(d >> 24) & 1F`. Updating the remaining
size to `n` keeps the top byte of `d`, puts `n` in bits 8-23 and clears the
low 8 bits.

### 12.3 Immediate transfers

`ImmWrite(data, size)`:
- No descriptor: descriptor = `data`. If it is `00008000` (reset):
  `RecvStop()`, `Deactivate()`, descriptor invalid.
- Read descriptor: log error "Received EXI IMM write ... after read
  command ...", descriptor invalid.
- Modem write descriptor: the top `size` bytes of `data`, most significant
  first, are written (12.5).
- Register write descriptor: starting at its first register, while `size`
  is left and the register number is < 32: store the top byte of `data`,
  shift `data` left by 8, next register. If register 1 or 2 was written,
  `ScheduleUpdateInterrupts(CPU, 0)`. Descriptor invalid.

`ImmRead(size)`:
- No descriptor: log error "Received EXI IMM read ({size} bytes) with no
  pending transfer", return 0.
- Write descriptor: log error "Received EXI IMM read ... after write command
  ...", descriptor invalid, return 0.
- Modem read descriptor: read `size` bytes (12.5) into a zeroed 4-byte
  buffer; return it as a BE u32 (first byte in the top position).
- Register read descriptor: if the first register is 0, return
  `02020000` (**Quirk**: the descriptor is not cleared). Else for `z <
  size` while the register number + `z` < 32, OR register `first + z` into
  bits `(3 - z) * 8`; descriptor invalid; return.

### 12.4 DMA

`DMAWrite(addr, size)`: no descriptor: log error "Received EXI DMA write
{addr} ({size} bytes) after read command {descriptor}" (**Quirk**: the same
text as the next case) and keep the descriptor. Read descriptor: that error,
descriptor invalid. Register descriptor: error "Received EXI DMA write ...
to registers ...", descriptor invalid. Modem write: write `size` bytes from
`memory.GetPointerForRange(addr, size)` (12.5).

`DMARead(addr, size)`: no descriptor: error "Received EXI DMA read ... with
no pending transfer". Write descriptor: error "... after write command ...",
invalid. Register descriptor: error "... to registers ...", invalid. Modem
read: read `size` bytes into `GetPointerForRange(addr, size)`.

### 12.5 Modem buffers

Reading `size` bytes (`req` = remaining size in the descriptor):
1. If `size > req`: log error "More bytes requested ({size}) than
   originally requested for transfer {:x}", `size = req`.
2. AT kind: copy `n = min(size, AT reply length)` bytes from the AT reply
   and remove them; register `05` = `u8(remaining reply length)`; interrupt
   `02` = (reply not empty). Packet kind: under the lock, copy and remove
   `n = min(size, receive length)` bytes from the receive buffer, then the
   receive-buffer update (12.6) from the CPU. Other kinds: error "Invalid
   modem read transfer type {:x}". Destination bytes past `n` are left as
   they are.
3. Remaining = `req - size`; if 0 the descriptor becomes invalid, else
   update its size.

Writing `size` bytes (`exp` = remaining size):
1. If `size > exp`: log error "More bytes received ({size}) than expected for
   transfer {:x}" and return (nothing stored, descriptor unchanged).
2. AT kind: append to the AT command bytes, run the AT commands (12.7),
   register `04` = `u8(AT command length)` (what is left after running).
   Packet kind: append to the send buffer; interrupt `10` on; call the
   network interface's `SendAndRemoveAllHDLCFrames(&send buffer)`. Other:
   error "Invalid modem write transfer type {:x}".
3. Remaining = `exp - size`; descriptor update as above.

`SendComplete()` (called by the tapserver glue when at least one frame went
out): interrupt `10` on, from the CPU.

### 12.6 Receive buffer

`AddToReceiveBuffer(data)` (from the tapserver thread): under the lock,
append `data`, then the update from a non-CPU thread.

Update: `n = min(receive length, 0x200)`; register `0B` = `n >> 8`, `0C` =
`n & FF`; interrupt `20` = (receive length >= BE16 of registers `10`/`11`).

### 12.7 AT commands

While the AT command bytes contain `\r` or `\n`: take the text before the
first one as the command and remove it together with that one terminator;
info "Received AT command: {command}"; then:
- starts with `ATZ`, or equals `ATH0`: `RecvStop()`, `Deactivate()`, reply
  `OK\r`;
- starts with `ATD`: if `Activate()` succeeds, `RecvStart()` and reply
  `OK\rCONNECT 115200\r`; else reply `OK\rNO ANSWER\r`;
- anything else (including empty): reply `OK\r`.

Replying appends to the AT reply, sets register `05` = `u8(reply length)`
and interrupt `02` = (reply not empty), from the CPU.

## 13. Undefined behaviour in the old code, and what to do instead

The old code has a few paths that read or write out of bounds, shift by a
negative amount or depend on the clock. The harness never drives them (it
keeps a shadow model of the device state and skips the unsafe step), so any
safe behaviour keeps the hashes equal. Each item gives the recommended
behaviour, chosen so that it matches the old code's defined neighbours.

### 13.1 AGP (section 11)

| Path | Old code | Do instead |
|---|---|---|
| ROM file opens but is empty | ROM mask = `FFFFFFFF` on an empty vector; ROM reads index past the end | treat ROM reads of an empty ROM as `00` (what the missing-file case gives: 0x2000 zero bytes, mask 0) |
| `AE0C0000` bit write with `pos > end + 63` (and not the "special" position) | shift count `end + 63 - pos` is negative | change no bit; the rest of the row (position += 1, return position = 0, hash) still happens |
| `AE0B0000` read with `pos > 67` | shift count `63 - (pos - 4)` is negative | the bit reads as 0, so `v = 0A`; position += 1 and the hash as usual |
| `AE0B0000` read or `AE0C0000` 64-bit store when the EEPROM has fewer than 8 bytes (a nonstandard `.sav` size of 1-7) | reads or writes a u64 past the end of the byte vector | read: `v = 0A`; store: skip the store, write status still becomes true |
| `AE040000` read with the EEPROM position at or past the EEPROM size (the position grows with `AE0C`/`AE0B`) | reads past the end | `v = FF`, as if there were no EEPROM |

The u64 accesses into the byte vector are also type-punned through a cast;
write them with `std::memcpy` (or byte loops) giving the same host-order
value.

### 13.2 Modem (section 12)

- `ImmRead(size)` with `size > 4` copies `size` bytes into a 4-byte buffer.
  EXI immediate transfers are 1 to 4 bytes; clamp `size` to 4 for the copy
  and keep the descriptor bookkeeping with the clamped size.
- `DMAWrite`/`DMARead` pass `memory.GetPointerForRange(addr, size)` on
  without checking it. A bad range gives a null pointer, and the old code then
  appends from or copies to null. Instead, a null pointer means
  nothing is copied: the transfer still counts against the descriptor, and a
  write still runs the AT command or HDLC step on what is already buffered.
  The harness only uses valid ranges.

### 13.3 Magnetic card readers (section 9.3)

`53` WriteData with the track byte `31`, `32`, `34` or `35`: the old code
takes track `t`'s data from payload offset `69 * t` (the track number, not
its position in the list). The length check allows only `69 * count` bytes,
so for these lists the subspan runs past the end of the payload. Store the
`k`-th listed track from offset `69 * k`. For `30`, `33` and `36` this is
identical to the old code, and those are the only defined cases. The harness
sends `31`/`32`/`34`/`35` only with a wrong length (the `ILLEGAL_ERR` path).

### 13.4 JVS (section 5.6)

`20` SwitchInput with a player count above 2 calls
`IOPorts::GetSwitchInputs(i)` for `i >= 2`. In the old code that is an
`ASSERT` (a yes/no panic; continuing reads past the 2-player array). Instead,
send zero bytes for players 2 and up, `bytes_per_player` of them each. An
`ASSERT` may stay or go: the harness records only that an assertion fired
(13.8), and does not drive this path. A truncated `20` followed by another
command reads that command byte as a parameter (5.6), which is defined; the
harness only ends frames with truncated commands.

### 13.5 Deck reader JSON (section 8.2)

`"index"`, `"table"` and `"quantity"` are converted from `double` with a
plain cast. Out-of-range values, NaN and infinities are UB. Convert in-range
integral values exactly as the cast does. For anything else (negative, too
large, non-integral beyond truncation, NaN), clamp to the type's range, with
NaN as 0. The harness writes only in-range integers.

### 13.6 Media board (section 2)

- `dimm_disc` is only set by `InitDIMM`. A game read (2.6 step 15) before
  `InitDIMM` dereferences null. Treat a missing reader as a failed read
  (return 1). The harness always calls `InitDIMM` after `Init`.
- Select with no usable timeout polls with -1 (infinite). That is the
  specified behaviour. The harness's fake `poll` never blocks, so the new
  code must make the same `poll` call and not substitute a finite timeout.
- `BindEphemeralPort` seeds `std::mt19937` from the clock, so the ports it
  tries are not reproducible. Keep the distribution, the attempt count and
  the per-attempt info log ("BindEphemeralPort: bind ..."). The harness
  masks the port of a bind that this log line follows, and drops the line
  itself from the log hash.
- The old file includes `Common/UnixUtil.h`, which the tree no longer has
  (deleted in W1). The only thing used from it was a "retry while the call
  returns -1 with `errno == EINTR`" wrapper around POSIX socket calls. The
  new file must do that retry itself (a small local helper), not include
  the header. The harness supplies a shim only for building the old file.
- "Unhandled Media Board Read/Write" panics format argument 0 twice, so the
  "length" shows the offset (**Quirk**, keep it; see 14).

### 13.7 EXI baseboard (section 10.2)

When the backup file cannot be opened, the fallback name is
`tribackup_tmp_<std::rand()><GameID>.bin` after `std::srand(time)`. That
name is not reproducible. Keep the behaviour (same panic text, `srand` with
the current time, one `rand()`); the harness does not drive it.

### 13.8 Assertions in general

The harness fakes a yes/no panic as "yes". It records an `ASSERT` (an alert
whose text starts with `Failed check:` or holds a `Source:` line) only as
the line `ASSERT` in the log stream, without file, line or condition. Other
alerts are recorded in full in the behaviour stream (style, type and text).

### 13.9 What could not be pinned down

- **Windows-only socket behaviour.** The media board's socket paths are
  specified for both branches (2.9), but the harness runs only the POSIX
  branch against fake sockets. Real blocking, partial sends, `WSAGetLastError`
  numbering on Windows and `SO_SNDTIMEO` on Windows (milliseconds, not a
  `timeval`) are specified from reading the code and are not checked.
- **Threads.** The modem's receive path runs on the tapserver thread
  (`AddToReceiveBuffer`, `SendComplete`) and takes a lock. The harness
  delivers frames on the main thread between operations, so lock ordering
  and interleavings are not checked.
- **Real time.** The magnetic card reader's person model uses fixed
  simulated steps (50 ms per update; 500 ms and 100 ms thresholds) and no
  wall clock, so it is covered. The media board's ephemeral port (13.6) and
  the EXI baseboard fallback name (13.7) are not.
- **Real game data.** The harness serves a synthetic disc pattern and random
  `segaboot.gcm`, network config and card files. Behaviour with real
  firmware or games is covered only to the extent that it follows the same
  paths.

## 14. Strings

### 14.1 User-visible strings (must stay verbatim)

Panic alerts. A `T` suffix means the format goes through translation
(`PanicAlertFmtT`), so the string is also a translation key. Do not change
it, its argument indexes or its `{0}` reuse.

| Where | Kind | Text |
|---|---|---|
| media board Init (2.5) | PanicAlertFmt | `Failed to open/create: {}` (the full path of each of `trinetcfg.bin`, `trinetctrl.bin`, `triextra.bin`, `tridimm.bin`, `backup.bin`, checked in that order) |
| media board Init | PanicAlertFmt | `Failed to open segaboot.gcm({}), which is required for test menus.` |
| media board Init | PanicAlertFmt | `Failed to read: {}` (the segaboot path) |
| media board DMA read (2.6) | PanicAlertFmtT | `Unhandled Media Board Read: offset={0:08x} length={0:08x}` (**Quirk**: both fields show argument 0, the offset) |
| media board DMA write | PanicAlertFmtT | `Unhandled Media Board Write: offset={0:08x} length={0:08x}` (same quirk) |
| media board Execute1 | PanicAlertFmtT | `Unhandled Media Board Command:{0:04x}` |
| media board DI Execute | PanicAlertFmtT | `Unhandled Media Board Execute:{0:04x}` |
| media board ExecuteCommand | PanicAlertFmtT | `Unhandled Media Board Command:{0:02x}` |
| game type (2.2) | PanicAlertFmtT | `Unknown game ID:{0:08x}, using default controls.` |
| SI baseboard (3.3) | PanicAlertFmt | `SI: Unknown command` |
| EXI baseboard (10.2) | PanicAlertFmt | `Failed to open {}\nFile might be in use.` |

On-screen messages:

| Where | Call | Text |
|---|---|---|
| media board bind failure (2.9) | `OSD::AddMessage(msg, OSD::Duration::SHORT, OSD::Color::RED)` | `Failed to bind socket {ip}:{port}` (also logged as an error) |
| I/O ports test switch (4.3) | `OSD::AddMessage(..., OSD::Duration::NORMAL, OSD::Color::RED)` | `Test menu is disabled due to missing SegaBoot.` |
| magnetic card (9) | `Core::DisplayMessage(..., 4000)` | `Inserted Magnetic Card`, `Writing Magnetic Card`, `Inserting cleaning card`, `Ejected Magnetic Card`, `Created Magnetic Card` |

### 14.2 Log format strings

Every log format string in the old files, as a `fmt` format string with the
C escapes of the source (`\n`, `\"`). Each entry gives the level (E error,
W warning, N notice, I info, D debug) and the log category. Debug lines are
compiled out at the default log level, so the harness never sees them. A
string listed once may be logged from several places. The harness hashes
every line it sees as `<level letter><LogType number> <formatted text>`, so
for the log hash to match, both the level and the category matter as well
as the text.

The SI baseboard's "Overflow details" message prints three host pointers
with `fmt::ptr`. The harness masks `0x` followed by 9 to 16 hex digits as
`0xPTR`, so print real pointers in the same `fmt::ptr` form. The
"BindEphemeralPort:" info line is left out of the log hash (13.6).

#### Media board (2)

- W AMMEDIABOARD: `Invalid MediaBoardRange: start=0x{:08x}, size=0x{:06x}, buffer_size=0x{:06x}`
- W AMMEDIABOARD: `GC-AM: Bad GuestSocket value: {}`
- E AMMEDIABOARD: `GuestSocket not found. This should not happen.`
- E AMMEDIABOARD: `GC-AM: Read overflow: offset=0x{:08x}, length={}, source_size={}`
- E AMMEDIABOARD: `GC-AM: Memory buffer too small: address=0x{:08x}, length={}, span={}`
- E AMMEDIABOARD: `GC-AM: Write overflow: offset=0x{:08x}, length={}, destination_size={}`
- E AMMEDIABOARD: `GC-AM: failed to create socket ({})`
- D AMMEDIABOARD: `GC-AM: {:08x} {:08x} {:08x} {:08x}`
- D AMMEDIABOARD: `GC-AM: TestHardware phase 2 ({}): sending result response (testStatus=2, checkProgress=100)`
- E AMMEDIABOARD: `Bad IP redirection string: {}`
- I AMMEDIABOARD: `BindEphemeralPort: bind ({}:{}) = {} ({})`
- I AMMEDIABOARD: `NetDIMMConnect: {}:{}`
- I AMMEDIABOARD: `NetDIMMConnect: Redirecting to: {}:{}`
- I AMMEDIABOARD_NET: `NetDIMMConnect: connect( {}({}), ({},{}:{}) ):{} ({})`
- W AMMEDIABOARD: `NetDIMMConnect: connect: {} ({})`
- E AMMEDIABOARD: `NetDIMMConnect: PlatformPoll: {}`
- E AMMEDIABOARD: `NetDIMMConnect: getsockopt: {}`
- I AMMEDIABOARD_NET: `NetDIMMConnect: connect( {}({}) ) succeeded`
- E AMMEDIABOARD_NET: `NetDIMMConnect: connect( {}({}) ) failed with error {}: {}`
- E AMMEDIABOARD_NET: `NetDIMMAccept: bad parmeters`
- D AMMEDIABOARD: `NetDIMMAccept: {}({})`
- E AMMEDIABOARD: `NetDIMMAccept: PlatformPoll: {}`
- D AMMEDIABOARD: `NetDIMMAccept: Timeout.`
- E AMMEDIABOARD: `AMMBCommandAccept: accept( {}({}) ) failed: {}`
- N AMMEDIABOARD: `AMMBCommandAccept: {}:{}`
- N AMMEDIABOARD: `AMMBCommandAccept: Translating result to: {}:{}`
- I AMMEDIABOARD: `GetAdjustedBindIPv4Port: Considering game modified IP: {}`
- I AMMEDIABOARD: `GetAdjustedBindIPv4Port: Redirecting to: {}:{}`
- N AMMEDIABOARD: `NetDIMMBind: {}({}) {}, {}, {}:{}`
- I AMMEDIABOARD_NET: `NetDIMMBind: bind( {}({}), ({},{}:{}) ):{}`
- E AMMEDIABOARD: `NetDIMMBind: {} with error {}: {}`
- E AMMEDIABOARD_NET: `AMMBCommandRecv: Bad data offset or length: off={:08x} len={}`
- E AMMEDIABOARD_NET: `GC-AM: recv( {}, 0x{:08x}, {} ) failed with error {}: {}`
- I AMMEDIABOARD_NET: `GC-AM: recv( {}, 0x{:08x}, {} ):0 shutdown received`
- D AMMEDIABOARD_NET: `GC-AM: recv( {}, 0x{:08x}, {} ):{} {}`
- E AMMEDIABOARD_NET: `AMMBCommandSend: Bad data offset or length: off={:08x} len={}`
- E AMMEDIABOARD_NET: `GC-AM: send( {}({}), 0x{:08x}, {} ) failed with error {}: {}`
- D AMMEDIABOARD_NET: `GC-AM: send( {}({}), 0x{:08x}, {} ): {} {}`
- N AMMEDIABOARD_NET: `GC-AM: socket( {}, {} ):{}`
- N AMMEDIABOARD_NET: `GC-AM: closesocket( {}({}) ):{}`
- E AMMEDIABOARD_NET: `AMMBCommandConnect: Unexpected length: {}`
- E AMMEDIABOARD_NET: `AMMBCommandConnect: Bad address offset: {:08x}`
- E AMMEDIABOARD_NET: `AMMBCommandAccept: Bad address offset or addrlen: off={:08x} len={}`
- E AMMEDIABOARD_NET: `AMMBCommandBind: Unexpected length: {}`
- E AMMEDIABOARD_NET: `AMMBCommandBind: Bad address offset: {:08x}`
- E AMMEDIABOARD_NET: `Bad FDSET offset: {:08x}`
- E AMMEDIABOARD_NET: `AMMBCommandSelect: Bad timeout offset: {:08x}`
- W AMMEDIABOARD: `AMMBCommandSelect: Infinite timout!`
- D AMMEDIABOARD_NET: `GC-AM: select( {}, 0x{:08x} 0x{:08x} 0x{:08x} 0x{:08x} ) timeout={}`
- D AMMEDIABOARD: `AMMBCommandSelect: Polling with socket count: {}`
- D AMMEDIABOARD_NET: `GC-AM: select result: {}`
- E AMMEDIABOARD_NET: `GC-AM: select failed: {} ({})`
- E AMMEDIABOARD_NET: `AMMBCommandSetSockOpt: Bad optval offset or length: off={:08x} len={}`
- N AMMEDIABOARD_NET: `GC-AM: setsockopt( {:d}, {:04x}, {}, {:p}, {} ):{:d} ({})`
- N AMMEDIABOARD_NET: `GC-AM: modifyMyIPaddr({})`
- E AMMEDIABOARD: `GC-AM: Write overflow: address=0x{:08x}, length={}, span={}`
- E AMMEDIABOARD: `GC-AM: Read overflow: address=0x{:08x}, length={}, span={}`
- D AMMEDIABOARD: `GC-AM: {:08x} {:08x} DMA=addr:{:08x},len:{:08x} Keys: {:08x} {:08x} {:08x}`
- E AMMEDIABOARD: `Unhandled Media Board Read: offset={0:08x} length={0:08x}`
- I AMMEDIABOARD: `GC-AM: Read All.Net Buffer ({:08x},{})`
- D AMMEDIABOARD: `GC-AM: NetConfig Read (intercepted) offset={:08x} config[0]={}`
- D AMMEDIABOARD: `GC-AM: Read Execute1 response (saved)`
- D AMMEDIABOARD: `GC-AM: Read Execute2 response (saved)`
- D AMMEDIABOARD: `GC-AM: Read MediaBoard ({:08x},{:08x})`
- D AMMEDIABOARD: `GC-AM: Execute command: (2){0:04X}`
- D AMMEDIABOARD: `GC-AM: TestHardware (Execute2): type={:08x} str_ptr={:08x}`
- E AMMEDIABOARD_NET: `GC-AM: Invalid size for address: InetAddr():{}`
- N AMMEDIABOARD_NET: `GC-AM: InetAddr( {} )`
- N AMMEDIABOARD_NET: `GC-AM: listen( {}, {} ):{:d}`
- I AMMEDIABOARD_NET: `GC-AM: SetTimeOuts( {}({}), {}, {}, {} ):{}`
- E AMMEDIABOARD_NET: `GC-AM: Invalid Socket: SetTimeOuts( {}({}), {}, {}, {} ):{}`
- N AMMEDIABOARD_NET: `GC-AM: GetParambyDHCPExec({})`
- D AMMEDIABOARD_NET: `GC-AM: GetLastError( {}({}) ):EWOULDBLOCK`
- N AMMEDIABOARD_NET: `GC-AM: GetLastError( {}({}) ):{}`
- N AMMEDIABOARD_NET: `GC-AM: InitLink`
- N AMMEDIABOARD_NET: `GC-AM: AllNetInit`
- D AMMEDIABOARD: `GC-AM: Cleanup command {:04x} (Execute2)`
- E AMMEDIABOARD: `GC-AM: Command:{0:04x}`
- E AMMEDIABOARD: `GC-AM: Command Unhandled!`
- E AMMEDIABOARD: `GC-AM: Invalid firmware buffer range: offset={}, length={}`
- E AMMEDIABOARD: `GC-AM: Invalid DIMM Disc read from: offset={}, length={}`
- E AMMEDIABOARD: `GC-AM: Invalid firmware write: offset={}, length={}`
- I AMMEDIABOARD: `GC-AM: Write Firmware ({:08x})`
- D AMMEDIABOARD: `GC-AM: Execute command: (1):{0:04X}`
- D AMMEDIABOARD: `GC-AM: TestHardware (Execute1 inner): type={:08x} str_ptr={:08x}`
- D AMMEDIABOARD: `GC-AM: Cleanup command {:04x} (Execute1)`
- E AMMEDIABOARD: `Unhandled Media Board Command:{0:04x}`
- D AMMEDIABOARD: `GC-AM: NetConfig persist to trinetcfg.bin (status={})`
- D AMMEDIABOARD: `GC-AM: Write MediaBoard ({:08x},{:08x})`
- E AMMEDIABOARD: `Unhandled Media Board Write: offset={0:08x} length={0:08x}`
- D AMMEDIABOARD: `GC-AM: TestHardware: ({:08x})`
- D AMMEDIABOARD: `GC-AM:               ({:08x})`
- N AMMEDIABOARD: `GC-AM: 0x605`
- N AMMEDIABOARD_NET: `GC-AM: SetupLink:`
- N AMMEDIABOARD_NET: `GC-AM:  Size: ({}) `
- N AMMEDIABOARD_NET: `GC-AM:  Port: ({})`
- N AMMEDIABOARD_NET: `GC-AM:LinkNum:({:02x})`
- N AMMEDIABOARD_NET: `GC-AM:        ({:02x})`
- N AMMEDIABOARD_NET: `GC-AM:        ({:04x})`
- N AMMEDIABOARD_NET: `GC-AM:   IP:  ({})`
- N AMMEDIABOARD_NET: `GC-AM:        ({:08x})`
- N AMMEDIABOARD_NET: `GC-AM: SearchDevices: ({})`
- N AMMEDIABOARD_NET: `GC-AM:        Offset: ({:04x})`
- N AMMEDIABOARD_NET: `GC-AM:                ({:08x})`
- E AMMEDIABOARD_NET: `SearchDevices: Bad data offset: {:08x}`
- N AMMEDIABOARD_NET: `GC-AM: {:08x} {:08x} {:08x} {:08x}`
- N AMMEDIABOARD_NET: `GC-AM: 0x608( {} {} {} )`
- N AMMEDIABOARD_NET: `GC-AM: 0x614`
- E AMMEDIABOARD: `GC-AM: Execute buffer UNKNOWN:{0:04x}`
- E AMMEDIABOARD: `Unhandled Media Board Execute:{0:04x}`
- E AMMEDIABOARD: `Unhandled Media Board Command:{0:02x}`
- W AMMEDIABOARD: `GC-AM: Bad socket fd used by the debugger: {}`

#### SI baseboard (3)

- E SERIALINTERFACE_AMBB: `Bad command_length: {} for request_length: {}`
- E SERIALINTERFACE_AMBB: `GC-AM: data_in overflow in {}`
- E SERIALINTERFACE_AMBB: `GC-AM: data_out overflow in {}`
- E SERIALINTERFACE_AMBB: `Overflow details:\n - data_in(begin={}, current={}, end={}, n_in={})\n - data_out(offset={}, size={}, n_out={})`
- D SERIALINTERFACE_AMBB: `GC-AM: Command 0x10, {:02x} (READ STATUS&SWITCHES)`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x11, {:02x} (READ SERIAL NR)`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x12, {:02x} {:02x}`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x14, {:02x} {:02x}`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x15, {:02x} (READ FIRM VERSION)`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x16, {:02x} (READ FPGA VERSION)`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x1F, {:02x} {:02x} {:02x} {:02x} {:02x} (REGION)`
- D SERIALINTERFACE_AMBB: `GC-AM: Command 0x21, {:02x}, {:02x}, {:02x}, {:02x}`
- D SERIALINTERFACE_AMBB: `GC-AM: Command 0x22, {:02x}, {:02x}, {:02x}, {:02x}, {:02x}, {:02x}, {:02x}`
- D SERIALINTERFACE_AMBB: `GC-AM: Command 0x23, {:02x} {:02x}`
- D SERIALINTERFACE_AMBB: `GC-AM: Command 0x24, {:02x} {:02x}`
- N SERIALINTERFACE_AMBB: `GC-AM: Command 0x60, {:02x} {:02x} {:02x}`
- E SERIALINTERFACE_AMBB: `GC-AM: Command {:02x} (unknown) {:02x} {:02x} {:02x} {:02x} {:02x}`
- D SERIALINTERFACE_AMBB: `Command response: {}`
- E SERIALINTERFACE: `Unknown SI command (0x{:08x})`
- W SERIALINTERFACE_AMBB: `Unhandled SendCommand(0x{:08x}, 0x{:02x})`

#### I/O ports (4)

- W SERIALINTERFACE_JVSIO: `SetGenericOutputs: Unexpected byte count: {}`
- D SERIALINTERFACE_JVSIO: `SetGenericOutputs: set:{:02x} clr:{:02x} ({:02x})`
- I SERIALINTERFACE_JVSIO: `JVS-IO: Slot 1: {}`
- I SERIALINTERFACE_JVSIO: `JVS-IO: Slot 2: {}`

#### JVS (5)

- E SERIALINTERFACE_JVSIO: `Expected JVSIO_SYNC`
- E SERIALINTERFACE_JVSIO: `Empty payload`
- E SERIALINTERFACE_JVSIO: `Bad checksum`
- W SERIALINTERFACE_JVSIO: `Unexpected destination node: 0x{:02x}`
- W SERIALINTERFACE_JVSIO: `Excess request bytes: {}`
- E SERIALINTERFACE_JVSIO: `Reset: ParameterSizeError`
- N SERIALINTERFACE_JVSIO: `Reset`
- E SERIALINTERFACE_JVSIO: `SetAddress: ParameterSizeError`
- I SERIALINTERFACE_JVSIO: `SetAddress: {}`
- E SERIALINTERFACE_JVSIO: `ProcessBroadcastFrame: UnknownCommand: {:02x}`
- I SERIALINTERFACE_JVSIO: `DataRetransmit`
- E SERIALINTERFACE_JVSIO: `HandleCommand: cmd:{:02x} StatusCode:{}`
- E SERIALINTERFACE_JVSIO: `HandleCommand: cmd:{:02x} ReportCode:{}`
- I SERIALINTERFACE_JVSIO: `CommandRevision`
- I SERIALINTERFACE_JVSIO: `JVSRevision`
- I SERIALINTERFACE_JVSIO: `CommVersion`
- I SERIALINTERFACE_JVSIO: `MainID: {}`
- D SERIALINTERFACE_JVSIO: `SwitchInput: player_count:{} bytes_per_player:{}`
- D SERIALINTERFACE_JVSIO: `AnalogInput: channel_count:{}`
- D SERIALINTERFACE_JVSIO: `GenericOutput: {}`
- D SERIALINTERFACE_JVSIO: `CoinInput: slot_count:{}`
- N SERIALINTERFACE_JVSIO: `CoinCounter: slot:{} adjustment:{}`
- I SERIALINTERFACE_JVSIO: `IOIdentify`
- I SERIALINTERFACE_JVSIO: `FeatureCheck`
- I SERIALINTERFACE_JVSIO: `NAMCO_COMMAND({:02x}): {:02x}`
- E SERIALINTERFACE_JVSIO: `Unknown NAMCO_COMMAND: {:02x}`

#### F-Zero AX (6.1)

- I SERIALINTERFACE_JVSIO: `JVS-IO: {}: {}`
- W SERIALINTERFACE_AMBB: `SteeringWheel: Bad checksum!`
- D SERIALINTERFACE_AMBB: `SteeringWheel: Request: {:02x}`
- D SERIALINTERFACE_AMBB: `SteeringWheel: servo_position: {}`

#### Mario Kart GP (6.2)

- I SERIALINTERFACE_JVSIO: `JVS-IO: {}: {}`
- D SERIALINTERFACE_AMBB: `SteeringWheel: Request: {:02x}`
- W SERIALINTERFACE_AMBB: `SteeringWheel: Unknown command: {:08x}`
- D SERIALINTERFACE_AMBB: `SteeringWheel: FFB: {:04x} {:04x} {:04x}`
- I SERIALINTERFACE_AMBB: `SteeringWheel: Reset`

#### Touchscreen (6.3)

- W SERIALINTERFACE_AMBB: `Unexpected write of {} bytes to touchscreen.`

#### IC card reader (7)

- W SERIALINTERFACE_CARD: `Unexpected size of {} for file: {}`
- E SERIALINTERFACE_CARD: `Failed to read from file: {}`
- I SERIALINTERFACE_CARD: `Loaded {} bytes from file: {}`
- E SERIALINTERFACE_CARD: `Bad checksum!`
- E SERIALINTERFACE_CARD: `Undersized payload size {} for command: {:02x}`
- W SERIALINTERFACE_CARD: `Oversized payload size {} for command: {:02x}`
- E SERIALINTERFACE_CARD: `Unexpected card session: {:04x}`
- I SERIALINTERFACE_CARD: `GetStatus`
- I SERIALINTERFACE_CARD: `SetBaudrate: {:02x}`
- I SERIALINTERFACE_CARD: `FieldOn`
- I SERIALINTERFACE_CARD: `FieldOff`
- I SERIALINTERFACE_CARD: `InsertCheck: {}`
- I SERIALINTERFACE_CARD: `AntiCollision: unk0:{:04x} unk1:{:02x}`
- I SERIALINTERFACE_CARD: `SelectCard: {:02x}`
- W SERIALINTERFACE_CARD: `SelectCard: Unexpected Card ID.`
- I SERIALINTERFACE_CARD: `ReadPage: session:{:04x} page:{}`
- I SERIALINTERFACE_CARD: `WritePage: session:{:04x} unk:{} page:{}`
- I SERIALINTERFACE_CARD: `DecreaseUseCount: session:{:04x}, page:{} amount:{}`
- I SERIALINTERFACE_CARD: `HaltCard: session:{:04x}`
- I SERIALINTERFACE_CARD: `ReadPages: session:{:04x} page:{} page_count:{}`
- E SERIALINTERFACE_CARD: `WritePages: Undersized payload size: {}`
- I SERIALINTERFACE_CARD: `WritePages: session:{:04x} page:{} page_count:{}`
- E SERIALINTERFACE_CARD: `Unknown command: {:02x}`
- W SERIALINTERFACE_CARD: `ReadData: Attempt to read beyond end of card.`
- D SERIALINTERFACE_CARD: `\n{}`
- W SERIALINTERFACE_CARD: `WriteData: Read-only page.`
- W SERIALINTERFACE_CARD: `WriteData: Attempt to write beyond end of card.`
- N SERIALINTERFACE_CARD: `DecreaseUseCount: {} -> {}`
- E SERIALINTERFACE_CARD: `FlushData: Failed to write to: {}`
- N SERIALINTERFACE_CARD: `Creating new IC Card data.`
- N SERIALINTERFACE_CARD: `InsertCard`
- N SERIALINTERFACE_CARD: `EjectCard`

#### Deck reader (8)

- E SERIALINTERFACE_CARD: `LoadCardDeckFromFile: {}`
- E SERIALINTERFACE_CARD: `LoadCardDeckFromFile: Invalid JSON root object.`
- E SERIALINTERFACE_CARD: `LoadCardDeckFromFile: Invalid \"cards\" array.`
- E SERIALINTERFACE_CARD: `LoadCardDeckFromFile: Invalid \"index\" field.`
- I SERIALINTERFACE_CARD: `SelfTest`
- I SERIALINTERFACE_CARD: `SensLock`
- I SERIALINTERFACE_CARD: `ProgramVersion`
- I SERIALINTERFACE_CARD: `BootVersion`
- I SERIALINTERFACE_CARD: `ProgramChecksum`
- I SERIALINTERFACE_CARD: `BootChecksum`
- I SERIALINTERFACE_CARD: `CameraCheck`
- I SERIALINTERFACE_CARD: `ShutterGet`
- I SERIALINTERFACE_CARD: `ShutterAuto`
- I SERIALINTERFACE_CARD: `Shutter`
- I SERIALINTERFACE_CARD: `ShutterLoad`
- I SERIALINTERFACE_CARD: `ShutterSave`
- I SERIALINTERFACE_CARD: `SensCard`
- I SERIALINTERFACE_CARD: `ReadCard`
- I SERIALINTERFACE_CARD: `FirmwareUpdate`
- N SERIALINTERFACE_CARD: `Writing firmware to: {}`
- E SERIALINTERFACE_CARD: `Failed to open: {}`
- E SERIALINTERFACE_CARD: `Unknown command: {:02x}`
- I SERIALINTERFACE_CARD: `Fake FirmwareUpdate done.`
- E SERIALINTERFACE_CARD: `Failed to write {} bytes to firmware.`

#### Magnetic card reader (9)

- W SERIALINTERFACE_CARD: `Shutdown before ejecting card!`
- I SERIALINTERFACE_CARD: `Command_33_ReadData: CardCapture`
- N SERIALINTERFACE_CARD: `Capturing Card.`
- D SERIALINTERFACE_CARD: `Command_33_ReadData: mode {:02x} track {:02x}`
- W SERIALINTERFACE_CARD: `Command_33_ReadData: Empty track {:02x}`
- W SERIALINTERFACE_CARD: `Command_53_WriteData: Failed.`
- I SERIALINTERFACE_CARD: `Command_53_WriteData: Writing {} track(s) to memory.`
- N SERIALINTERFACE_CARD: `Inserting cleaning card.`
- N SERIALINTERFACE_CARD: `Reading card data from: {}`
- E SERIALINTERFACE_CARD: `ReadCardFile: Incorrect card file size: {}`
- E SERIALINTERFACE_CARD: `File read failed.`
- D SERIALINTERFACE_CARD: `ReadCardFile: track:{} data:{}`
- N SERIALINTERFACE_CARD: `Writing card data to: {}`
- D SERIALINTERFACE_CARD: `WriteCardFile: Skipping empty track:{}`
- D SERIALINTERFACE_CARD: `WriteCardFile: track:{} data:{}`
- E SERIALINTERFACE_CARD: `File write failed.`
- W SERIALINTERFACE_CARD: `SetPError: {:02x}`
- W SERIALINTERFACE_CARD: `SetSError: {:02x}`
- D SERIALINTERFACE_CARD: `Process: ENQ`
- E SERIALINTERFACE_CARD: `Process: Unexpected {:02x}`
- D SERIALINTERFACE_CARD: `Writing ACK`
- W SERIALINTERFACE_CARD: `Writing NACK`
- D SERIALINTERFACE_CARD: `ReceivePacket: {}`
- W SERIALINTERFACE_CARD: `ReceivePacket: Bad packet size!`
- W SERIALINTERFACE_CARD: `ReceivePacket: Missing END_OF_TEXT!`
- W SERIALINTERFACE_CARD: `ReceivePacket: Bad checksum!`
- I SERIALINTERFACE_CARD: `ReceivePacket: Cancelling command: {:02x}`
- W SERIALINTERFACE_CARD: `ReceivePacket: Running command: {:02x} interrupted by command: {:02x}`
- N SERIALINTERFACE_CARD: `Inserting card.`
- N SERIALINTERFACE_CARD: `Removing card.`
- D SERIALINTERFACE_CARD: `StepStateMachine command: {:02x}`
- W SERIALINTERFACE_CARD: `HandlePacket: Unhandled command {:02x}`
- I SERIALINTERFACE_CARD: `MoveCard: {}`
- N SERIALINTERFACE_CARD: `Ejecting card.`
- N SERIALINTERFACE_CARD: `Dispensing a new card.`

#### C1231BR (9.6)

- I SERIALINTERFACE_CARD: `Shutter: {}`

#### C1231LR (9.6)

(no log strings)

#### EXI baseboard (10)

- D SP1: `AM-BB: ChipSelect={}`
- D SP1: `AM-BB: IRQ`
- E SP1: `AM-BB: Backup DMA Write overflow: address=0x{:08x}, length={}, span={}`
- N SP1: `AM-BB: COMMAND: Backup DMA Write: {:08x} {:x}`
- E SP1: `AM-BB: Backup DMA Read overflow: address=0x{:08x}, length={}, span={}`
- N SP1: `AM-BB: COMMAND: Backup DMA Read: {:08x} {:x}`
- D SP1: `AM-BB: > {:02x}`
- D SP1: `AM-BB: cs: {:02x}, w: {:02x}`
- D SP1: `AM-BB: COMMAND: BackupOffsetSet:{:04x}`
- D SP1: `AM-BB: COMMAND: BackupWrite:{:04x}-{:02x}`
- D SP1: `AM-BB: COMMAND: BackupRead :{:04x}`
- N SP1: `AM-BB: COMMAND: DMAOffsetLengthSet :{:04x} {:02x}`
- N SP1: `AM-BB: COMMAND: ReadISR  :{:02x} {:02x}:{:02x} {:02x}`
- N SP1: `AM-BB: COMMAND: WriteISR :{:02x} {:02x}`
- N SP1: `AM-BB: COMMAND: ReadIMR  :{:02x} {:02x}`
- N SP1: `AM-BB: COMMAND: WriteIMR :{:02x} {:02x}`
- N SP1: `AM-BB: COMMAND: WriteLANCNT :{:02x} {:02x}`
- E SP1: `AM-BB: COMMAND: {:02x} {:02x} {:02x}`
- E SP1: `Unknown AM-BB command: {:02x}`
- D SP1: `AM-BB < {:02x}`

#### AGP (11)

- I EXPANSIONINTERFACE: `Loaded GBA rom: {} card: {}`
- I EXPANSIONINTERFACE: `Loaded GBA sav: {} card: {}`
- D EXPANSIONINTERFACE: `AGP read {:x}`
- D EXPANSIONINTERFACE: `AGP command {:x}`

#### Modem (12)

- I SP1: `Created tapserver physical network interface.`
- E SP1: `Received EXI IMM write {:x} ({} bytes) after read command {:x}`
- E SP1: `Received EXI DMA write {:x} ({} bytes) after read command {:x}`
- E SP1: `Received EXI DMA write {:x} ({} bytes) to registers {:x}`
- E SP1: `Received EXI IMM read ({} bytes) with no pending transfer`
- E SP1: `Received EXI IMM read ({} bytes) after write command {:x}`
- E SP1: `Received EXI DMA read {:x} ({} bytes) with no pending transfer`
- E SP1: `Received EXI DMA read {:x} ({} bytes) after write command {:x}`
- E SP1: `Received EXI DMA read {:x} ({} bytes) to registers {:x}`
- E SP1: `More bytes requested ({}) than originally requested for transfer {:x}`
- E SP1: `Invalid modem read transfer type {:x}`
- E SP1: `More bytes received ({}) than expected for transfer {:x}`
- E SP1: `Invalid modem write transfer type {:x}`
- I SP1: `Received AT command: {}`

## 15. Keys, credentials and identifiers

The old files hold **no cryptographic keys, IVs or credentials**:

- The media board's command keys (`InitKeys`, 2.5/2.6) are not constants.
  The game's IPL writes them into emulated RAM (words 0, 4 and 8), and the
  board reads them from there at run time, XORs the command words with them
  and multiplies them by a per-command seed. Nothing secret is in the code.
- No Nintendo keys, IPL/BIOS, DSP ROM, firmware or game data are in these
  files. `segaboot.gcm` (the Triforce boot firmware) is a user-supplied file;
  the spec describes only how it is opened and read. The harness writes
  random bytes in its place.

The old code does contain fixed identifiers. They are behaviour data that
games read back, not secrets, and this spec lists them where they are used.
**Owner decision:** they are listed so that phase B can match the old
behaviour. If the owner prefers neutral values, phase B can change them; then
the affected hashes are expected to differ and the cases must be noted.

| Identifier | Where | Use |
|---|---|---|
| `AADE-01B98394904` | 3.3, SI baseboard `11` | baseboard serial number reply |
| `A89E-27A50364511` | 2.7, 2.11 | media board serial (Execute `0x103`) |
| `AP:S1234-5678,OS:S9012-3456,0000` | 9.3 `F0` | magnetic card reader version reply |
| IC card serial prefixes `95 71 26 4x`, `95 71 44 00`, `95 71 76 50` | 7.4 | the first serial bytes of a new card |
| region table (20 bytes) | 3.3 `1F` | region reply |
| All.Net reply string (252 bytes) | 2.11 | network setup reply (`sega.com`, a fixed date, `place_id=1234`) |
| deck reader version strings | 8.2 | version replies |
| JVS board identify strings | 5.6 | board names (Namco, Sega boards) |

None of these is a Nintendo or Sega secret: they are serial-number-shaped
placeholders, version strings and a configuration reply. No action is
needed beyond the owner's call above.

## 16. Test plan and harness

### 16.1 What the harness does

`tools/w43-harness/` builds one program, `w43h`, from:
- the 15 device files under test;
- the original `Triforce/SerialDevice.cpp` and `EXI/Modem/TAPServerModem.cpp`;
- the real `Common` file, string, network, random, socket-context, JSON and
  config code;
- `fakes.cpp`, which fakes every other neighbour: emulated memory (24 MiB,
  mirrored), CoreTiming (events are queued and fired on demand), EXI/SI
  interrupts, pads, SConfig/game ID, OSD and display messages, HLE patches,
  DiscIO (a fake volume and blob reader with a deterministic pattern), the
  tapserver connection, the logger and the panic handler;
- `harness.cpp`, which drives the devices.

Sockets are faked by linking with `-Wl,--wrap=` for `socket`, `close`, `bind`,
`connect`, `listen`, `accept`, `recv`, `send`, `setsockopt`, `getsockopt`,
`poll` and `ioctl`. Fake descriptors start at 1000. The result of each call
comes from the case seed and a per-call-kind counter, so it does not depend
on timing. `close` and `ioctl` on descriptors below 1000 go to the real
functions.

The harness includes only the devices' public headers. It uses the
declarations that sections 2.2, 3.1, 4.1, 5.1, 6, 7.1, 8.1, 9.1, 10.1, 11.1
and 12.1 say may be kept (12.1 must be kept), so it builds unchanged against
the new files as long as those declarations stay.

Each case runs in a forked child, in its own directory
`<dir>/case/{user/Triforce,work}`. The user path points at `user`, and the
current directory is `work`, because AGP with an empty path writes `.sav`
there (11.2). A case drives a seeded random sequence through the device's
public interface. It includes savestate round trips into fresh instances:
`STATE` lines carry the size and hash of the saved bytes, so the `DoState`
layouts are checked byte for byte. At the end it hashes the files the device
left and the emulated RAM.

Every observable effect becomes a line: replies, register reads, memory
writes, files, events, interrupts, socket and tapserver calls, alerts and OSD
messages. These go to the **behaviour** hash. Log lines go to the separate
**log** hash. Before hashing, the case directory is replaced by `$DIR`, and
`0x` followed by 9 to 16 hex digits by `0xPTR`. The case kinds:

| Kind | Drives |
|---|---|
| `jvs-namco`, `jvs-13551`, `jvs-13844` | a JVS board directly with random frames (escapes, bad checksums, broadcasts, address assignment, every command) |
| `si-<game>` (12 game IDs, including an unknown and a short one) | the SI baseboard `RunBuffer` with every sub-command, the JVS board, serial devices and adapters of that game, test/service switches, coin and pad input |
| `wheel-fzero`, `wheel-mkgp`, `touch` | the serial wheel and touchscreen devices |
| `iccard`, `deck` | IC card and deck reader packets, card files, JSON decks, firmware mode |
| `magcard-br`, `magcard-lr` | the C1231BR / C1231LR card readers: packets, ACK/NAK, card files, insert/eject, the simulated person |
| `exi-baseboard` | the EXI baseboard commands, interrupts, DMA and backup file |
| `agp`, `agp-32m` | AGP ROM and EEPROM commands with several save sizes, and a sparse 32 MiB ROM |
| `modem` | the modem registers, AT commands, packets and tapserver frames |
| `mb-misc`, `mb-cmd`, `mb-net` | the media board: init/firmware/keys, DMA reads/writes, Execute1/Execute2/DI Execute board commands, network commands against fake sockets |

### 16.2 Build and run

Needs g++ 13 or later and the third-party headers `fmt/` (any recent fmt
works with `FMT_HEADER_ONLY`), `picojson.h` and `mbedtls/` (headers only).
If the system lacks them, point `W43_INC` (colon-separated) at folders that
have them. Build output goes to scratch space:

```
W43_INC=/path/inc:/path/inc/picojson tools/w43-harness/build.sh <out_dir> [Source/Core dir]
<out_dir>/w43h --dir <scratch_dir> --seed 1 --cases 6 > new.txt
diff tools/w43-harness/expected-seed1.txt new.txt
```

Options: `--seed N` (default 1), `--cases N` per kind (default 6),
`--only kind,kind`, `--case K` (one case index), `--trace` (print every line
with a `B` or `L` prefix: use it with `--only` and `--case` to find the first
differing line), `--list`. Each case prints `kind #i seed=... beh=<hash>
log=<hash> lines=<behaviour lines>/<log lines>`, and the run ends with
`cases=N crashes=M`. A crash (a signal or a non-zero exit in the child) is
reported as `CRASH status=...` and counts as a failure. Use a separate
`<out_dir>` for each source tree. `build.sh` reuses objects only when they
are newer than their sources and the flags are unchanged. To stop a stray
run, use `pkill -x w43h`.

### 16.3 Old-side results

Built against the old files at f2330ef (this branch):
- `--seed 1 --cases 6`: 29 kinds, 174 cases, 0 crashes, 348 hashes (one
  behaviour and one log hash per case). The reference output is
  `tools/w43-harness/expected-seed1.txt`.
- The output is byte-identical across repeated runs, across different
  `--dir` paths, and across `-O0`, `-O1` and `-O2` builds (`W43_OPT`).
- `--seed 7 --cases 25` (725 cases) gives 0 crashes and identical output
  for two different `--dir` paths.

### 16.4 Acceptance for phase B

1. `build.sh` builds `w43h` against the new files with no changes to
   `tools/w43-harness/`.
2. `--seed 1 --cases 6` gives no crashes, and every `beh=` hash equals
   `expected-seed1.txt`.
3. `log=` hashes are expected to match too when the log strings of section
   14 are kept verbatim. Any case whose log hash differs must be explained
   by a deliberate rewording.
4. For extra coverage, also run `--seed 7 --cases 25` against both trees
   (the old tree from git history, built by someone allowed to, or in CI)
   and compare.
5. A behaviour difference is found with `--only <kind> --case <i> --trace`
   on both sides; the first differing `B` line names the operation.

If the harness itself changes, `expected-seed1.txt` must be regenerated
from the old files with the same commit of the harness.
