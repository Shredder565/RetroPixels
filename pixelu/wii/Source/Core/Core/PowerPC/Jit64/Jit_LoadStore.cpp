// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include "Common/Assert.h"
#include "Common/BitSet.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/x64ABI.h"
#include "Common/x64Emitter.h"

#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace Gen;

namespace
{
struct LoadKind
{
  int bits = 0;
  bool sign_extend = false;
  bool byte_reversed = false;
};

// Integer loads: the D-form opcodes 32..43 and their X-form counterparts.
// An unknown opcode reports a panic and yields a zero-sized load.
LoadKind DecodeIntegerLoad(UGeckoInstruction inst)
{
  if (inst.OPCD != 31)
  {
    switch (inst.OPCD & ~1u)
    {
    case 32:  // lwz, lwzu
      return {32, false, false};
    case 34:  // lbz, lbzu
      return {8, false, false};
    case 40:  // lhz, lhzu
      return {16, false, false};
    case 42:  // lha, lhau
      return {16, true, false};
    default:
      break;
    }
  }
  else
  {
    switch (inst.SUBOP10)
    {
    case 23:   // lwzx
    case 55:   // lwzux
      return {32, false, false};
    case 534:  // lwbrx
      return {32, false, true};
    case 87:   // lbzx
    case 119:  // lbzux
      return {8, false, false};
    case 279:  // lhzx
    case 311:  // lhzux
      return {16, false, false};
    case 790:  // lhbrx
      return {16, false, true};
    case 343:  // lhax
    case 375:  // lhaux
      return {16, true, false};
    default:
      break;
    }
  }
  PanicAlertFmt("Invalid instruction");
  return {};
}

// Store widths, by primary opcode (update forms are the odd neighbours).
int IntegerStoreBits(u32 opcd)
{
  switch (opcd & ~1u)
  {
  case 36:  // stw
    return 32;
  case 44:  // sth
    return 16;
  case 38:  // stb
    return 8;
  default:
    return 0;
  }
}

// Store widths for the indexed forms, by extended opcode without the
// update bit.
int IndexedStoreBits(u32 subop)
{
  switch (subop & ~32u)
  {
  case 151:  // stwx
  case 662:  // stwbrx
    return 32;
  case 407:  // sthx
  case 918:  // sthbrx
    return 16;
  case 215:  // stbx
    return 8;
  default:
    return -1;
  }
}

u32 SignedImmediate(UGeckoInstruction inst)
{
  return static_cast<u32>(static_cast<s32>(inst.SIMM_16));
}
}  // namespace

void Jit64::lXXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 d = inst.RD;
  const bool indexed = inst.OPCD == 31;

  // Per-form debug switches.
  FALLBACK_IF(bJITLoadStorelbzxOff && indexed && inst.SUBOP10 == 87);
  FALLBACK_IF(bJITLoadStorelXzOff && (inst.OPCD == 34 || inst.OPCD == 40 || inst.OPCD == 32));
  FALLBACK_IF(bJITLoadStorelwzOff && inst.OPCD == 32);

  LoadKind kind = DecodeIntegerLoad(inst);

  FlushRegistersBeforeSlowAccess();

  // lbz followed by extsb of the same register is a single sign-extending
  // byte load on x86.
  if (CanMergeNextInstructions(1) && kind.bits == 8)
  {
    const UGeckoInstruction next = js.op[1].inst;
    if (next.OPCD == 31 && next.SUBOP10 == 954 && next.RS == d && next.RA == d && !next.Rc)
    {
      js.downcountAmount++;
      js.skipInstructions = 1;
      kind.sign_extend = true;
    }
  }

  // An update with a zero displacement (or a known-zero index) is the same
  // as the plain form.
  bool update;
  if (indexed)
    update = (inst.SUBOP10 & 0x20) != 0 && (!gpr.IsImm(b) || gpr.Imm32(b) != 0);
  else
    update = (inst.OPCD & 1) != 0 && inst.SIMM_16 != 0;

  RCX64Reg Rd = jo.memcheck ? gpr.RevertableBind(d, RCMode::Write) : gpr.Bind(d, RCMode::Write);

  RCOpArg address;
  s32 displacement = 0;
  // The address is formed in RSCRATCH2, and copied to rA afterwards for an
  // update, so that a faulting load leaves rA untouched.
  bool address_in_scratch = false;

  if (a == 0 && !update)
  {
    address = indexed ? gpr.BindOrImm(b, RCMode::Read) : RCOpArg::Imm32(SignedImmediate(inst));
  }
  else if (update && (a == 0 || a == d))
  {
    PanicAlertFmt("Invalid instruction");
  }
  else if (!jo.memcheck && gpr.IsImm(a) && (!indexed || gpr.IsImm(b)))
  {
    // Both parts are known.
    const u32 ea = gpr.Imm32(a) + (indexed ? gpr.Imm32(b) : SignedImmediate(inst));
    address = RCOpArg::Imm32(ea);
    if (update)
      gpr.SetImmediate32(a, ea);
  }
  else
  {
    // A known index is handled like a displacement.
    const bool constant_index = !indexed || gpr.IsImm(b);
    s32 offset = 0;
    if (constant_index)
      offset = indexed ? gpr.SImm32(b) : static_cast<s32>(inst.SIMM_16);
    RCOpArg Rb = constant_index ? RCOpArg{} : gpr.Use(b, RCMode::Read);

    if (!jo.memcheck && (update || constant_index))
    {
      // Without memchecks rA can be updated in place before the load.
      address = gpr.Bind(a, update ? RCMode::ReadWrite : RCMode::Read);
      RegCache::Realize(address, Rb);
      if (!constant_index)
        ADD(32, address, Rb);
      else if (update)
        ADD(32, address, Imm32(static_cast<u32>(offset)));
      else
        displacement = offset;
    }
    else
    {
      address_in_scratch = true;
      address = RCOpArg::R(RSCRATCH2);
      RCOpArg Ra = gpr.Use(a, RCMode::Read);
      RegCache::Realize(address, Ra, Rb);
      if (constant_index)
        MOV_sum(32, RSCRATCH2, Ra, Imm32(static_cast<u32>(offset)));
      else
        MOV_sum(32, RSCRATCH2, Ra, Rb);
    }
  }

  const bool copy_back = update && address_in_scratch;
  RCX64Reg Ra = copy_back ? gpr.Bind(a, RCMode::ReadWrite) : RCX64Reg{};
  RegCache::Realize(address, Ra, Rd);

  BitSet32 live = CallerSavedRegistersInUse();
  if (copy_back)
    live[RSCRATCH2] = true;

  SafeLoadToReg(Rd, address, kind.bits, displacement, live, kind.sign_extend);

  if (copy_back)
    MOV(32, Ra, address);

  if (kind.byte_reversed)
    BSWAP(kind.bits, Rd);
}

void Jit64::dcbx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);
  FALLBACK_IF(m_accurate_cpu_cache_enabled);

  // Recognizes the cache-flush loop
  //     dcbX  0, rB
  //     addi  rB, rB, 32
  //     bdnz  .-8
  // and runs as many iterations here as the downcount allows.
  const auto is_flush_loop = [&] {
    if (inst.RA != 0 || inst.RB == 0 || !CanMergeNextInstructions(2))
      return false;
    const UGeckoInstruction addi = js.op[1].inst;
    return (addi.hex & 0xfc00'ffff) == 0x38000020 && addi.RA_6 == inst.RB &&
           addi.RD_2 == inst.RB && js.op[2].inst.hex == 0x4200fff8;
  };
  const bool batch = is_flush_loop();

  RCOpArg Ra = inst.RA ? gpr.Use(inst.RA, RCMode::Read) : RCOpArg::Imm32(0);
  RCX64Reg Rb = gpr.Bind(inst.RB, batch ? RCMode::ReadWrite : RCMode::Read);
  RegCache::Realize(Ra, Rb);

  // With `batch`, RSCRATCH2 ends up as the number of extra iterations done
  // here. CTR and the downcount are reduced by that many iterations so that
  // the real addi and bdnz, which still run, finish the last one.
  RCX64Reg lines;
  if (batch)
  {
    RCX64Reg per_iteration = gpr.Scratch();
    RCX64Reg remaining = gpr.Scratch();
    lines = gpr.Scratch();
    RegCache::Realize(per_iteration, remaining, lines);

    // DIV writes RAX and RDX.
    static_assert(RSCRATCH == Gen::EAX && RSCRATCH2 == Gen::EDX);

    const u8 cycles = static_cast<u8>(js.op[0].opinfo->num_cycles + js.op[1].opinfo->num_cycles +
                                      js.op[2].opinfo->num_cycles);

    // Zero extra iterations when out of cycles; also the high half for DIV.
    XOR(32, R(RSCRATCH2), R(RSCRATCH2));
    MOV(32, R(RSCRATCH), PPCSTATE(downcount));
    TEST(32, R(RSCRATCH), R(RSCRATCH));
    const FixupBranch no_cycles = J_CC(CC_LE);

    MOV(32, R(lines), PPCSTATE_CTR);
    MOV(32, R(remaining), R(RSCRATCH));
    MOV(32, R(per_iteration), Imm32(cycles));
    DIV(32, R(per_iteration));
    // extra = min(downcount / cycles, CTR - 1), unsigned, so CTR == 0
    // (2^32 iterations) is handled too.
    LEA(32, RSCRATCH2, MDisp(lines, -1));
    CMP(32, R(RSCRATCH), R(RSCRATCH2));
    CMOVcc(32, RSCRATCH2, R(RSCRATCH), CC_B);

    SUB(32, R(lines), R(RSCRATCH2));
    MOV(32, PPCSTATE_CTR, R(lines));
    // extra * cycles <= downcount, so this cannot overflow.
    IMUL(32, per_iteration, R(RSCRATCH2));
    SUB(32, R(remaining), R(per_iteration));
    MOV(32, PPCSTATE(downcount), R(remaining));

    SetJumpTarget(no_cycles);

    LEA(32, lines, MDisp(RSCRATCH2, 1));

    if (IsBranchWatchEnabled())
    {
      // Report the extra bdnz iterations as taken branches.
      const BitSet32 keep = (CallerSavedRegistersInUse() | BitSet32{RSCRATCH2}) &
                            ~BitSet32{int(per_iteration), int(remaining)};
      static_assert(RSCRATCH2 != ABI_PARAM1);
      const PPCAnalyst::CodeOp& bdnz = js.op[2];

      ABI_PushRegistersAndAdjustStack(keep, 0);
      MOV(64, R(ABI_PARAM1), ImmPtr(&m_branch_watch));
      // RSCRATCH2 is ABI_PARAM2 on Windows and ABI_PARAM3 elsewhere, so it
      // is read first.
      MOV(32, R(ABI_PARAM4), R(RSCRATCH2));
      MOV(64, R(ABI_PARAM2), Imm64(Core::FakeBranchWatchCollectionKey{bdnz.address, bdnz.branchTo}));
      MOV(32, R(ABI_PARAM3), Imm32(bdnz.inst.hex));
      if (m_ppc_state.msr.IR)
        ABI_CallFunction(&Core::BranchWatch::HitVirtualTrue_fk_n);
      else
        ABI_CallFunction(&Core::BranchWatch::HitPhysicalTrue_fk_n);
      ABI_PopRegistersAndAdjustStack(keep, 0);
    }
  }

  const X64Reg ea = RSCRATCH;
  MOV_sum(32, ea, Ra, Rb);

  if (batch)
  {
    // rB advances by 32 per extra iteration; the real addi adds the last.
    SHL(32, R(RSCRATCH2), Imm8(5));
    ADD(32, R(Rb), R(RSCRATCH2));
  }

  const X64Reg tmp = RSCRATCH2;
  RCX64Reg physical = gpr.Scratch();
  RegCache::Realize(physical);

  const bool translate = (m_ppc_state.feature_flags & FEATURE_FLAG_MSR_IR) != 0;
  FixupBranch untranslated;
  const u8* const next_line = GetCodePtr();
  MOV(32, R(physical), R(ea));
  if (translate)
  {
    untranslated = BATAddressLookup(physical, tmp, m_mmu.GetIBATTable().data());
    MOV(32, R(tmp), R(ea));
    AND(32, R(tmp), Imm32(PowerPC::BAT_PAGE_SIZE - 1));
    AND(32, R(physical), Imm32(~(PowerPC::BAT_PAGE_SIZE - 1)));
    OR(32, R(physical), R(tmp));
  }

  // One bit per 32-byte line, 32 lines per word of the bitset: invalidate
  // only when a block was compiled from this line.
  SHR(32, R(physical), Imm8(10));
  MOV(64, R(tmp), ImmPtr(GetBlockCache()->GetBlockBitSet()));
  MOV(32, R(physical), MComplex(tmp, physical, SCALE_4, 0));
  MOV(32, R(tmp), R(ea));
  SHR(32, R(tmp), Imm8(5));
  BT(32, R(physical), R(tmp));
  const FixupBranch has_code = J_CC(CC_C, Jump::Near);

  if (batch)
  {
    ADD(32, R(ea), Imm8(32));
    SUB(32, R(lines), Imm8(1));
    J_CC(CC_NZ, next_line);
  }

  SwitchToFarCode();
  SetJumpTarget(has_code);
  if (translate)
    SetJumpTarget(untranslated);

  BitSet32 live = CallerSavedRegistersInUse();
  live[X64Reg(tmp)] = false;
  live[X64Reg(ea)] = false;
  live[X64Reg(physical)] = false;
  if (batch)
    live[X64Reg(lines)] = false;

  ABI_PushRegistersAndAdjustStack(live, 0);
  if (batch)
  {
    ABI_CallFunctionPRR(JitInterface::InvalidateICacheLinesFromJIT, &m_system.GetJitInterface(), ea,
                        lines);
  }
  else
  {
    ABI_CallFunctionPR(JitInterface::InvalidateICacheLineFromJIT, &m_system.GetJitInterface(), ea);
  }
  ABI_PopRegistersAndAdjustStack(live, 0);
  // The invalidation may have freed the block that is running.
  asm_routines.ResetStack(*this);

  const FixupBranch back = J(Jump::Near);
  SwitchToNearCode();
  SetJumpTarget(back);
}

void Jit64::dcbt(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  // The data cache is not emulated, so the touch itself is a no-op. A dcbst
  // of the same line right after it is a prefetch idiom rather than code
  // being written, and its block-cache invalidation is skipped.
  if (!CanMergeNextInstructions(1))
    return;
  const UGeckoInstruction next = js.op[1].inst;
  if (next.OPCD == 31 && next.SUBOP10 == 54 && next.RA == inst.RA && next.RB == inst.RB)
    js.skipInstructions = 1;
}

void Jit64::dcbz(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  const u32 a = inst.RA;
  const u32 b = inst.RB;

  FlushRegistersBeforeSlowAccess();

  // RSCRATCH = (rA|0 + rB) rounded down to the 32-byte line.
  {
    RCOpArg Ra = a != 0 ? gpr.Use(a, RCMode::Read) : RCOpArg::Imm32(0);
    RCOpArg Rb = gpr.Use(b, RCMode::Read);
    RegCache::Realize(Ra, Rb);
    MOV_sum(32, RSCRATCH, Ra, Rb);
    AND(32, R(RSCRATCH), Imm32(~31u));
  }

  // Optional game hack: leave [0x80000000, 0x80008000) alone.
  FixupBranch skip_low;
  if (m_low_dcbz_hack)
  {
    CMP(32, R(RSCRATCH), Imm32(0x8000'8000));
    skip_low = J_CC(CC_L);
  }

  const bool inline_zero = (m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR) &&
                           jo.fastmem_arena && !m_accurate_cpu_cache_enabled;

  if (inline_zero)
  {
    // Zero the line directly when the DBAT maps it to RAM.
    MOV(64, R(RSCRATCH2), ImmPtr(m_mmu.GetDBATTable().data()));
    PUSH(RSCRATCH);
    SHR(32, R(RSCRATCH), Imm8(PowerPC::BAT_INDEX_SHIFT));
    TEST(32, MComplex(RSCRATCH2, RSCRATCH, SCALE_4, 0), Imm32(PowerPC::BAT_PHYSICAL_BIT));
    POP(RSCRATCH);
    const FixupBranch not_ram = J_CC(CC_Z, Jump::Near);

    const OpArg line = MComplex(RMEM, RSCRATCH, SCALE_1, 0);
    if (cpu_info.bAVX)
    {
      VXORPS(XMM0, XMM0, R(XMM0));
      VMOVAPS(line, YMM0);
      VZEROUPPER();
    }
    else
    {
      XORPS(XMM0, R(XMM0));
      MOVAPS(line, XMM0);
      MOVAPS(MComplex(RMEM, RSCRATCH, SCALE_1, 16), XMM0);
    }

    SwitchToFarCode();
    SetJumpTarget(not_ram);
  }

  FlushPCBeforeSlowAccess();
  const BitSet32 live = CallerSavedRegistersInUse();
  ABI_PushRegistersAndAdjustStack(live, 0);
  ABI_CallFunctionPR(PowerPC::ClearDCacheLineFromJit, &m_mmu, RSCRATCH);
  ABI_PopRegistersAndAdjustStack(live, 0);

  if (inline_zero)
  {
    const FixupBranch back = J(Jump::Near);
    SwitchToNearCode();
    SetJumpTarget(back);
  }

  if (m_low_dcbz_hack)
    SetJumpTarget(skip_low);
}

void Jit64::stX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  const u32 s = inst.RS;
  const u32 a = inst.RA;
  const s32 offset = static_cast<s16>(inst.SIMM_16);
  const bool update = (inst.OPCD & 1) != 0 && offset != 0;

  if (a == 0 && update)
    PanicAlertFmt("Invalid stX");

  const int bits = IntegerStoreBits(inst.OPCD);
  if (bits == 0)
  {
    ASSERT_MSG(DYNA_REC, 0, "stX: Invalid access size.");
    return;
  }

  FlushRegistersBeforeSlowAccess();

  if (a != 0 && !gpr.IsImm(a))
  {
    RCX64Reg Ra = gpr.Bind(a, update ? RCMode::ReadWrite : RCMode::Read);
    RCOpArg value;
    if (!WriteClobbersRegValue(bits, /* swap */ true))
    {
      value = gpr.BindOrImm(s, RCMode::Read);
    }
    else if (gpr.IsImm(s))
    {
      value = RCOpArg::Imm32(gpr.Imm32(s));
    }
    else
    {
      // The swap would destroy rS, so store a copy.
      RCOpArg Rs = gpr.Use(s, RCMode::Read);
      RegCache::Realize(Rs);
      value = RCOpArg::R(RSCRATCH2);
      MOV(32, value, Rs);
    }
    RegCache::Realize(Ra, value);
    SafeWriteRegToReg(value, Ra, bits, offset, CallerSavedRegistersInUse(),
                      SAFE_LOADSTORE_CLOBBER_RSCRATCH_INSTEAD_OF_ADDR);
    if (update)
      ADD(32, Ra, Imm32(static_cast<u32>(offset)));
    return;
  }

  // The address is a constant.
  const u32 address = (a != 0 ? gpr.Imm32(a) : 0) + static_cast<u32>(offset);
  bool may_fault;
  {
    RCOpArg Rs = gpr.Use(s, RCMode::Read);
    RegCache::Realize(Rs);
    may_fault = WriteToConstAddress(bits, Rs, address, CallerSavedRegistersInUse());
  }
  if (!update)
    return;

  if (jo.memcheck && may_fault)
  {
    // rA changes only if the store went through.
    RCOpArg Ra = gpr.RevertableBind(a, RCMode::Write);
    RegCache::Realize(Ra);
    MemoryExceptionCheck();
    MOV(32, Ra, Imm32(address));
  }
  else
  {
    gpr.SetImmediate32(a, address);
  }
}

void Jit64::stXx(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  const u32 a = inst.RA;
  const u32 b = inst.RB;
  const u32 s = inst.RS;
  const bool update = (inst.SUBOP10 & 32) != 0;
  const bool byte_reverse = (inst.SUBOP10 & 512) != 0;
  FALLBACK_IF(a == 0 || (update && a == s) || (update && jo.memcheck && a == b));

  int bits = IndexedStoreBits(inst.SUBOP10);
  if (bits < 0)
  {
    PanicAlertFmt("stXx: invalid access size");
    bits = 0;
  }

  FlushRegistersBeforeSlowAccess();

  // A byte-reversed store is a plain store on x86, and vice versa.
  const bool clobbers = WriteClobbersRegValue(bits, /* swap */ !byte_reverse);

  RCOpArg Ra = update ? gpr.Bind(a, RCMode::ReadWrite) : gpr.Use(a, RCMode::Read);
  RCOpArg Rb = gpr.Use(b, RCMode::Read);
  RCOpArg Rs;
  if (!clobbers)
    Rs = gpr.BindOrImm(s, RCMode::Read);
  else if (gpr.IsImm(s))
    Rs = RCOpArg::Imm32(gpr.Imm32(s));
  else
    Rs = gpr.Use(s, RCMode::Read);
  RegCache::Realize(Ra, Rb, Rs);

  MOV_sum(32, RSCRATCH2, Ra, Rb);

  if (clobbers && !Rs.IsImm())
  {
    MOV(32, R(RSCRATCH), Rs);
    Rs = RCOpArg::R(RSCRATCH);
  }

  BitSet32 live = CallerSavedRegistersInUse();
  if (update)
    live[RSCRATCH2] = true;
  SafeWriteRegToReg(Rs, RSCRATCH2, bits, 0, live, byte_reverse ? SAFE_LOADSTORE_NO_SWAP : 0);

  if (update)
    MOV(32, Ra, R(RSCRATCH2));
}

// lmw and stmw are used by a few games' video decoders. A fault part way
// through leaves the earlier registers written, as before.
void Jit64::lmw(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  const u32 a = inst.RA;
  const u32 first = inst.RD;

  FlushRegistersBeforeSlowAccess();

  {
    RCOpArg Ra = a != 0 ? gpr.Use(a, RCMode::Read) : RCOpArg::Imm32(0);
    RegCache::Realize(Ra);
    MOV_sum(32, RSCRATCH2, Ra, Imm32(SignedImmediate(inst)));
  }

  for (u32 reg = first; reg < 32; reg++)
  {
    const s32 disp = static_cast<s32>((reg - first) * 4);
    SafeLoadToReg(RSCRATCH, R(RSCRATCH2), 32, disp,
                  CallerSavedRegistersInUse() | BitSet32{RSCRATCH2}, false);
    RCOpArg Rt = gpr.Bind(reg, RCMode::Write);
    RegCache::Realize(Rt);
    MOV(32, Rt, R(RSCRATCH));
  }
}

void Jit64::stmw(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  const u32 a = inst.RA;
  const u32 first = inst.RD;

  FlushRegistersBeforeSlowAccess();

  for (u32 reg = first; reg < 32; reg++)
  {
    RCOpArg Ra = a != 0 ? gpr.Use(a, RCMode::Read) : RCOpArg::Imm32(0);
    RCOpArg Rt = gpr.Use(reg, RCMode::Read);
    RegCache::Realize(Ra, Rt);

    // The base goes to RSCRATCH and the value to RSCRATCH2 (unless it is a
    // constant), as the store may swap or clobber both.
    if (Ra.IsZero())
      XOR(32, R(RSCRATCH), R(RSCRATCH));
    else
      MOV(32, R(RSCRATCH), Ra);
    if (!Rt.IsImm())
    {
      MOV(32, R(RSCRATCH2), Rt);
      Rt = RCOpArg::R(RSCRATCH2);
    }
    const s32 disp = static_cast<s32>((reg - first) * 4 + SignedImmediate(inst));
    SafeWriteRegToReg(Rt, RSCRATCH, 32, disp, CallerSavedRegistersInUse());
  }
}

void Jit64::eieio(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStoreOff);

  // Gather pipe checks are normally batched at the end of the block, but
  // FIFO writes must not be reordered across eieio.
  if (jo.optimizeGatherPipe && js.fifoBytesSinceCheck > 0)
    js.mustCheckFifo = true;
}
