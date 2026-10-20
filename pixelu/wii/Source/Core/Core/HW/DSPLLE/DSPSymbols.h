// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// A disassembly listing of the DSP's instruction memories, rebuilt whenever new
// code is loaded, with lookups between addresses and listing lines.

#pragma once

#include "Common/CommonTypes.h"

namespace DSP
{
struct SDSP;
}

namespace DSP::Symbols
{
// Appends the instructions from start_addr up to end_addr (IROM when bit 15 is set,
// otherwise IRAM) to the listing.
void AutoDisassembly(const SDSP& dsp, u16 start_addr, u16 end_addr);

void Clear();

// Both return -1 when there is no such address or line.
int Addr2Line(u16 address);
int Line2Addr(int line);

// "----" for lines that do not exist.
const char* GetLineText(int line);
}  // namespace DSP::Symbols
