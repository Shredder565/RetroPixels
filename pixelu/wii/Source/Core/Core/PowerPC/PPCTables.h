// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Static description of every Gekko instruction: its name, a coarse class,
// a nominal cycle cost and a set of property flags that the analyser and the
// JITs use to reason about inputs, outputs and side effects.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Gekko.h"

// Instruction properties. Each is one bit; the combined names are shorthands.
enum InstructionFlags : u64
{
  // Condition register / XER effects.
  FL_SET_CR0 = (1ull << 0),
  FL_SET_CR1 = (1ull << 1),
  FL_SET_CRn = (1ull << 2),  // field selected by crfD
  FL_SET_CA = (1ull << 3),
  FL_READ_CA = (1ull << 4),
  FL_RC_BIT = (1ull << 5),    // Rc=1 updates CR0 (integer form)
  FL_RC_BIT_F = (1ull << 6),  // Rc=1 updates CR1 (floating-point form)

  // The instruction may leave the current block.
  FL_ENDBLOCK = (1ull << 7),

  // GPR operands.
  FL_IN_A = (1ull << 8),
  FL_IN_A0 = (1ull << 9),  // rA, except that rA=0 reads as the constant 0
  FL_IN_B = (1ull << 10),
  FL_IN_C = (1ull << 11),
  FL_IN_S = (1ull << 12),
  FL_IN_AB = FL_IN_A | FL_IN_B,
  FL_IN_AC = FL_IN_A | FL_IN_C,
  FL_IN_ABC = FL_IN_A | FL_IN_B | FL_IN_C,
  FL_IN_SB = FL_IN_S | FL_IN_B,
  FL_IN_A0B = FL_IN_A0 | FL_IN_B,
  FL_IN_A0BC = FL_IN_A0 | FL_IN_B | FL_IN_C,
  FL_OUT_D = (1ull << 13),
  FL_OUT_A = (1ull << 14),
  FL_OUT_AD = FL_OUT_A | FL_OUT_D,

  // Miscellaneous behaviour.
  FL_TIMER = (1ull << 15),            // reads the time base (mftb)
  FL_CHECKEXCEPTIONS = (1ull << 16),  // exceptions must be checked afterwards (rfi)
  FL_NO_REORDER = (1ull << 17),       // optimisations must not move code across it
  FL_USE_FPU = (1ull << 18),
  FL_LOADSTORE = (1ull << 19),
  FL_SET_FPRF = (1ull << 20),
  FL_READ_FPRF = (1ull << 21),
  FL_SET_OE = (1ull << 22),  // OE=1 form: updates XER[OV/SO]

  // FPR operands.
  FL_IN_FLOAT_A = (1ull << 23),
  FL_IN_FLOAT_B = (1ull << 24),
  FL_IN_FLOAT_C = (1ull << 25),
  FL_IN_FLOAT_S = (1ull << 26),
  FL_IN_FLOAT_D = (1ull << 27),
  FL_IN_FLOAT_AB = FL_IN_FLOAT_A | FL_IN_FLOAT_B,
  FL_IN_FLOAT_AC = FL_IN_FLOAT_A | FL_IN_FLOAT_C,
  FL_IN_FLOAT_ABC = FL_IN_FLOAT_A | FL_IN_FLOAT_B | FL_IN_FLOAT_C,
  FL_OUT_FLOAT_D = (1ull << 28),
  // Double-precision results leave ps1 of frD untouched, so frD is also read.
  FL_INOUT_FLOAT_D = FL_IN_FLOAT_D | FL_OUT_FLOAT_D,
  // The result depends on the exact bit pattern of the operand (moves,
  // sign manipulation, selects) rather than on its numeric value.
  FL_IN_FLOAT_A_BITEXACT = (1ull << 29),
  FL_IN_FLOAT_B_BITEXACT = (1ull << 30),
  FL_IN_FLOAT_C_BITEXACT = (1ull << 31),
  FL_IN_FLOAT_AB_BITEXACT = FL_IN_FLOAT_A_BITEXACT | FL_IN_FLOAT_B_BITEXACT,
  FL_IN_FLOAT_BC_BITEXACT = FL_IN_FLOAT_B_BITEXACT | FL_IN_FLOAT_C_BITEXACT,

  // Exceptions the instruction can raise.
  FL_PROGRAMEXCEPTION = (1ull << 32),  // program exception other than FP
  FL_FLOAT_EXCEPTION = (1ull << 33),   // floating-point enabled exception
  FL_FLOAT_DIV = (1ull << 34),         // ... including divide by zero

  // Whole-CR and CR-field reads/writes.
  FL_SET_ALL_CR = (1ull << 35),
  FL_READ_CRn = (1ull << 36),    // field selected by crfS
  FL_READ_CR_BI = (1ull << 37),  // bit selected by BI
  FL_READ_ALL_CR = (1ull << 38),
  FL_SET_CRx = FL_SET_CR0 | FL_SET_CR1 | FL_SET_CRn | FL_SET_ALL_CR,
  FL_READ_CRx = FL_READ_CRn | FL_READ_CR_BI | FL_READ_ALL_CR,

  FL_SET_MSR = (1ull << 39),
};

enum class OpType
{
  Invalid,
  Subtable,
  Integer,
  CR,
  SPR,
  System,
  SystemFP,
  Load,
  Store,
  LoadFP,
  StoreFP,
  DoubleFP,
  SingleFP,
  LoadPS,
  StorePS,
  PS,
  DataCache,
  InstructionCache,
  Branch,
  Unknown,
};

// Per-instruction profiling counters.
struct GekkoOPStats
{
  u64 run_count;
  u32 compile_count;
  u32 last_use;
};

struct GekkoOPInfo
{
  const char* opname;
  OpType type;
  u32 num_cycles;
  u64 flags;
  // Counters for this instruction (writable even though the info is const).
  GekkoOPStats* stats;
};

namespace PPCTables
{
// Never returns null: unrecognised encodings map to "unknown_instruction".
const GekkoOPInfo* GetOpInfo(UGeckoInstruction inst, u32 pc);

bool IsValidInstruction(UGeckoInstruction inst, u32 pc);

void CountInstruction(UGeckoInstruction inst, u32 pc);
void CountInstructionCompile(const GekkoOPInfo* info, u32 pc);
void PrintInstructionRunCounts();
void LogCompiledInstructions();
const char* GetInstructionName(UGeckoInstruction inst, u32 pc);
}  // namespace PPCTables
