// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceIPL.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"
#include "Common/Timer.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Sram.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"

namespace ExpansionInterface
{
Common::Flags<RTCFlag> g_rtc_flags;

namespace
{
// The chip's address map, as seen in the command word.
constexpr u32 ROM_SIZE = 0x200000;
constexpr u32 SRAM_START = 0x800000;
constexpr u32 SRAM_SIZE = 0x44;
constexpr u32 UART_START = 0x800400;
constexpr u32 UART_SIZE = 0x50;
constexpr u32 RTC_FLAGS_ADDRESS = 0x840020;
constexpr u32 EUART_START = 0xc00000;
constexpr u32 EUART_SIZE = 8;

// Boot ROM layout: a 256-byte copyright header, the scrambled BS1/BS2 code and the two fonts.
constexpr u32 ROM_HEADER_SIZE = 0x100;
constexpr u32 SCRAMBLED_START = 0x100;
constexpr u32 SCRAMBLED_SIZE = 0x1afe00;
constexpr u32 SJIS_FONT_OFFSET = 0x1aff00;
constexpr u32 ANSI_FONT_OFFSET = 0x1fcf00;
constexpr u32 FONTS_END = 0x1ff474;
// Sizes of the two fonts in a real boot ROM.
constexpr u64 SJIS_FONT_SIZE = 0x4a24d;
constexpr u64 ANSI_FONT_SIZE = 0x2575;

// Without a boot ROM dump only the header text is put in place; the retail ROMs start with it.
constexpr std::string_view COPYRIGHT_TEXT = "(C) 1999-2001 Nintendo.  All rights reserved."
                                            "(C) 1999 ArtX Inc.  All rights reserved.";
constexpr std::string_view PAL_REVISION_TEXT = "PAL  Revision 1.0  ";

constexpr u32 COMMAND_LENGTH = sizeof(u32);

// One of the descrambler's three shift registers: a 16-bit Galois LFSR that shifts right and
// applies its taps when a 1 falls out.
class ShiftRegister
{
public:
  constexpr ShiftRegister(u16 seed, u16 taps) : m_state(seed), m_taps(taps) {}

  constexpr bool Bit(int index) const { return ((m_state >> index) & 1) != 0; }

  constexpr void Clock()
  {
    const bool falling_out = Bit(0);
    m_state = static_cast<u16>(m_state >> 1);
    if (falling_out)
      m_state ^= m_taps;
  }

private:
  u16 m_state;
  u16 m_taps;
};

// The keystream the boot ROM is XORed with. Register A clocks every step; B clocks when A's low
// bit is 0, C when A's and B's low bits are equal. The output bit is a running parity over a
// nonlinear mix of the registers' low bits, and every 8 bits form one key byte (first bit in the
// most significant position).
class BootRomKeystream
{
public:
  u8 NextByte()
  {
    u8 key = 0;
    for (int bit = 0; bit < 8; ++bit)
      key = static_cast<u8>((key << 1) | (NextBit() ? 1 : 0));
    return key;
  }

private:
  bool NextBit()
  {
    const bool a0 = m_a.Bit(0);
    const bool a1 = m_a.Bit(1);
    const bool b0 = m_b.Bit(0);
    const bool b1 = m_b.Bit(1);
    const bool c0 = m_c.Bit(0);

    const bool mix = a1 != c0;
    const bool either_b = b0 || b1;
    const bool product = (a0 != (b1 != c0)) && (a0 != b0);
    m_parity = m_parity != (mix != (either_b != product));

    if (a0 == b0)
      m_c.Clock();
    if (!a0)
      m_b.Clock();
    m_a.Clock();

    return m_parity;
  }

  ShiftRegister m_a{0x2953, 0xa740};
  ShiftRegister m_b{0xd9c2, 0xfb10};
  ShiftRegister m_c{0x3ff1, 0xb3d0};
  bool m_parity = true;
};

constexpr bool InRange(u32 address, u32 start, u32 size)
{
  return address >= start && address - start < size;
}

u64 SecondsSinceStart(Core::System& system)
{
  return system.GetCoreTiming().GetTicks() / system.GetSystemTimers().GetTicksPerSecond();
}
}  // namespace

void CEXIIPL::Descrambler(u8* data, u32 size)
{
  BootRomKeystream keystream;
  for (u32 offset = 0; offset < size; ++offset)
    data[offset] ^= keystream.NextByte();
}

CEXIIPL::CEXIIPL(Core::System& system) : IEXIDevice(system)
{
  m_rom = std::make_unique<u8[]>(ROM_SIZE);

  // A GameCube can boot from the user's own boot ROM dump. The Wii has no boot ROM here, only
  // the fonts.
  const bool use_dump = !system.IsWii() && Config::Get(Config::SESSION_LOAD_IPL_DUMP);
  if (use_dump && LoadFileToIPL(SConfig::GetInstance().m_strBootROM, 0))
  {
    Descrambler(&m_rom[SCRAMBLED_START], SCRAMBLED_SIZE);
    const char* header = reinterpret_cast<const char*>(m_rom.get());
    INFO_LOG_FMT(BOOT, "Loaded bootrom: {}",
                 std::string_view(header, strnlen(header, ROM_HEADER_SIZE)));
  }
  else
  {
    std::string header_text(COPYRIGHT_TEXT);
    if (!DiscIO::IsNTSC(SConfig::GetInstance().m_region))
      header_text += PAL_REVISION_TEXT;
    std::fill_n(m_rom.get(), ROM_HEADER_SIZE, u8{0});
    std::memcpy(m_rom.get(), header_text.data(), header_text.size());

    const std::string font_dir = File::GetSysDirectory() + GC_SYS_DIR + DIR_SEP;
    LoadFontFile(font_dir + FONT_SHIFT_JIS, SJIS_FONT_OFFSET);
    LoadFontFile(font_dir + FONT_WINDOWS_1252, ANSI_FONT_OFFSET);
  }

  // The clock starts from zero and is refreshed whenever a command is latched. The language is
  // taken from the settings, as the GameCube lets users change it freely.
  Sram& sram = system.GetSRAM();
  sram.rtc = 0;
  sram.settings.language = static_cast<u8>(Config::Get(Config::MAIN_GC_LANGUAGE));
  sram.settings.rtc_bias = 0;
  FixSRAMChecksums(&sram);
}

CEXIIPL::~CEXIIPL() = default;

void CEXIIPL::DoState(PointerWrap& p)
{
  p.Do(m_system.GetSRAM());
  p.Do(g_rtc_flags);
  p.Do(m_command);
  p.Do(m_command_bytes);
  p.Do(m_cursor);
  p.Do(m_uart_line);
  p.Do(m_fonts_loaded);
}

bool CEXIIPL::LoadFileToIPL(const std::string& filename, u32 offset)
{
  if (offset >= ROM_SIZE)
    return false;

  File::IOFile file(filename, "rb");
  if (!file)
    return false;

  const u64 length = std::min<u64>(file.GetSize(), ROM_SIZE - offset);
  if (!file.ReadBytes(&m_rom[offset], length))
    return false;

  m_fonts_loaded = true;
  return true;
}

std::string CEXIIPL::FindIPLDump(const std::string& path_prefix)
{
  for (const char* region_dir : {USA_DIR, EUR_DIR, JAP_DIR})
  {
    std::string candidate = path_prefix + DIR_SEP + region_dir + DIR_SEP + GC_IPL;
    if (File::Exists(candidate))
      return candidate;
  }
  return {};
}

bool CEXIIPL::HasIPLDump()
{
  return !FindIPLDump(File::GetUserPath(D_GCUSER_IDX)).empty() ||
         !FindIPLDump(File::GetSysDirectory() + GC_SYS_DIR).empty();
}

void CEXIIPL::LoadFontFile(const std::string& filename, u32 offset)
{
  // The fonts shipped in Sys are free replacements whose glyph padding differs from the real
  // ones, which misplaces text in some games. If the user has their own boot ROM dump and allows
  // its use, take the fonts from it instead.
  std::string dump_path;
  if (Config::Get(Config::SESSION_LOAD_IPL_DUMP))
  {
    dump_path = FindIPLDump(File::GetUserPath(D_GCUSER_IDX));
    if (dump_path.empty())
      dump_path = FindIPLDump(File::GetSysDirectory() + GC_SYS_DIR);
  }

  File::IOFile dump;
  if (!dump_path.empty())
    dump.Open(dump_path, "rb");
  if (!dump)
  {
    LoadFileToIPL(filename, offset);
    return;
  }

  const bool sjis = offset == SJIS_FONT_OFFSET;
  INFO_LOG_FMT(BOOT, "Found IPL dump, loading {} font from {}", sjis ? "Shift JIS" : "Windows-1252",
               dump_path);

  const u64 font_size = sjis ? SJIS_FONT_SIZE : ANSI_FONT_SIZE;
  if (!dump.Seek(offset, File::SeekOrigin::Begin) || !dump.ReadBytes(&m_rom[offset], font_size))
  {
    WARN_LOG_FMT(BOOT, "Failed to read font from IPL dump.");
    return;
  }

  m_fonts_loaded = true;
}

void CEXIIPL::SetCS(int cs)
{
  // Selecting the chip starts a new command.
  if (cs != 0)
  {
    m_command_bytes = 0;
    m_cursor = 0;
  }
}

bool CEXIIPL::IsPresent() const
{
  return true;
}

void CEXIIPL::AccessROM(u8& data)
{
  // The ROM is read-only; writes go nowhere and do not advance the cursor. It is kept
  // descrambled in memory, so the descrambler enable bit is not modelled.
  if (CommandIsWrite())
    return;

  const u32 rom_offset = (CommandAddress() + m_cursor++) % ROM_SIZE;
  data = m_rom[rom_offset];

  const bool in_fonts = rom_offset >= SJIS_FONT_OFFSET && rom_offset <= FONTS_END;
  if (in_fonts && !m_fonts_loaded)
  {
    if (rom_offset >= ANSI_FONT_OFFSET)
    {
      PanicAlertFmtT("Error: Trying to access Windows-1252 fonts but they are not loaded. "
                     "Games may not show fonts correctly, or crash.");
    }
    else
    {
      PanicAlertFmtT("Error: Trying to access Shift JIS fonts but they are not loaded. "
                     "Games may not show fonts correctly, or crash.");
    }
    // Warn only once.
    m_fonts_loaded = true;
  }
}

void CEXIIPL::AccessSRAM(u8& data)
{
  Sram& sram = m_system.GetSRAM();
  const u32 sram_offset = (CommandAddress() - SRAM_START + m_cursor++) % SRAM_SIZE;
  if (CommandIsWrite())
    sram[sram_offset] = data;
  else
    data = sram[sram_offset];
}

void CEXIIPL::AccessUART(u8& data)
{
  // Reads report an empty transmit queue: output is consumed instantly.
  if (!CommandIsWrite())
  {
    data = 0;
    return;
  }

  // OSReport output: collect characters and log a line at each carriage return.
  if (data != 0)
    m_uart_line.push_back(static_cast<char>(data));
  if (data == '\r')
  {
    NOTICE_LOG_FMT(OSREPORT, "{}", SHIFTJISToUTF8(m_uart_line));
    m_uart_line.clear();
  }
}

void CEXIIPL::TransferByte(u8& data)
{
  // The first four bytes after selection form the command word; the chip answers 0xff.
  if (m_command_bytes < COMMAND_LENGTH)
  {
    m_command = (m_command << 8) | data;
    data = 0xff;
    if (++m_command_bytes == COMMAND_LENGTH)
    {
      // Refresh the clock whenever a command is complete. Real hardware ticks on its own.
      m_system.GetSRAM().rtc = GetEmulatedTime(m_system, GC_EPOCH);
      DEBUG_LOG_FMT(EXPANSIONINTERFACE, "IPL command: {} at {:08x} (low bits {:02x})",
                    CommandIsWrite() ? "write" : "read", CommandAddress(), m_command & 0x3f);
    }
    return;
  }

  const u32 address = CommandAddress();
  DEBUG_LOG_FMT(EXPANSIONINTERFACE, "IPL data: {} at {:08x}: {:02x}",
                CommandIsWrite() ? "write" : "read", address, data);

  if (address < ROM_SIZE)
  {
    AccessROM(data);
  }
  else if (InRange(address, SRAM_START, SRAM_SIZE))
  {
    AccessSRAM(data);
  }
  else if (InRange(address, UART_START, UART_SIZE))
  {
    const u32 reg = address - UART_START;
    if (reg == 0)
      AccessUART(data);
    else if (reg == 0x4c)
      DEBUG_LOG_FMT(OSREPORT, "UART Barnacle {:x}", data);
    // Register 0xc is written after a Barnacle read; nothing to do.
  }
  else if (address == RTC_FLAGS_ADDRESS)
  {
    if (CommandIsWrite())
      g_rtc_flags.m_hex = data;
    else
      data = static_cast<u8>(g_rtc_flags.m_hex);
  }
  else if (InRange(address, EUART_START, EUART_SIZE))
  {
    // Register 0 only needs to accept the init writes; register 4 is the data FIFO.
    if (address - EUART_START == 4)
      AccessUART(data);
  }
  else
  {
    NOTICE_LOG_FMT(EXPANSIONINTERFACE, "IPL-DEV Accessing unknown device");
  }
}

u32 CEXIIPL::GetEmulatedTime(Core::System& system, u32 epoch)
{
  // Movies and NetPlay need a clock every player agrees on: their recorded or negotiated start
  // time plus the emulated time since boot. Otherwise use the host clock and the user's offset.
  u64 seconds;
  auto& movie = system.GetMovie();
  if (movie.IsMovieActive())
  {
    seconds = movie.GetRecordingStartTime() + SecondsSinceStart(system);
  }
  else if (NetPlay::IsNetPlayRunning())
  {
    seconds = NetPlay_GetEmulatedTime() + SecondsSinceStart(system);
  }
  else
  {
    ASSERT(!Core::WantsDeterminism());
    seconds = Common::Timer::GetLocalTimeSinceJan1970() -
              static_cast<u64>(system.GetSystemTimers().GetLocalTimeRTCOffset());
  }

  return static_cast<u32>(seconds) - epoch;
}
}  // namespace ExpansionInterface
