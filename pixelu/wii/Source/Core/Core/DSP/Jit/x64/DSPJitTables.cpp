// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/Jit/x64/DSPJitTables.h"

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"
#include "Core/DSP/Jit/x64/DSPEmitter.h"

namespace DSP::JIT::x64
{
namespace
{
// An instruction matches a pattern when (inst & mask) == bits; the first match
// wins, so narrower patterns come before the families they carve out of.
struct Pattern
{
  u16 bits;
  u16 mask;
  JITHandler handler;
};

using E = DSPEmitter;

constexpr Pattern MAIN_PATTERNS[] = {
    // 0000 0000 0000 00xx .. 0000 0000 0001 xxxx
    {0x0000, 0xfffc, &E::OpNop},
    {0x0000, 0xfff0, &E::OpModifyAr},  // DAR, IAR, SUBARN by bits 2-3
    {0x0010, 0xfff0, &E::OpAddarn},
    {0x0021, 0xffff, &E::OpHalt},

    // Hardware loops and long immediates/addresses: 0000 0000 xxxr rrrr
    {0x0040, 0xffe0, &E::OpLoop},
    {0x0060, 0xffe0, &E::OpBloop},
    {0x0080, 0xffe0, &E::OpLri},
    {0x00c0, 0xffe0, &E::OpLr},
    {0x00e0, 0xffe0, &E::OpSr},

    // 0000 001d xxxx xxxx: accumulator immediates, conditional flow
    {0x0200, 0xfeff, &E::OpAddi},
    {0x0220, 0xfeff, &E::OpLogicImm},  // XORI
    {0x0240, 0xfeff, &E::OpLogicImm},  // ANDI
    {0x0260, 0xfeff, &E::OpLogicImm},  // ORI
    {0x0280, 0xfeff, &E::OpCmpi},
    {0x02a0, 0xfeff, &E::OpTestBits},  // ANDF
    {0x02c0, 0xfeff, &E::OpTestBits},  // ANDCF
    {0x0210, 0xfef0, &E::OpIlrr},      // ILRR, ILRRD, ILRRI, ILRRN by bits 2-3
    {0x02ca, 0xfffe, &E::OpShiftByAcm1},  // LSRN, ASRN
    {0x0270, 0xfff0, &E::OpIf},
    {0x0290, 0xfff0, &E::OpJump},
    {0x02b0, 0xfff0, &E::OpCall},
    {0x02d0, 0xfff0, &E::OpRet},
    {0x02f0, 0xfff0, &E::OpRti},

    {0x0400, 0xfe00, &E::OpAddis},
    {0x0600, 0xfe00, &E::OpCmpis},
    {0x0800, 0xf800, &E::OpLris},

    // 0001 xxxx xxxx xxxx
    {0x1000, 0xff00, &E::OpLoopi},
    {0x1100, 0xff00, &E::OpBloopi},
    {0x1200, 0xfe00, &E::OpSrBit},  // SBCLR, SBSET by bit 8
    {0x1400, 0xfe00, &E::OpShiftImm},  // LSL, LSR, ASL, ASR by bits 6-7
    {0x1600, 0xff00, &E::OpSi},
    {0x1700, 0xff10, &E::OpJumpReg},
    {0x1710, 0xff10, &E::OpCallReg},
    {0x1800, 0xfe00, &E::OpLrr},  // LRR, LRRD, LRRI, LRRN by bits 7-8
    {0x1a00, 0xfe00, &E::OpSrr},  // SRR, SRRD, SRRI, SRRN by bits 7-8
    {0x1c00, 0xfc00, &E::OpMrr},

    // 001x xxxx xxxx xxxx: short addresses through $cr
    {0x2000, 0xf800, &E::OpLrs},
    {0x2800, 0xfe00, &E::OpSrsh},
    {0x2c00, 0xfc00, &E::OpSrs},

    // 0011 xxxx xxxx xxxx: logic and variable shifts
    {0x3080, 0xfe80, &E::OpLogicAcc},    // XORC
    {0x3280, 0xfe80, &E::OpNotc},
    {0x3480, 0xfc80, &E::OpShiftByAx},   // LSRNRX
    {0x3880, 0xfc80, &E::OpShiftByAx},   // ASRNRX
    {0x3c80, 0xfc80, &E::OpShiftByAcm},  // LSRNR, ASRNR by bit 9
    {0x3c00, 0xfc80, &E::OpLogicAcc},    // ANDC, ORC by bit 9
    {0x3000, 0xf080, &E::OpLogicAx},     // XORR, ANDR, ORR by bits 10-11

    // 010x and 0110: additions, subtractions (bit 12) and moves
    {0x4000, 0xe800, &E::OpAddSubAxPart},  // ADDR, SUBR
    {0x4800, 0xec00, &E::OpAddSubAx},      // ADDAX, SUBAX
    {0x4c00, 0xee00, &E::OpAddSubAcc},     // ADD, SUB
    {0x4e00, 0xee00, &E::OpAddSubProd},    // ADDP, SUBP
    {0x6000, 0xf800, &E::OpMovr},
    {0x6800, 0xfc00, &E::OpMovax},
    {0x6c00, 0xfe00, &E::OpMov},
    {0x6e00, 0xfe00, &E::OpMovp},

    // 0111 xxxx xxxx xxxx
    {0x7000, 0xfc00, &E::OpAddaxl},
    {0x7400, 0xfc00, &E::OpIncDec},  // INCM, INC
    {0x7800, 0xfc00, &E::OpIncDec},  // DECM, DEC
    {0x7c00, 0xfe00, &E::OpNeg},
    {0x7e00, 0xfe00, &E::OpMovnp},

    // 1000 xxxx xxxx xxxx
    {0x8000, 0xf700, &E::OpNop},  // NX
    {0x8100, 0xf700, &E::OpClr},
    {0x8200, 0xff00, &E::OpCmp},
    {0x8300, 0xff00, &E::OpMulaxh},
    {0x8400, 0xff00, &E::OpClrp},
    {0x8500, 0xff00, &E::OpTstprod},
    {0x8600, 0xfe00, &E::OpTstaxh},
    {0x8a00, 0xfe00, &E::OpModeBit},  // M2, M0
    {0x8c00, 0xfc00, &E::OpModeBit},  // CLR15, SET15, SET16, SET40

    // 1001 / 101x / 110x: multiplies (accumulator use in bits 9-10)
    {0x9100, 0xf700, &E::OpAsr16},
    {0x9000, 0xf000, &E::OpMul},  // MUL, MULMVZ, MULAC, MULMV
    {0xa100, 0xf700, &E::OpAbs},
    {0xb100, 0xf700, &E::OpTst},
    {0xa000, 0xe000, &E::OpMulx},  // MULX, MULXMVZ, MULXAC, MULXMV
    {0xc100, 0xe700, &E::OpCmpaxh},
    {0xc000, 0xe000, &E::OpMulc},  // MULC, MULCMVZ, MULCAC, MULCMV

    // 1110 xxxx xxxx xxxx: MADDX, MSUBX, MADDC, MSUBC
    {0xe000, 0xf000, &E::OpMaddxc},

    // 1111 xxxx xxxx xxxx
    {0xf000, 0xfe00, &E::OpLsl16},
    {0xf200, 0xfa00, &E::OpMadd},  // MADD, MSUB by bit 10
    {0xf400, 0xfe00, &E::OpLsr16},
    {0xf800, 0xfc00, &E::OpAddpaxz},
    {0xfc00, 0xfe00, &E::OpClrl},
    {0xfe00, 0xfe00, &E::OpMovpz},
};

constexpr Pattern EXT_PATTERNS[] = {
    {0x00, 0xf0, &E::ExtModifyAr},  // NOP, DR, IR, NR by bits 2-3
    {0x10, 0xf0, &E::ExtMove},
    {0x20, 0xe0, &E::ExtStore},       // S, SN
    {0x40, 0xc0, &E::ExtLoad},        // L, LN
    {0x80, 0xc0, &E::ExtLoadStore},   // LS, SL and their N/M forms
    {0xc3, 0xc3, &E::ExtLoadDualAx},  // LDAX and its N/M forms
    {0xc0, 0xc0, &E::ExtLoadDual},    // LD and its N/M forms
};

template <size_t N>
JITHandler Find(u16 inst, const Pattern (&patterns)[N])
{
  for (const Pattern& pattern : patterns)
  {
    if ((inst & pattern.mask) == pattern.bits)
      return pattern.handler;
  }
  return nullptr;
}

std::array<JITHandler, 0x10000> s_main;
std::array<JITHandler, 0x100> s_ext;
bool s_ready = false;
}  // namespace

JITHandler MainHandler(UDSPInstruction inst)
{
  return s_main[inst];
}

JITHandler ExtHandler(UDSPInstruction inst)
{
  const u16 bits = (inst >> 12) == 0x3 ? (inst & 0x7f) : (inst & 0xff);
  return s_ext[bits];
}

void InitHandlerTables()
{
  if (s_ready)
    return;
  for (u32 inst = 0; inst < s_main.size(); ++inst)
    s_main[inst] = Find(static_cast<u16>(inst), MAIN_PATTERNS);
  for (u32 bits = 0; bits < s_ext.size(); ++bits)
    s_ext[bits] = Find(static_cast<u16>(bits), EXT_PATTERNS);
  s_ready = true;
}
}  // namespace DSP::JIT::x64
