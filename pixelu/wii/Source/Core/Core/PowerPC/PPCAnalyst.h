// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Static analysis of guest code.
//
// PPCAnalyzer decodes a run of instructions into a CodeBuffer for the JITs:
// per-instruction register/CR/flag usage, liveness information computed
// backwards over the block, float-format facts computed forwards, optional
// branch following and instruction reordering.
//
// The free functions discover and size functions for the symbol database.

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/RangeSet.h"
#include "Core/PowerPC/PPCTables.h"

class PPCSymbolDB;

namespace Common
{
struct Symbol;
}

namespace Core
{
class CPUThreadGuard;
}

namespace PPCAnalyst
{
struct CodeOp
{
  UGeckoInstruction inst;
  const GekkoOPInfo* opinfo = nullptr;
  u32 address = 0;
  u32 branchTo = 0;  // UINT32_MAX when the target is not known statically
  BitSet32 regsIn;
  BitSet32 regsOut;
  BitSet32 fregsIn;
  s8 fregOut = 0;  // -1 when no FPR is written
  BitSet8 crIn;
  BitSet8 crOut;
  bool branchUsesCtr = false;
  bool branchIsIdleLoop = false;
  BitSet8 wantsCR;
  bool wantsFPRF = false;
  bool wantsCA = false;
  bool wantsCAInFlags = false;  // CA may stay in the host carry flag
  BitSet8 outputCR;
  bool outputFPRF = false;
  bool outputCA = false;
  bool canEndBlock = false;
  bool canCauseException = false;
  bool skipLRStack = false;  // call whose return was inlined by branch following
  bool skip = false;         // instruction elided by branch following (an inlined blr)

  // Backwards liveness: what later instructions of the block will read or
  // write, and what is dead (overwritten before being read) from here on.
  BitSet8 crWillBeRead;
  BitSet8 crWillBeWritten;
  BitSet8 crDiscardable;
  BitSet32 gprWillBeRead;
  BitSet32 gprWillBeWritten;
  BitSet32 fprWillBeRead;
  BitSet32 fprWillBeWritten;
  BitSet32 gprDiscardable;
  BitSet32 fprDiscardable;
  // FPRs that later instructions consume through the vector unit (all FPR
  // inputs except those of double stores, which move the raw bits via a GPR).
  BitSet32 fprInXmm;

  // Forward facts about FPR contents at this instruction:
  // the value is representable as a single...
  BitSet32 fprIsSingle;
  // ...ps0 and ps1 hold the same value...
  BitSet32 fprIsDuplicated;
  // ...and the value came from single-precision arithmetic, so converting it
  // with host instructions cannot disturb denormal or SNaN bit patterns that
  // some games rely on being preserved through moves.
  BitSet32 fprIsStoreSafeBeforeInst;
  BitSet32 fprIsStoreSafeAfterInst;

  BitSet32 GetFregsOut() const
  {
    BitSet32 out;
    if (fregOut >= 0)
      out[fregOut] = true;
    return out;
  }
};

struct BlockStats
{
  u32 numCycles;
};

struct BlockRegStats
{
  bool any;
};

using CodeBuffer = std::vector<CodeOp>;

struct CodeBlock
{
  u32 m_address = 0;           // guest address of the first instruction
  u32 m_num_instructions = 0;  // instructions decoded into the buffer

  BlockStats* m_stats = nullptr;
  BlockRegStats* m_gpa = nullptr;
  BlockRegStats* m_fpa = nullptr;  // m_fpa->any: the block uses the FPU

  bool m_broken = false;            // no instruction that ends the block was found
  bool m_memory_exception = false;  // the very first fetch failed

  BitSet8 m_gqr_used;         // GQRs read by quantized loads/stores
  BitSet8 m_gqr_modified;     // GQRs written by mtspr
  BitSet32 m_gpr_inputs;      // GPRs read before being written in the block
  Common::RangeSet<u32> m_physical_addresses;  // memory the block's code occupies
};

class PPCAnalyzer
{
public:
  enum AnalystOption
  {
    // Conditional branches (and traps) do not end the block; execution
    // continues with the fall-through path. Needs JIT support.
    OPTION_CONDITIONAL_CONTINUE = (1 << 0),
    // Inline unconditional branches and calls, and a blr that returns from
    // an inlined call.
    OPTION_BRANCH_FOLLOW = (1 << 1),
    // Reserved; not implemented.
    OPTION_COMPLEX_BLOCK = (1 << 2),
    // Reserved; not implemented.
    OPTION_FORWARD_JUMP = (1 << 3),
    // Move CR-writing instructions (compares, Rc=1) next to the branches
    // that consume them.
    OPTION_BRANCH_MERGE = (1 << 4),
    // Move carry producers and consumers together so CA can stay in the
    // host flags.
    OPTION_CARRY_MERGE = (1 << 5),
    // Move cror next to the fcmp that feeds it.
    OPTION_CROR_MERGE = (1 << 6),
  };

  void SetOption(AnalystOption option) { m_options |= option; }
  void ClearOption(AnalystOption option) { m_options &= ~(option); }
  bool HasOption(AnalystOption option) const { return !!(m_options & option); }
  void SetDebuggingEnabled(bool enabled) { m_is_debugging_enabled = enabled; }
  void SetBranchFollowingEnabled(bool enabled) { m_enable_branch_following = enabled; }
  void SetFloatExceptionsEnabled(bool enabled) { m_enable_float_exceptions = enabled; }
  void SetDivByZeroExceptionsEnabled(bool enabled) { m_enable_div_by_zero_exceptions = enabled; }

  // Decodes up to block_size instructions from address into buffer and fills
  // in block. Returns the address after the last decoded instruction (or the
  // last followed branch target).
  u32 Analyze(u32 address, CodeBlock* block, CodeBuffer* buffer, std::size_t block_size) const;

private:
  enum class BubbleKind
  {
    Carry,
    Compare,
    Cror,
  };

  bool CanSwapAdjacentOps(const CodeOp& a, const CodeOp& b) const;
  bool WantsToBubble(const CodeOp& op, BubbleKind kind) const;
  void BubblePass(CodeOp* code, u32 count, bool backwards, BubbleKind kind) const;
  void ReorderInstructions(u32 instructions, CodeOp* code) const;
  void SetInstructionStats(CodeBlock* block, CodeOp* code, const GekkoOPInfo* opinfo) const;
  bool IsBusyWaitLoop(CodeBlock* block, CodeOp* code, size_t instructions) const;

  u32 m_options = 0;
  bool m_is_debugging_enabled = false;
  bool m_enable_branch_following = false;
  bool m_enable_float_exceptions = false;
  bool m_enable_div_by_zero_exceptions = false;
};

// Scans [startAddr, endAddr) for call targets and adds the functions found
// (plus exception handlers and functions that directly follow known ones).
void FindFunctions(const Core::CPUThreadGuard& guard, u32 startAddr, u32 endAddr,
                   PPCSymbolDB* func_db);
// Determines the size, calls and flags of the function at startAddr.
// Returns false if no valid end was found. max_size = 0 means unbounded.
bool AnalyzeFunction(const Core::CPUThreadGuard& guard, u32 startAddr, Common::Symbol& func,
                     u32 max_size = 0);
bool ReanalyzeFunction(const Core::CPUThreadGuard& guard, u32 start_addr, Common::Symbol& func,
                       u32 max_size = 0);

}  // namespace PPCAnalyst
