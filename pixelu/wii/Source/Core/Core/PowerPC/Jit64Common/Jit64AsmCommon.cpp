// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"

#include <array>
#include <cstddef>
#include <utility>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
#include "Common/JitRegister.h"
#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"

using namespace Gen;

namespace
{
// Host registers the quantized routines may clobber without saving: the
// three scratch registers plus XMM0 and XMM1, which carry the operands.
BitSet32 QuantizedSaveSet()
{
  return ABI_ALL_CALLER_SAVED &
         ~BitSet32{RSCRATCH, RSCRATCH2, RSCRATCH_EXTRA, XMM0 + 16, XMM1 + 16};
}

// Loads additionally keep RSCRATCH2, which holds the scale for the multiply
// that follows the access.
BitSet32 QuantizedLoadSaveSet()
{
  return QuantizedSaveSet() | BitSet32{RSCRATCH2};
}

bool IsReservedQuantizeType(EQuantizeType type)
{
  return type == QUANTIZE_INVALID1 || type == QUANTIZE_INVALID2 || type == QUANTIZE_INVALID3;
}

// Width in bits of one quantized element.
int QuantizedElementBits(EQuantizeType type)
{
  switch (type)
  {
  case QUANTIZE_FLOAT:
    return 32;
  case QUANTIZE_U8:
  case QUANTIZE_S8:
    return 8;
  case QUANTIZE_U16:
  case QUANTIZE_S16:
    return 16;
  default:
    return 0;
  }
}

// Shared routines receive the address in a register, run with the guest's
// data translation on, and have no instruction PC of their own.
int QuantizedAccessFlags(bool is_inline)
{
  if (is_inline)
    return 0;
  return EmuCodeBlock::SAFE_LOADSTORE_NO_FASTMEM | EmuCodeBlock::SAFE_LOADSTORE_NO_PROLOG |
         EmuCodeBlock::SAFE_LOADSTORE_DR_ON | EmuCodeBlock::SAFE_LOADSTORE_NO_UPDATE_PC;
}

// Bit fields of a double, for the bit-exact double -> single narrowing.
constexpr u64 DOUBLE_SIGN_AND_TOP_EXP = 0xC000000000000000ULL;   // single bits 31-30
constexpr u64 DOUBLE_LOW_EXP_AND_FRAC = 0x07FFFFFFE0000000ULL;   // single bits 29-0
constexpr u64 DOUBLE_FRACTION = 0x000FFFFFFFFFFFFFULL;
constexpr u64 DOUBLE_IMPLICIT_ONE = 0x0010000000000000ULL;

alignas(16) constexpr std::array<u64, 2> NARROW_HIGH_MASK = {DOUBLE_SIGN_AND_TOP_EXP, 0};
alignas(16) constexpr std::array<u64, 2> NARROW_LOW_MASK = {DOUBLE_LOW_EXP_AND_FRAC, 0};
alignas(16) constexpr std::array<u64, 2> FRACTION_MASK = {DOUBLE_FRACTION, 0};
alignas(16) constexpr std::array<u64, 2> IMPLICIT_ONE_BIT = {DOUBLE_IMPLICIT_ONE, 0};

// Per-lane constants for the paired-single rounding fix.
alignas(16) constexpr std::array<u64, 2> LANE_SIGN_BITS = {0x8000000000000000ULL,
                                                           0x8000000000000000ULL};
alignas(16) constexpr std::array<u64, 2> LANE_ONES = {1, 1};

// Clamp bounds for quantized stores.
alignas(16) constexpr std::array<float, 4> CLAMP_U16_MAX = {65535.0f, 65535.0f, 65535.0f,
                                                            65535.0f};
alignas(16) constexpr float CLAMP_S16_MAX = 32767.0f;
alignas(16) constexpr float CLAMP_S16_MIN = -32768.0f;
alignas(16) constexpr float CLAMP_U8_MAX = 255.0f;
alignas(16) constexpr float CLAMP_S8_MAX = 127.0f;
alignas(16) constexpr float CLAMP_S8_MIN = -128.0f;
}  // namespace

// Narrows the double in XMM0 to single-precision bits in RSCRATCH exactly as
// the Gekko does, without CVTSD2SS (which would quiet SNaNs and flush
// denormals). The Gekko always takes double bits 63-62 and 58-29, except when
// the exponent is in [874, 896], where the result is a single denormal: the
// significand with its implicit one shifted right by (905 - exponent) + 21.
// Out-of-range values that the documentation calls undefined therefore
// produce the same bits as on hardware.
//
// Clobbers RSCRATCH, RSCRATCH2, XMM0 and XMM1.
void CommonAsmRoutines::GenConvertDoubleToSingle()
{
  constexpr u16 DENORMAL_EXP_FIRST = 874;
  constexpr u16 DENORMAL_EXP_LAST = 896;
  constexpr u16 DENORMAL_SHIFT_BASE = 905 + 21;

  const u8* const entry = GetCodePtr();
  const X64Reg result = RSCRATCH;
  const X64Reg raw = RSCRATCH2;

  MOVQ_xmm(R(result), XMM0);
  MOV(64, R(raw), R(result));
  SHR(64, R(result), Imm8(52));
  AND(16, R(result), Imm16(0x7FF));
  // result = exponent - first; unsigned <= range means a denormal result.
  SUB(16, R(result), Imm16(DENORMAL_EXP_FIRST));
  CMP(16, R(result), Imm16(static_cast<u16>(DENORMAL_EXP_LAST - DENORMAL_EXP_FIRST)));
  const FixupBranch to_denormal = J_CC(CC_NA);

  if (cpu_info.bBMI2FastParallelBitOps)
  {
    MOV(64, R(result), Imm64(DOUBLE_SIGN_AND_TOP_EXP | DOUBLE_LOW_EXP_AND_FRAC));
    PEXT(64, result, raw, R(result));
  }
  else
  {
    // High two bits land in bits 31-30 after >> 32, the rest after >> 29.
    avx_op(&XEmitter::VPAND, &XEmitter::PAND, XMM1, R(XMM0), MConst(NARROW_HIGH_MASK));
    PSRLQ(XMM1, 32);
    PAND(XMM0, MConst(NARROW_LOW_MASK));
    PSRLQ(XMM0, 29);
    POR(XMM0, R(XMM1));
    MOVD_xmm(R(result), XMM0);
  }
  RET();

  SetJumpTarget(to_denormal);
  // shift = DENORMAL_SHIFT_BASE - exponent = (DENORMAL_SHIFT_BASE - first) - result
  NEG(16, R(result));
  ADD(16, R(result), Imm16(static_cast<u16>(DENORMAL_SHIFT_BASE - DENORMAL_EXP_FIRST)));
  MOVQ_xmm(XMM1, R(result));
  PAND(XMM0, MConst(FRACTION_MASK));
  POR(XMM0, MConst(IMPLICIT_ONE_BIT));
  PSRLQ(XMM0, R(XMM1));
  MOVD_xmm(R(result), XMM0);
  // Bring the sign over from bit 63.
  SHR(64, R(raw), Imm8(32));
  AND(32, R(raw), Imm32(0x80000000));
  OR(32, R(result), R(raw));
  RET();

  Common::JitRegister::Register(entry, GetCodePtr(), "JIT_cdts");
}

void CommonAsmRoutines::RaiseFPSCRException(u32 exception_bit)
{
  // FX only goes up on a 0 -> 1 transition of an exception bit.
  TEST(32, PPCSTATE(fpscr), Imm32(exception_bit));
  const FixupBranch already_set = J_CC(CC_NZ);
  OR(32, PPCSTATE(fpscr), Imm32(FPSCR_FX | exception_bit));
  SetJumpTarget(already_set);
}

// frsqrte: XMM0 = estimate of 1/sqrt(XMM0), matching the Gekko's lookup
// table. Positive normal inputs are handled inline; everything else either
// has a fixed answer or goes to Common::ApproximateReciprocalSquareRoot.
// Clobbers all three scratch registers.
void CommonAsmRoutines::GenFrsqrte()
{
  static_assert(sizeof(Common::BaseAndDec) == 8, "table lookups below use SCALE_8");
  constexpr int dec_offset = static_cast<int>(offsetof(Common::BaseAndDec, m_dec));
  constexpr int base_offset = static_cast<int>(offsetof(Common::BaseAndDec, m_base));

  const u8* const entry = GetCodePtr();
  const X64Reg bits = RSCRATCH;
  const X64Reg tmp = RSCRATCH2;
  const X64Reg top = RSCRATCH_EXTRA;  // sign and exponent, later the table index

  MOVQ_xmm(R(bits), XMM0);
  MOV(64, R(top), R(bits));
  SHR(64, R(top), Imm8(52));

  // A positive normal number has sign:exponent in [1, 0x7FE].
  LEA(32, tmp, MDisp(top, -1));
  CMP(32, R(tmp), Imm32(0x7FE));
  const FixupBranch special = J_CC(CC_AE, Jump::Near);

  // Result exponent: 0x3FF - ((exponent - 0x3FD) >> 1), placed at bit 52.
  SUB(32, R(top), Imm32(0x3FD));
  SAR(32, R(top), Imm8(1));
  MOV(32, R(tmp), Imm32(0x3FF));
  SUB(32, R(tmp), R(top));
  SHL(64, R(tmp), Imm8(52));

  // Index: exponent parity and the top four fraction bits (input bits 52-48).
  MOV(64, R(top), R(bits));
  SHR(64, R(top), Imm8(48));
  AND(32, R(top), Imm8(0x1F));

  // Fraction: base + dec * (next eleven fraction bits), placed at bit 26.
  PUSH(tmp);
  MOV(64, R(tmp), ImmPtr(GetConstantFromPool(Common::frsqrte_expected)));
  SHR(64, R(bits), Imm8(37));
  AND(32, R(bits), Imm32(0x7FF));
  IMUL(32, bits, MComplex(tmp, top, SCALE_8, dec_offset));
  ADD(32, R(bits), MComplex(tmp, top, SCALE_8, base_offset));
  SHL(64, R(bits), Imm8(26));
  POP(tmp);

  OR(64, R(tmp), R(bits));
  MOVQ_xmm(XMM0, R(tmp));
  RET();

  SetJumpTarget(special);
  AND(32, R(top), Imm32(0x7FF));
  CMP(32, R(top), Imm32(0x7FF));
  const FixupBranch nan_or_infinity = J_CC(CC_E);

  // Shifting out the sign leaves ZF = zero and CF = negative.
  MOV(64, R(tmp), R(bits));
  SHL(64, R(tmp), Imm8(1));
  const FixupBranch not_zero = J_CC(CC_NZ);

  // +-0: divide-by-zero, result is infinity of the same sign.
  RaiseFPSCRException(FPSCR_ZX);
  MOV(64, R(tmp), Imm64(0x7FF0'0000'0000'0000));
  OR(64, R(tmp), R(bits));
  MOVQ_xmm(XMM0, R(tmp));
  RET();

  SetJumpTarget(nan_or_infinity);
  MOV(64, R(tmp), R(bits));
  SHL(64, R(tmp), Imm8(12));
  const FixupBranch infinity = J_CC(CC_Z);
  // NaN: returned quieted.
  BTS(64, R(bits), Imm8(51));
  MOVQ_xmm(XMM0, R(bits));
  RET();

  SetJumpTarget(infinity);
  TEST(64, R(bits), R(bits));
  const FixupBranch negative_infinity = J_CC(CC_S);
  // +inf: result +0.
  XORPD(XMM0, R(XMM0));
  RET();

  // Flags still come from the SHL above.
  SetJumpTarget(not_zero);
  const FixupBranch positive_denormal = J_CC(CC_NC);

  // Negative input: invalid operation, default QNaN.
  SetJumpTarget(negative_infinity);
  RaiseFPSCRException(FPSCR_VXSQRT);
  MOV(64, R(tmp), Imm64(0x7FF8'0000'0000'0000));
  MOVQ_xmm(XMM0, R(tmp));
  RET();

  SetJumpTarget(positive_denormal);
  ABI_PushRegistersAndAdjustStack(QuantizedSaveSet(), 8);
  ABI_CallFunction(Common::ApproximateReciprocalSquareRoot);
  ABI_PopRegistersAndAdjustStack(QuantizedSaveSet(), 8);
  RET();

  Common::JitRegister::Register(entry, GetCodePtr(), "JIT_Frsqrte");
}

// fres: XMM0 = estimate of 1/XMM0 from the Gekko's table. Inputs whose
// exponent lies in [895, 1149) are handled inline; the rest, and zero (after
// raising ZX), go to Common::ApproximateReciprocal.
// Clobbers all three scratch registers.
void CommonAsmRoutines::GenFres()
{
  static_assert(sizeof(Common::BaseAndDec) == 8, "table lookups below use SCALE_8");
  constexpr int dec_offset = static_cast<int>(offsetof(Common::BaseAndDec, m_dec));
  constexpr int base_offset = static_cast<int>(offsetof(Common::BaseAndDec, m_base));
  constexpr u32 INLINE_EXP_FIRST = 895;
  constexpr u32 INLINE_EXP_END = 1149;

  const u8* const entry = GetCodePtr();
  const X64Reg bits = RSCRATCH;
  const X64Reg tmp = RSCRATCH2;
  const X64Reg high = RSCRATCH_EXTRA;  // exponent, then sign | exponent

  MOVQ_xmm(R(bits), XMM0);
  TEST(64, R(bits), R(bits));
  const FixupBranch zero = J_CC(CC_Z);

  MOV(64, R(high), R(bits));
  SHR(64, R(high), Imm8(52));
  MOV(32, R(tmp), R(high));
  AND(32, R(high), Imm32(0x7FF));
  AND(32, R(tmp), Imm32(0x800));
  SUB(32, R(high), Imm32(INLINE_EXP_FIRST));
  CMP(32, R(high), Imm32(INLINE_EXP_END - INLINE_EXP_FIRST));
  const FixupBranch out_of_range = J_CC(CC_AE);

  // Result exponent 0x7FD - exponent, with the sign kept, placed at bit 52.
  SUB(32, R(high), Imm32(0x7FD - INLINE_EXP_FIRST));
  NEG(32, R(high));
  OR(32, R(high), R(tmp));
  SHL(64, R(high), Imm8(52));

  // tmp = table index (fraction bits 51-47), bits = next ten fraction bits.
  MOV(64, R(tmp), R(bits));
  SHR(64, R(bits), Imm8(37));
  SHR(64, R(tmp), Imm8(47));
  AND(32, R(bits), Imm32(0x3FF));
  AND(32, R(tmp), Imm8(0x1F));

  // Fraction: base - (dec * bits + 1) / 2, placed at bit 29.
  PUSH(high);
  MOV(64, R(high), ImmPtr(GetConstantFromPool(Common::fres_expected)));
  IMUL(32, bits, MComplex(high, tmp, SCALE_8, dec_offset));
  ADD(32, R(bits), Imm8(1));
  SHR(32, R(bits), Imm8(1));
  MOV(32, R(tmp), MComplex(high, tmp, SCALE_8, base_offset));
  SUB(32, R(tmp), R(bits));
  SHL(64, R(tmp), Imm8(29));
  POP(high);

  OR(64, R(tmp), R(high));
  MOVQ_xmm(XMM0, R(tmp));
  RET();

  SetJumpTarget(zero);
  RaiseFPSCRException(FPSCR_ZX);

  SetJumpTarget(out_of_range);
  ABI_PushRegistersAndAdjustStack(QuantizedSaveSet(), 8);
  ABI_CallFunction(Common::ApproximateReciprocal);
  ABI_PopRegistersAndAdjustStack(QuantizedSaveSet(), 8);
  RET();

  Common::JitRegister::Register(entry, GetCodePtr(), "JIT_Fres");
}

// mfcr: builds the 32-bit CR in RSCRATCH from the eight 64-bit emulated CR
// fields, field 0 ending up in the top nibble.
// Clobbers all three scratch registers.
void CommonAsmRoutines::GenMfcr()
{
  const u8* const entry = GetCodePtr();
  const X64Reg cr = RSCRATCH;
  const X64Reg flag = RSCRATCH2;
  const X64Reg field = RSCRATCH_EXTRA;

  XOR(32, R(cr), R(cr));
  // SETcc only writes the low byte; the LEAs read all of `flag`.
  XOR(32, R(flag), R(flag));

  for (int index = 0; index < 8; ++index)
  {
    if (index > 0)
      SHL(32, R(cr), Imm8(4));
    MOV(64, R(field), PPCSTATE_CR(index));

    // EQ (bit 1): the low 32 bits are zero.
    TEST(32, R(field), R(field));
    SETcc(CC_Z, R(flag));
    LEA(32, cr, MComplex(cr, flag, SCALE_2, 0));

    // GT (bit 2): the value is positive.
    TEST(64, R(field), R(field));
    SETcc(CC_G, R(flag));
    LEA(32, cr, MComplex(cr, flag, SCALE_4, 0));

    // SO (bit 0) and LT (bit 3) are stored at bits 59 and 62.
    SHR(64, R(field), Imm8(PowerPC::CR_EMU_SO_BIT));
    AND(32, R(field), Imm8(PowerPC::CR_LT | PowerPC::CR_SO));
    OR(32, R(cr), R(field));
  }
  RET();

  Common::JitRegister::Register(entry, GetCodePtr(), "JIT_Mfcr");
}

// Corrects the rounding of an fmadds result computed with an error-free
// transformation. A single-rounded result can only be wrong when it sits
// exactly halfway between two singles (as a double: low 29 bits ==
// 0x10000000) and the residual error is non-zero; it is then stepped one ulp
// towards the error's sign.
//
// In: XMM0 = first error term, XMM1 = result, XMM2 = negated second error
// term. Out: XMM1. Clobbers RSCRATCH, RSCRATCH2, XMM0, XMM2 and flags.
void CommonAsmRoutines::GenerateFmaddsEft()
{
  const X64Reg result_bits = RSCRATCH;
  const X64Reg probe = RSCRATCH2;

  // (bits << 3) + 0x80000000 is zero in its low 32 bits exactly at a tie.
  MOVQ_xmm(R(result_bits), XMM1);
  MOV(32, R(probe), Imm32(0x80000000));
  LEA(32, probe, MComplex(probe, result_bits, SCALE_8, 0));
  TEST(32, R(probe), R(probe));
  const FixupBranch tie = J_CC(CCFlags::CC_Z);

  const u8* const done = GetCodePtr();
  RET();

  SetJumpTarget(tie);
  SUBSD(XMM0, R(XMM2));
  XORPD(XMM2, R(XMM2));
  UCOMISD(XMM0, R(XMM2));
  J_CC(CCFlags::CC_E, done);

  // step = (error ^ result) < 0 ? -1 : +1, applied to the bit pattern.
  MOVQ_xmm(R(probe), XMM0);
  XOR(64, R(probe), R(result_bits));
  SAR(64, R(probe), Imm8(63));
  OR(64, R(probe), Imm8(1));
  ADD(64, R(result_bits), R(probe));
  MOVQ_xmm(XMM1, R(result_bits));
  RET();
}

// The two-lane version of GenerateFmaddsEft for ps_madd and friends.
//
// In: XMM0 = first error terms, XMM1 = results, XMM2 = negated second error
// terms. Out: XMM1. Clobbers RSCRATCH, XMM0-XMM3 and flags.
void CommonAsmRoutines::GeneratePsMaddEft()
{
  // XMM3 = all-ones in each lane whose result is at a tie.
  avx_op(&XEmitter::VPSLLQ, &XEmitter::PSLLQ, XMM3, XMM1, 35);
  if (cpu_info.bSSE4_1)
  {
    PCMPEQQ(XMM3, MConst(LANE_SIGN_BITS));
  }
  else
  {
    // Compare 16-bit pieces, then spread the result of the top dword's high
    // half over the whole lane.
    PCMPEQW(XMM3, MConst(LANE_SIGN_BITS));
    PSHUFD(XMM3, R(XMM3), 0xF5);
  }

  // Most of the time neither lane is a tie.
  if (cpu_info.bSSE4_1)
  {
    PTEST(XMM3, R(XMM3));
  }
  else
  {
    PMOVMSKB(RSCRATCH, R(XMM3));
    TEST(32, R(RSCRATCH), R(RSCRATCH));
  }
  const FixupBranch any_tie = J_CC(CCFlags::CC_NZ);
  RET();
  SetJumpTarget(any_tie);

  // XMM2 = lanes whose error is exactly zero.
  SUBPD(XMM0, R(XMM2));
  XORPD(XMM2, R(XMM2));
  CMPPD(XMM2, R(XMM0), CMP_EQ);

  // XMM0 = per-lane step of -1 or +1 from the sign of error ^ result.
  PXOR(XMM0, R(XMM1));
  PSRAD(XMM0, 31);
  PSHUFD(XMM0, R(XMM0), 0xF5);
  POR(XMM0, MConst(LANE_ONES));

  // Step only lanes that are at a tie and have a non-zero error.
  PANDN(XMM2, R(XMM3));
  PAND(XMM0, R(XMM2));
  PADDQ(XMM1, R(XMM0));
  RET();
}

const u8** CommonAsmRoutines::GenQuantizedTable(bool load, bool single)
{
  // Jit64's psq_l/psq_st index the table with the GQR type already in a
  // byte position, so the table must start on a 256-byte boundary.
  const u8** const table = reinterpret_cast<const u8**>(AlignCodeTo(256));
  ReserveCodeSpace(8 * sizeof(u8*));

  for (u32 index = 0; index < 8; ++index)
  {
    const auto type = static_cast<EQuantizeType>(index);
    table[index] =
        load ? GenQuantizedLoadRuntime(single, type) : GenQuantizedStoreRuntime(single, type);
  }
  return table;
}

void CommonAsmRoutines::GenQuantizedLoads()
{
  paired_load_quantized = GenQuantizedTable(true, false);
}

void CommonAsmRoutines::GenQuantizedSingleLoads()
{
  single_load_quantized = GenQuantizedTable(true, true);
}

void CommonAsmRoutines::GenQuantizedStores()
{
  paired_store_quantized = GenQuantizedTable(false, false);
}

void CommonAsmRoutines::GenQuantizedSingleStores()
{
  single_store_quantized = GenQuantizedTable(false, true);
}

const u8* CommonAsmRoutines::GenQuantizedLoadRuntime(bool single, EQuantizeType type)
{
  const u8* const routine_start = GetCodePtr();
  const u8* const entry = AlignCode4();
  GenQuantizedLoad(single, type, -1);
  RET();
  Common::JitRegister::Register(routine_start, GetCodePtr(), "JIT_QuantizedLoad_{}_{}",
                                std::to_underlying(type), single);
  return entry;
}

const u8* CommonAsmRoutines::GenQuantizedStoreRuntime(bool single, EQuantizeType type)
{
  const u8* const routine_start = GetCodePtr();
  const u8* const entry = AlignCode4();
  GenQuantizedStore(single, type, -1);
  RET();
  Common::JitRegister::Register(routine_start, GetCodePtr(), "JIT_QuantizedStore_{}_{}",
                                std::to_underlying(type), single);
  return entry;
}

void QuantizedMemoryRoutines::ApplyQuantizeScale(bool single, int quantize, bool load)
{
  const auto& table = load ? m_dequantizeTableS : m_quantizeTableS;

  // The table holds each scale twice (one per lane), so scale s is at float
  // index 2 * s. The shared routines get the GQR scale field as s << 8 in
  // RSCRATCH2; >> 5 turns that into the byte offset 8 * s.
  OpArg factor;
  if (quantize == -1)
  {
    SHR(32, R(RSCRATCH2), Imm8(5));
    LEA(64, RSCRATCH, MConst(table));
    factor = MRegSum(RSCRATCH2, RSCRATCH);
  }
  else if (quantize > 0)
  {
    factor = MConst(table, static_cast<size_t>(quantize) * 2);
  }
  else
  {
    // Scale 0 is a factor of one.
    return;
  }

  if (single)
  {
    MULSS(XMM0, factor);
  }
  else
  {
    MOVQ_xmm(XMM1, factor);
    MULPS(XMM0, R(XMM1));
  }
}

// In: one (single) or two singles in XMM0 and the address in RSCRATCH_EXTRA;
// for the shared routines (quantize == -1) also the scale field in RSCRATCH2.
void QuantizedMemoryRoutines::GenQuantizedStore(bool single, EQuantizeType type, int quantize)
{
  // Reserved GQR types trap, as in the old JIT.
  if (IsReservedQuantizeType(type))
  {
    UD2();
    return;
  }

  const bool is_inline = quantize != -1;
  const int access_bits = QuantizedElementBits(type) * (single ? 1 : 2);

  if (type == QUANTIZE_FLOAT)
  {
    GenQuantizedStoreFloat(single, is_inline);
  }
  else if (single)
  {
    ApplyQuantizeScale(true, quantize, false);

    // Saturate to the integer range before the truncating conversion.
    const auto clamp_from_zero = [&](const OpArg& max) {
      XORPS(XMM1, R(XMM1));
      MAXSS(XMM0, R(XMM1));
      MINSS(XMM0, max);
    };
    const auto clamp = [&](const OpArg& min, const OpArg& max) {
      MAXSS(XMM0, min);
      MINSS(XMM0, max);
    };
    switch (type)
    {
    case QUANTIZE_U8:
      clamp_from_zero(MConst(CLAMP_U8_MAX));
      break;
    case QUANTIZE_S8:
      clamp(MConst(CLAMP_S8_MIN), MConst(CLAMP_S8_MAX));
      break;
    case QUANTIZE_U16:
      clamp_from_zero(MConst(CLAMP_U16_MAX));
      break;
    case QUANTIZE_S16:
      clamp(MConst(CLAMP_S16_MIN), MConst(CLAMP_S16_MAX));
      break;
    default:
      break;
    }
    CVTTSS2SI(RSCRATCH, R(XMM0));
  }
  else
  {
    ApplyQuantizeScale(false, quantize, false);

    // PACKUSDW saturates unsigned 16-bit directly; the shuffle fallback below
    // needs non-negative inputs.
    const bool has_packusdw = cpu_info.bSSE4_1;
    if (type == QUANTIZE_U16 && !has_packusdw)
    {
      XORPS(XMM1, R(XMM1));
      MAXPS(XMM0, R(XMM1));
    }

    // CVTTPS2DQ turns positive overflow into 0x80000000, so clip from above
    // first; the packing below does the rest of the saturation.
    MINPS(XMM0, MConst(CLAMP_U16_MAX));
    CVTTPS2DQ(XMM0, R(XMM0));

    // Pack both lanes into RSCRATCH in guest order.
    switch (type)
    {
    case QUANTIZE_U8:
    case QUANTIZE_S8:
      PACKSSDW(XMM0, R(XMM0));
      if (type == QUANTIZE_U8)
        PACKUSWB(XMM0, R(XMM0));
      else
        PACKSSWB(XMM0, R(XMM0));
      MOVD_xmm(R(RSCRATCH), XMM0);
      break;
    case QUANTIZE_U16:
      if (has_packusdw)
      {
        PACKUSDW(XMM0, R(XMM0));
        MOVD_xmm(R(RSCRATCH), XMM0);
        BSWAP(32, RSCRATCH);
        ROL(32, R(RSCRATCH), Imm8(16));
      }
      else
      {
        // Values are in [0, 65535], so moving words 0 and 2 together packs them.
        PSHUFLW(XMM0, R(XMM0), 2);
        MOVD_xmm(R(RSCRATCH), XMM0);
        BSWAP(32, RSCRATCH);
      }
      break;
    case QUANTIZE_S16:
      PACKSSDW(XMM0, R(XMM0));
      MOVD_xmm(R(RSCRATCH), XMM0);
      BSWAP(32, RSCRATCH);
      ROL(32, R(RSCRATCH), Imm8(16));
      break;
    default:
      break;
    }
  }

  // Paired values were put in guest byte order above.
  int flags = QuantizedAccessFlags(is_inline);
  if (!single)
    flags |= SAFE_LOADSTORE_NO_SWAP;
  SafeWriteRegToReg(RSCRATCH, RSCRATCH_EXTRA, access_bits, 0, QuantizedSaveSet(), flags);
}

void QuantizedMemoryRoutines::GenQuantizedStoreFloat(bool single, bool /*isInline*/)
{
  if (single)
  {
    MOVD_xmm(R(RSCRATCH), XMM0);
    return;
  }

  // Swap each single into guest order while keeping lane 0 first.
  if (cpu_info.bSSSE3)
  {
    PSHUFB(XMM0, MConst(pbswapShuffle2x4));
    MOVQ_xmm(R(RSCRATCH), XMM0);
  }
  else
  {
    MOVQ_xmm(R(RSCRATCH), XMM0);
    ROL(64, R(RSCRATCH), Imm8(32));
    BSWAP(64, RSCRATCH);
  }
}

// In: the address in RSCRATCH_EXTRA; for the shared routines (quantize == -1)
// the scale field in RSCRATCH2 (as 0x0000ss00). Out: XMM0, with lane 1 set to
// 1.0 for single loads.
void QuantizedMemoryRoutines::GenQuantizedLoad(bool single, EQuantizeType type, int quantize)
{
  if (IsReservedQuantizeType(type))
  {
    UD2();
    return;
  }

  const bool is_inline = quantize != -1;
  if (type == QUANTIZE_FLOAT)
  {
    GenQuantizedLoadFloat(single, is_inline);
    return;
  }

  const int access_bits = QuantizedElementBits(type) * (single ? 1 : 2);
  const bool is_signed = type == QUANTIZE_S8 || type == QUANTIZE_S16;
  const bool is_byte = type == QUANTIZE_U8 || type == QUANTIZE_S8;

  // Single values are sign-extended by the load; paired values are unpacked
  // below.
  SafeLoadToReg(RSCRATCH_EXTRA, R(RSCRATCH_EXTRA), access_bits, 0, QuantizedLoadSaveSet(),
                single && is_signed, QuantizedAccessFlags(is_inline));
  // A paired byte load comes back as a byte-swapped halfword; undo that.
  if (!single && is_byte)
    ROR(16, R(RSCRATCH_EXTRA), Imm8(8));

  if (single)
  {
    CVTSI2SS(XMM0, R(RSCRATCH_EXTRA));
    ApplyQuantizeScale(true, quantize, true);
    UNPCKLPS(XMM0, MConst(m_one));
    return;
  }

  // Widen the two elements to 32-bit integers in lanes 0 and 1.
  if (!is_byte)
    ROL(32, R(RSCRATCH_EXTRA), Imm8(16));
  MOVD_xmm(XMM0, R(RSCRATCH_EXTRA));
  if (cpu_info.bSSE4_1)
  {
    switch (type)
    {
    case QUANTIZE_U8:
      PMOVZXBD(XMM0, R(XMM0));
      break;
    case QUANTIZE_S8:
      PMOVSXBD(XMM0, R(XMM0));
      break;
    case QUANTIZE_U16:
      PMOVZXWD(XMM0, R(XMM0));
      break;
    default:
      PMOVSXWD(XMM0, R(XMM0));
      break;
    }
  }
  else if (is_signed)
  {
    // Interleave each element with itself, then shift the copy down.
    if (is_byte)
      PUNPCKLBW(XMM0, R(XMM0));
    PUNPCKLWD(XMM0, R(XMM0));
    PSRAD(XMM0, is_byte ? 24 : 16);
  }
  else
  {
    // Interleave with zeros.
    PXOR(XMM1, R(XMM1));
    if (is_byte)
      PUNPCKLBW(XMM0, R(XMM1));
    PUNPCKLWD(XMM0, R(XMM1));
  }
  CVTDQ2PS(XMM0, R(XMM0));
  ApplyQuantizeScale(false, quantize, true);
}

void QuantizedMemoryRoutines::GenQuantizedLoadFloat(bool single, bool isInline)
{
  // Float loads need no scale, so RSCRATCH2 is not preserved here.
  SafeLoadToReg(RSCRATCH_EXTRA, R(RSCRATCH_EXTRA), single ? 32 : 64, 0, QuantizedSaveSet(), false,
                QuantizedAccessFlags(isInline));

  if (single)
  {
    MOVD_xmm(XMM0, R(RSCRATCH_EXTRA));
    UNPCKLPS(XMM0, MConst(m_one));
  }
  else
  {
    // The 64-bit load swapped the pair as a whole; put lane 0 back first.
    ROL(64, R(RSCRATCH_EXTRA), Imm8(32));
    MOVQ_xmm(XMM0, R(RSCRATCH_EXTRA));
  }
}
