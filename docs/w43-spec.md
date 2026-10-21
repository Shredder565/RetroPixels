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

<!-- next -->
