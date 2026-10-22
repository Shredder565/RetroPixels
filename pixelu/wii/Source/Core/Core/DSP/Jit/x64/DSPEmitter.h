// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The x86-64 recompiler for the GameCube/Wii DSP.
//
// Code is translated one block at a time. A block starts where the DSP's pc
// was when no code existed for it, and ends at an unconditional branch, at the
// instruction before an idle loop, or after MAX_BLOCK_INSTRUCTIONS
// instructions. Every block returns the number of cycles it used (one per
// instruction, or IDLE_SKIP_CYCLES for an idle loop) to a small dispatcher,
// which looks up the block for the new pc and keeps going until the cycle
// budget is spent or the DSP halts.
//
// Unconditional jumps and calls to blocks that are complete may chain
// straight into the target while the budget lasts ("linking"). A block whose
// own unconditional branches could not be linked yet is not a link target; it
// waits for its targets to be compiled, and is translated again once they are.

#pragma once

#include <set>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"

#include "Core/DSP/DSPCommon.h"
#include "Core/DSP/Jit/DSPEmitterBase.h"
#include "Core/DSP/Jit/x64/DSPJitRegCache.h"

class PointerWrap;

namespace DSP
{
class DSPCore;
enum class StackRegister;
}  // namespace DSP

namespace DSP::JIT::x64
{
class DSPEmitter final : public JIT::DSPEmitter, public Gen::X64CodeBlock
{
public:
  explicit DSPEmitter(DSPCore& dsp);
  ~DSPEmitter() override;

  DSPEmitter(const DSPEmitter&) = delete;
  DSPEmitter& operator=(const DSPEmitter&) = delete;
  DSPEmitter(DSPEmitter&&) = delete;
  DSPEmitter& operator=(DSPEmitter&&) = delete;

  // What an address register does after an access.
  enum class ArStep
  {
    None,
    Decrement,
    Increment,
    AddIndex,  // + the $ix register of the same number
  };

  u16 RunCycles(u16 cycles) override;
  void ClearIRAM() override;
  void DoState(PointerWrap& p) override;

  // Instruction translators, found through DSPJitTables. Each emits the code of
  // one instruction (or of the extension in its low bits) at m_pc.

  // Control flow (DSPJitBranch.cpp)
  void OpJump(UDSPInstruction opc);
  void OpCall(UDSPInstruction opc);
  void OpJumpReg(UDSPInstruction opc);
  void OpCallReg(UDSPInstruction opc);
  void OpIf(UDSPInstruction opc);
  void OpRet(UDSPInstruction opc);
  void OpRti(UDSPInstruction opc);
  void OpHalt(UDSPInstruction opc);
  void OpLoop(UDSPInstruction opc);
  void OpBloop(UDSPInstruction opc);
  void OpLoopi(UDSPInstruction opc);
  void OpBloopi(UDSPInstruction opc);

  // Register moves, immediates, address registers, status bits (DSPJitMisc.cpp)
  void OpNop(UDSPInstruction opc);
  void OpModifyAr(UDSPInstruction opc);
  void OpAddarn(UDSPInstruction opc);
  void OpMrr(UDSPInstruction opc);
  void OpLri(UDSPInstruction opc);
  void OpLris(UDSPInstruction opc);
  void OpSrBit(UDSPInstruction opc);
  void OpModeBit(UDSPInstruction opc);

  // Memory (DSPJitLoadStore.cpp)
  void OpLr(UDSPInstruction opc);
  void OpSr(UDSPInstruction opc);
  void OpSi(UDSPInstruction opc);
  void OpLrr(UDSPInstruction opc);
  void OpSrr(UDSPInstruction opc);
  void OpIlrr(UDSPInstruction opc);
  void OpLrs(UDSPInstruction opc);
  void OpSrs(UDSPInstruction opc);
  void OpSrsh(UDSPInstruction opc);

  // Accumulator arithmetic, logic and shifts (DSPJitArithmetic.cpp)
  void OpClr(UDSPInstruction opc);
  void OpClrl(UDSPInstruction opc);
  void OpTestBits(UDSPInstruction opc);
  void OpTst(UDSPInstruction opc);
  void OpTstaxh(UDSPInstruction opc);
  void OpCmp(UDSPInstruction opc);
  void OpCmpaxh(UDSPInstruction opc);
  void OpCmpi(UDSPInstruction opc);
  void OpCmpis(UDSPInstruction opc);
  void OpLogicAx(UDSPInstruction opc);
  void OpLogicAcc(UDSPInstruction opc);
  void OpNotc(UDSPInstruction opc);
  void OpLogicImm(UDSPInstruction opc);
  void OpAddSubAxPart(UDSPInstruction opc);
  void OpAddSubAx(UDSPInstruction opc);
  void OpAddSubAcc(UDSPInstruction opc);
  void OpAddSubProd(UDSPInstruction opc);
  void OpAddaxl(UDSPInstruction opc);
  void OpAddi(UDSPInstruction opc);
  void OpAddis(UDSPInstruction opc);
  void OpIncDec(UDSPInstruction opc);
  void OpNeg(UDSPInstruction opc);
  void OpAbs(UDSPInstruction opc);
  void OpMovr(UDSPInstruction opc);
  void OpMovax(UDSPInstruction opc);
  void OpMov(UDSPInstruction opc);
  void OpLsl16(UDSPInstruction opc);
  void OpLsr16(UDSPInstruction opc);
  void OpAsr16(UDSPInstruction opc);
  void OpShiftImm(UDSPInstruction opc);
  void OpShiftByAcm1(UDSPInstruction opc);
  void OpShiftByAx(UDSPInstruction opc);
  void OpShiftByAcm(UDSPInstruction opc);

  // Product register and multiplies (DSPJitMultiplier.cpp)
  void OpClrp(UDSPInstruction opc);
  void OpTstprod(UDSPInstruction opc);
  void OpMovp(UDSPInstruction opc);
  void OpMovnp(UDSPInstruction opc);
  void OpMovpz(UDSPInstruction opc);
  void OpAddpaxz(UDSPInstruction opc);
  void OpMulaxh(UDSPInstruction opc);
  void OpMul(UDSPInstruction opc);
  void OpMulx(UDSPInstruction opc);
  void OpMulc(UDSPInstruction opc);
  void OpMaddxc(UDSPInstruction opc);
  void OpMadd(UDSPInstruction opc);

  // Extensions (DSPJitExtOps.cpp)
  void ExtModifyAr(UDSPInstruction opc);
  void ExtMove(UDSPInstruction opc);
  void ExtStore(UDSPInstruction opc);
  void ExtLoad(UDSPInstruction opc);
  void ExtLoadStore(UDSPInstruction opc);
  void ExtLoadDual(UDSPInstruction opc);
  void ExtLoadDualAx(UDSPInstruction opc);

private:
  // Per-block bookkeeping, indexed by start address.
  struct BlockTable
  {
    std::vector<const u8*> entry;   // what the dispatcher jumps to (the stub if not compiled)
    std::vector<const u8*> link;    // where linked branches enter, or null if not linkable
    std::vector<u16> length;        // instructions in the block
    std::vector<std::vector<u16>> waiting;  // link targets that were not linkable yet
    std::set<u16> waiting_blocks;   // the blocks whose waiting list is not empty
  };

  // --- DSPEmitter.cpp: block management ---
  void BuildDispatcher();
  void BuildStub();
  void ResetAllBlocks();
  void ResetBlock(u16 start);
  bool CompileCurrent();
  void CompileBlock(u16 start);
  void EmitInstruction(UDSPInstruction inst);
  void EmitInterpreterCall(UDSPInstruction inst);
  void EmitExceptionCheck(u16 cycles);
  void EmitLoopEnd(bool after_branch);
  void EmitExit(u16 cycles);
  u16 BlockEndCycles() const;
  void AddWaiting(u16 block, u16 target);
  void RemoveWaiting(u16 block, u16 target);
  void ClearWaiting(u16 block);
  static u32 CompileCurrentThunk(DSPEmitter& emitter);

  // --- DSPJitBranch.cpp ---
  // Emits body(opc) so that it runs only if condition (opc & 0xf) holds.
  void EmitConditional(UDSPInstruction opc, void (DSPEmitter::*body)(UDSPInstruction));
  void EmitBranchExit();
  void EmitBlockLink(u16 target);
  void EmitLoopCounterStep();
  void EmitLoopSkip(u16 last_address);
  void JumpBody(UDSPInstruction opc);
  void CallBody(UDSPInstruction opc);
  void JumpRegBody(UDSPInstruction opc);
  void CallRegBody(UDSPInstruction opc);
  void IfBody(UDSPInstruction opc);
  void RetBody(UDSPInstruction opc);
  void RtiBody(UDSPInstruction opc);

  // --- DSPJitCCUtil.cpp: status register ---
  // Whether the flags an instruction sets can be seen by anything (the
  // analyzer decides; with its SR analysis disabled, always).
  bool WantFlags() const;
  // Branches to the returned fixup when condition `cond` (0-15) does NOT hold.
  Gen::FixupBranch EmitSkipUnless(u8 cond);
  // Adds the zero, sign, over-s32 and top-two-bits flags of the 64-bit value to `flags`.
  void CollectFlags40(Gen::X64Reg value, Gen::X64Reg flags);
  // Replaces the six result flags of $sr with those of a 64-bit value.
  void SetFlags40(Gen::X64Reg value);
  // The same after an addition (subtract = false) or subtraction a - b; result is
  // the stored (sign-extended) value. Adds carry and overflow.
  void SetFlagsAddSub(Gen::X64Reg a, Gen::X64Reg b, Gen::X64Reg result, bool subtract);
  // The six result flags of a 16-bit $acX.m result; with full_acc != INVALID_REG the
  // over-s32 flag comes from the whole accumulator instead.
  void SetFlags16(Gen::X64Reg value, Gen::X64Reg full_acc);
  // Writes `flags` into the result bits (SR_CMP_MASK) of $sr.
  void CommitFlags(Gen::X64Reg flags);

  // --- DSPJitUtil.cpp: operands, memory, stacks, address registers, product ---
  // Reads a register as an instruction operand: $st0-$st3 pop their stack, and
  // $acX.m saturates in 40-bit mode. Zero and None give the 16 bits zero extended.
  void ReadOperand(int reg, Gen::X64Reg dst, RegisterExtension ext);
  // Writes a register as an instruction result: $st0-$st3 push their stack.
  void WriteOperand(int reg, Gen::X64Reg src);
  void WriteOperandImm(int reg, u16 value);
  // After a load into $acX.m in 40-bit mode: $acX.h takes the sign, $acX.l is cleared.
  void ExtendMid(int reg);
  void ExtendMidImm(int reg, u16 value);

  void PushStack(int stack, Gen::X64Reg value);
  void PushStackImm(int stack, u16 value);
  void PopStack(int stack, Gen::X64Reg dst);

  // Data memory with the address in a register; the value read ends up zero
  // extended in RAX. Both clobber every caller-saved register.
  void EmitDataRead(Gen::X64Reg address);
  void EmitDataWrite(Gen::X64Reg address, Gen::X64Reg value);
  // Data memory at an address known while compiling.
  void EmitDataReadImm(u16 address);
  void EmitDataWriteImm(u16 address, Gen::X64Reg value);
  // Instruction memory; the value ends up zero extended in RAX.
  void EmitCodeRead(Gen::X64Reg address);

  // Address register updates; they use the helper registers only.
  void EmitArIncrement(int ar);
  void EmitArDecrement(int ar);
  void EmitArAddIndex(int ar, int ix);
  void EmitArSubIndex(int ar);
  void EmitArStep(int ar, ArStep step);
  // Shared tail of the index updates; see DSPJitUtil.cpp.
  void EmitArIndexWrap(int ar, const Gen::FixupBranch& to_backward);

  // The 40-bit value of the product register (with its second middle part added).
  void LoadProduct(Gen::X64Reg dst);
  // Stores the low 40 bits of src into the product register.
  void StoreProduct(Gen::X64Reg src);
  // Rounds a 64-bit accumulator value to its middle word (round half to even).
  void RoundToMid(Gen::X64Reg value);

  // --- DSPJitArithmetic.cpp ---
  // Sign extends a value from bit 39.
  void EmitSext40(Gen::X64Reg value);
  // acc = acc + RDX (or - RDX), with the flags of an addition or subtraction.
  void EmitAccAddSub(int acc, bool subtract);
  // Shifts accumulator acc by the count in the low bits of EAX (see the file).
  void EmitVariableShift(int acc, bool arithmetic, bool bit6_shifts_left);
  // Flags of a comparison a - b (a in RAX, b in RDX).
  void EmitCompare();

  // --- DSPJitMultiplier.cpp ---
  // lhs = lhs * rhs, doubled unless $sr says otherwise.
  void EmitMultiply(Gen::X64Reg lhs, Gen::X64Reg rhs);
  // The MULX-family product of the two 16-bit halves selected by the opcode.
  void EmitMultiplyAx(Gen::X64Reg lhs, Gen::X64Reg rhs, int lhs_high, int rhs_high);
  void LoadMulxOperands(UDSPInstruction opc, Gen::X64Reg lhs, Gen::X64Reg rhs);

  // --- DSPJitExtOps.cpp ---
  // Puts the deferred register writes of the extension into effect.
  void CommitExtension();
  // Loads the extension's (first) value from @$arN; keep_sr also saves $sr with it.
  void EmitExtLoadFirst(int dst_reg, int ar, bool keep_sr);
  // Loads the second value of the LD family: from @$arN again if $arN and $ar3
  // point into the same 1K page, otherwise from @$ar3.
  void EmitExtLoadSecond(int dst_reg, int ar);

  // What a block that ends in an idle loop reports as used, so the caller moves on.
  static constexpr u16 IDLE_SKIP_CYCLES = 0x1000;

  DSPCore& m_dsp_core;
  RegisterFile m_regs;
  BlockTable m_blocks;

  u16 m_cycles_left = 0;

  // Translation state.
  u16 m_block_start = 0;
  u16 m_pc = 0;
  int m_ext_first_reg = -1;   // destination of the value in EXT_FIRST
  int m_ext_second_reg = -1;  // destination of the value in EXT_SECOND
  bool m_ext_first_has_sr = false;  // bits 16-31 of EXT_FIRST hold $sr

  const u8* m_enter = nullptr;
  const u8* m_return = nullptr;
  const u8* m_leave = nullptr;
  const u8* m_stub = nullptr;
};

}  // namespace DSP::JIT::x64
