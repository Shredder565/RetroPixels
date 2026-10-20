// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Integer arithmetic, logical, rotate, shift, compare and trap instructions.

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <bit>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
// CR0 from a 32-bit result, in the emulator's 64-bit CR field format: the sign-extended result
// gives LT/GT/EQ directly. A zero result gets bit 63 so that setting SO cannot make it read as
// "greater than".
void SetCR0(PowerPC::PowerPCState& ppc_state, u32 value)
{
  u64 field = static_cast<u64>(static_cast<s64>(static_cast<s32>(value)));
  if (value == 0)
    field = u64{1} << 63;
  constexpr u64 so_bit = u64{1} << PowerPC::CR_EMU_SO_BIT;
  field = (field & ~so_bit) | (static_cast<u64>(ppc_state.GetXER_SO()) << PowerPC::CR_EMU_SO_BIT);
  ppc_state.cr.fields[0] = field;
}

void SetRA(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, u32 value)
{
  ppc_state.gpr[inst.RA] = value;
  if (inst.Rc)
    SetCR0(ppc_state, value);
}

template <typename T>
void CompareIntoField(PowerPC::PowerPCState& ppc_state, u32 crfd, T a, T b)
{
  u32 flags = a < b ? PowerPC::CR_LT : (a > b ? PowerPC::CR_GT : PowerPC::CR_EQ);
  if (ppc_state.GetXER_SO())
    flags |= PowerPC::CR_SO;
  ppc_state.cr.SetField(crfd, flags);
}

// Signed overflow of x + y = sum: both operands have the same sign and the sum the other one.
bool AddOverflows(u32 x, u32 y, u32 sum)
{
  return ((x ^ sum) & (y ^ sum) & 0x80000000) != 0;
}

// The XO-form add/subtract instructions all compute x + y + carry_in and differ only in their
// operands and in whether XER[CA] is written.
void ExtendedAdd(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst, u32 x, u32 y,
                 u32 carry_in, bool write_ca)
{
  const u64 wide = u64{x} + y + carry_in;
  const u32 result = static_cast<u32>(wide);
  ppc_state.gpr[inst.RD] = result;
  if (write_ca)
    ppc_state.SetCarry(static_cast<u32>(wide >> 32));
  if (inst.OE)
    ppc_state.SetXER_OV(AddOverflows(x, y, result));
  if (inst.Rc)
    SetCR0(ppc_state, result);
}

// tw/twi: TO bits are lt, gt, eq (signed) and lt, gt (unsigned).
bool TrapConditionMet(u32 to, u32 a, u32 b)
{
  const s32 sa = static_cast<s32>(a);
  const s32 sb = static_cast<s32>(b);
  return ((to & 0x10) && sa < sb) || ((to & 0x08) && sa > sb) || ((to & 0x04) && a == b) ||
         ((to & 0x02) && a < b) || ((to & 0x01) && a > b);
}
}  // namespace

u32 Interpreter::Helper_Carry(u32 value1, u32 value2)
{
  return (u64{value1} + value2) >> 32;
}

static void Trap(Interpreter& interpreter, PowerPC::PowerPCState& ppc_state, Core::System& system,
                 bool& end_block)
{
  GenerateProgramException(ppc_state, ProgramExceptionCause::Trap);
  system.GetPowerPC().CheckExceptions();
  end_block = true;
}

// ---- D-form arithmetic and logic

void Interpreter::addi(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 base = inst.RA ? ppc_state.gpr[inst.RA] : 0;
  ppc_state.gpr[inst.RD] = base + static_cast<u32>(inst.SIMM_16);
}

void Interpreter::addis(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 base = inst.RA ? ppc_state.gpr[inst.RA] : 0;
  ppc_state.gpr[inst.RD] = base + (static_cast<u32>(inst.SIMM_16) << 16);
}

void Interpreter::addic(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 a = ppc_state.gpr[inst.RA];
  const u32 imm = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  ppc_state.gpr[inst.RD] = a + imm;
  ppc_state.SetCarry(Helper_Carry(a, imm));
}

void Interpreter::addic_rc(Interpreter& interpreter, UGeckoInstruction inst)
{
  addic(interpreter, inst);
  SetCR0(interpreter.m_ppc_state, interpreter.m_ppc_state.gpr[inst.RD]);
}

// frD = SIMM - rA; CA is the carry of ~rA + SIMM + 1.
void Interpreter::subfic(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 a = ppc_state.gpr[inst.RA];
  const u32 imm = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  const u32 not_a = ~a;
  const u64 wide = u64{not_a} + imm + 1;
  ppc_state.gpr[inst.RD] = static_cast<u32>(wide);
  ppc_state.SetCarry(static_cast<u32>(wide >> 32));
}

void Interpreter::mulli(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const s64 product = s64{static_cast<s32>(ppc_state.gpr[inst.RA])} * s64{inst.SIMM_16};
  ppc_state.gpr[inst.RD] = static_cast<u32>(product);
}

void Interpreter::andi_rc(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 result = ppc_state.gpr[inst.RS] & inst.UIMM;
  ppc_state.gpr[inst.RA] = result;
  SetCR0(ppc_state, result);
}

void Interpreter::andis_rc(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 result = ppc_state.gpr[inst.RS] & (u32{inst.UIMM} << 16);
  ppc_state.gpr[inst.RA] = result;
  SetCR0(ppc_state, result);
}

void Interpreter::ori(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.gpr[inst.RA] = ppc_state.gpr[inst.RS] | inst.UIMM;
}

void Interpreter::oris(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.gpr[inst.RA] = ppc_state.gpr[inst.RS] | (u32{inst.UIMM} << 16);
}

void Interpreter::xori(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.gpr[inst.RA] = ppc_state.gpr[inst.RS] ^ inst.UIMM;
}

void Interpreter::xoris(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ppc_state.gpr[inst.RA] = ppc_state.gpr[inst.RS] ^ (u32{inst.UIMM} << 16);
}

// ---- compares and traps

void Interpreter::cmpi(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  CompareIntoField<s32>(ppc_state, inst.CRFD, static_cast<s32>(ppc_state.gpr[inst.RA]),
                        inst.SIMM_16);
}

void Interpreter::cmpli(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  CompareIntoField<u32>(ppc_state, inst.CRFD, ppc_state.gpr[inst.RA], inst.UIMM);
}

void Interpreter::cmp(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  CompareIntoField<s32>(ppc_state, inst.CRFD, static_cast<s32>(ppc_state.gpr[inst.RA]),
                        static_cast<s32>(ppc_state.gpr[inst.RB]));
}

void Interpreter::cmpl(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  CompareIntoField<u32>(ppc_state, inst.CRFD, ppc_state.gpr[inst.RA], ppc_state.gpr[inst.RB]);
}

void Interpreter::twi(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 a = ppc_state.gpr[inst.RA];
  const u32 b = static_cast<u32>(static_cast<s32>(inst.SIMM_16));
  DEBUG_LOG_FMT(POWERPC, "twi: rA={:#x} SIMM={:#x} TO={:#x}", a, b, u32{inst.TO});
  if (TrapConditionMet(inst.TO, a, b))
    Trap(interpreter, ppc_state, interpreter.m_system, interpreter.m_end_block);
}

void Interpreter::tw(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 a = ppc_state.gpr[inst.RA];
  const u32 b = ppc_state.gpr[inst.RB];
  DEBUG_LOG_FMT(POWERPC, "tw: rA={:#x} rB={:#x} TO={:#x}", a, b, u32{inst.TO});
  if (TrapConditionMet(inst.TO, a, b))
    Trap(interpreter, ppc_state, interpreter.m_system, interpreter.m_end_block);
}

// ---- rotates and shifts

void Interpreter::rlwimix(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 mask = MakeRotationMask(inst.MB, inst.ME);
  const u32 inserted = std::rotl(ppc_state.gpr[inst.RS], inst.SH) & mask;
  SetRA(ppc_state, inst, (ppc_state.gpr[inst.RA] & ~mask) | inserted);
}

void Interpreter::rlwinmx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  SetRA(ppc_state, inst,
        std::rotl(ppc_state.gpr[inst.RS], inst.SH) & MakeRotationMask(inst.MB, inst.ME));
}

void Interpreter::rlwnmx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const int amount = static_cast<int>(ppc_state.gpr[inst.RB] & 31);
  SetRA(ppc_state, inst,
        std::rotl(ppc_state.gpr[inst.RS], amount) & MakeRotationMask(inst.MB, inst.ME));
}

// Shift amounts are six bits wide: 32..63 shift everything out.
void Interpreter::slwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 amount = ppc_state.gpr[inst.RB] & 63;
  SetRA(ppc_state, inst, amount >= 32 ? 0 : ppc_state.gpr[inst.RS] << amount);
}

void Interpreter::srwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const u32 amount = ppc_state.gpr[inst.RB] & 63;
  SetRA(ppc_state, inst, amount >= 32 ? 0 : ppc_state.gpr[inst.RS] >> amount);
}

// Algebraic right shifts set CA when a negative value loses one bits.
static void ShiftRightAlgebraic(PowerPC::PowerPCState& ppc_state, UGeckoInstruction inst,
                                u32 amount)
{
  const s32 value = static_cast<s32>(ppc_state.gpr[inst.RS]);
  u32 result;
  bool carry;
  if (amount >= 32)
  {
    result = value < 0 ? 0xFFFFFFFF : 0;
    carry = value < 0;
  }
  else
  {
    result = static_cast<u32>(value >> amount);
    const u32 lost = amount == 0 ? 0 : (static_cast<u32>(value) << (32 - amount));
    carry = value < 0 && lost != 0;
  }
  ppc_state.SetCarry(carry);
  SetRA(ppc_state, inst, result);
}

void Interpreter::srawx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  ShiftRightAlgebraic(ppc_state, inst, ppc_state.gpr[inst.RB] & 63);
}

void Interpreter::srawix(Interpreter& interpreter, UGeckoInstruction inst)
{
  ShiftRightAlgebraic(interpreter.m_ppc_state, inst, inst.SH);
}

// ---- X-form logic

void Interpreter::andx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, s.gpr[inst.RS] & s.gpr[inst.RB]);
}

void Interpreter::andcx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, s.gpr[inst.RS] & ~s.gpr[inst.RB]);
}

void Interpreter::orx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, s.gpr[inst.RS] | s.gpr[inst.RB]);
}

void Interpreter::orcx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, s.gpr[inst.RS] | ~s.gpr[inst.RB]);
}

void Interpreter::xorx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, s.gpr[inst.RS] ^ s.gpr[inst.RB]);
}

void Interpreter::nandx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, ~(s.gpr[inst.RS] & s.gpr[inst.RB]));
}

void Interpreter::norx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, ~(s.gpr[inst.RS] | s.gpr[inst.RB]));
}

void Interpreter::eqvx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, ~(s.gpr[inst.RS] ^ s.gpr[inst.RB]));
}

void Interpreter::cntlzwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, static_cast<u32>(std::countl_zero(s.gpr[inst.RS])));
}

void Interpreter::extsbx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, static_cast<u32>(static_cast<s32>(static_cast<s8>(s.gpr[inst.RS]))));
}

void Interpreter::extshx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  SetRA(s, inst, static_cast<u32>(static_cast<s32>(static_cast<s16>(s.gpr[inst.RS]))));
}

// ---- XO-form arithmetic

void Interpreter::addx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, s.gpr[inst.RA], s.gpr[inst.RB], 0, false);
}

void Interpreter::addcx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, s.gpr[inst.RA], s.gpr[inst.RB], 0, true);
}

void Interpreter::addex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, s.gpr[inst.RA], s.gpr[inst.RB], s.GetCarry(), true);
}

void Interpreter::addmex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, s.gpr[inst.RA], 0xFFFFFFFF, s.GetCarry(), true);
}

void Interpreter::addzex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, s.gpr[inst.RA], 0, s.GetCarry(), true);
}

void Interpreter::subfx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, ~s.gpr[inst.RA], s.gpr[inst.RB], 1, false);
}

void Interpreter::subfcx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, ~s.gpr[inst.RA], s.gpr[inst.RB], 1, true);
}

void Interpreter::subfex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, ~s.gpr[inst.RA], s.gpr[inst.RB], s.GetCarry(), true);
}

void Interpreter::subfmex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, ~s.gpr[inst.RA], 0xFFFFFFFF, s.GetCarry(), true);
}

void Interpreter::subfzex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  ExtendedAdd(s, inst, ~s.gpr[inst.RA], 0, s.GetCarry(), true);
}

// rD = -rA. OV only for the most negative number.
void Interpreter::negx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 a = s.gpr[inst.RA];
  const u32 result = 0u - a;
  s.gpr[inst.RD] = result;
  if (inst.OE)
    s.SetXER_OV(a == 0x80000000);
  if (inst.Rc)
    SetCR0(s, result);
}

void Interpreter::mullwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const s64 product =
      s64{static_cast<s32>(s.gpr[inst.RA])} * s64{static_cast<s32>(s.gpr[inst.RB])};
  const u32 result = static_cast<u32>(product);
  s.gpr[inst.RD] = result;
  if (inst.OE)
    s.SetXER_OV(product != s64{static_cast<s32>(result)});
  if (inst.Rc)
    SetCR0(s, result);
}

void Interpreter::mulhwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const s64 product =
      s64{static_cast<s32>(s.gpr[inst.RA])} * s64{static_cast<s32>(s.gpr[inst.RB])};
  const u32 result = static_cast<u32>(static_cast<u64>(product) >> 32);
  s.gpr[inst.RD] = result;
  if (inst.Rc)
    SetCR0(s, result);
}

void Interpreter::mulhwux(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u64 product = u64{s.gpr[inst.RA]} * u64{s.gpr[inst.RB]};
  const u32 result = static_cast<u32>(product >> 32);
  s.gpr[inst.RD] = result;
  if (inst.Rc)
    SetCR0(s, result);
}

// Division by zero and 0x80000000 / -1 are undefined on PowerPC; the Gekko returns -1 for a
// negative dividend and 0 otherwise (unsigned: always 0), and sets OV if requested.
void Interpreter::divwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const s32 a = static_cast<s32>(s.gpr[inst.RA]);
  const s32 b = static_cast<s32>(s.gpr[inst.RB]);
  const bool undefined = b == 0 || (a == static_cast<s32>(0x80000000) && b == -1);
  const u32 result = undefined ? (a < 0 ? 0xFFFFFFFF : 0) : static_cast<u32>(a / b);
  s.gpr[inst.RD] = result;
  if (inst.OE)
    s.SetXER_OV(undefined);
  if (inst.Rc)
    SetCR0(s, result);
}

void Interpreter::divwux(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& s = interpreter.m_ppc_state;
  const u32 a = s.gpr[inst.RA];
  const u32 b = s.gpr[inst.RB];
  const u32 result = b == 0 ? 0 : a / b;
  s.gpr[inst.RD] = result;
  if (inst.OE)
    s.SetXER_OV(b == 0);
  if (inst.Rc)
    SetCR0(s, result);
}
