// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Maps instruction words to the interpreter's handlers. The JIT uses the same
// lookups for the instructions it leaves to the interpreter.

#pragma once

#include "Core/DSP/DSPCommon.h"

namespace DSP::Interpreter
{
class Interpreter;

using InterpreterFunction = void (Interpreter::*)(UDSPInstruction);

// Handler of the main instruction encoded in inst.
InterpreterFunction GetOp(UDSPInstruction inst);
// Handler of the extension in the low bits of inst (7 bits in the 0x3xxx group).
InterpreterFunction GetExtOp(UDSPInstruction inst);

// Fills both tables; later calls do nothing.
void InitInstructionTables();
}  // namespace DSP::Interpreter
