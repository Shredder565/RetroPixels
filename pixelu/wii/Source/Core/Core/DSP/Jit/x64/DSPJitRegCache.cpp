// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/Jit/x64/DSPJitRegCache.h"

#include "Common/Assert.h"
#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
// Distance in bytes from the start of the SDSP to one of its members.
int DistanceTo(const SDSP& state, const void* member)
{
  const auto* base = reinterpret_cast<const u8*>(&state);
  return static_cast<int>(static_cast<const u8*>(member) - base);
}

// What a plain register store keeps of a 16-bit value (see StoreWord).
u16 NormalizeWord(int reg, u16 value)
{
  switch (reg)
  {
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    return static_cast<u16>(static_cast<s16>(static_cast<s8>(value & 0xff)));
  case DSP_REG_CR:
  case DSP_REG_PRODH:
    return static_cast<u16>(value & 0xff);
  case DSP_REG_SR:
    return static_cast<u16>(value & ~SR_100);
  default:
    return value;
  }
}
}  // namespace

RegisterFile::RegisterFile(XEmitter& code, const SDSP& state) : m_code(code)
{
  m_ar = DistanceTo(state, &state.r.ar[0]);
  m_ix = DistanceTo(state, &state.r.ix[0]);
  m_wr = DistanceTo(state, &state.r.wr[0]);
  m_st = DistanceTo(state, &state.r.st[0]);
  m_cr = DistanceTo(state, &state.r.cr);
  m_sr = DistanceTo(state, &state.r.sr);
  m_prod = DistanceTo(state, &state.r.prod);
  m_ax = DistanceTo(state, &state.r.ax[0]);
  m_ax_stride = DistanceTo(state, &state.r.ax[1]) - m_ax;
  m_ac = DistanceTo(state, &state.r.ac[0]);
  m_ac_stride = DistanceTo(state, &state.r.ac[1]) - m_ac;
  m_pc = DistanceTo(state, &state.pc);
  m_control = DistanceTo(state, &state.control_reg);
  m_exceptions = DistanceTo(state, &state.exceptions);
  m_ext_irq = DistanceTo(state, &state.external_interrupt_waiting);
  m_stack_depth = DistanceTo(state, &state.reg_stack_ptrs[0]);
  m_stacks = DistanceTo(state, &state.reg_stacks[0][0]);
  m_stack_stride = DistanceTo(state, &state.reg_stacks[1][0]) - m_stacks;

  static_assert(sizeof(state.external_interrupt_waiting) == 1);
}

int RegisterFile::AccOffset(int acc) const
{
  return m_ac + acc * m_ac_stride;
}

OpArg RegisterFile::Word(int reg) const
{
  const int index = reg & 3;
  switch (reg)
  {
  case DSP_REG_AR0:
  case DSP_REG_AR1:
  case DSP_REG_AR2:
  case DSP_REG_AR3:
    return MDisp(STATE_REG, m_ar + 2 * index);
  case DSP_REG_IX0:
  case DSP_REG_IX1:
  case DSP_REG_IX2:
  case DSP_REG_IX3:
    return MDisp(STATE_REG, m_ix + 2 * index);
  case DSP_REG_WR0:
  case DSP_REG_WR1:
  case DSP_REG_WR2:
  case DSP_REG_WR3:
    return MDisp(STATE_REG, m_wr + 2 * index);
  case DSP_REG_ST0:
  case DSP_REG_ST1:
  case DSP_REG_ST2:
  case DSP_REG_ST3:
    return MDisp(STATE_REG, m_st + 2 * index);
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    return AccHigh(reg - DSP_REG_ACH0);
  case DSP_REG_CR:
    return MDisp(STATE_REG, m_cr);
  case DSP_REG_SR:
    return MDisp(STATE_REG, m_sr);
  case DSP_REG_PRODL:
  case DSP_REG_PRODM:
  case DSP_REG_PRODH:
  case DSP_REG_PRODM2:
    return MDisp(STATE_REG, m_prod + 2 * (reg - DSP_REG_PRODL));
  case DSP_REG_AXL0:
  case DSP_REG_AXL1:
    return MDisp(STATE_REG, m_ax + (reg - DSP_REG_AXL0) * m_ax_stride);
  case DSP_REG_AXH0:
  case DSP_REG_AXH1:
    return MDisp(STATE_REG, m_ax + (reg - DSP_REG_AXH0) * m_ax_stride + 2);
  case DSP_REG_ACL0:
  case DSP_REG_ACL1:
    return MDisp(STATE_REG, AccOffset(reg - DSP_REG_ACL0));
  case DSP_REG_ACM0:
  case DSP_REG_ACM1:
    return MDisp(STATE_REG, AccOffset(reg - DSP_REG_ACM0) + 2);
  default:
    ASSERT_MSG(DSPLLE, false, "no DSP register {}", reg);
    return MDisp(STATE_REG, m_sr);
  }
}

OpArg RegisterFile::Acc(int acc) const
{
  return MDisp(STATE_REG, AccOffset(acc));
}

OpArg RegisterFile::AccHigh(int acc) const
{
  return MDisp(STATE_REG, AccOffset(acc) + 4);
}

OpArg RegisterFile::Ax(int ax) const
{
  return MDisp(STATE_REG, m_ax + ax * m_ax_stride);
}

OpArg RegisterFile::Product() const
{
  return MDisp(STATE_REG, m_prod);
}

OpArg RegisterFile::Pc() const
{
  return MDisp(STATE_REG, m_pc);
}

OpArg RegisterFile::ControlReg() const
{
  return MDisp(STATE_REG, m_control);
}

OpArg RegisterFile::Exceptions() const
{
  return MDisp(STATE_REG, m_exceptions);
}

OpArg RegisterFile::ExternalInterruptWaiting() const
{
  return MDisp(STATE_REG, m_ext_irq);
}

OpArg RegisterFile::StackDepth(int stack) const
{
  return MDisp(STATE_REG, m_stack_depth + stack);
}

OpArg RegisterFile::StackTop(int stack) const
{
  return MDisp(STATE_REG, m_st + 2 * stack);
}

OpArg RegisterFile::StackEntry(int stack, X64Reg index) const
{
  return MComplex(STATE_REG, index, SCALE_2, m_stacks + stack * m_stack_stride);
}

void RegisterFile::LoadWord(int reg, X64Reg dst, RegisterExtension ext)
{
  switch (ext)
  {
  case RegisterExtension::Sign:
    m_code.MOVSX(64, 16, dst, Word(reg));
    break;
  case RegisterExtension::Zero:
    m_code.MOVZX(64, 16, dst, Word(reg));
    break;
  case RegisterExtension::None:
    m_code.MOV(16, R(dst), Word(reg));
    break;
  }
}

void RegisterFile::StoreWord(int reg, X64Reg src)
{
  ASSERT(src != HELPER_D);
  ASSERT(reg < DSP_REG_ST0 || reg > DSP_REG_ST3);
  switch (reg)
  {
  case DSP_REG_ACH0:
  case DSP_REG_ACH1:
    // The whole 32-bit field, so the 64-bit accumulator stays sign extended.
    m_code.MOVSX(32, 8, HELPER_D, R(src));
    m_code.MOV(32, AccHigh(reg - DSP_REG_ACH0), R(HELPER_D));
    break;
  case DSP_REG_CR:
  case DSP_REG_PRODH:
    m_code.MOVZX(32, 8, HELPER_D, R(src));
    m_code.MOV(16, Word(reg), R(HELPER_D));
    break;
  case DSP_REG_SR:
    m_code.MOV(32, R(HELPER_D), R(src));
    m_code.AND(32, R(HELPER_D), Imm32(static_cast<u16>(~SR_100)));
    m_code.MOV(16, Word(reg), R(HELPER_D));
    break;
  default:
    m_code.MOV(16, Word(reg), R(src));
    break;
  }
}

void RegisterFile::StoreWordImm(int reg, u16 value)
{
  ASSERT(reg < DSP_REG_ST0 || reg > DSP_REG_ST3);
  const u16 kept = NormalizeWord(reg, value);
  if (reg == DSP_REG_ACH0 || reg == DSP_REG_ACH1)
  {
    const u32 high = static_cast<u32>(static_cast<s32>(static_cast<s16>(kept)));
    m_code.MOV(32, AccHigh(reg - DSP_REG_ACH0), Imm32(high));
    return;
  }
  m_code.MOV(16, Word(reg), Imm16(kept));
}

void RegisterFile::LoadAcc(int acc, X64Reg dst)
{
  m_code.MOV(64, R(dst), Acc(acc));
  m_code.SHL(64, R(dst), Imm8(24));
  m_code.SAR(64, R(dst), Imm8(24));
}

void RegisterFile::StoreAcc(int acc, X64Reg src)
{
  m_code.MOV(64, R(HELPER_D), R(src));
  m_code.SHL(64, R(HELPER_D), Imm8(24));
  m_code.SAR(64, R(HELPER_D), Imm8(24));
  m_code.MOV(64, Acc(acc), R(HELPER_D));
}

void RegisterFile::LoadAx(int ax, X64Reg dst)
{
  m_code.MOVSX(64, 32, dst, Ax(ax));
}

}  // namespace DSP::JIT::x64
