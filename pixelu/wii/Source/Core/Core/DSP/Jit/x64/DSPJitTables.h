// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Which recompiler routine translates an instruction word. A null handler
// means the instruction (or extension) is left to the interpreter.

#pragma once

#include "Core/DSP/DSPCommon.h"

namespace DSP::JIT::x64
{
class DSPEmitter;

using JITHandler = void (DSPEmitter::*)(UDSPInstruction);

// The translator of the main instruction in inst.
JITHandler MainHandler(UDSPInstruction inst);
// The translator of the extension in the low bits of inst (seven bits in the
// 0x3xxx group, eight elsewhere).
JITHandler ExtHandler(UDSPInstruction inst);

// Fills the lookup tables; later calls do nothing.
void InitHandlerTables();
}  // namespace DSP::JIT::x64
