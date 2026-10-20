// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/RangeSizeSet.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/JitAsm.h"
#include "Core/PowerPC/Jit64/RegCache/FPURegCache.h"
#include "Core/PowerPC/Jit64/RegCache/GPRRegCache.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/BlockCache.h"
#include "Core/PowerPC/Jit64Common/Jit64AsmCommon.h"
#include "Core/PowerPC/Jit64Common/TrampolineCache.h"
#include "Core/PowerPC/JitCommon/ConstantPropagation.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class HostDisassembler;
namespace PowerPC
{
class PowerPCManager;
}
namespace PPCAnalyst
{
struct CodeBlock;
struct CodeOp;
}  // namespace PPCAnalyst

// The x86-64 recompiler.
//
// A guest block is translated instruction by instruction into near code;
// rarely taken paths (exception exits, slow memory accesses, NaN handling)
// go to far code. Guest registers are cached in host registers by gpr/fpr
// for the length of a block, and known integer values are folded by
// constant propagation. Blocks end in exits that the block cache can patch
// into direct jumps to other blocks. Fastmem accesses that fault are
// patched to jump to slow-path trampolines.
class Jit64 : public JitBase, public QuantizedMemoryRoutines
{
public:
  explicit Jit64(Core::System& system);
  Jit64(const Jit64&) = delete;
  Jit64(Jit64&&) = delete;
  Jit64& operator=(const Jit64&) = delete;
  Jit64& operator=(Jit64&&) = delete;
  ~Jit64() override;

  // --- JitBase interface ---------------------------------------------------

  void Init() override;
  void Shutdown() override;

  bool HandleFault(uintptr_t access_address, SContext* ctx) override;
  bool BackPatch(SContext* ctx);

  void EnableOptimization();
  void EnableBlockLink();

  void Jit(u32 em_address) override;
  void Jit(u32 em_address, bool clear_cache_and_retry_on_failure);
  bool DoJit(u32 em_address, JitBlock* b, u32 nextPC);

  void EraseSingleBlock(const JitBlock& block) override;
  std::vector<MemoryStats> GetMemoryStats() const override;

  std::size_t DisassembleNearCode(const JitBlock& block, std::ostream& stream) const override;
  std::size_t DisassembleFarCode(const JitBlock& block, std::ostream& stream) const override;

  // Points the near and far emitters at the largest free ranges. Returns
  // false if either region is completely full.
  bool SetEmitterStateToFreeCodeRegion();

  BitSet32 CallerSavedRegistersInUse(BitSet32 additional_registers = {}) const;
  BitSet8 ComputeStaticGQRs(const PPCAnalyst::CodeBlock&) const;

  void IntializeSpeculativeConstants();

  void FlushRegistersBeforeSlowAccess();

  JitCommon::ConstantPropagation& GetConstantPropagation() { return m_constant_propagation; }

  JitBlockCache* GetBlockCache() override { return &blocks; }
  void Trace();

  void ClearCache() override;

  const CommonAsmRoutines* GetAsmRoutines() override { return &asm_routines; }
  const char* GetName() const override { return "JIT64"; }

  void Run() override;
  void SingleStep() override;

  // --- Code generation helpers used by the instruction handlers ------------

  void EmitUpdateMembase();
  void MSRUpdated(const Gen::OpArg& msr, Gen::X64Reg scratch_reg);

  // Block exits. `bl` exits also push a return prediction for the BLR
  // optimization.
  void FakeBLCall(u32 after);
  void WriteExit(u32 destination, bool bl = false, u32 after = 0);
  void JustWriteExit(u32 destination, bool bl, u32 after);
  void WriteExitDestInRSCRATCH(bool bl = false, u32 after = 0);
  void WriteBLRExit();
  void WriteExceptionExit();
  void WriteExternalExceptionExit();
  void WriteRfiExitDestInRSCRATCH();
  void WriteIdleExit(u32 destination);
  template <bool condition>
  void WriteBranchWatch(u32 origin, u32 destination, UGeckoInstruction inst, BitSet32 caller_save);
  void WriteBranchWatchDestInRSCRATCH(u32 origin, UGeckoInstruction inst, BitSet32 caller_save);

  // Emits the end-of-block bookkeeping (gather pipe, perf monitor,
  // profiling). Returns true if anything was emitted, which may clobber
  // RSCRATCH.
  bool Cleanup();

  // XER and CR0 helpers.
  void GenerateConstantOverflow(bool overflow);
  void GenerateConstantOverflow(s64 val);
  void GenerateOverflow(Gen::CCFlags cond = Gen::CCFlags::CC_NO);
  void FinalizeCarryOverflow(bool oe, bool inv = false);
  void FinalizeCarry(Gen::CCFlags cond);
  void FinalizeCarry(bool ca);
  void FlushCarry();
  void ComputeRC(preg_t preg, bool needs_test = true, bool needs_sext = true);
  void FinalizeImmediateRC(s32 value);

  void AndWithMask(Gen::X64Reg reg, u32 mask);
  void RotateLeft(int bits, Gen::X64Reg regOp, const Gen::OpArg& arg, u8 rotate);

  // Compare-and-branch fusion.
  bool CheckMergedBranch(u32 crf) const;
  void DoMergedBranch();
  void DoMergedBranchCondition();
  void DoMergedBranchImmediate(s64 val);

  // Condition register fields are kept in the 64-bit "emulated CR" format.
  void GetCRFieldBit(int field, int bit, Gen::X64Reg out, bool negate = false);
  // Clobbers RDX.
  void SetCRFieldBit(int field, int bit, Gen::X64Reg in);
  void ClearCRFieldBit(int field, int bit);
  void SetCRFieldBit(int field, int bit);
  void FixGTBeforeSettingCRFieldBit(Gen::X64Reg reg);
  // Branch taken when the given CR bit is set (or clear).
  Gen::FixupBranch JumpIfCRFieldBit(int field, int bit, bool jump_if_set = true);

  // Floating point helpers.
  void UpdateFPExceptionSummary(Gen::X64Reg fpscr, Gen::X64Reg tmp1, Gen::X64Reg tmp2);
  void SetFPRFIfNeeded(const Gen::OpArg& xmm, bool single);
  void FinalizeSingleResult(Gen::X64Reg output, const Gen::OpArg& input, bool packed = true,
                            bool duplicate = false);
  void FinalizeDoubleResult(Gen::X64Reg output, const Gen::OpArg& input);
  [[nodiscard]] Gen::FixupBranch HandleNaNs(UGeckoInstruction inst, Gen::X64Reg xmm,
                                            Gen::X64Reg clobber, std::optional<Gen::OpArg> Ra,
                                            std::optional<Gen::OpArg> Rb,
                                            std::optional<Gen::OpArg> Rc);

  void MultiplyImmediate(u32 imm, int a, int d, bool overflow);

  typedef u32 (*Operation)(u32 a, u32 b);
  void regimmop(int d, int a, bool binary, u32 value, Operation doop,
                void (Gen::XEmitter::*op)(int, const Gen::OpArg&, const Gen::OpArg&),
                bool Rc = false, bool carry = false);
  void FloatCompare(UGeckoInstruction inst, bool upper = false);
  void UpdateMXCSR();

  // --- Instruction handlers -------------------------------------------------

  using Instruction = void (Jit64::*)(UGeckoInstruction instCode);
  void FallBackToInterpreter(UGeckoInstruction _inst);
  void DoNothing(UGeckoInstruction _inst);
  void HLEFunction(u32 hook_index);

  void DynaRunTable4(UGeckoInstruction inst);
  void DynaRunTable19(UGeckoInstruction inst);
  void DynaRunTable31(UGeckoInstruction inst);
  void DynaRunTable59(UGeckoInstruction inst);
  void DynaRunTable63(UGeckoInstruction inst);

  // Integer.
  void addx(UGeckoInstruction inst);
  void mulli(UGeckoInstruction inst);
  void mulhwXx(UGeckoInstruction inst);
  void mullwx(UGeckoInstruction inst);
  void divwux(UGeckoInstruction inst);
  void divwx(UGeckoInstruction inst);
  void srawix(UGeckoInstruction inst);
  void srawx(UGeckoInstruction inst);
  void arithXex(UGeckoInstruction inst);
  void extsXx(UGeckoInstruction inst);
  void boolX(UGeckoInstruction inst);
  void reg_imm(UGeckoInstruction inst);
  void cmpXX(UGeckoInstruction inst);
  void cntlzwx(UGeckoInstruction inst);
  void rlwinmx(UGeckoInstruction inst);
  void rlwimix(UGeckoInstruction inst);
  void rlwnmx(UGeckoInstruction inst);
  void negx(UGeckoInstruction inst);
  void slwx(UGeckoInstruction inst);
  void srwx(UGeckoInstruction inst);
  void subfic(UGeckoInstruction inst);
  void subfx(UGeckoInstruction inst);
  void twX(UGeckoInstruction inst);

  // Branches and system.
  void sc(UGeckoInstruction _inst);
  void rfi(UGeckoInstruction _inst);
  void bx(UGeckoInstruction inst);
  void bclrx(UGeckoInstruction _inst);
  void bcctrx(UGeckoInstruction _inst);
  void bcx(UGeckoInstruction inst);

  // System registers.
  void mtspr(UGeckoInstruction inst);
  void mfspr(UGeckoInstruction inst);
  void mtmsr(UGeckoInstruction inst);
  void mfmsr(UGeckoInstruction inst);
  void mftb(UGeckoInstruction inst);
  void mtcrf(UGeckoInstruction inst);
  void mfcr(UGeckoInstruction inst);
  void mcrf(UGeckoInstruction inst);
  void mcrxr(UGeckoInstruction inst);
  void mcrfs(UGeckoInstruction inst);
  void mffsx(UGeckoInstruction inst);
  void mtfsb0x(UGeckoInstruction inst);
  void mtfsb1x(UGeckoInstruction inst);
  void mtfsfix(UGeckoInstruction inst);
  void mtfsfx(UGeckoInstruction inst);
  void crXXX(UGeckoInstruction inst);

  // Paired singles.
  void ps_mr(UGeckoInstruction inst);
  void ps_mergeXX(UGeckoInstruction inst);
  void ps_res(UGeckoInstruction inst);
  void ps_rsqrte(UGeckoInstruction inst);
  void ps_sum(UGeckoInstruction inst);
  void ps_muls(UGeckoInstruction inst);
  void ps_cmpXX(UGeckoInstruction inst);

  // Floating point.
  void fp_arith(UGeckoInstruction inst);
  void fcmpX(UGeckoInstruction inst);
  void fctiwx(UGeckoInstruction inst);
  void fmrx(UGeckoInstruction inst);
  void frspx(UGeckoInstruction inst);
  void frsqrtex(UGeckoInstruction inst);
  void fresx(UGeckoInstruction inst);
  void fmaddXX(UGeckoInstruction inst);
  void fsign(UGeckoInstruction inst);
  void fselx(UGeckoInstruction inst);

  // Loads and stores.
  void lfXXX(UGeckoInstruction inst);
  void stfXXX(UGeckoInstruction inst);
  void stfiwx(UGeckoInstruction inst);
  void psq_lXX(UGeckoInstruction inst);
  void psq_stXX(UGeckoInstruction inst);
  void stX(UGeckoInstruction inst);
  void dcbt(UGeckoInstruction inst);
  void dcbz(UGeckoInstruction inst);
  void lXXx(UGeckoInstruction inst);
  void stXx(UGeckoInstruction inst);
  void lmw(UGeckoInstruction inst);
  void stmw(UGeckoInstruction inst);
  void dcbx(UGeckoInstruction inst);
  void eieio(UGeckoInstruction inst);

private:
  void CompileInstruction(PPCAnalyst::CodeOp& op);

  bool HandleFunctionHooking(u32 address);

  void FreeRanges();
  void ResetFreeMemoryRanges();

  void LogGeneratedCode() const;

  static void ImHere(Jit64& jit);

  // DoJit pieces.
  void EmitConstantGQRChecks();
  void EmitGatherPipeInterruptCheck(const PPCAnalyst::CodeOp& op, size_t index);
  void EmitBreakpointCheck(const PPCAnalyst::CodeOp& op);
  void EmitFPUnavailableCheck(const PPCAnalyst::CodeOp& op);
  void CompileWithConstantPropagation(PPCAnalyst::CodeOp& op);
  void EmitMemoryExceptionExit(const PPCAnalyst::CodeOp& op);
  void FlushAfterInstruction(const PPCAnalyst::CodeOp& op);
  // Far-code stub that recompiles the block without the given assumption.
  const u8* EmitAssumptionFailureStub(u32 exception_type);

  // Exit pieces.
  void PushBLRPrediction(u32 after);
  void EmitCheckExceptionsAndDispatch(void (*check)(PowerPC::PowerPCManager&));

  // Conditional branch pieces. The skips jump past the taken path when CTR
  // or the CR bit says the branch is not taken.
  struct BranchSkips
  {
    std::optional<Gen::FixupBranch> ctr;
    std::optional<Gen::FixupBranch> condition;
  };
  BranchSkips EmitBranchConditionTests(UGeckoInstruction inst);
  void BindBranchSkips(const BranchSkips& skips);
  // A branch the analyzer followed: no exit, only bookkeeping.
  void EmitFlattenedBranch(UGeckoInstruction inst);
  // Flushed exit to js.op->branchTo (idle exit for idle loops).
  void ExitToBranchTarget(UGeckoInstruction inst);
  // The fall-through after a conditional branch. Without conditional
  // continue the block exits to the next instruction; `watch_after_exit`
  // also emits the continuing-path watch call after that exit.
  void EmitBranchNotTaken(UGeckoInstruction inst, bool watch_after_exit);

  JitBlockCache blocks{*this};
  TrampolineCache trampolines{*this};

  GPRRegCache gpr{*this};
  FPURegCache fpr{*this};

  JitCommon::ConstantPropagation m_constant_propagation;

  Jit64AsmRoutineManager asm_routines{*this};

  Common::RangeSizeSet<u8*> m_free_ranges_near;
  Common::RangeSizeSet<u8*> m_free_ranges_far;

  // Debugging aids: call ImHere at every block entry, optionally logging.
  const bool m_im_here_debug = false;
  const bool m_im_here_log = false;
  std::map<u32, int> m_been_here;
  std::unique_ptr<HostDisassembler> m_disassembler;
};
