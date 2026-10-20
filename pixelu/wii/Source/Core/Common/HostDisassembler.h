// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>

#include "Common/CommonTypes.h"

// Prints host machine code (the JIT's output) for the debugger's JIT view.
class HostDisassembler
{
public:
  enum class Platform
  {
    x86_64,
    aarch64,
  };

  virtual ~HostDisassembler() = default;

  // The best disassembler this build has for arch; the base class (a hex dump) when none.
  static std::unique_ptr<HostDisassembler> Factory(Platform arch);

  // Writes one line per instruction in [begin, end) and returns the number of instructions.
  // The base class writes all the bytes as hex on one line and returns 0.
  virtual std::size_t Disassemble(const u8* begin, const u8* end, std::ostream& stream);
};
