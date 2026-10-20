// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/PPCAnalyst.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/System.h"

namespace PPCAnalyst
{
namespace
{
// How many unconditional branches one block may inline.
constexpr u32 MAX_FOLLOWED_BRANCHES = 2;

constexpr u32 INSTR_BLR = 0x4E800020;
constexpr u32 INSTR_BLRL = 0x4E800021;
constexpr u32 INSTR_BCTR = 0x4E800420;
constexpr u32 INSTR_BCTRL = 0x4E800421;
constexpr u32 INSTR_RFI = 0x4C000064;
constexpr u32 INSTR_NOP = 0x60000000;  // ori r0, r0, 0

bool IsExtended(UGeckoInstruction inst, u32 opcd, u32 xo)
{
  return inst.OPCD == opcd && inst.SUBOP10 == xo;
}

bool IsMfspr(UGeckoInstruction inst)
{
  return IsExtended(inst, 31, 339);
}

bool IsMtspr(UGeckoInstruction inst)
{
  return IsExtended(inst, 31, 467);
}

// The SPR field stores the two 5-bit halves of the number swapped.
u32 SprNumber(UGeckoInstruction inst)
{
  DEBUG_ASSERT(IsMfspr(inst) || IsMtspr(inst));
  return (inst.SPRU << 5) | (inst.SPRL & 0x1F);
}

bool NameStartsWith(const CodeOp& op, std::string_view prefix)
{
  return std::string_view(op.opinfo->opname).starts_with(prefix);
}

// Target of bx/bcx, which encode it in the instruction.
std::optional<u32> ImmediateBranchTarget(UGeckoInstruction inst, u32 pc)
{
  u32 offset;
  if (inst.OPCD == 18)
    offset = SignExt26(inst.LI << 2);
  else if (inst.OPCD == 16)
    offset = SignExt16(inst.BD << 2);
  else
    return std::nullopt;
  return inst.AA ? offset : pc + offset;
}

// The BO field says "branch always" when it neither decrements CTR nor
// tests a condition.
bool BranchesAlways(u32 bo)
{
  return (bo & BO_DONT_DECREMENT_FLAG) && (bo & BO_DONT_CHECK_CONDITION);
}

// mtspr is flagged as ending blocks, but only the performance monitor
// control registers actually require it.
bool InstructionCanEndBlock(const CodeOp& op)
{
  if (!(op.opinfo->flags & FL_ENDBLOCK))
    return false;
  if (!IsMtspr(op.inst))
    return true;
  const u32 spr = SprNumber(op.inst);
  return spr == SPR_MMCR0 || spr == SPR_MMCR1;
}

// Instructions after which the JIT may check the gather pipe and so must be
// able to flush every register.
bool CanCauseGatherPipeInterruptCheck(const CodeOp& op)
{
  if (IsExtended(op.inst, 31, 854))  // eieio
    return true;
  const OpType type = op.opinfo->type;
  return type == OpType::Store || type == OpType::StoreFP || type == OpType::StorePS;
}

bool IsCarryProducer(const CodeOp& op)
{
  const u64 flags = op.opinfo->flags;
  return (flags & FL_SET_CA) && !(flags & FL_SET_OE) && op.opinfo->type == OpType::Integer;
}

bool IsCror(const CodeOp& op)
{
  return IsExtended(op.inst, 19, 449);
}

// ---------------------------------------------------------------------------
// Function discovery
// ---------------------------------------------------------------------------

// Calls a non-leaf function (and is not itself marked evil/rfi).
void MarkNiceCallers(PPCSymbolDB* db, Common::Symbol* func)
{
  const bool calls_non_leaf = std::ranges::any_of(func->calls, [db](const Common::SCall& call) {
    const Common::Symbol* callee = db->GetSymbolFromAddr(call.function);
    return callee != nullptr && (callee->flags & Common::FFLAG_LEAF) == 0;
  });
  if (calls_non_leaf && !(func->flags & (Common::FFLAG_EVIL | Common::FFLAG_RFI)))
    func->flags |= Common::FFLAG_ONLYCALLSNICELEAFS;
}

// Every bl target in the range is assumed to start a function.
void AddCallTargets(const Core::CPUThreadGuard& guard, u32 begin, u32 end, PPCSymbolDB* db)
{
  auto& mmu = guard.GetSystem().GetMMU();
  for (u32 addr = begin; addr < end; addr += 4)
  {
    const PowerPC::TryReadInstResult fetched = mmu.TryReadInstruction(addr);
    const UGeckoInstruction inst{fetched.hex};
    if (!fetched.valid || !PPCTables::IsValidInstruction(inst, addr))
      continue;
    if (inst.OPCD != 18 || !inst.LK)
      continue;
    const u32 target = *ImmediateBranchTarget(inst, addr);
    if (PowerPC::MMU::HostIsRAMAddress(guard, target))
      db->AddFunction(guard, target);
  }
}

// The exception vectors (MSR[IP] = 0) that get a symbol when they hold code. Each symbol is
// named "<kind>_handler".
struct ExceptionVector
{
  u32 offset;
  std::string_view kind;
};

constexpr ExceptionVector s_exception_vectors[] = {
    {0x0100, "system_reset_exception"},
    {0x0200, "machine_check_exception"},
    {0x0300, "dsi_exception"},
    {0x0400, "isi_exception"},
    {0x0500, "external_interrupt_exception"},
    {0x0600, "alignment_exception"},
    {0x0700, "program_exception"},
    {0x0800, "floating_point_unavailable_exception"},
    {0x0900, "decrementer_exception"},
    {0x0C00, "system_call_exception"},
    {0x0D00, "trace_exception"},
    {0x0E00, "floating_point_assist_exception"},
    {0x0F00, "performance_monitor_interrupt"},
    {0x1300, "instruction_address_breakpoint_exception"},
    {0x1400, "system_management_interrupt"},
    {0x1700, "thermal_management_interrupt_exception"},
};

void AddExceptionHandlers(const Core::CPUThreadGuard& guard, PPCSymbolDB* db)
{
  auto& mmu = guard.GetSystem().GetMMU();
  for (const ExceptionVector& vector : s_exception_vectors)
  {
    const u32 address = 0x80000000 | vector.offset;
    const PowerPC::TryReadInstResult fetched = mmu.TryReadInstruction(address);
    if (!fetched.valid || !PPCTables::IsValidInstruction(fetched.hex, address))
      continue;
    if (const Common::Symbol* func = db->AddFunction(guard, address))
      db->RenameSymbol(*func, std::string(vector.kind) + "_handler");
  }
}

// Code that directly follows a known function (after alignment padding of
// zero words or nops) is usually another function.
void AddFollowingFunctions(const Core::CPUThreadGuard& guard, PPCSymbolDB* db)
{
  std::vector<u32> ends;
  db->ForEachSymbol([&](const Common::Symbol& sym) { ends.push_back(sym.address + sym.size); });

  auto& mmu = guard.GetSystem().GetMMU();
  for (u32 location : ends)
  {
    while (true)
    {
      PowerPC::TryReadInstResult fetched = mmu.TryReadInstruction(location);
      while (fetched.valid && (location & 0xF) != 0 &&
             (fetched.hex == 0 || fetched.hex == INSTR_NOP))
      {
        location += 4;
        fetched = mmu.TryReadInstruction(location);
      }
      if (!fetched.valid || !PPCTables::IsValidInstruction(fetched.hex, location))
        break;
      const Common::Symbol* func = db->AddFunction(guard, location);
      if (!func)
        break;
      location += func->size;
    }
  }
}

struct SizeTally
{
  int count = 0;
  int total_size = 0;

  void Add(u32 size)
  {
    ++count;
    total_size += size;
  }
  int Average() const { return count == 0 ? 0 : total_size / count; }
};
}  // namespace

// A function ends at the first blr/rfi that no earlier conditional branch
// jumps past. Calls (bl, or branches leaving the function) are recorded;
// indirect calls make the function "evil".
bool AnalyzeFunction(const Core::CPUThreadGuard& guard, u32 startAddr, Common::Symbol& func,
                     u32 max_size)
{
  if (func.name.empty())
    func.Rename(fmt::format("zz_{:08x}_", startAddr));
  if (func.analyzed)
    return true;

  auto& mmu = guard.GetSystem().GetMMU();

  func.calls.clear();
  func.callers.clear();
  func.size = 0;
  func.flags = Common::FFLAG_LEAF;

  u32 farthest_internal_target = startAddr;
  int internal_branches = 0;

  const auto finish = [&](u32 last_instruction) {
    func.address = startAddr;
    func.analyzed = true;
    func.hash = HashSignatureDB::ComputeCodeChecksum(guard, startAddr, last_instruction);
    if (internal_branches == 0)
      func.flags |= Common::FFLAG_STRAIGHT;
    return true;
  };

  for (u32 addr = startAddr;; addr += 4)
  {
    func.size += 4;
    if (func.size >= JitBase::code_buffer_size * 4 ||
        !PowerPC::MMU::HostIsInstructionRAMAddress(guard, addr))
    {
      return false;
    }

    if (max_size != 0 && func.size > max_size)
    {
      // Cut off at the size limit.
      func.size -= 4;
      return finish(addr - 4);
    }

    const PowerPC::TryReadInstResult fetched = mmu.TryReadInstruction(addr);
    const UGeckoInstruction inst{fetched.hex};
    if (!fetched.valid || !PPCTables::IsValidInstruction(inst, addr))
      return false;

    switch (inst.hex)
    {
    case INSTR_BLR:
    case INSTR_RFI:
      if (farthest_internal_target > addr)
        continue;  // a branch inside the function goes past this return
      return finish(addr);
    case INSTR_BLRL:
    case INSTR_BCTR:
    case INSTR_BCTRL:
      func.flags &= ~Common::FFLAG_LEAF;
      func.flags |= Common::FFLAG_EVIL;
      continue;
    default:
      break;
    }

    const std::optional<u32> target = ImmediateBranchTarget(inst, addr);
    if (!target)
      continue;

    const bool leaves = *target < startAddr || (max_size != 0 && *target >= startAddr + max_size);
    if (inst.LK || leaves)
    {
      func.calls.emplace_back(*target, addr);
      func.flags &= ~Common::FFLAG_LEAF;
    }
    else if (inst.OPCD == 16)
    {
      farthest_internal_target = std::max(farthest_internal_target, *target);
      ++internal_branches;
    }
  }
}

bool ReanalyzeFunction(const Core::CPUThreadGuard& guard, u32 start_addr, Common::Symbol& func,
                       u32 max_size)
{
  ASSERT_MSG(SYMBOLS, func.analyzed, "The function wasn't previously analyzed!");
  func.analyzed = false;
  return AnalyzeFunction(guard, start_addr, func, max_size);
}

void FindFunctions(const Core::CPUThreadGuard& guard, u32 startAddr, u32 endAddr,
                   PPCSymbolDB* func_db)
{
  AddCallTargets(guard, startAddr, endAddr, func_db);
  AddExceptionHandlers(guard, func_db);
  AddFollowingFunctions(guard, func_db);

  func_db->FillInCallers();
  func_db->Index();

  SizeTally leaf, nice, unnice;
  int timer = 0, rfi = 0, straight_leaf = 0;
  func_db->ForEachSymbolWithMutation([&](Common::Symbol& f) {
    if (f.address == 4)
    {
      WARN_LOG_FMT(SYMBOLS, "Weird function");
      return;
    }

    MarkNiceCallers(func_db, &f);
    if (f.name.starts_with("zzz"))
    {
      if (f.flags & Common::FFLAG_LEAF)
        f.Rename(f.name + "_leaf");
      if (f.flags & Common::FFLAG_STRAIGHT)
        f.Rename(f.name + "_straight");
    }

    if (f.flags & Common::FFLAG_LEAF)
      leaf.Add(f.size);
    else if (f.flags & Common::FFLAG_ONLYCALLSNICELEAFS)
      nice.Add(f.size);
    else
      unnice.Add(f.size);

    if (f.flags & Common::FFLAG_TIMERINSTRUCTIONS)
      ++timer;
    if (f.flags & Common::FFLAG_RFI)
      ++rfi;
    if ((f.flags & Common::FFLAG_STRAIGHT) && (f.flags & Common::FFLAG_LEAF))
      ++straight_leaf;
  });

  INFO_LOG_FMT(SYMBOLS,
               "Functions analyzed. {} leafs, {} nice, {} unnice."
               "{} timer, {} rfi. {} are branchless leafs.",
               leaf.count, nice.count, unnice.count, timer, rfi, straight_leaf);
  INFO_LOG_FMT(SYMBOLS, "Average size: {} (leaf), {} (nice), {}(unnice)", leaf.Average(),
               nice.Average(), unnice.Average());
}

// ---------------------------------------------------------------------------
// Block analysis
// ---------------------------------------------------------------------------

bool PPCAnalyzer::CanSwapAdjacentOps(const CodeOp& a, const CodeOp& b) const
{
  if (m_is_debugging_enabled)
  {
    auto& breakpoints = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
    if (breakpoints.IsAddressBreakPoint(a.address) || breakpoints.IsAddressBreakPoint(b.address))
      return false;
  }

  // Moving anything across an instruction that can fault would make the
  // state at the fault differ from the program order.
  if (a.canCauseException || b.canCauseException || a.canEndBlock || b.canEndBlock)
    return false;

  const u64 a_flags = a.opinfo->flags;
  const u64 b_flags = b.opinfo->flags;
  constexpr u64 pinned = FL_TIMER | FL_NO_REORDER | FL_SET_OE;
  if ((a_flags | b_flags) & pinned)
    return false;
  constexpr u64 carry = FL_SET_CA | FL_READ_CA;
  if ((a_flags & carry) && (b_flags & carry))
    return false;

  // Only integer instructions are moved past.
  if (b.opinfo->type != OpType::Integer)
    return false;

  // No dependency in either direction and no write/write conflict.
  if (b.regsOut & (a.regsIn | a.regsOut))
    return false;
  if (a.regsOut & b.regsIn)
    return false;
  if (b.crOut & (a.crIn | a.crOut))
    return false;
  if (a.crOut & b.crIn)
    return false;

  return true;
}

bool PPCAnalyzer::WantsToBubble(const CodeOp& op, BubbleKind kind) const
{
  switch (kind)
  {
  case BubbleKind::Carry:
    return IsCarryProducer(op);
  case BubbleKind::Compare:
    return static_cast<bool>(op.crOut);
  case BubbleKind::Cror:
    return IsCror(op);
  }
  return false;
}

// Moves each candidate instruction one step at a time in the scan direction
// while it can be swapped with its neighbour. After a swap the instruction
// that is now behind it gets another chance, since the swap may have
// unblocked it.
void PPCAnalyzer::BubblePass(CodeOp* code, u32 count, bool backwards, BubbleKind kind) const
{
  const int step = backwards ? -1 : 1;
  const int first = backwards ? static_cast<int>(count) - 1 : 0;
  const int last = backwards ? 0 : static_cast<int>(count) - 1;

  for (int pos = first; pos != last; pos += step)
  {
    for (int i = pos;; i -= step)
    {
      CodeOp& here = code[i];
      if (!WantsToBubble(here, kind))
        break;

      // A carry instruction that already sits next to its partner stays.
      if (kind == BubbleKind::Carry && i != first)
      {
        const u64 here_flags = here.opinfo->flags;
        const u64 behind_flags = code[i - step].opinfo->flags;
        if (!backwards && (here_flags & FL_READ_CA) && (behind_flags & FL_SET_CA))
          break;
        if (backwards && (here_flags & FL_SET_CA) && (behind_flags & FL_READ_CA))
          break;
      }

      CodeOp& ahead = code[i + step];
      if (!CanSwapAdjacentOps(here, ahead))
        break;
      std::swap(here, ahead);

      if (i == first)
        break;
    }
  }
}

void PPCAnalyzer::ReorderInstructions(u32 instructions, CodeOp* code) const
{
  // Carry users are pulled together from both sides; a single direction
  // often leaves addc/adde pairs apart.
  if (HasOption(OPTION_CARRY_MERGE))
  {
    BubblePass(code, instructions, false, BubbleKind::Carry);
    BubblePass(code, instructions, true, BubbleKind::Carry);
  }

  // CR producers move down towards the branches that read them.
  if (HasOption(OPTION_BRANCH_MERGE))
    BubblePass(code, instructions, false, BubbleKind::Compare);

  // cror moves up towards the fcmp it almost always combines.
  if (HasOption(OPTION_CROR_MERGE))
    BubblePass(code, instructions, true, BubbleKind::Cror);
}

void PPCAnalyzer::SetInstructionStats(CodeBlock* block, CodeOp* code,
                                      const GekkoOPInfo* opinfo) const
{
  const u64 flags = opinfo->flags;
  const UGeckoInstruction inst = code->inst;
  const bool is_cr_logic = opinfo->type == OpType::CR;

  // The first FPU instruction of a block can raise FP-unavailable.
  bool first_fpu_instruction = false;
  if (flags & FL_USE_FPU)
  {
    first_fpu_instruction = !block->m_fpa->any;
    block->m_fpa->any = true;
  }

  // CR fields read.
  BitSet8 cr_in;
  if (flags & FL_READ_ALL_CR)
  {
    cr_in = BitSet8(0xFF);
  }
  else if (flags & FL_READ_CRn)
  {
    cr_in[inst.CRFS] = true;
  }
  else if (flags & FL_READ_CR_BI)
  {
    cr_in[inst.BI >> 2] = true;
  }
  else if (is_cr_logic)
  {
    cr_in[inst.CRBA >> 2] = true;
    cr_in[inst.CRBB >> 2] = true;
    // Only one bit of the destination field changes; the rest pass through.
    cr_in[inst.CRBD >> 2] = true;
  }
  code->crIn = cr_in;

  // CR fields written.
  BitSet8 cr_out;
  if (flags & FL_SET_ALL_CR)
    cr_out = BitSet8(0xFF);
  else if (flags & FL_SET_CRn)
    cr_out[inst.CRFD] = true;
  else if ((flags & FL_SET_CR0) || ((flags & FL_RC_BIT) && inst.Rc))
    cr_out[0] = true;
  else if ((flags & FL_SET_CR1) || ((flags & FL_RC_BIT_F) && inst.Rc))
    cr_out[1] = true;
  else if (is_cr_logic)
    cr_out[inst.CRBD >> 2] = true;
  code->crOut = cr_out;

  code->wantsFPRF = (flags & FL_READ_FPRF) != 0;
  code->outputFPRF = (flags & FL_SET_FPRF) != 0;
  code->canEndBlock = InstructionCanEndBlock(*code);
  code->canCauseException = first_fpu_instruction || (flags & (FL_LOADSTORE | FL_PROGRAMEXCEPTION)) ||
                            (m_enable_float_exceptions && (flags & FL_FLOAT_EXCEPTION)) ||
                            (m_enable_div_by_zero_exceptions && (flags & FL_FLOAT_DIV));

  code->wantsCA = (flags & FL_READ_CA) != 0;
  code->outputCA = (flags & FL_SET_CA) != 0;
  // CA can live in the host carry flag only between an integer instruction
  // that both consumes and produces it and its neighbours.
  code->wantsCAInFlags = HasOption(OPTION_CARRY_MERGE) && code->wantsCA && code->outputCA &&
                         opinfo->type == OpType::Integer;
  // mfspr/mtspr of XER read/write CA in memory, not in flags.
  if (IsMfspr(inst))
    code->wantsCA = SprNumber(inst) == SPR_XER;
  if (IsMtspr(inst))
    code->outputCA = SprNumber(inst) == SPR_XER;

  // GPRs.
  BitSet32 gpr_in, gpr_out;
  if (flags & FL_OUT_A)
    gpr_out[inst.RA] = true;
  if (flags & FL_OUT_D)
    gpr_out[inst.RD] = true;
  if ((flags & FL_IN_A) || ((flags & FL_IN_A0) && inst.RA != 0))
    gpr_in[inst.RA] = true;
  if (flags & FL_IN_B)
    gpr_in[inst.RB] = true;
  if (flags & FL_IN_C)
    gpr_in[inst.RC] = true;
  if (flags & FL_IN_S)
    gpr_in[inst.RS] = true;
  // lmw/stmw cover rD/rS through r31.
  if (inst.OPCD == 46)
    gpr_out |= BitSet32(~0u << inst.RD);
  else if (inst.OPCD == 47)
    gpr_in |= BitSet32(~0u << inst.RS);
  code->regsIn = gpr_in;
  code->regsOut = gpr_out;

  // FPRs.
  code->fregOut = (flags & FL_OUT_FLOAT_D) ? static_cast<s8>(inst.FD) : s8{-1};
  BitSet32 fpr_in;
  if (flags & FL_IN_FLOAT_A)
    fpr_in[inst.FA] = true;
  if (flags & FL_IN_FLOAT_B)
    fpr_in[inst.FB] = true;
  if (flags & FL_IN_FLOAT_C)
    fpr_in[inst.FC] = true;
  if (flags & FL_IN_FLOAT_D)
    fpr_in[inst.FD] = true;
  if (flags & FL_IN_FLOAT_S)
    fpr_in[inst.FS] = true;
  code->fregsIn = fpr_in;

  // Branches.
  code->branchTo = ImmediateBranchTarget(inst, code->address).value_or(UINT32_MAX);
  const bool bc_family = inst.OPCD == 16 || IsExtended(inst, 19, 16) || IsExtended(inst, 19, 528);
  code->branchUsesCtr = bc_family && !(inst.BO & BO_DONT_DECREMENT_FLAG);
}

// A busy-wait loop branches back to the block start, contains no other
// branch, uses only integer and load instructions, and never overwrites a
// register whose incoming value it read (so every iteration behaves alike).
bool PPCAnalyzer::IsBusyWaitLoop(CodeBlock* block, CodeOp* code, size_t instructions) const
{
  BitSet32 read_before_written;
  BitSet32 written;
  for (size_t i = 0; i <= instructions; ++i)
  {
    const CodeOp& op = code[i];
    const OpType type = op.opinfo->type;
    if (type == OpType::Branch)
    {
      if (op.branchUsesCtr)
        return false;
      if (op.branchTo == block->m_address && i == instructions)
        return true;
      continue;
    }
    // nop is "ori r0,r0,0", which would otherwise count as reading r0.
    if (op.inst.hex == INSTR_NOP)
      continue;
    if (type != OpType::Integer && type != OpType::Load)
      return false;

    read_before_written |= op.regsIn & ~written;
    if (op.regsOut & read_before_written)
      return false;
    written |= op.regsOut;
  }
  return false;
}

u32 PPCAnalyzer::Analyze(u32 address, CodeBlock* block, CodeBuffer* buffer,
                         std::size_t block_size) const
{
  *block->m_stats = {};
  block->m_gpa->any = true;
  block->m_fpa->any = false;
  block->m_address = address;
  block->m_broken = false;
  block->m_memory_exception = false;
  block->m_num_instructions = 0;
  block->m_gqr_used = BitSet8(0);
  block->m_physical_addresses.clear();

  CodeOp* const code = buffer->data();
  auto& system = Core::System::GetInstance();
  auto& mmu = system.GetMMU();

  const bool follow_branches = m_enable_branch_following && HasOption(OPTION_BRANCH_FOLLOW);
  const bool continue_past_conditionals = HasOption(OPTION_CONDITIONAL_CONTINUE);

  bool found_exit = false;
  // An inlined call whose matching blr can still be inlined.
  bool pending_call = false;
  size_t call_index = 0;
  u32 followed = 0;
  u32 decoded = 0;

  // ---- Decode -------------------------------------------------------------
  for (std::size_t i = 0; i < block_size; ++i)
  {
    const PowerPC::TryReadInstResult fetched = mmu.TryReadInstruction(address);
    if (!fetched.valid)
    {
      if (i == 0)
        block->m_memory_exception = true;
      break;
    }
    ++decoded;

    const UGeckoInstruction inst{fetched.hex};
    const GekkoOPInfo* opinfo = PPCTables::GetOpInfo(inst, address);
    CodeOp& op = code[i];
    op = {};
    op.opinfo = opinfo;
    op.address = address;
    op.inst = inst;
    block->m_stats->numCycles += opinfo->num_cycles;
    block->m_physical_addresses.insert(fetched.physical_address,
                                       fetched.physical_address + sizeof(UGeckoInstruction));
    SetInstructionStats(block, &op, opinfo);

    const bool is_bclr = IsExtended(inst, 19, 16);

    bool follow = false;
    if (follow_branches)
    {
      const bool unconditional_branch =
          inst.OPCD == 18 || (inst.OPCD == 16 && BranchesAlways(inst.BO));
      if (unconditional_branch && block_size > 1)
      {
        follow = true;
        if (inst.LK)
        {
          pending_call = true;
          call_index = i;
        }
      }
      else if (is_bclr && !inst.LK && pending_call)
      {
        // A return from the inlined call goes to the instruction after it.
        // Only an unconditional one is followed, and only while LR provably
        // still holds that address (nothing in between wrote it).
        op.branchTo = code[call_index].address + 4;
        if (BranchesAlways(inst.BO) && followed < MAX_FOLLOWED_BRANCHES)
        {
          follow = true;
          pending_call = false;
          op.skip = true;
          code[call_index].skipLRStack = true;
        }
      }
      else if (IsMtspr(inst) && SprNumber(inst) == SPR_LR)
      {
        pending_call = false;
      }
    }

    bool conditional = false;
    if (continue_past_conditionals)
    {
      const bool bc_conditional = !BranchesAlways(inst.BO);
      conditional = (inst.OPCD == 16 && bc_conditional) || (is_bclr && bc_conditional) ||
                    inst.OPCD == 3 || IsExtended(inst, 31, 4) ||  // twi, tw
                    (IsExtended(inst, 19, 528) && !(inst.BO_2 & BO_DONT_CHECK_CONDITION));
    }

    op.branchIsIdleLoop = op.branchTo == block->m_address && IsBusyWaitLoop(block, code, i);

    if (follow && followed < MAX_FOLLOWED_BRANCHES)
    {
      ++followed;
      address = op.branchTo;
      continue;
    }

    address += 4;
    if (conditional)
    {
      // Skipping a conditional branch breaks the call/return pairing.
      pending_call = false;
    }
    else if (InstructionCanEndBlock(op))
    {
      found_exit = true;
      break;
    }
  }

  block->m_num_instructions = decoded;
  if (decoded > 1)
    ReorderInstructions(decoded, code);
  if ((!found_exit && decoded > 0) || block_size == 1)
    block->m_broken = true;

  // ---- Backward pass: liveness ---------------------------------------------
  // Whatever follows the block is assumed to need FPRF and CA.
  auto& power_pc = system.GetPowerPC();
  auto& symbol_db = power_pc.GetSymbolDB();
  bool fprf_needed = true;
  bool ca_needed = true;
  BitSet8 cr_read, cr_written, cr_dead;
  BitSet32 gpr_read, gpr_written, gpr_dead;
  BitSet32 fpr_read, fpr_written, fpr_dead, fpr_in_xmm;

  for (int i = static_cast<int>(decoded) - 1; i >= 0; --i)
  {
    CodeOp& op = code[i];

    if (CanCauseGatherPipeInterruptCheck(op))
    {
      gpr_dead = {};
      fpr_dead = {};
      cr_dead = {};
    }

    // HLE hooks and breakpoints hand control elsewhere before the
    // instruction runs; all state must be in memory there.
    const bool hooked =
        static_cast<bool>(HLE::TryReplaceFunction(symbol_db, op.address, power_pc.GetMode())) ||
        power_pc.GetBreakPoints().IsAddressBreakPoint(op.address);
    const bool may_leave = hooked || op.canEndBlock || op.canCauseException;

    const bool reads_fprf = op.wantsFPRF;
    const bool reads_ca = op.wantsCA;
    op.wantsFPRF = fprf_needed || may_leave;
    op.wantsCA = ca_needed || may_leave;
    fprf_needed = (fprf_needed || reads_fprf || may_leave) && (!op.outputFPRF || reads_fprf);
    ca_needed = (ca_needed || reads_ca || may_leave) && (!op.outputCA || reads_ca);

    op.gprWillBeRead = gpr_read;
    op.gprWillBeWritten = gpr_written;
    op.fprWillBeRead = fpr_read;
    op.fprWillBeWritten = fpr_written;
    op.crWillBeRead = cr_read;
    op.crWillBeWritten = cr_written;
    op.gprDiscardable = gpr_dead;
    op.fprDiscardable = fpr_dead;
    op.crDiscardable = cr_dead;
    op.fprInXmm = fpr_in_xmm;

    const BitSet32 fpr_out = op.GetFregsOut();
    gpr_read = (gpr_read & ~op.regsOut) | op.regsIn;
    gpr_written |= op.regsOut;
    fpr_read = (fpr_read & ~fpr_out) | op.fregsIn;
    fpr_written |= fpr_out;
    cr_read = (cr_read & ~op.crOut) | op.crIn;
    cr_written |= op.crOut;

    // Double stores move the raw bits through a GPR.
    if (!NameStartsWith(op, "stfd"))
      fpr_in_xmm |= op.fregsIn;

    if (hooked)
    {
      gpr_read = {};
      gpr_written = {};
      fpr_read = {};
      fpr_written = {};
      fpr_in_xmm = {};
      cr_read = {};
      cr_written = {};
      gpr_dead = {};
      fpr_dead = {};
      cr_dead = {};
    }
    else if (op.canEndBlock || op.canCauseException)
    {
      gpr_dead = {};
      fpr_dead = {};
      cr_dead = {};
    }
    else
    {
      gpr_dead = (gpr_dead | op.regsOut) & ~op.regsIn;
      fpr_dead = (fpr_dead | fpr_out) & ~op.fregsIn;
      cr_dead = (cr_dead | op.crOut) & ~op.crIn;
    }
  }

  // ---- Forward pass: FPR formats and GQR usage -----------------------------
  BitSet32 fpr_single, fpr_duplicated, fpr_store_safe;
  BitSet8 gqr_used, gqr_modified;
  for (u32 i = 0; i < decoded; ++i)
  {
    CodeOp& op = code[i];
    const u64 flags = op.opinfo->flags;
    const OpType type = op.opinfo->type;

    op.fprIsSingle = fpr_single;
    op.fprIsDuplicated = fpr_duplicated;
    op.fprIsStoreSafeBeforeInst = fpr_store_safe;

    if (op.fregOut >= 0)
    {
      const int out = op.fregOut;

      // Inputs whose exact bits are copied into the result.
      BitSet32 copied;
      if (flags & FL_IN_FLOAT_A_BITEXACT)
        copied[op.inst.FA] = true;
      if (flags & FL_IN_FLOAT_B_BITEXACT)
        copied[op.inst.FB] = true;
      if (flags & FL_IN_FLOAT_C_BITEXACT)
        copied[op.inst.FC] = true;

      const bool rounds_to_single = type == OpType::SingleFP || NameStartsWith(op, "frsp");
      bool single;
      bool duplicated;
      if (rounds_to_single || NameStartsWith(op, "lfs"))
      {
        single = true;
        duplicated = true;
      }
      else if (copied)
      {
        single = (fpr_single & copied) == copied;
        duplicated = false;
      }
      else
      {
        single = type == OpType::PS || type == OpType::LoadPS;
        duplicated = false;
      }
      fpr_single[out] = single;
      fpr_duplicated[out] = duplicated;

      if (NameStartsWith(op, "mtfs"))
      {
        // Changing the FP mode (e.g. flush-to-zero) mid-block invalidates
        // what earlier instructions established.
        fpr_store_safe = BitSet32(0);
      }
      else if (copied)
      {
        fpr_store_safe[out] = (fpr_store_safe & copied) == copied;
      }
      else
      {
        fpr_store_safe[out] = rounds_to_single || type == OpType::PS;
      }
    }
    op.fprIsStoreSafeAfterInst = fpr_store_safe;

    if (type == OpType::StorePS || type == OpType::LoadPS)
      gqr_used[op.inst.OPCD == 4 ? op.inst.Ix : op.inst.I] = true;

    if (IsMtspr(op.inst))
    {
      const int gqr = static_cast<int>(SprNumber(op.inst)) - SPR_GQR0;
      if (gqr >= 0 && gqr <= 7)
        gqr_modified[gqr] = true;
    }
  }

  block->m_gqr_used = gqr_used;
  block->m_gqr_modified = gqr_modified;
  block->m_gpr_inputs = gpr_read;
  return address;
}

}  // namespace PPCAnalyst
