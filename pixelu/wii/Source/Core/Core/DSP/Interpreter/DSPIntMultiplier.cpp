// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The multiplier: 16x16 products into $prod, optionally moving or adding the
// previous product into an accumulator in the same instruction.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/Interpreter/DSPIntCCUtil.h"

namespace DSP::Interpreter
{
namespace
{
constexpr int Bit(UDSPInstruction opc, int position)
{
  return (opc >> position) & 1;
}
}  // namespace

// a * b as the multiplier computes it. Operands are signed unless SET15 is in
// effect and the instruction allows an unsigned operand; unless M0 is in
// effect the product is doubled (fractional multiply).
s64 Interpreter::MultiplyRaw(u16 a, u16 b, Signedness mode) const
{
  s64 product;
  const bool unsigned_allowed = IsSRFlagSet(SR_MUL_UNSIGNED);
  if (unsigned_allowed && mode == Signedness::BothUnsigned)
    product = static_cast<s64>(u32{a} * u32{b});
  else if (unsigned_allowed && mode == Signedness::FirstUnsigned)
    product = s64{a} * s64{static_cast<s16>(b)};
  else
    product = s64{static_cast<s16>(a)} * s64{static_cast<s16>(b)};

  if (!IsSRFlagSet(SR_MUL_MODIFY))
    product *= 2;
  return product;
}

// The MULX family multiplies a half of $ax0 by a half of $ax1. With SET15, a low
// half counts as unsigned: two low halves give an unsigned product, one low and
// one high half a mixed one (the low half being the unsigned operand).
s64 Interpreter::MultiplyAxHalves(bool ax0_high, bool ax1_high) const
{
  const u16 first = AxHalf(0, ax0_high);
  const u16 second = AxHalf(1, ax1_high);

  if (ax0_high == ax1_high)
    return MultiplyRaw(first, second, ax0_high ? Signedness::Signed : Signedness::BothUnsigned);
  if (!ax0_high)
    return MultiplyRaw(first, second, Signedness::FirstUnsigned);
  return MultiplyRaw(second, first, Signedness::FirstUnsigned);
}

// The ...AC, ...MV and ...MVZ forms: the accumulator gets a value derived from
// the old product while the new product is stored.
void Interpreter::MultiplyAndMove(int acc, ProductMove move, s64 new_product)
{
  s64 acc_value;
  switch (move)
  {
  case ProductMove::AddToAcc:
    acc_value = Acc(acc) + Product();
    break;
  case ProductMove::MoveRounded:
    acc_value = ProductRounded();
    break;
  default:
    acc_value = Product();
    break;
  }

  ZeroWriteBackLog();
  SetProduct(new_product);
  SetAcc(acc, acc_value);
  SetFlags40(Acc(acc));
}

// ---------------------------------------------------------------------------
// Product register
// ---------------------------------------------------------------------------

// CLRP: the hardware's "zero" product, as a non-zero pair of partial products.
void Interpreter::clrp(const UDSPInstruction)
{
  ZeroWriteBackLog();
  auto& prod = Regs().r.prod;
  prod.l = 0x0000;
  prod.m = 0xfff0;
  prod.h = 0x00ff;
  prod.m2 = 0x0010;
}

// TSTPROD
void Interpreter::tstprod(const UDSPInstruction)
{
  SetFlags40(Product());
  ZeroWriteBackLog();
}

// MOVP / MOVNP / MOVPZ $acD: the product, its negation, or it rounded
void Interpreter::movp(const UDSPInstruction opc)
{
  StoreAccMoved(Bit(opc, 8), Product());
}

void Interpreter::movnp(const UDSPInstruction opc)
{
  StoreAccMoved(Bit(opc, 8), -Product());
}

void Interpreter::movpz(const UDSPInstruction opc)
{
  StoreAccMoved(Bit(opc, 8), ProductRounded());
}

// ADDPAXZ $acD, $axS: rounded product plus $axS with its low word cleared.
// The overflow flag is not computed.
void Interpreter::addpaxz(const UDSPInstruction opc)
{
  const int acc = Bit(opc, 8);
  const s64 product = Product();
  const s64 ax_high = s64{AxWhole(Bit(opc, 9))} & ~s64{0xffff};

  ZeroWriteBackLog();
  SetAcc(acc, ProductRounded() + ax_high);
  const s64 stored = Acc(acc);
  SetFlags40(stored, CarriedOnAdd(static_cast<u64>(product), static_cast<u64>(stored)), false);
}

// ---------------------------------------------------------------------------
// Plain multiplies
// ---------------------------------------------------------------------------

// MULAXH: $ax0.h squared
void Interpreter::mulaxh(const UDSPInstruction)
{
  const u16 value = Regs().r.ax[0].h;
  const s64 product = MultiplyRaw(value, value, Signedness::Signed);
  ZeroWriteBackLog();
  SetProduct(product);
}

// MUL $axS.l, $axS.h
void Interpreter::mul(const UDSPInstruction opc)
{
  const auto& ax = Regs().r.ax[Bit(opc, 11)];
  const s64 product = MultiplyRaw(ax.h, ax.l, Signedness::Signed);
  ZeroWriteBackLog();
  SetProduct(product);
}

// MULAC / MULMV / MULMVZ $axS.l, $axS.h, $acR
void Interpreter::mulac(const UDSPInstruction opc)
{
  const auto& ax = Regs().r.ax[Bit(opc, 11)];
  MultiplyAndMove(Bit(opc, 8), ProductMove::AddToAcc, MultiplyRaw(ax.l, ax.h, Signedness::Signed));
}

void Interpreter::mulmv(const UDSPInstruction opc)
{
  const auto& ax = Regs().r.ax[Bit(opc, 11)];
  MultiplyAndMove(Bit(opc, 8), ProductMove::Move, MultiplyRaw(ax.l, ax.h, Signedness::Signed));
}

void Interpreter::mulmvz(const UDSPInstruction opc)
{
  const auto& ax = Regs().r.ax[Bit(opc, 11)];
  MultiplyAndMove(Bit(opc, 8), ProductMove::MoveRounded,
                  MultiplyRaw(ax.l, ax.h, Signedness::Signed));
}

// ---------------------------------------------------------------------------
// $ax0 half times $ax1 half (bit 12 picks the $ax0 half, bit 11 the $ax1 half)
// ---------------------------------------------------------------------------

void Interpreter::mulx(const UDSPInstruction opc)
{
  const s64 product = MultiplyAxHalves(Bit(opc, 12) != 0, Bit(opc, 11) != 0);
  ZeroWriteBackLog();
  SetProduct(product);
}

void Interpreter::mulxac(const UDSPInstruction opc)
{
  MultiplyAndMove(Bit(opc, 8), ProductMove::AddToAcc,
                  MultiplyAxHalves(Bit(opc, 12) != 0, Bit(opc, 11) != 0));
}

void Interpreter::mulxmv(const UDSPInstruction opc)
{
  MultiplyAndMove(Bit(opc, 8), ProductMove::Move,
                  MultiplyAxHalves(Bit(opc, 12) != 0, Bit(opc, 11) != 0));
}

void Interpreter::mulxmvz(const UDSPInstruction opc)
{
  MultiplyAndMove(Bit(opc, 8), ProductMove::MoveRounded,
                  MultiplyAxHalves(Bit(opc, 12) != 0, Bit(opc, 11) != 0));
}

// ---------------------------------------------------------------------------
// $acS.m times $axT.h (bit 12 is S, bit 11 is T)
// ---------------------------------------------------------------------------

void Interpreter::mulc(const UDSPInstruction opc)
{
  const s64 product = MultiplyRaw(Regs().r.ac[Bit(opc, 12)].m, Regs().r.ax[Bit(opc, 11)].h,
                                  Signedness::Signed);
  ZeroWriteBackLog();
  SetProduct(product);
}

void Interpreter::mulcac(const UDSPInstruction opc)
{
  MultiplyAndMove(Bit(opc, 8), ProductMove::AddToAcc,
                  MultiplyRaw(Regs().r.ac[Bit(opc, 12)].m, Regs().r.ax[Bit(opc, 11)].h,
                              Signedness::Signed));
}

void Interpreter::mulcmv(const UDSPInstruction opc)
{
  MultiplyAndMove(Bit(opc, 8), ProductMove::Move,
                  MultiplyRaw(Regs().r.ac[Bit(opc, 12)].m, Regs().r.ax[Bit(opc, 11)].h,
                              Signedness::Signed));
}

void Interpreter::mulcmvz(const UDSPInstruction opc)
{
  MultiplyAndMove(Bit(opc, 8), ProductMove::MoveRounded,
                  MultiplyRaw(Regs().r.ac[Bit(opc, 12)].m, Regs().r.ax[Bit(opc, 11)].h,
                              Signedness::Signed));
}

// ---------------------------------------------------------------------------
// Multiply-accumulate into $prod (always signed)
// ---------------------------------------------------------------------------

// MADDX / MSUBX: bit 9 picks the $ax0 half, bit 8 the $ax1 half
void Interpreter::maddx(const UDSPInstruction opc)
{
  const s64 term = MultiplyRaw(AxHalf(0, Bit(opc, 9) != 0), AxHalf(1, Bit(opc, 8) != 0),
                               Signedness::Signed);
  const s64 product = Product() + term;
  ZeroWriteBackLog();
  SetProduct(product);
}

void Interpreter::msubx(const UDSPInstruction opc)
{
  const s64 term = MultiplyRaw(AxHalf(0, Bit(opc, 9) != 0), AxHalf(1, Bit(opc, 8) != 0),
                               Signedness::Signed);
  const s64 product = Product() - term;
  ZeroWriteBackLog();
  SetProduct(product);
}

// MADDC / MSUBC $acS.m, $axT.h: bit 9 is S, bit 8 is T
void Interpreter::maddc(const UDSPInstruction opc)
{
  const s64 term = MultiplyRaw(Regs().r.ac[Bit(opc, 9)].m, Regs().r.ax[Bit(opc, 8)].h,
                               Signedness::Signed);
  const s64 product = Product() + term;
  ZeroWriteBackLog();
  SetProduct(product);
}

void Interpreter::msubc(const UDSPInstruction opc)
{
  const s64 term = MultiplyRaw(Regs().r.ac[Bit(opc, 9)].m, Regs().r.ax[Bit(opc, 8)].h,
                               Signedness::Signed);
  const s64 product = Product() - term;
  ZeroWriteBackLog();
  SetProduct(product);
}

// MADD / MSUB $axS.l, $axS.h
void Interpreter::madd(const UDSPInstruction opc)
{
  const auto& ax = Regs().r.ax[Bit(opc, 8)];
  const s64 product = Product() + MultiplyRaw(ax.l, ax.h, Signedness::Signed);
  ZeroWriteBackLog();
  SetProduct(product);
}

void Interpreter::msub(const UDSPInstruction opc)
{
  const auto& ax = Regs().r.ax[Bit(opc, 8)];
  const s64 product = Product() - MultiplyRaw(ax.l, ax.h, Signedness::Signed);
  ZeroWriteBackLog();
  SetProduct(product);
}
}  // namespace DSP::Interpreter
