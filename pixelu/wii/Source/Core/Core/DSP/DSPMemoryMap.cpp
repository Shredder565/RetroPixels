// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Address decoding for the DSP's instruction and data spaces. The top four bits
// of an address select the memory; everything else is unmapped.

#include "Core/DSP/DSPCore.h"

#include "Common/Logging/Log.h"

#include "Core/DSP/DSPTables.h"

namespace DSP
{
namespace
{
enum InstructionBank : u16
{
  IBANK_IRAM = 0x0,
  IBANK_IROM = 0x8,
};

enum DataBank : u16
{
  DBANK_DRAM = 0x0,
  DBANK_COEF = 0x1,
  DBANK_HW = 0xf,
};

constexpr u16 BankOf(u16 address)
{
  return static_cast<u16>(address >> 12);
}
}  // namespace

u16 SDSP::ReadIMEM(u16 address) const
{
  const u16 bank = BankOf(address);
  if (bank == IBANK_IRAM)
    return iram[address & DSP_IRAM_MASK];
  if (bank == IBANK_IROM)
    return irom[address & DSP_IROM_MASK];

  ERROR_LOG_FMT(DSPLLE, "{:04x} instruction fetch from unmapped address {:04x}", pc, address);
  return 0;
}

u16 SDSP::ReadDMEM(u16 address)
{
  const u16 bank = BankOf(address);
  if (bank == DBANK_DRAM)
    return dram[address & DSP_DRAM_MASK];

  if (bank == DBANK_COEF)
  {
    DEBUG_LOG_FMT(DSPLLE, "{:04x} coefficient ROM read at {:04x}", pc, address);
    return coef[address & DSP_COEF_MASK];
  }

  if (bank == DBANK_HW)
    return ReadIFX(address);

  ERROR_LOG_FMT(DSPLLE, "{:04x} data read from unmapped address {:04x}", pc, address);
  return 0;
}

void SDSP::WriteDMEM(u16 address, u16 value)
{
  const u16 bank = BankOf(address);
  if (bank == DBANK_DRAM)
  {
    dram[address & DSP_DRAM_MASK] = value;
    return;
  }

  if (bank == DBANK_HW)
  {
    WriteIFX(address, value);
    return;
  }

  // The coefficient ROM and the unmapped banks ignore writes.
  ERROR_LOG_FMT(DSPLLE, "{:04x} data write to unwritable address {:04x}", pc, address);
}

u16 SDSP::PeekInstruction() const
{
  return ReadIMEM(pc);
}

u16 SDSP::FetchInstruction()
{
  const u16 word = PeekInstruction();
  ++pc;
  return word;
}

void SDSP::SkipInstruction()
{
  pc = static_cast<u16>(pc + GetOpTemplate(PeekInstruction())->size);
}
}  // namespace DSP
