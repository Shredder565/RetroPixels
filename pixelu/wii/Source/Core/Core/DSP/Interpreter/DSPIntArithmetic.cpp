// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Accumulator arithmetic, logic, comparisons and shifts.
//
// Field layout reminders (see the instruction templates): the destination
// accumulator is usually bit 8 (bit 11 for the short 0x8xxx forms), an $axS
// source is bit 9, and the 0x3xxx logic forms use bits 8/9 the same way.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/Interpreter/DSPIntCCUtil.h"
#include "Core/DSP/Interpreter/DSPIntUtil.h"

namespace DSP::Interpreter
{
namespace
{
constexpr int Bit(UDSPInstruction opc, int position)
{
  return (opc >> position) & 1;
}

// A shift count held in a register: bits 0-5 are the magnitude field and bit 6
// the sign of a 7-bit two's complement number (a zero magnitude means no shift).
int RegisterShiftAmount(u16 field)
{
  const int magnitude = field & 0x3f;
  if (magnitude == 0)
    return 0;
  return (field & 0x40) != 0 ? magnitude - 64 : magnitude;
}
}  // namespace

// ---------------------------------------------------------------------------
// Shared bodies
// ---------------------------------------------------------------------------

void Interpreter::StoreAccWithAdd(int acc, s64 a, s64 b)
{
  ZeroWriteBackLog();
  SetAcc(acc, a + b);
  SetFlagsAdd(a, b, Acc(acc));
}

void Interpreter::StoreAccWithSub(int acc, s64 a, s64 b)
{
  ZeroWriteBackLog();
  SetAcc(acc, a - b);
  SetFlagsSub(a, b, Acc(acc));
}

// The flags describe the value as computed, before it is cut to 40 bits.
void Interpreter::StoreAccMoved(int acc, s64 value)
{
  ZeroWriteBackLog();
  SetAcc(acc, value);
  SetFlags40(value);
}

// The flags describe what ended up in the accumulator.
void Interpreter::StoreAccShifted(int acc, s64 value)
{
  ZeroWriteBackLog();
  SetAcc(acc, value);
  SetFlags40(Acc(acc));
}

void Interpreter::FinishMidLogic(int acc, u16 result)
{
  ZeroWriteBackLogPreserveAcc(acc);
  Regs().r.ac[acc].m = result;
  SetFlags16(static_cast<s16>(result), false, false, ExceedsS32(Acc(acc)));
}

s64 Interpreter::SignedAxPart(int selector) const
{
  // 0/1: $ax0.l/$ax1.l, 2/3: $ax0.h/$ax1.h, as a value in the middle word.
  const s16 part = selector < 2 ? AxLow(selector) : AxHigh(selector - 2);
  return static_cast<s64>(part) * 0x10000;
}

void Interpreter::ShiftBy7BitAmount(int acc, u16 amount_field, bool arithmetic,
                                    bool positive_is_left)
{
  int left = RegisterShiftAmount(amount_field);
  if (!positive_is_left)
    left = -left;

  s64 result;
  if (arithmetic)
  {
    const s64 value = Acc(acc);
    result = left >= 0 ? static_cast<s64>(static_cast<u64>(value) << left) : value >> -left;
  }
  else
  {
    const u64 value = static_cast<u64>(Acc(acc)) & ACC40_BITS;
    result = static_cast<s64>(left >= 0 ? value << left : value >> -left);
  }
  StoreAccShifted(acc, result);
}

// ---------------------------------------------------------------------------
// Clearing and testing
// ---------------------------------------------------------------------------

// CLR $acR
void Interpreter::clr(const UDSPInstruction opc)
{
  SetAcc(Bit(opc, 11), 0);
  SetFlags40(0);
  ZeroWriteBackLog();
}

// CLRL $acR.l: round to the middle word.
void Interpreter::clrl(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccMoved(acc, RoundToMiddleWord(Acc(acc)));
}

// ANDCF $acD.m, #I: LZ = all bits of I are set in $acD.m
void Interpreter::andcf(const UDSPInstruction opc)
{
  const u16 mask = Regs().FetchInstruction();
  const u16 mid = Regs().r.ac[Bit(opc, 8)].m;
  SetLogicZero((mid & mask) == mask);
}

// ANDF $acD.m, #I: LZ = no bit of I is set in $acD.m
void Interpreter::andf(const UDSPInstruction opc)
{
  const u16 mask = Regs().FetchInstruction();
  const u16 mid = Regs().r.ac[Bit(opc, 8)].m;
  SetLogicZero((mid & mask) == 0);
}

// TST $acR
void Interpreter::tst(const UDSPInstruction opc)
{
  SetFlags40(Acc(Bit(opc, 11)));
  ZeroWriteBackLog();
}

// TSTAXH $axR.h
void Interpreter::tstaxh(const UDSPInstruction opc)
{
  SetFlags16(AxHigh(Bit(opc, 8)), false, false, false);
  ZeroWriteBackLog();
}

// ---------------------------------------------------------------------------
// Comparisons (subtractions that only set flags)
// ---------------------------------------------------------------------------

// CMP: $ac0 - $ac1
void Interpreter::cmp(const UDSPInstruction)
{
  const s64 first = Acc(0);
  const s64 second = Acc(1);
  SetFlagsSub(first, second, WrapTo40Bits(first - second));
  ZeroWriteBackLog();
}

// CMPAXH $acS, $axR.h
void Interpreter::cmpaxh(const UDSPInstruction opc)
{
  const s64 value = Acc(Bit(opc, 11));
  const s64 operand = SignedAxPart(2 + Bit(opc, 12));
  SetFlagsSub(value, operand, WrapTo40Bits(value - operand));
  ZeroWriteBackLog();
}

// CMPI $acD, #I (I is the middle word)
void Interpreter::cmpi(const UDSPInstruction opc)
{
  const s64 value = Acc(Bit(opc, 8));
  const s64 operand = static_cast<s64>(static_cast<s16>(Regs().FetchInstruction())) * 0x10000;
  SetFlagsSub(value, operand, WrapTo40Bits(value - operand));
}

// CMPIS $acD, #I (8-bit I)
void Interpreter::cmpis(const UDSPInstruction opc)
{
  const s64 value = Acc(Bit(opc, 8));
  const s64 operand = static_cast<s64>(static_cast<s8>(opc & 0xff)) * 0x10000;
  SetFlagsSub(value, operand, WrapTo40Bits(value - operand));
}

// ---------------------------------------------------------------------------
// Logic on the middle word
// ---------------------------------------------------------------------------

// XORR / ANDR / ORR $acD.m, $axS.h
void Interpreter::xorr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, Regs().r.ac[acc].m ^ Regs().r.ax[Bit(opc, 9)].h);
}

void Interpreter::andr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, Regs().r.ac[acc].m & Regs().r.ax[Bit(opc, 9)].h);
}

void Interpreter::orr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, Regs().r.ac[acc].m | Regs().r.ax[Bit(opc, 9)].h);
}

// ANDC / ORC / XORC $acD.m, $ac(1-D).m
void Interpreter::andc(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, Regs().r.ac[acc].m & Regs().r.ac[1 - acc].m);
}

void Interpreter::orc(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, Regs().r.ac[acc].m | Regs().r.ac[1 - acc].m);
}

void Interpreter::xorc(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, Regs().r.ac[acc].m ^ Regs().r.ac[1 - acc].m);
}

// NOT $acD.m
void Interpreter::notc(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  FinishMidLogic(acc, static_cast<u16>(~Regs().r.ac[acc].m));
}

// XORI / ANDI / ORI $acD.m, #I
void Interpreter::xori(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const u16 imm = Regs().FetchInstruction();
  FinishMidLogic(acc, Regs().r.ac[acc].m ^ imm);
}

void Interpreter::andi(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const u16 imm = Regs().FetchInstruction();
  FinishMidLogic(acc, Regs().r.ac[acc].m & imm);
}

void Interpreter::ori(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const u16 imm = Regs().FetchInstruction();
  FinishMidLogic(acc, Regs().r.ac[acc].m | imm);
}

// ---------------------------------------------------------------------------
// Additions
// ---------------------------------------------------------------------------

// ADDR $acD, $(0x18+S)
void Interpreter::addr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), SignedAxPart((opc >> 9) & 3));
}

// ADDAX $acD, $axS
void Interpreter::addax(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), AxWhole(Bit(opc, 9)));
}

// ADD $acD, $ac(1-D)
void Interpreter::add(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), Acc(1 - acc));
}

// ADDP $acD
void Interpreter::addp(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), Product());
}

// ADDAXL $acD, $axS.l (unsigned, into the low word)
void Interpreter::addaxl(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), s64{Regs().r.ax[Bit(opc, 9)].l});
}

// ADDI $acD, #I (I is the middle word)
void Interpreter::addi(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const s64 value = Acc(acc);
  const s64 imm = static_cast<s64>(static_cast<s16>(Regs().FetchInstruction())) * 0x10000;
  StoreAccWithAdd(acc, value, imm);
}

// ADDIS $acD, #I (8-bit I)
void Interpreter::addis(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), static_cast<s64>(static_cast<s8>(opc & 0xff)) * 0x10000);
}

// INCM / INC $acD
void Interpreter::incm(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), 0x10000);
}

void Interpreter::inc(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithAdd(acc, Acc(acc), 1);
}

// ---------------------------------------------------------------------------
// Subtractions
// ---------------------------------------------------------------------------

// SUBR $acD, $(0x18+S)
void Interpreter::subr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, Acc(acc), SignedAxPart((opc >> 9) & 3));
}

// SUBAX $acD, $axS
void Interpreter::subax(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, Acc(acc), AxWhole(Bit(opc, 9)));
}

// SUB $acD, $ac(1-D)
void Interpreter::sub(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, Acc(acc), Acc(1 - acc));
}

// SUBP $acD
void Interpreter::subp(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, Acc(acc), Product());
}

// DECM / DEC $acD
void Interpreter::decm(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, Acc(acc), 0x10000);
}

void Interpreter::dec(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, Acc(acc), 1);
}

// NEG $acD (flags as for 0 - $acD)
void Interpreter::neg(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccWithSub(acc, 0, Acc(acc));
}

// ABS $acD
void Interpreter::abs(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 11);
  const s64 value = Acc(acc);
  StoreAccShifted(acc, value < 0 ? -value : value);
}

// ---------------------------------------------------------------------------
// Moves
// ---------------------------------------------------------------------------

// MOVR $acD, $(0x18+S)
void Interpreter::movr(const UDSPInstruction opc)
{
  StoreAccMoved(Bit(opc, 8), SignedAxPart((opc >> 9) & 3));
}

// MOVAX $acD, $axS
void Interpreter::movax(const UDSPInstruction opc)
{
  StoreAccMoved(Bit(opc, 8), AxWhole(Bit(opc, 9)));
}

// MOV $acD, $ac(1-D)
void Interpreter::mov(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccMoved(acc, Acc(1 - acc));
}

// ---------------------------------------------------------------------------
// Shifts
// ---------------------------------------------------------------------------

// LSL16 / LSR16 $acR: by 16, LSR16 shifting zeros in at bit 39
void Interpreter::lsl16(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccShifted(acc, static_cast<s64>(static_cast<u64>(Acc(acc)) << 16));
}

void Interpreter::lsr16(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccShifted(acc, static_cast<s64>((static_cast<u64>(Acc(acc)) & ACC40_BITS) >> 16));
}

// ASR16 $acR
void Interpreter::asr16(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 11);
  StoreAccShifted(acc, Acc(acc) >> 16);
}

// LSL / ASL $acR, #I: left by the 6-bit I (the same operation)
void Interpreter::lsl(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  StoreAccShifted(acc, static_cast<s64>(static_cast<u64>(Acc(acc)) << (opc & 0x3f)));
}

void Interpreter::asl(const UDSPInstruction opc)
{
  lsl(opc);
}

// LSR / ASR $acR, #-I: the 6-bit field holds the negated right shift count
void Interpreter::lsr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const int count = (64 - (opc & 0x3f)) & 0x3f;
  StoreAccShifted(acc, static_cast<s64>((static_cast<u64>(Acc(acc)) & ACC40_BITS) >> count));
}

void Interpreter::asr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const int count = (64 - (opc & 0x3f)) & 0x3f;
  StoreAccShifted(acc, Acc(acc) >> count);
}

// LSRN / ASRN: $ac0 by $ac1.m, a positive count shifting right
void Interpreter::lsrn(const UDSPInstruction)
{
  ShiftBy7BitAmount(0, Regs().r.ac[1].m, false, false);
}

void Interpreter::asrn(const UDSPInstruction)
{
  ShiftBy7BitAmount(0, Regs().r.ac[1].m, true, false);
}

// LSRNRX / ASRNRX $acD, $axS.h: a positive count shifting left
void Interpreter::lsrnrx(const UDSPInstruction opc)
{
  ShiftBy7BitAmount(Bit(opc, 8), Regs().r.ax[Bit(opc, 9)].h, false, true);
}

void Interpreter::asrnrx(const UDSPInstruction opc)
{
  ShiftBy7BitAmount(Bit(opc, 8), Regs().r.ax[Bit(opc, 9)].h, true, true);
}

// LSRNR / ASRNR $acD, $ac(1-D).m: a positive count shifting left
void Interpreter::lsrnr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  ShiftBy7BitAmount(acc, Regs().r.ac[1 - acc].m, false, true);
}

void Interpreter::asrnr(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  ShiftBy7BitAmount(acc, Regs().r.ac[1 - acc].m, true, true);
}
}  // namespace DSP::Interpreter
