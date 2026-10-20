// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

#include "Common/CommonTypes.h"

// What the DSP core needs from the program embedding it. The emulator provides
// these in HW/DSPLLE/DSPHost.cpp; tools that only assemble or disassemble can
// stub them out.

namespace DSP
{
class DSPCore;
}

namespace DSP::Host
{
// Byte access to ARAM (or, on Wii, main memory) for the accelerator.
u8 ReadHostMemory(u32 addr);
void WriteHostMemory(u8 value, u32 addr);

// DMA between main memory and DSP memory; size is in bytes.
void DMAToDSP(u16* dst, u32 addr, u32 size);
void DMAFromDSP(const u16* src, u32 addr, u32 size);

void OSD_AddMessage(std::string str, u32 ms);
bool OnThread();
bool IsWiiHost();

// Raises the DSP interrupt towards the CPU.
void InterruptRequest();

// New code arrived in IRAM, either by DMA from main memory or by loading a state.
void CodeLoaded(DSPCore& dsp, u32 addr, size_t size);
void CodeLoaded(DSPCore& dsp, const u8* ptr, size_t size);
}  // namespace DSP::Host
