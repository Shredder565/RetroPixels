// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "IMLInstruction.h"
#include "IMLSegment.h"

// ---- analysis (IMLAnalyzer.cpp) ----

// fills liveIn/liveOut of every block. A register is live where its current
// value can still be read by a later instruction
void IMLAnalyzer_ComputeLiveness(IMLFunction& func);

// approximate loop nesting depth of every block (from backward branches in guest address order)
void IMLAnalyzer_ComputeLoopDepth(IMLFunction& func);

// sanity checks used in debug builds and by the test harness: returns false
// and describes the first problem found
bool IMLAnalyzer_Verify(IMLFunction& func, std::string* problem);

// ---- optimizer (IMLOptimizer.cpp) ----

// removes instructions without side effects whose results are never read
void IMLOptimizer_RemoveDeadCode(IMLFunction& func);
void IMLOptimizer_Run(IMLFunction& func);

// ---- debug output (IMLDebug.cpp) ----

std::string IMLDebug_FormatReg(const IMLFunction& func, IMLReg r);
std::string IMLDebug_FormatInstruction(const IMLFunction& func, const IMLInstruction& inst);
std::string IMLDebug_FormatFunction(const IMLFunction& func);
void IMLDebug_Dump(const IMLFunction& func);
