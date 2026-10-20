// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Static analysis of the DSP code in IRAM and IROM, redone whenever new code is
// loaded. It marks instruction starts, the ends of hardware loops, places after
// which pending exceptions must be checked, and mail-polling loops that can be
// skipped while idle.

#pragma once

#include <array>
#include "Common/CommonTypes.h"

// With this defined every instruction counts as updating $sr: tracking which ones
// really need to was never made reliable (flags read inside calls, sticky bits).
#define DISABLE_UPDATE_SR_ANALYSIS

namespace DSP
{
struct SDSP;
}

namespace DSP
{
class Analyzer
{
public:
  explicit Analyzer();
  ~Analyzer();

  Analyzer(const Analyzer&) = default;
  Analyzer& operator=(const Analyzer&) = default;

  Analyzer(Analyzer&&) = default;
  Analyzer& operator=(Analyzer&&) = default;

  // Forgets all earlier results and analyzes IRAM and IROM again.
  void Analyze(const SDSP& dsp);

  [[nodiscard]] bool IsStartOfInstruction(u16 address) const
  {
    return HasFlag(address, FLAG_INSTRUCTION_START);
  }

  [[nodiscard]] bool IsIdleSkip(u16 address) const { return HasFlag(address, FLAG_IDLE_LOOP); }

  [[nodiscard]] bool IsLoopStart(u16 address) const { return HasFlag(address, FLAG_LOOP_BEGIN); }

  [[nodiscard]] bool IsLoopEnd(u16 address) const { return HasFlag(address, FLAG_LOOP_LAST); }

  [[nodiscard]] bool IsUpdateSR(u16 address) const
  {
#ifdef DISABLE_UPDATE_SR_ANALYSIS
    (void)address;
    return true;
#else
    return HasFlag(address, FLAG_SETS_SR);
#endif
  }

  [[nodiscard]] bool IsCheckExceptions(u16 address) const
  {
    return HasFlag(address, FLAG_CHECK_EXCEPTIONS);
  }

private:
  enum : u8
  {
    FLAG_INSTRUCTION_START = 1 << 0,
    FLAG_IDLE_LOOP = 1 << 1,
    FLAG_LOOP_BEGIN = 1 << 2,
    FLAG_LOOP_LAST = 1 << 3,
    FLAG_SETS_SR = 1 << 4,
    FLAG_CHECK_EXCEPTIONS = 1 << 5,
  };

  [[nodiscard]] bool HasFlag(u16 address, u8 flag) const
  {
    return (m_flags[address] & flag) != 0;
  }

  void Mark(u16 address, u8 flag) { m_flags[address] |= flag; }

  // Both scan the half-open range [first, last).
  void ScanInstructions(const SDSP& dsp, u16 first, u16 last);
  void ScanIdleLoops(const SDSP& dsp, u16 first, u16 last);

  std::array<u8, 65536> m_flags{};
};
}  // namespace DSP
