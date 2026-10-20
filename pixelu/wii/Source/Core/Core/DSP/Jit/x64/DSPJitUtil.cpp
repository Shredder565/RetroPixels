// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Building blocks shared by the instruction translators: register operands
// with their side effects, the four hardware stacks, memory accesses, the
// address register arithmetic and the product register.

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/x64ABI.h"

#include "Core/DSP/DSPCore.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
u16 ReadIfxThunk(SDSP& state, u16 address)
{
  return state.ReadIFX(address);
}

void WriteIfxThunk(SDSP& state, u16 address, u16 value)
{
  state.WriteIFX(address, value);
}

bool IsStackRegister(int reg)
{
  return reg >= DSP_REG_ST0 && reg <= DSP_REG_ST3;
}

bool IsHelper(X64Reg reg)
{
  return reg == HELPER_A || reg == HELPER_B || reg == HELPER_C || reg == HELPER_D;
}
}  // namespace

// ---------------------------------------------------------------------------
// Register operands

void DSPEmitter::ReadOperand(int reg, X64Reg dst, RegisterExtension ext)
{
  ASSERT(!IsHelper(dst));

  if (IsStackRegister(reg))
  {
    PopStack(reg - DSP_REG_ST0, dst);
    if (ext == RegisterExtension::Sign)
      MOVSX(64, 16, dst, R(dst));
    return;
  }

  if (reg != DSP_REG_ACM0 && reg != DSP_REG_ACM1)
  {
    m_regs.LoadWord(reg, dst, ext == RegisterExtension::Sign ? ext : RegisterExtension::Zero);
    return;
  }

  // In 40-bit mode a middle word whose accumulator does not fit in 32 bits
  // reads as the largest 16-bit value of the accumulator's sign.
  TEST(16, m_regs.Word(DSP_REG_SR), Imm16(SR_40_MODE_BIT));
  FixupBranch plain = J_CC(CC_Z, Jump::Near);
  m_regs.LoadAcc(reg - DSP_REG_ACM0, dst);
  MOVSX(64, 32, HELPER_A, R(dst));
  CMP(64, R(HELPER_A), R(dst));
  FixupBranch in_range = J_CC(CC_E, Jump::Near);
  SAR(64, R(dst), Imm8(63));         // -1 or 0
  XOR(32, R(dst), Imm32(0x7fff));    // 0x...8000 or 0x7fff
  FixupBranch saturated = J(Jump::Near);
  SetJumpTarget(plain);
  SetJumpTarget(in_range);
  MOV(16, R(dst), m_regs.Word(reg));
  SetJumpTarget(saturated);
  if (ext == RegisterExtension::Sign)
    MOVSX(64, 16, dst, R(dst));
  else
    MOVZX(32, 16, dst, R(dst));
}

void DSPEmitter::WriteOperand(int reg, X64Reg src)
{
  ASSERT(!IsHelper(src));
  if (IsStackRegister(reg))
    PushStack(reg - DSP_REG_ST0, src);
  else
    m_regs.StoreWord(reg, src);
}

void DSPEmitter::WriteOperandImm(int reg, u16 value)
{
  if (IsStackRegister(reg))
    PushStackImm(reg - DSP_REG_ST0, value);
  else
    m_regs.StoreWordImm(reg, value);
}

void DSPEmitter::ExtendMid(int reg)
{
  if (reg != DSP_REG_ACM0 && reg != DSP_REG_ACM1)
    return;
  const int acc = reg - DSP_REG_ACM0;
  TEST(16, m_regs.Word(DSP_REG_SR), Imm16(SR_40_MODE_BIT));
  FixupBranch not_40bit = J_CC(CC_Z, Jump::Near);
  MOVSX(32, 16, HELPER_A, m_regs.Word(reg));
  SAR(32, R(HELPER_A), Imm8(31));
  MOV(32, m_regs.AccHigh(acc), R(HELPER_A));
  MOV(16, m_regs.Word(DSP_REG_ACL0 + acc), Imm16(0));
  SetJumpTarget(not_40bit);
}

void DSPEmitter::ExtendMidImm(int reg, u16 value)
{
  if (reg != DSP_REG_ACM0 && reg != DSP_REG_ACM1)
    return;
  const int acc = reg - DSP_REG_ACM0;
  TEST(16, m_regs.Word(DSP_REG_SR), Imm16(SR_40_MODE_BIT));
  FixupBranch not_40bit = J_CC(CC_Z, Jump::Near);
  MOV(32, m_regs.AccHigh(acc), Imm32((value & 0x8000) != 0 ? 0xffffffffu : 0u));
  MOV(16, m_regs.Word(DSP_REG_ACL0 + acc), Imm16(0));
  SetJumpTarget(not_40bit);
}

// ---------------------------------------------------------------------------
// Stacks. $stN is the top entry; reg_stacks[N][depth] holds the one below it.

void DSPEmitter::PushStack(int stack, X64Reg value)
{
  ASSERT(value != HELPER_A && value != HELPER_B);
  MOVZX(32, 8, HELPER_A, m_regs.StackDepth(stack));
  ADD(32, R(HELPER_A), Imm8(1));
  AND(32, R(HELPER_A), Imm8(DSP_STACK_MASK));
  MOV(8, m_regs.StackDepth(stack), R(HELPER_A));
  MOVZX(32, 16, HELPER_B, m_regs.StackTop(stack));
  MOV(16, m_regs.StackEntry(stack, HELPER_A), R(HELPER_B));
  MOV(16, m_regs.StackTop(stack), R(value));
}

void DSPEmitter::PushStackImm(int stack, u16 value)
{
  MOVZX(32, 8, HELPER_A, m_regs.StackDepth(stack));
  ADD(32, R(HELPER_A), Imm8(1));
  AND(32, R(HELPER_A), Imm8(DSP_STACK_MASK));
  MOV(8, m_regs.StackDepth(stack), R(HELPER_A));
  MOVZX(32, 16, HELPER_B, m_regs.StackTop(stack));
  MOV(16, m_regs.StackEntry(stack, HELPER_A), R(HELPER_B));
  MOV(16, m_regs.StackTop(stack), Imm16(value));
}

void DSPEmitter::PopStack(int stack, X64Reg dst)
{
  ASSERT(dst != HELPER_A && dst != HELPER_B);
  MOVZX(32, 16, dst, m_regs.StackTop(stack));
  MOVZX(32, 8, HELPER_A, m_regs.StackDepth(stack));
  MOVZX(32, 16, HELPER_B, m_regs.StackEntry(stack, HELPER_A));
  MOV(16, m_regs.StackTop(stack), R(HELPER_B));
  SUB(32, R(HELPER_A), Imm8(1));
  AND(32, R(HELPER_A), Imm8(DSP_STACK_MASK));
  MOV(8, m_regs.StackDepth(stack), R(HELPER_A));
}

// ---------------------------------------------------------------------------
// Memory. With the address in a register:
//   0000-0fff  data RAM
//   1000-1fff  coefficient ROM (mirrored every 0x800 words)
//   2000-ffff  hardware registers (the handler sorts out what is mapped)
// Writes go to data RAM up to 0fff and to the hardware registers above.

void DSPEmitter::EmitDataRead(X64Reg address)
{
  SDSP& state = m_dsp_core.DSPState();
  MOVZX(32, 16, HELPER_D, R(address));
  CMP(32, R(HELPER_D), Imm32(0x0fff));
  FixupBranch beyond_dram = J_CC(CC_A, Jump::Near);
  MOV(64, R(HELPER_A), ImmPtr(state.dram));
  MOVZX(32, 16, EAX, MComplex(HELPER_A, HELPER_D, SCALE_2, 0));
  FixupBranch from_dram = J(Jump::Near);

  SetJumpTarget(beyond_dram);
  CMP(32, R(HELPER_D), Imm32(0x1fff));
  FixupBranch beyond_coef = J_CC(CC_A, Jump::Near);
  AND(32, R(HELPER_D), Imm32(DSP_COEF_MASK));
  MOV(64, R(HELPER_A), ImmPtr(state.coef));
  MOVZX(32, 16, EAX, MComplex(HELPER_A, HELPER_D, SCALE_2, 0));
  FixupBranch from_coef = J(Jump::Near);

  SetJumpTarget(beyond_coef);
  MOV(32, R(ABI_PARAM2), R(HELPER_D));
  MOV(64, R(ABI_PARAM1), ImmPtr(&state));
  ABI_CallFunction(ReadIfxThunk);
  MOVZX(32, 16, EAX, R(ABI_RETURN));

  SetJumpTarget(from_dram);
  SetJumpTarget(from_coef);
}

void DSPEmitter::EmitDataWrite(X64Reg address, X64Reg value)
{
  SDSP& state = m_dsp_core.DSPState();
  MOVZX(32, 16, HELPER_C, R(value));
  MOVZX(32, 16, HELPER_D, R(address));
  CMP(32, R(HELPER_D), Imm32(0x0fff));
  FixupBranch beyond_dram = J_CC(CC_A, Jump::Near);
  MOV(64, R(HELPER_A), ImmPtr(state.dram));
  MOV(16, MComplex(HELPER_A, HELPER_D, SCALE_2, 0), R(HELPER_C));
  FixupBranch done = J(Jump::Near);

  SetJumpTarget(beyond_dram);
  MOV(32, R(ABI_PARAM3), R(HELPER_C));
  MOV(32, R(ABI_PARAM2), R(HELPER_D));
  MOV(64, R(ABI_PARAM1), ImmPtr(&state));
  ABI_CallFunction(WriteIfxThunk);

  SetJumpTarget(done);
}

// With the address known, by 4K bank: 0 data RAM, 1 coefficient ROM (reads
// only), f hardware registers. Nothing else is mapped.
void DSPEmitter::EmitDataReadImm(u16 address)
{
  SDSP& state = m_dsp_core.DSPState();
  switch (address >> 12)
  {
  case 0x0:
    MOV(64, R(HELPER_A), ImmPtr(state.dram + (address & DSP_DRAM_MASK)));
    MOVZX(32, 16, EAX, MatR(HELPER_A));
    break;
  case 0x1:
    MOV(64, R(HELPER_A), ImmPtr(state.coef + (address & DSP_COEF_MASK)));
    MOVZX(32, 16, EAX, MatR(HELPER_A));
    break;
  case 0xf:
    ABI_CallFunctionPC(ReadIfxThunk, &state, address);
    MOVZX(32, 16, EAX, R(ABI_RETURN));
    break;
  default:
    ERROR_LOG_FMT(DSPLLE, "{:04x} DSP ERROR: Read from UNKNOWN ({:04x}) memory", state.pc,
                  address);
    XOR(32, R(EAX), R(EAX));
    break;
  }
}

void DSPEmitter::EmitDataWriteImm(u16 address, X64Reg value)
{
  SDSP& state = m_dsp_core.DSPState();
  switch (address >> 12)
  {
  case 0x0:
    MOV(64, R(HELPER_A), ImmPtr(state.dram + (address & DSP_DRAM_MASK)));
    MOV(16, MatR(HELPER_A), R(value));
    break;
  case 0xf:
    MOVZX(32, 16, HELPER_C, R(value));
    MOV(32, R(ABI_PARAM3), R(HELPER_C));
    MOV(32, R(ABI_PARAM2), Imm32(address));
    MOV(64, R(ABI_PARAM1), ImmPtr(&state));
    ABI_CallFunction(WriteIfxThunk);
    break;
  default:
    ERROR_LOG_FMT(DSPLLE, "{:04x} DSP ERROR: Write to UNKNOWN ({:04x}) memory", state.pc,
                  address);
    break;
  }
}

// Instruction memory: IRAM up to 0fff, the ROM (mirrored) everywhere above.
void DSPEmitter::EmitCodeRead(X64Reg address)
{
  SDSP& state = m_dsp_core.DSPState();
  MOVZX(32, 16, HELPER_D, R(address));
  CMP(32, R(HELPER_D), Imm32(0x0fff));
  FixupBranch beyond_iram = J_CC(CC_A, Jump::Near);
  MOV(64, R(HELPER_A), ImmPtr(state.iram));
  FixupBranch load = J(Jump::Near);
  SetJumpTarget(beyond_iram);
  AND(32, R(HELPER_D), Imm32(DSP_IROM_MASK));
  MOV(64, R(HELPER_A), ImmPtr(state.irom));
  SetJumpTarget(load);
  MOVZX(32, 16, EAX, MComplex(HELPER_A, HELPER_D, SCALE_2, 0));
}

// ---------------------------------------------------------------------------
// Address registers. $wrN is a wrap mask: the register steps within the
// power-of-two sized window it describes. All of this works on 32-bit values
// and stores the low 16 bits.
//   HELPER_A  $wrN
//   HELPER_B  step / scratch
//   HELPER_C  old value, then the bits that changed
//   HELPER_D  new value

void DSPEmitter::EmitArIncrement(int ar)
{
  MOVZX(32, 16, HELPER_A, m_regs.Word(DSP_REG_WR0 + ar));
  MOVZX(32, 16, HELPER_C, m_regs.Word(DSP_REG_AR0 + ar));
  LEA(32, HELPER_D, MDisp(HELPER_C, 1));
  XOR(32, R(HELPER_C), R(HELPER_D));
  LEA(32, HELPER_B, MRegSum(HELPER_A, HELPER_A));
  OR(32, R(HELPER_B), Imm8(2));
  CMP(32, R(HELPER_C), R(HELPER_B));
  FixupBranch inside = J_CC(CC_BE, Jump::Near);
  SUB(32, R(HELPER_D), R(HELPER_A));
  SUB(32, R(HELPER_D), Imm8(1));
  SetJumpTarget(inside);
  MOV(16, m_regs.Word(DSP_REG_AR0 + ar), R(HELPER_D));
}

void DSPEmitter::EmitArDecrement(int ar)
{
  MOVZX(32, 16, HELPER_A, m_regs.Word(DSP_REG_WR0 + ar));
  MOVZX(32, 16, HELPER_C, m_regs.Word(DSP_REG_AR0 + ar));
  LEA(32, HELPER_D, MRegSum(HELPER_C, HELPER_A));
  XOR(32, R(HELPER_C), R(HELPER_D));
  LEA(32, HELPER_B, MRegSum(HELPER_A, HELPER_A));
  OR(32, R(HELPER_B), Imm8(2));
  AND(32, R(HELPER_C), R(HELPER_B));
  CMP(32, R(HELPER_C), R(HELPER_A));
  FixupBranch inside = J_CC(CC_BE, Jump::Near);
  SUB(32, R(HELPER_D), R(HELPER_A));
  SUB(32, R(HELPER_D), Imm8(1));
  SetJumpTarget(inside);
  MOV(16, m_regs.Word(DSP_REG_AR0 + ar), R(HELPER_D));
}

// Expects HELPER_A = $wr, HELPER_C = old ^ step ^ new, HELPER_D = new. Falls into
// the forward case, or comes from to_backward for the backward one.
void DSPEmitter::EmitArIndexWrap(int ar, const FixupBranch& to_backward)
{
  LEA(32, HELPER_B, MRegSum(HELPER_A, HELPER_A));
  OR(32, R(HELPER_B), Imm8(2));
  AND(32, R(HELPER_C), R(HELPER_B));
  CMP(32, R(HELPER_C), R(HELPER_A));
  FixupBranch forward_done = J_CC(CC_BE, Jump::Near);
  SUB(32, R(HELPER_D), R(HELPER_A));
  SUB(32, R(HELPER_D), Imm8(1));
  FixupBranch forward_wrapped = J(Jump::Near);

  SetJumpTarget(to_backward);
  LEA(32, HELPER_B, MRegSum(HELPER_A, HELPER_A));
  OR(32, R(HELPER_B), Imm8(2));
  AND(32, R(HELPER_C), R(HELPER_B));
  LEA(32, HELPER_B, MComplex(HELPER_D, HELPER_A, SCALE_1, 1));
  XOR(32, R(HELPER_B), R(HELPER_D));
  AND(32, R(HELPER_B), R(HELPER_C));
  CMP(32, R(HELPER_B), R(HELPER_A));
  FixupBranch backward_done = J_CC(CC_A, Jump::Near);
  LEA(32, HELPER_D, MComplex(HELPER_D, HELPER_A, SCALE_1, 1));

  SetJumpTarget(forward_done);
  SetJumpTarget(forward_wrapped);
  SetJumpTarget(backward_done);
  MOV(16, m_regs.Word(DSP_REG_AR0 + ar), R(HELPER_D));
}

void DSPEmitter::EmitArAddIndex(int ar, int ix)
{
  MOVZX(32, 16, HELPER_A, m_regs.Word(DSP_REG_WR0 + ar));
  MOVSX(32, 16, HELPER_B, m_regs.Word(DSP_REG_IX0 + ix));
  MOVZX(32, 16, HELPER_C, m_regs.Word(DSP_REG_AR0 + ar));
  LEA(32, HELPER_D, MRegSum(HELPER_C, HELPER_B));
  XOR(32, R(HELPER_C), R(HELPER_B));
  XOR(32, R(HELPER_C), R(HELPER_D));
  TEST(32, R(HELPER_B), R(HELPER_B));
  const FixupBranch negative = J_CC(CC_S, Jump::Near);
  EmitArIndexWrap(ar, negative);
}

void DSPEmitter::EmitArSubIndex(int ar)
{
  // Subtracting ix is adding ~ix + 1; which way the wrap goes depends on ~ix.
  MOVZX(32, 16, HELPER_A, m_regs.Word(DSP_REG_WR0 + ar));
  MOVSX(32, 16, HELPER_B, m_regs.Word(DSP_REG_IX0 + ar));
  NOT(32, R(HELPER_B));
  MOVZX(32, 16, HELPER_C, m_regs.Word(DSP_REG_AR0 + ar));
  LEA(32, HELPER_D, MComplex(HELPER_C, HELPER_B, SCALE_1, 1));
  XOR(32, R(HELPER_C), R(HELPER_B));
  XOR(32, R(HELPER_C), R(HELPER_D));
  CMP(32, R(HELPER_B), Imm32(0x7fff));
  const FixupBranch backward = J_CC(CC_AE, Jump::Near);
  EmitArIndexWrap(ar, backward);
}

void DSPEmitter::EmitArStep(int ar, ArStep step)
{
  switch (step)
  {
  case ArStep::None:
    break;
  case ArStep::Decrement:
    EmitArDecrement(ar);
    break;
  case ArStep::Increment:
    EmitArIncrement(ar);
    break;
  case ArStep::AddIndex:
    EmitArAddIndex(ar, ar);
    break;
  }
}

// ---------------------------------------------------------------------------
// Product register: l, m, h (8 bits used) and a second middle part m2 that is
// added in when the product is read.

void DSPEmitter::LoadProduct(X64Reg dst)
{
  ASSERT(dst != HELPER_A);
  MOV(64, R(dst), m_regs.Product());
  MOV(64, R(HELPER_A), R(dst));
  SHL(64, R(dst), Imm8(24));
  SAR(64, R(dst), Imm8(24));
  SHR(64, R(HELPER_A), Imm8(48));
  SHL(64, R(HELPER_A), Imm8(16));
  ADD(64, R(dst), R(HELPER_A));
}

void DSPEmitter::StoreProduct(X64Reg src)
{
  ASSERT(src != HELPER_A);
  MOV(64, R(HELPER_A), R(src));
  SHL(64, R(HELPER_A), Imm8(24));
  SHR(64, R(HELPER_A), Imm8(24));
  MOV(64, m_regs.Product(), R(HELPER_A));
}

void DSPEmitter::RoundToMid(X64Reg value)
{
  ASSERT(value != HELPER_A);
  // value + 0x7fff + bit 16, then the low word cleared.
  MOV(64, R(HELPER_A), R(value));
  SHR(64, R(HELPER_A), Imm8(16));
  AND(32, R(HELPER_A), Imm8(1));
  LEA(64, value, MComplex(value, HELPER_A, SCALE_1, 0x7fff));
  AND(64, R(value), Imm32(0xffff0000u));
}

}  // namespace DSP::JIT::x64
