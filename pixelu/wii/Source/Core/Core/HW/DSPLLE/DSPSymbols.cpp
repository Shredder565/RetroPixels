// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPLLE/DSPSymbols.h"

#include <map>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPDisassembler.h"

namespace DSP::Symbols
{
namespace
{
struct Listing
{
  std::vector<std::string> text;
  std::map<u16, int> line_of_address;
  std::map<int, u16> address_of_line;
  // Numbers the lines; a failed instruction gets a number but no text.
  int next_line = 0;
};

Listing& TheListing()
{
  static Listing listing;
  return listing;
}
}  // namespace

int Addr2Line(u16 address)
{
  const Listing& listing = TheListing();
  const auto it = listing.line_of_address.find(address);
  return it != listing.line_of_address.end() ? it->second : -1;
}

int Line2Addr(int line)
{
  const Listing& listing = TheListing();
  const auto it = listing.address_of_line.find(line);
  return it != listing.address_of_line.end() ? it->second : -1;
}

const char* GetLineText(int line)
{
  const Listing& listing = TheListing();
  if (line < 0 || static_cast<size_t>(line) >= listing.text.size())
    return "----";
  return listing.text[static_cast<size_t>(line)].c_str();
}

void AutoDisassembly(const SDSP& dsp, u16 start_addr, u16 end_addr)
{
  constexpr size_t MEMORY_WORDS = DSP_IRAM_SIZE;
  static_assert(MEMORY_WORDS == DSP_IROM_SIZE);

  AssemblerSettings settings;
  settings.show_pc = true;
  settings.show_hex = true;
  DSPDisassembler disassembler(settings);

  const bool rom = (start_addr & 0x8000) != 0;
  const u16* const memory = rom ? dsp.irom : dsp.iram;

  Listing& listing = TheListing();
  for (u16 addr = start_addr; addr < end_addr;)
  {
    listing.address_of_line[listing.next_line] = addr;
    listing.line_of_address[addr] = listing.next_line;

    std::string line;
    if (!disassembler.DisassembleOpcode(memory, MEMORY_WORDS, &addr, line))
    {
      ERROR_LOG_FMT(DSPLLE, "disasm failed at {:04x}", addr);
      return;
    }
    listing.text.push_back(std::move(line));
    ++listing.next_line;
  }
}

void Clear()
{
  TheListing() = Listing{};
}
}  // namespace DSP::Symbols
