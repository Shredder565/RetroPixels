// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Jumps, calls, returns, conditional execution, HALT and the hardware loops.
//
// Every branch first sets pc to the instruction after itself, so a branch not
// taken just falls through. A taken branch stores its target in pc and leaves
// the block (or, for unconditional ones, may chain into the target block).

#include "Core/DSP/Jit/x64/DSPEmitter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPAnalyzer.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPTables.h"

using namespace Gen;

namespace DSP::JIT::x64
{
namespace
{
constexpr int CALL_STACK = static_cast<int>(StackRegister::Call);
constexpr int DATA_STACK = static_cast<int>(StackRegister::Data);
constexpr int LOOP_ADDRESS_STACK = static_cast<int>(StackRegister::LoopAddress);
constexpr int LOOP_COUNTER_STACK = static_cast<int>(StackRegister::LoopCounter);
}  // namespace

void DSPEmitter::EmitConditional(UDSPInstruction opc, void (DSPEmitter::*body)(UDSPInstruction))
{
  const u8 cond = opc & 0xf;
  if (cond == 0xf)
  {
    (this->*body)(opc);
    return;
  }
  const FixupBranch skip = EmitSkipUnless(cond);
  (this->*body)(opc);
  SetJumpTarget(skip);
}

// A taken branch reports the instructions before it (the branch itself is
// counted only once the block's translation goes on past it).
void DSPEmitter::EmitBranchExit()
{
  const auto& analyzer = m_dsp_core.DSPState().GetAnalyzer();
  EmitExit(analyzer.IsIdleSkip(m_block_start) ? IDLE_SKIP_CYCLES :
                                                m_blocks.length[m_block_start]);
}

// Chains straight into a finished target block if the budget covers this block
// and that one. Branches into the part of this block translated so far stay
// unlinked; targets not ready yet are remembered.
void DSPEmitter::EmitBlockLink(u16 target)
{
  if (target >= m_block_start && target <= m_pc)
    return;

  const u8* const target_code = m_blocks.link[target];
  if (target_code == nullptr)
  {
    AddWaiting(m_block_start, target);
    return;
  }

  const u16 used = m_blocks.length[m_block_start];
  const u16 needed = static_cast<u16>(used + m_blocks.length[target]);
  CMP(16, MatR(CYCLE_COUNTER), Imm16(needed));
  FixupBranch short_budget = J_CC(CC_BE, Jump::Near);
  SUB(16, MatR(CYCLE_COUNTER), Imm16(used));
  JMP(target_code, true);
  SetJumpTarget(short_budget);
}

void DSPEmitter::JumpBody(UDSPInstruction opc)
{
  const u16 target = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  if (GetOpTemplate(opc)->uncond_branch)
    EmitBlockLink(target);
  MOV(16, m_regs.Pc(), Imm16(target));
  EmitBranchExit();
}

void DSPEmitter::OpJump(UDSPInstruction opc)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 2)));
  EmitConditional(opc, &DSPEmitter::JumpBody);
}

void DSPEmitter::CallBody(UDSPInstruction opc)
{
  PushStackImm(CALL_STACK, static_cast<u16>(m_pc + 2));
  const u16 target = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  if (GetOpTemplate(opc)->uncond_branch)
    EmitBlockLink(target);
  MOV(16, m_regs.Pc(), Imm16(target));
  EmitBranchExit();
}

void DSPEmitter::OpCall(UDSPInstruction opc)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 2)));
  EmitConditional(opc, &DSPEmitter::CallBody);
}

void DSPEmitter::JumpRegBody(UDSPInstruction opc)
{
  ReadOperand((opc >> 5) & 0x7, RAX, RegisterExtension::None);
  MOV(16, m_regs.Pc(), R(EAX));
  EmitBranchExit();
}

void DSPEmitter::OpJumpReg(UDSPInstruction opc)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 1)));
  EmitConditional(opc, &DSPEmitter::JumpRegBody);
}

void DSPEmitter::CallRegBody(UDSPInstruction opc)
{
  PushStackImm(CALL_STACK, static_cast<u16>(m_pc + 1));
  ReadOperand((opc >> 5) & 0x7, RAX, RegisterExtension::None);
  MOV(16, m_regs.Pc(), R(EAX));
  EmitBranchExit();
}

void DSPEmitter::OpCallReg(UDSPInstruction opc)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 1)));
  EmitConditional(opc, &DSPEmitter::CallRegBody);
}

void DSPEmitter::IfBody(UDSPInstruction)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 1)));
}

// IFcc: run the next instruction only if the condition holds. Either way the
// block ends here; the next instruction starts a block of its own.
void DSPEmitter::OpIf(UDSPInstruction opc)
{
  const u16 next = static_cast<u16>(m_pc + 1);
  const u16 next_size = GetOpTemplate(m_dsp_core.DSPState().ReadIMEM(next))->size;
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(next + next_size)));
  EmitConditional(opc, &DSPEmitter::IfBody);
  EmitBranchExit();
}

void DSPEmitter::RetBody(UDSPInstruction)
{
  PopStack(CALL_STACK, RAX);
  MOV(16, m_regs.Pc(), R(EAX));
  EmitBranchExit();
}

void DSPEmitter::OpRet(UDSPInstruction opc)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 1)));
  EmitConditional(opc, &DSPEmitter::RetBody);
}

void DSPEmitter::RtiBody(UDSPInstruction)
{
  PopStack(DATA_STACK, RAX);
  m_regs.StoreWord(DSP_REG_SR, RAX);
  PopStack(CALL_STACK, RAX);
  MOV(16, m_regs.Pc(), R(EAX));
  EmitBranchExit();
}

void DSPEmitter::OpRti(UDSPInstruction opc)
{
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(m_pc + 1)));
  EmitConditional(opc, &DSPEmitter::RtiBody);
}

void DSPEmitter::OpHalt(UDSPInstruction)
{
  OR(16, m_regs.ControlReg(), Imm16(CR_HALT));
  PopStack(CALL_STACK, RAX);
  MOV(16, m_regs.Pc(), R(EAX));
}

// ---------------------------------------------------------------------------
// Hardware loops. A loop pushes its return address (Call), the address of its
// last instruction (LoopAddress) and its count (LoopCounter); the block
// translator calls EmitLoopCounterStep after that last instruction.

void DSPEmitter::EmitLoopCounterStep()
{
  CMP(16, m_regs.StackTop(LOOP_COUNTER_STACK), Imm16(0));
  FixupBranch no_count = J_CC(CC_E, Jump::Near);
  CMP(16, m_regs.StackTop(LOOP_ADDRESS_STACK), Imm16(static_cast<u16>(m_pc - 1)));
  FixupBranch other_loop = J_CC(CC_NE, Jump::Near);

  SUB(16, m_regs.StackTop(LOOP_COUNTER_STACK), Imm16(1));
  FixupBranch finished = J_CC(CC_Z, Jump::Near);
  // Back to the first instruction of the loop.
  MOVZX(32, 16, EAX, m_regs.StackTop(CALL_STACK));
  MOV(16, m_regs.Pc(), R(EAX));
  FixupBranch repeated = J(Jump::Near);

  SetJumpTarget(finished);
  PopStack(CALL_STACK, HELPER_C);
  PopStack(LOOP_ADDRESS_STACK, HELPER_C);
  PopStack(LOOP_COUNTER_STACK, HELPER_C);

  SetJumpTarget(repeated);
  SetJumpTarget(other_loop);
  SetJumpTarget(no_count);
}

// A loop with a count of zero skips its body, up to and including last_address.
void DSPEmitter::EmitLoopSkip(u16 last_address)
{
  const u16 last_size = GetOpTemplate(m_dsp_core.DSPState().ReadIMEM(last_address))->size;
  MOV(16, m_regs.Pc(), Imm16(static_cast<u16>(last_address + last_size)));
  EmitBranchExit();
}

// LOOP $reg: repeat the next instruction.
void DSPEmitter::OpLoop(UDSPInstruction opc)
{
  const u16 body = static_cast<u16>(m_pc + 1);
  ReadOperand(opc & 0x1f, RDX, RegisterExtension::Zero);
  TEST(16, R(EDX), R(EDX));
  FixupBranch empty = J_CC(CC_Z, Jump::Near);
  PushStack(LOOP_COUNTER_STACK, RDX);
  PushStackImm(CALL_STACK, body);
  PushStackImm(LOOP_ADDRESS_STACK, body);
  MOV(16, m_regs.Pc(), Imm16(body));
  FixupBranch started = J(Jump::Near);
  SetJumpTarget(empty);
  EmitLoopSkip(body);
  SetJumpTarget(started);
}

// BLOOP $reg, end: repeat the instructions from the next one up to end.
void DSPEmitter::OpBloop(UDSPInstruction opc)
{
  const u16 body = static_cast<u16>(m_pc + 2);
  const u16 last = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  ReadOperand(opc & 0x1f, RDX, RegisterExtension::Zero);
  TEST(16, R(EDX), R(EDX));
  FixupBranch empty = J_CC(CC_Z, Jump::Near);
  PushStack(LOOP_COUNTER_STACK, RDX);
  PushStackImm(CALL_STACK, body);
  PushStackImm(LOOP_ADDRESS_STACK, last);
  MOV(16, m_regs.Pc(), Imm16(body));
  FixupBranch started = J(Jump::Near);
  SetJumpTarget(empty);
  EmitLoopSkip(last);
  SetJumpTarget(started);
}

// LOOPI #count: as LOOP with a count known now.
void DSPEmitter::OpLoopi(UDSPInstruction opc)
{
  const u16 count = opc & 0xff;
  const u16 body = static_cast<u16>(m_pc + 1);
  if (count == 0)
  {
    EmitLoopSkip(body);
    return;
  }
  PushStackImm(CALL_STACK, body);
  PushStackImm(LOOP_ADDRESS_STACK, body);
  PushStackImm(LOOP_COUNTER_STACK, count);
  MOV(16, m_regs.Pc(), Imm16(body));
}

// BLOOPI #count, end: as BLOOP with a count known now.
void DSPEmitter::OpBloopi(UDSPInstruction opc)
{
  const u16 count = opc & 0xff;
  const u16 body = static_cast<u16>(m_pc + 2);
  const u16 last = m_dsp_core.DSPState().ReadIMEM(static_cast<u16>(m_pc + 1));
  if (count == 0)
  {
    EmitLoopSkip(last);
    return;
  }
  PushStackImm(CALL_STACK, body);
  PushStackImm(LOOP_ADDRESS_STACK, last);
  PushStackImm(LOOP_COUNTER_STACK, count);
  MOV(16, m_regs.Pc(), Imm16(body));
}

}  // namespace DSP::JIT::x64
