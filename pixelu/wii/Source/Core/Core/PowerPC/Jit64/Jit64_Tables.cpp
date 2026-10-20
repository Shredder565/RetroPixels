// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include <array>
#include <cstddef>

#include "Common/Assert.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PPCTables.h"

namespace
{
struct OpEntry
{
  u32 opcode;
  Jit64::Instruction handler;
};

using J = Jit64;

// Primary opcodes (bits 0-5). 4, 19, 31, 59 and 63 have sub-tables.
constexpr std::array<OpEntry, 54> PRIMARY_OPS{{
    // Sub-tables.
    {4, &J::DynaRunTable4},
    {19, &J::DynaRunTable19},
    {31, &J::DynaRunTable31},
    {59, &J::DynaRunTable59},
    {63, &J::DynaRunTable63},
    // Branches, traps and system calls.
    {3, &J::twX},
    {16, &J::bcx},
    {17, &J::sc},
    {18, &J::bx},
    // Integer with immediate.
    {7, &J::mulli},
    {8, &J::subfic},
    {10, &J::cmpXX},
    {11, &J::cmpXX},
    {12, &J::reg_imm},
    {13, &J::reg_imm},
    {14, &J::reg_imm},
    {15, &J::reg_imm},
    {20, &J::rlwimix},
    {21, &J::rlwinmx},
    {23, &J::rlwnmx},
    {24, &J::reg_imm},
    {25, &J::reg_imm},
    {26, &J::reg_imm},
    {27, &J::reg_imm},
    {28, &J::reg_imm},
    {29, &J::reg_imm},
    // Integer loads and stores.
    {32, &J::lXXx},
    {33, &J::lXXx},
    {34, &J::lXXx},
    {35, &J::lXXx},
    {36, &J::stX},
    {37, &J::stX},
    {38, &J::stX},
    {39, &J::stX},
    {40, &J::lXXx},
    {41, &J::lXXx},
    {42, &J::lXXx},
    {43, &J::lXXx},
    {44, &J::stX},
    {45, &J::stX},
    {46, &J::lmw},
    {47, &J::stmw},
    // Floating point and paired loads and stores.
    {48, &J::lfXXX},
    {49, &J::lfXXX},
    {50, &J::lfXXX},
    {51, &J::lfXXX},
    {52, &J::stfXXX},
    {53, &J::stfXXX},
    {54, &J::stfXXX},
    {55, &J::stfXXX},
    {56, &J::psq_lXX},
    {57, &J::psq_lXX},
    {60, &J::psq_stXX},
    {61, &J::psq_stXX},
}};

// Opcode 4 with a 10-bit extended opcode: paired compares, moves, merges.
constexpr std::array<OpEntry, 13> OP4_X_FORM{{
    {0, &J::ps_cmpXX},
    {32, &J::ps_cmpXX},
    {40, &J::fsign},
    {64, &J::ps_cmpXX},
    {72, &J::ps_mr},
    {96, &J::ps_cmpXX},
    {136, &J::fsign},
    {264, &J::fsign},
    {528, &J::ps_mergeXX},
    {560, &J::ps_mergeXX},
    {592, &J::ps_mergeXX},
    {624, &J::ps_mergeXX},
    {1014, &J::dcbz},  // dcbz_l
}};

// Opcode 4 with a 5-bit extended opcode (A-form paired arithmetic).
constexpr std::array<OpEntry, 17> OP4_A_FORM{{
    {10, &J::ps_sum},
    {11, &J::ps_sum},
    {12, &J::ps_muls},
    {13, &J::ps_muls},
    {14, &J::fmaddXX},
    {15, &J::fmaddXX},
    {18, &J::fp_arith},
    {20, &J::fp_arith},
    {21, &J::fp_arith},
    {23, &J::fselx},
    {24, &J::ps_res},
    {25, &J::fp_arith},
    {26, &J::ps_rsqrte},
    {28, &J::fmaddXX},
    {29, &J::fmaddXX},
    {30, &J::fmaddXX},
    {31, &J::fmaddXX},
}};

// Opcode 4 with a 6-bit extended opcode: indexed quantized loads and stores.
constexpr std::array<OpEntry, 4> OP4_QUANTIZED_X{{
    {6, &J::psq_lXX},
    {7, &J::psq_stXX},
    {38, &J::psq_lXX},
    {39, &J::psq_stXX},
}};

// Opcode 19: branches to LR/CTR, CR logic, rfi.
constexpr std::array<OpEntry, 13> OP19{{
    {0, &J::mcrf},
    {16, &J::bclrx},
    {33, &J::crXXX},
    {50, &J::rfi},
    {129, &J::crXXX},
    {150, &J::DoNothing},  // isync
    {193, &J::crXXX},
    {225, &J::crXXX},
    {257, &J::crXXX},
    {289, &J::crXXX},
    {417, &J::crXXX},
    {449, &J::crXXX},
    {528, &J::bcctrx},
}};

// Opcode 31: register-register integer, indexed loads/stores, cache and
// system instructions. Entries that name FallBackToInterpreter are listed
// to document that they are deliberately interpreted.
constexpr std::array<OpEntry, 107> OP31{{
    // Arithmetic (OE variants are 512 higher).
    {8, &J::subfx},
    {10, &J::addx},
    {11, &J::mulhwXx},
    {40, &J::subfx},
    {75, &J::mulhwXx},
    {104, &J::negx},
    {136, &J::arithXex},
    {138, &J::arithXex},
    {200, &J::arithXex},
    {202, &J::arithXex},
    {232, &J::arithXex},
    {234, &J::arithXex},
    {235, &J::mullwx},
    {266, &J::addx},
    {459, &J::divwux},
    {491, &J::divwx},
    {520, &J::subfx},
    {522, &J::addx},
    {552, &J::subfx},
    {616, &J::negx},
    {648, &J::arithXex},
    {650, &J::arithXex},
    {712, &J::arithXex},
    {714, &J::arithXex},
    {744, &J::arithXex},
    {746, &J::arithXex},
    {747, &J::mullwx},
    {778, &J::addx},
    {971, &J::divwux},
    {1003, &J::divwx},
    // Logic, compares, shifts.
    {0, &J::cmpXX},
    {24, &J::slwx},
    {26, &J::cntlzwx},
    {28, &J::boolX},
    {32, &J::cmpXX},
    {60, &J::boolX},
    {124, &J::boolX},
    {284, &J::boolX},
    {316, &J::boolX},
    {412, &J::boolX},
    {444, &J::boolX},
    {476, &J::boolX},
    {536, &J::srwx},
    {792, &J::srawx},
    {824, &J::srawix},
    {922, &J::extsXx},
    {954, &J::extsXx},
    // Data cache.
    {54, &J::dcbx},
    {86, &J::dcbx},
    {246, &J::dcbt},
    {278, &J::dcbt},
    {470, &J::dcbx},
    {758, &J::DoNothing},  // dcba
    {1014, &J::dcbz},
    // Indexed integer loads.
    {23, &J::lXXx},
    {55, &J::lXXx},
    {87, &J::lXXx},
    {119, &J::lXXx},
    {279, &J::lXXx},
    {311, &J::lXXx},
    {343, &J::lXXx},
    {375, &J::lXXx},
    {534, &J::lXXx},
    {790, &J::lXXx},
    // Indexed integer stores.
    {151, &J::stXx},
    {183, &J::stXx},
    {215, &J::stXx},
    {247, &J::stXx},
    {407, &J::stXx},
    {439, &J::stXx},
    {662, &J::stXx},
    {918, &J::stXx},
    // Reservations and string accesses.
    {20, &J::FallBackToInterpreter},   // lwarx
    {150, &J::FallBackToInterpreter},  // stwcx.
    {533, &J::FallBackToInterpreter},  // lswx
    {597, &J::FallBackToInterpreter},  // lswi
    {661, &J::FallBackToInterpreter},  // stswx
    {725, &J::FallBackToInterpreter},  // stswi
    // Indexed floating point loads and stores.
    {535, &J::lfXXX},
    {567, &J::lfXXX},
    {599, &J::lfXXX},
    {631, &J::lfXXX},
    {663, &J::stfXXX},
    {695, &J::stfXXX},
    {727, &J::stfXXX},
    {759, &J::stfXXX},
    {983, &J::stfiwx},
    // System registers.
    {19, &J::mfcr},
    {83, &J::mfmsr},
    {144, &J::mtcrf},
    {146, &J::mtmsr},
    {210, &J::FallBackToInterpreter},  // mtsr
    {242, &J::FallBackToInterpreter},  // mtsrin
    {339, &J::mfspr},
    {371, &J::mftb},
    {467, &J::mtspr},
    {512, &J::mcrxr},
    {595, &J::FallBackToInterpreter},  // mfsr
    {659, &J::FallBackToInterpreter},  // mfsrin
    // Traps, synchronisation, TLB and external control.
    {4, &J::twX},
    {306, &J::FallBackToInterpreter},  // tlbie
    {310, &J::FallBackToInterpreter},  // eciwx
    {438, &J::FallBackToInterpreter},  // ecowx
    {566, &J::DoNothing},              // tlbsync
    {598, &J::DoNothing},              // sync
    {854, &J::eieio},
    {982, &J::FallBackToInterpreter},  // icbi
}};

// Opcode 59: single-precision arithmetic (5-bit extended opcode).
constexpr std::array<OpEntry, 9> OP59{{
    {18, &J::fp_arith},
    {20, &J::fp_arith},
    {21, &J::fp_arith},
    {24, &J::fresx},
    {25, &J::fp_arith},
    {28, &J::fmaddXX},
    {29, &J::fmaddXX},
    {30, &J::fmaddXX},
    {31, &J::fmaddXX},
}};

// Opcode 63 with a 10-bit extended opcode: moves, compares, conversions,
// FPSCR access.
constexpr std::array<OpEntry, 15> OP63_X_FORM{{
    {0, &J::fcmpX},
    {12, &J::frspx},
    {14, &J::fctiwx},
    {15, &J::fctiwx},
    {32, &J::fcmpX},
    {38, &J::mtfsb1x},
    {40, &J::fsign},
    {64, &J::mcrfs},
    {70, &J::mtfsb0x},
    {72, &J::fmrx},
    {134, &J::mtfsfix},
    {136, &J::fsign},
    {264, &J::fsign},
    {583, &J::mffsx},
    {711, &J::mtfsfx},
}};

// Opcode 63 with a 5-bit extended opcode: double-precision arithmetic.
constexpr std::array<OpEntry, 10> OP63_A_FORM{{
    {18, &J::fp_arith},
    {20, &J::fp_arith},
    {21, &J::fp_arith},
    {23, &J::fselx},
    {25, &J::fp_arith},
    {26, &J::frsqrtex},
    {28, &J::fmaddXX},
    {29, &J::fmaddXX},
    {30, &J::fmaddXX},
    {31, &J::fmaddXX},
}};

template <size_t Size>
using HandlerTable = std::array<Jit64::Instruction, Size>;

// Writes `entries` into the table, repeated every `stride` slots: the table
// is indexed by a wider field than the extended opcode in these forms, and
// the extra bits are register numbers.
template <size_t Size, size_t Count>
constexpr void Place(HandlerTable<Size>& table, const std::array<OpEntry, Count>& entries,
                     u32 stride)
{
  for (u32 base = 0; base < Size; base += stride)
  {
    for (const OpEntry& entry : entries)
    {
      const u32 slot = base + entry.opcode;
      ASSERT(table[slot] == &Jit64::FallBackToInterpreter);
      table[slot] = entry.handler;
    }
  }
}

template <size_t Size>
constexpr HandlerTable<Size> InterpretEverything()
{
  HandlerTable<Size> table{};
  table.fill(&Jit64::FallBackToInterpreter);
  return table;
}

constexpr HandlerTable<64> PRIMARY_TABLE = [] {
  auto table = InterpretEverything<64>();
  Place(table, PRIMARY_OPS, 64);
  return table;
}();

constexpr HandlerTable<1024> TABLE4 = [] {
  auto table = InterpretEverything<1024>();
  Place(table, OP4_A_FORM, 32);
  Place(table, OP4_QUANTIZED_X, 64);
  Place(table, OP4_X_FORM, 1024);
  return table;
}();

constexpr HandlerTable<1024> TABLE19 = [] {
  auto table = InterpretEverything<1024>();
  Place(table, OP19, 1024);
  return table;
}();

constexpr HandlerTable<1024> TABLE31 = [] {
  auto table = InterpretEverything<1024>();
  Place(table, OP31, 1024);
  return table;
}();

constexpr HandlerTable<32> TABLE59 = [] {
  auto table = InterpretEverything<32>();
  Place(table, OP59, 32);
  return table;
}();

constexpr HandlerTable<1024> TABLE63 = [] {
  auto table = InterpretEverything<1024>();
  Place(table, OP63_X_FORM, 1024);
  Place(table, OP63_A_FORM, 32);
  return table;
}();
}  // namespace

void Jit64::DynaRunTable4(UGeckoInstruction inst)
{
  (this->*TABLE4[inst.SUBOP10])(inst);
}

void Jit64::DynaRunTable19(UGeckoInstruction inst)
{
  (this->*TABLE19[inst.SUBOP10])(inst);
}

void Jit64::DynaRunTable31(UGeckoInstruction inst)
{
  (this->*TABLE31[inst.SUBOP10])(inst);
}

void Jit64::DynaRunTable59(UGeckoInstruction inst)
{
  (this->*TABLE59[inst.SUBOP5])(inst);
}

void Jit64::DynaRunTable63(UGeckoInstruction inst)
{
  (this->*TABLE63[inst.SUBOP10])(inst);
}

void Jit64::CompileInstruction(PPCAnalyst::CodeOp& op)
{
  (this->*PRIMARY_TABLE[op.inst.OPCD])(op.inst);
  PPCTables::CountInstructionCompile(op.opinfo, js.compilerPC);
}
