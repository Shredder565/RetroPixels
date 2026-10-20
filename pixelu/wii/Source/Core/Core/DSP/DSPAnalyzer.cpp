// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPAnalyzer.h"

#include <array>
#include <cstddef>

#include "Common/Logging/Log.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPTables.h"

namespace DSP
{
namespace
{
// One word of an idle-loop pattern; "any" matches every value (a branch target).
struct PatternWord
{
  u16 value;
  bool any;
};

constexpr PatternWord W(u16 value)
{
  return {value, false};
}

constexpr PatternWord ANY{0, true};

struct IdlePattern
{
  std::array<PatternWord, 6> words;
  size_t length;
};

// Loops that do nothing but poll a mailbox (or a flag in DRAM) until something
// changes. Once the DSP is inside one it can give up the rest of its time slice.
constexpr std::array<IdlePattern, 8> IDLE_PATTERNS{{
    // AX: wait until the DSP mailbox is read (LRS $acX.m, @DMBH; ANDCF; JLZ)
    {{W(0x26fc), W(0x02c0), W(0x8000), W(0x029d), ANY}, 5},
    {{W(0x27fc), W(0x03c0), W(0x8000), W(0x029d), ANY}, 5},
    // AX: wait for mail from the CPU (LRS $acX.m, @CMBH; ANDCF; JLNZ)
    {{W(0x26fe), W(0x02c0), W(0x8000), W(0x029c), ANY}, 5},
    {{W(0x27fe), W(0x03c0), W(0x8000), W(0x029c), ANY}, 5},
    // AX variant with ANDF
    {{W(0x26fc), W(0x02a0), W(0x8000), W(0x029c), ANY}, 5},
    {{W(0x27fc), W(0x03a0), W(0x8000), W(0x029c), ANY}, 5},
    // Zelda: LR $ac0.m, @CMBH; ANDCF; JLNZ
    {{W(0x00de), W(0xfffe), W(0x02c0), W(0x8000), W(0x029c), ANY}, 6},
    // Zelda: LR $ax0.h, @0x0352; TSTAXH; JZ
    {{W(0x00da), W(0x0352), W(0x8600), W(0x0295), ANY}, 5},
}};

// Main opcodes whose effects (memory accesses that may reach the accelerator or
// the mailboxes) can raise an exception, so the next instruction must check.
constexpr std::array<u16, 14> MAY_RAISE_EXCEPTION{
    0x00c0, 0x00e0, 0x1600,                          // LR, SR, SI
    0x1800, 0x1880, 0x1900, 0x1980,                  // LRR family
    0x1a00, 0x1a80, 0x1b00, 0x1b80,                  // SRR family
    0x2000, 0x2800, 0x2c00,                          // LRS, SRSH, SRS
};

bool MayRaiseException(const DSPOPCTemplate& op)
{
  if (op.extended)
    return true;
  for (const u16 opcode : MAY_RAISE_EXCEPTION)
  {
    if (op.opcode == opcode)
      return true;
  }
  return false;
}

bool PatternMatches(const SDSP& dsp, const IdlePattern& pattern, u16 address)
{
  for (size_t i = 0; i < pattern.length; ++i)
  {
    const PatternWord& word = pattern.words[i];
    if (!word.any && dsp.ReadIMEM(static_cast<u16>(address + i)) != word.value)
      return false;
  }
  return true;
}
}  // namespace

Analyzer::Analyzer() = default;
Analyzer::~Analyzer() = default;

void Analyzer::Analyze(const SDSP& dsp)
{
  m_flags.fill(0);

  constexpr std::array<std::array<u16, 2>, 2> regions{{
      {0x0000, 0x1000},  // IRAM
      {0x8000, 0x9000},  // IROM
  }};

  for (const auto& [first, last] : regions)
  {
    ScanInstructions(dsp, first, last);
    ScanIdleLoops(dsp, first, last);
    INFO_LOG_FMT(DSPLLE, "Analyzed DSP code {:04x}-{:04x}", first, last - 1);
  }
}

void Analyzer::ScanInstructions(const SDSP& dsp, u16 first, u16 last)
{
  // A linear sweep: data embedded in code (jump tables) can throw it off, but the
  // ucodes in use do not do that.
  u16 address = first;
  while (address < last)
  {
    const UDSPInstruction word = dsp.ReadIMEM(address);
    const DSPOPCTemplate* const op = GetOpTemplate(word);
    if (op == nullptr)
    {
      // The opcode tables are not set up yet.
      ++address;
      continue;
    }

    Mark(address, FLAG_INSTRUCTION_START);

    const bool block_loop = (word & 0xffe0) == 0x0060 || (word & 0xff00) == 0x1100;
    const bool single_loop = (word & 0xffe0) == 0x0040 || (word & 0xff00) == 0x1000;
    if (block_loop)
    {
      // BLOOP/BLOOPI: the second word is the address of the last instruction.
      Mark(address, FLAG_LOOP_BEGIN);
      Mark(dsp.ReadIMEM(static_cast<u16>(address + 1)), FLAG_LOOP_LAST);
    }
    else if (single_loop)
    {
      // LOOP/LOOPI repeat just the next instruction.
      Mark(address, FLAG_LOOP_BEGIN);
      Mark(static_cast<u16>(address + 1), FLAG_LOOP_LAST);
    }

    if (MayRaiseException(*op))
      Mark(static_cast<u16>(address + op->size), FLAG_CHECK_EXCEPTIONS);

    address = static_cast<u16>(address + op->size);
  }
}

void Analyzer::ScanIdleLoops(const SDSP& dsp, u16 first, u16 last)
{
  for (size_t index = 0; index < IDLE_PATTERNS.size(); ++index)
  {
    for (u16 address = first; address < last; ++address)
    {
      if (!PatternMatches(dsp, IDLE_PATTERNS[index], address))
        continue;
      INFO_LOG_FMT(DSPLLE, "Idle loop (pattern {}) at {:04x}", index + 1, address);
      Mark(address, FLAG_IDLE_LOOP);
    }
  }
}
}  // namespace DSP
