// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace DSP
{
// Breakpoints on DSP instruction addresses: one flag per address of the 16-bit
// address space, so that the interpreter's per-instruction check is a lookup.
class DSPBreakpoints
{
public:
  DSPBreakpoints() { Clear(); }

  bool IsAddressBreakPoint(u32 addr) const { return m_flags[addr] != NONE; }

  // Returns false if there already is a breakpoint at addr.
  bool Add(u32 addr, bool temp = false)
  {
    if (m_flags[addr] != NONE)
      return false;
    m_flags[addr] = temp ? TEMPORARY : PERMANENT;
    return true;
  }

  // Returns whether there was a breakpoint at addr.
  bool Remove(u32 addr)
  {
    const bool had_one = m_flags[addr] != NONE;
    m_flags[addr] = NONE;
    return had_one;
  }

  void Clear() { m_flags.fill(NONE); }
  void DeleteByAddress(u32 addr) { m_flags[addr] = NONE; }

private:
  static constexpr u8 NONE = 0;
  static constexpr u8 PERMANENT = 1;
  static constexpr u8 TEMPORARY = 2;

  std::array<u8, 0x10000> m_flags;
};
}  // namespace DSP
