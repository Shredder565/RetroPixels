// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64Common/EmuCodeBlock.h"

#include <array>
#include <functional>

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/FloatUtils.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Jit64/Jit.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/Jit64Common/TrampolineCache.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

using namespace Gen;

namespace
{
// Selects a host type for an access width in bits and hands a value of it to
// `fn`, so callers can pick the matching template instantiation.
template <typename Fn>
void WithAccessType(int bits, Fn&& fn)
{
  switch (bits)
  {
  case 8:
    fn(u8{});
    break;
  case 16:
    fn(u16{});
    break;
  case 32:
    fn(u32{});
    break;
  case 64:
    fn(u64{});
    break;
  default:
    break;
  }
}

// The guest is big-endian, so an immediate stored without a swapping
// instruction has to be byte-reversed up front.
OpArg ByteReversedImm(int bits, const OpArg& imm)
{
  switch (bits)
  {
  case 32:
    return Imm32(Common::swap32(imm.Imm32()));
  case 16:
    return Imm16(Common::swap16(imm.Imm16()));
  default:
    return Imm8(imm.Imm8());
  }
}

// Gives an immediate operand the width of the access.
OpArg ResizeImm(int bits, const OpArg& arg)
{
  if (!arg.IsImm())
    return arg;
  if (bits == 8)
    return arg.AsImm8();
  if (bits == 16)
    return arg.AsImm16();
  return arg.AsImm32();
}

// The C++ store accessor for a host type. The swap variants take a value that
// is already in guest byte order.
template <typename T>
auto StoreAccessor(bool swap)
{
  if constexpr (std::is_same_v<T, u8>)
    return &PowerPC::WriteFromJit<u8>;
  else if constexpr (std::is_same_v<T, u16>)
    return swap ? &PowerPC::WriteFromJit<u16> : &PowerPC::WriteU16SwapFromJit;
  else if constexpr (std::is_same_v<T, u32>)
    return swap ? &PowerPC::WriteFromJit<u32> : &PowerPC::WriteU32SwapFromJit;
  else
    return swap ? &PowerPC::WriteFromJit<u64> : &PowerPC::WriteU64SwapFromJit;
}

// Masks for the Gekko's single-precision multiply operand rounding.
alignas(16) constexpr std::array<u64, 2> MANTISSA_KEEP_HIGH = {0xFFFFFFFFF8000000ULL,
                                                               0xFFFFFFFFF8000000ULL};
alignas(16) constexpr std::array<u64, 2> MANTISSA_ROUND_BIT = {0x8000000ULL, 0x8000000ULL};

// Clears the quiet bit of a double in the low lane; the high lane is kept.
alignas(16) constexpr std::array<u64, 2> DOUBLE_QUIET_CLEAR = {0xFFF7FFFFFFFFFFFFULL,
                                                               0xFFFFFFFFFFFFFFFFULL};

// Low-lane field masks used to classify a value for FPRF.
alignas(16) constexpr std::array<u64, 2> DOUBLE_EXP_LANE = {Common::DOUBLE_EXP, 0};
alignas(16) constexpr std::array<u64, 2> DOUBLE_FRAC_LANE = {Common::DOUBLE_FRAC, 0};
alignas(16) constexpr std::array<u64, 2> DOUBLE_MAGNITUDE_LANE = {~Common::DOUBLE_SIGN, 0};
alignas(16) constexpr std::array<u32, 4> FLOAT_EXP_LANE = {Common::FLOAT_EXP, 0, 0, 0};
alignas(16) constexpr std::array<u32, 4> FLOAT_FRAC_LANE = {Common::FLOAT_FRAC, 0, 0, 0};
alignas(16) constexpr std::array<u32, 4> FLOAT_MAGNITUDE_LANE = {~Common::FLOAT_SIGN, 0, 0, 0};

// Emits code that reads a constant-address MMIO register according to the
// way its handler is implemented.
template <typename T>
class MmioReadEmitter final : public MMIO::ReadHandlingMethodVisitor<T>
{
public:
  static constexpr int BITS = static_cast<int>(sizeof(T) * 8);

  MmioReadEmitter(Core::System& system, X64CodeBlock& code, BitSet32 live, X64Reg dst, u32 address,
                  bool sign_extend)
      : m_system(system), m_code(code), m_live(live), m_dst(dst), m_address(address),
        m_sign_extend(sign_extend)
  {
  }

  void VisitConstant(T value) override
  {
    // The value is known now; materialise it already extended.
    u32 extended = value;
    if (m_sign_extend && BITS < 32)
      extended = static_cast<u32>(static_cast<s32>(static_cast<std::make_signed_t<T>>(value)));
    m_code.MOV(32, R(m_dst), Imm32(extended));
  }

  void VisitDirect(const T* addr, u32 mask) override
  {
    constexpr u32 width_mask = BITS >= 32 ? 0xFFFFFFFFu : static_cast<u32>((1ULL << BITS) - 1);
    m_code.MOV(64, R(RSCRATCH), ImmPtr(addr));
    if ((mask & width_mask) == width_mask)
    {
      // Nothing to mask off, so the extension can happen in the load.
      Extend(MatR(RSCRATCH));
      return;
    }
    m_code.MOVZX(32, BITS, m_dst, MatR(RSCRATCH));
    m_code.AND(32, R(m_dst), Imm32(mask));
    if (m_sign_extend)
      m_code.MOVSX(32, BITS, m_dst, R(m_dst));
  }

  void VisitComplex(const std::function<T(Core::System&, u32)>* lambda) override
  {
    m_code.ABI_PushRegistersAndAdjustStack(m_live, 0);
    m_code.ABI_CallLambdaPC(lambda, &m_system, m_address);
    m_code.ABI_PopRegistersAndAdjustStack(m_live, 0);
    Extend(R(ABI_RETURN));
  }

private:
  void Extend(const OpArg& src)
  {
    if (m_sign_extend)
      m_code.MOVSX(32, BITS, m_dst, src);
    else
      m_code.MOVZX(32, BITS, m_dst, src);
  }

  Core::System& m_system;
  X64CodeBlock& m_code;
  BitSet32 m_live;
  X64Reg m_dst;
  u32 m_address;
  bool m_sign_extend;
};
}  // namespace

void EmuCodeBlock::MemoryExceptionCheck()
{
  auto& js = m_jit.js;

  if (js.generatingTrampoline)
  {
    // The block already has an exception exit for this access, emitted after
    // the original fastmem instruction; reuse it.
    if (js.trampolineExceptionHandler != nullptr)
    {
      TEST(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_DSI));
      J_CC(CC_NZ, js.trampolineExceptionHandler);
    }
    return;
  }

  // With memchecks on, the first slow access of an instruction gets a branch
  // that the instruction epilogue later points at its exception exit.
  const bool needs_branch = m_jit.jo.memcheck && js.fastmemLoadStore == nullptr &&
                            !js.fixupExceptionHandler;
  if (!needs_branch)
    return;

  TEST(32, PPCSTATE(Exceptions), Imm32(EXCEPTION_DSI));
  js.exceptionHandler = J_CC(CC_NZ, Jump::Near);
  js.fixupExceptionHandler = true;
}

// This block's emitter is the one cursor every helper writes through; far code
// is emitted by pointing it into m_far_code and parking the near position.
void EmuCodeBlock::SwitchToFarCode()
{
  u8* const near_ptr = GetWritableCodePtr();
  u8* const near_end = GetWritableCodeEnd();
  const bool near_failed = HasWriteFailed();

  SetCodePtr(m_far_code.GetWritableCodePtr(), m_far_code.GetWritableCodeEnd(),
             m_far_code.HasWriteFailed());

  m_near_code = near_ptr;
  m_near_code_end = near_end;
  m_near_code_write_failed = near_failed;
}

void EmuCodeBlock::SwitchToNearCode()
{
  // Hand the far position back to m_far_code before resuming near code.
  u8* const far_ptr = GetWritableCodePtr();
  m_far_code.SetCodePtr(far_ptr, GetWritableCodeEnd(), HasWriteFailed());
  SetCodePtr(m_near_code, m_near_code_end, m_near_code_write_failed);
}

void EmuCodeBlock::FlushPCBeforeSlowAccess()
{
  // The C++ accessors read PC for memory breakpoints, gather pipe interrupt
  // placement and log messages.
  MOV(32, PPCSTATE(pc), Imm32(m_jit.js.compilerPC));
}

FixupBranch EmuCodeBlock::BATAddressLookup(X64Reg addr, X64Reg tmp, const void* bat_table)
{
  constexpr int mapped_bit = MathUtil::IntLog2(PowerPC::BAT_MAPPED_BIT);
  MOV(64, R(tmp), ImmPtr(bat_table));
  SHR(32, R(addr), Imm8(PowerPC::BAT_INDEX_SHIFT));
  MOV(32, R(addr), MComplex(tmp, addr, SCALE_4, 0));
  BT(32, R(addr), Imm8(static_cast<u8>(mapped_bit)));
  return J_CC(CC_NC, m_far_code.Enabled() ? Jump::Near : Jump::Short);
}

FixupBranch EmuCodeBlock::CheckIfSafeAddress(const OpArg& reg_value, X64Reg reg_addr,
                                             BitSet32 registers_in_use)
{
  // The operands must survive too, so treat them as live.
  registers_in_use[reg_addr] = true;
  if (reg_value.IsSimpleReg())
    registers_in_use[reg_value.GetSimpleReg()] = true;

  const bool keep_scratch = registers_in_use[RSCRATCH];
  const bool keep_extra = registers_in_use[RSCRATCH_EXTRA];
  if (keep_scratch)
    PUSH(RSCRATCH);
  if (keep_extra)
    PUSH(RSCRATCH_EXTRA);

  // Look the page up in the data BAT table; the physical bit says whether the
  // fastmem arena backs it.
  if (reg_addr != RSCRATCH_EXTRA)
    MOV(32, R(RSCRATCH_EXTRA), R(reg_addr));
  MOV(64, R(RSCRATCH), ImmPtr(m_jit.m_mmu.GetDBATTable().data()));
  SHR(32, R(RSCRATCH_EXTRA), Imm8(PowerPC::BAT_INDEX_SHIFT));
  TEST(32, MComplex(RSCRATCH, RSCRATCH_EXTRA, SCALE_4, 0), Imm32(PowerPC::BAT_PHYSICAL_BIT));

  // POP leaves the flags alone.
  if (keep_extra)
    POP(RSCRATCH_EXTRA);
  if (keep_scratch)
    POP(RSCRATCH);

  return J_CC(CC_Z, m_far_code.Enabled() ? Jump::Near : Jump::Short);
}

void EmuCodeBlock::UnsafeWriteRegToReg(OpArg reg_value, X64Reg reg_addr, int accessSize, s32 offset,
                                       bool swap, MovInfo* info)
{
  if (info != nullptr)
  {
    info->address = GetWritableCodePtr();
    info->nonAtomicSwapStore = false;
  }

  const OpArg dest = MComplex(RMEM, reg_addr, SCALE_1, offset);
  if (!swap)
    MOV(accessSize, dest, reg_value);
  else if (reg_value.IsImm())
    MOV(accessSize, dest, ByteReversedImm(accessSize, reg_value));
  else
    SwapAndStore(accessSize, dest, reg_value.GetSimpleReg(), info);
}

void EmuCodeBlock::UnsafeWriteRegToReg(X64Reg reg_value, X64Reg reg_addr, int accessSize,
                                       s32 offset, bool swap, MovInfo* info)
{
  UnsafeWriteRegToReg(R(reg_value), reg_addr, accessSize, offset, swap, info);
}

bool EmuCodeBlock::UnsafeLoadToReg(X64Reg reg_value, OpArg opAddress, int accessSize, s32 offset,
                                   bool signExtend, MovInfo* info)
{
  if (opAddress.IsImm())
  {
    MOV(32, R(reg_value), Imm32(opAddress.Imm32() + static_cast<u32>(offset)));
    LoadAndSwap(accessSize, reg_value, MRegSum(RMEM, reg_value), signExtend, info);
    return false;
  }

  if (!opAddress.IsSimpleReg())
  {
    MOV(32, R(reg_value), opAddress);
    LoadAndSwap(accessSize, reg_value, MComplex(RMEM, reg_value, SCALE_1, offset), signExtend,
                info);
    return false;
  }

  X64Reg base = opAddress.GetSimpleReg();
  bool offset_folded = false;
  if (static_cast<u32>(offset) >= 0x1000)
  {
    // A negative or large displacement could step outside the arena when the
    // guest address wraps, so form the 32-bit address first. If that
    // overwrites the address register, the backpatcher has to undo it.
    offset_folded = base == reg_value;
    LEA(32, reg_value, MDisp(base, offset));
    base = reg_value;
    offset = 0;
  }
  LoadAndSwap(accessSize, reg_value, MComplex(RMEM, base, SCALE_1, offset), signExtend, info);
  return offset_folded;
}

void EmuCodeBlock::MMIOLoadToReg(MMIO::Mapping* mmio, X64Reg reg_value, BitSet32 registers_in_use,
                                 u32 address, int access_size, bool sign_extend)
{
  WithAccessType(access_size, [&](auto type) {
    using T = decltype(type);
    if constexpr (!std::is_same_v<T, u64>)
    {
      MmioReadEmitter<T> emitter(m_jit.m_system, *this, registers_in_use, reg_value, address,
                                 sign_extend);
      mmio->GetHandlerForRead<T>(address).Visit(emitter);
    }
  });
}

bool EmuCodeBlock::CanInlineBATCheck(int flags) const
{
  if ((flags & SAFE_LOADSTORE_FORCE_SLOW_ACCESS) != 0 || !m_jit.jo.fastmem_arena)
    return false;
  if (m_jit.m_ppc_state.m_enable_dcache)
    return false;
  return (flags & SAFE_LOADSTORE_DR_ON) != 0 ||
         (m_jit.m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR) != 0;
}

FixupBranch EmuCodeBlock::BeginSlowPath(FixupBranch to_slow)
{
  FixupBranch skip{};
  if (m_far_code.Enabled())
    SwitchToFarCode();
  else
    skip = J(Jump::Near);
  SetJumpTarget(to_slow);
  return skip;
}

void EmuCodeBlock::EndSlowPath(FixupBranch skip)
{
  if (m_far_code.Enabled())
  {
    skip = J(Jump::Near);
    SwitchToNearCode();
  }
  SetJumpTarget(skip);
}

void EmuCodeBlock::RecordBackpatch(const MovInfo& mov, TrampolineInfo info)
{
  const ptrdiff_t emitted = GetCodePtr() - info.start;
  if (emitted < BACKPATCH_SIZE)
    NOP(static_cast<size_t>(BACKPATCH_SIZE - emitted));
  info.len = static_cast<u16>(GetCodePtr() - info.start);
  info.nonAtomicSwapStoreSrc = mov.nonAtomicSwapStore ? mov.nonAtomicSwapStoreSrc : INVALID_REG;
  info.pc = m_jit.js.compilerPC;

  m_back_patch_info[mov.address] = info;
  m_jit.js.fastmemLoadStore = mov.address;
}

void EmuCodeBlock::TakeReadResult(X64Reg reg_value, int accessSize, bool signExtend)
{
  // The accessors return zero-extended values.
  if (signExtend && accessSize < 32)
    MOVSX(32, accessSize, reg_value, R(ABI_RETURN));
  else if (reg_value != ABI_RETURN)
    MOVZX(64, accessSize, reg_value, R(ABI_RETURN));
}

void EmuCodeBlock::SafeLoadToReg(X64Reg reg_value, const OpArg& opAddress, int accessSize,
                                 s32 offset, BitSet32 registersInUse, bool signExtend, int flags)
{
  // The destination is overwritten anyway, so it never needs saving.
  registersInUse[reg_value] = false;

  constexpr int no_fastmem_flags = SAFE_LOADSTORE_NO_FASTMEM | SAFE_LOADSTORE_NO_UPDATE_PC |
                                   SAFE_LOADSTORE_FORCE_SLOW_ACCESS;
  if (m_jit.jo.fastmem && (flags & no_fastmem_flags) == 0)
  {
    TrampolineInfo info;
    info.start = GetWritableCodePtr();
    MovInfo mov;
    info.offsetAddedToAddress =
        UnsafeLoadToReg(reg_value, opAddress, accessSize, offset, signExtend, &mov);
    info.read = true;
    info.signExtend = signExtend;
    info.op_reg = reg_value;
    info.op_arg = opAddress;
    info.accessSize = static_cast<u8>(accessSize / 8);
    info.offset = offset;
    info.registersInUse = registersInUse;
    info.flags = static_cast<u8>(flags);
    RecordBackpatch(mov, info);
    return;
  }

  if (opAddress.IsImm())
  {
    SafeLoadToRegImmediate(reg_value, opAddress.Imm32() + static_cast<u32>(offset), accessSize,
                           registersInUse, signExtend);
    return;
  }

  ASSERT_MSG(DYNA_REC, opAddress.IsSimpleReg(),
             "Incorrect use of SafeLoadToReg (address isn't register or immediate)");
  X64Reg address_reg = opAddress.GetSimpleReg();
  if (offset != 0)
  {
    LEA(32, RSCRATCH, MDisp(address_reg, offset));
    address_reg = RSCRATCH;
  }

  const bool inline_check = CanInlineBATCheck(flags);
  FixupBranch skip_slow{};
  if (inline_check)
  {
    const FixupBranch to_slow = CheckIfSafeAddress(R(reg_value), address_reg, registersInUse);
    UnsafeLoadToReg(reg_value, R(address_reg), accessSize, 0, signExtend);
    skip_slow = BeginSlowPath(to_slow);
  }

  // Shared asm routines cannot know the PC; their callers store it.
  if ((flags & SAFE_LOADSTORE_NO_UPDATE_PC) == 0)
    FlushPCBeforeSlowAccess();

  const size_t stack_offset = (flags & SAFE_LOADSTORE_NO_PROLOG) != 0 ? 8 : 0;
  ABI_PushRegistersAndAdjustStack(registersInUse, stack_offset);
  WithAccessType(accessSize, [&](auto type) {
    using T = decltype(type);
    ABI_CallFunctionPR(&PowerPC::ReadFromJit<T>, &m_jit.m_mmu, address_reg);
  });
  ABI_PopRegistersAndAdjustStack(registersInUse, stack_offset);

  MemoryExceptionCheck();
  TakeReadResult(reg_value, accessSize, signExtend);

  if (inline_check)
    EndSlowPath(skip_slow);
}

void EmuCodeBlock::SafeLoadToRegImmediate(X64Reg reg_value, u32 address, int accessSize,
                                          BitSet32 registersInUse, bool signExtend)
{
  // Plain RAM: a direct load cannot fault.
  if (m_jit.jo.fastmem_arena && m_jit.m_mmu.IsOptimizableRAMAddress(address, static_cast<u32>(accessSize)))
  {
    UnsafeLoadToReg(reg_value, Imm32(address), accessSize, 0, signExtend);
    return;
  }

  // A hardware register: inline its handler.
  if (accessSize != 64)
  {
    const u32 mmio_address = m_jit.m_mmu.IsOptimizableMMIOAccess(address, static_cast<u32>(accessSize));
    if (mmio_address != 0)
    {
      MMIOLoadToReg(m_jit.m_system.GetMemory().GetMMIOMapping(), reg_value, registersInUse,
                    mmio_address, accessSize, signExtend);
      return;
    }
  }

  FlushPCBeforeSlowAccess();
  ABI_PushRegistersAndAdjustStack(registersInUse, 0);
  WithAccessType(accessSize, [&](auto type) {
    using T = decltype(type);
    ABI_CallFunctionPC(&PowerPC::ReadFromJit<T>, &m_jit.m_mmu, address);
  });
  ABI_PopRegistersAndAdjustStack(registersInUse, 0);

  MemoryExceptionCheck();
  TakeReadResult(reg_value, accessSize, signExtend);
}

void EmuCodeBlock::SafeWriteRegToReg(OpArg reg_value, X64Reg reg_addr, int accessSize, s32 offset,
                                     BitSet32 registersInUse, int flags)
{
  const bool swap = (flags & SAFE_LOADSTORE_NO_SWAP) == 0;
  reg_value = ResizeImm(accessSize, reg_value);

  constexpr int no_fastmem_flags = SAFE_LOADSTORE_NO_FASTMEM | SAFE_LOADSTORE_NO_UPDATE_PC |
                                   SAFE_LOADSTORE_FORCE_SLOW_ACCESS;
  if (m_jit.jo.fastmem && (flags & no_fastmem_flags) == 0)
  {
    TrampolineInfo info;
    info.start = GetWritableCodePtr();
    MovInfo mov;
    UnsafeWriteRegToReg(reg_value, reg_addr, accessSize, offset, swap, &mov);
    info.read = false;
    info.op_arg = reg_value;
    info.op_reg = reg_addr;
    info.accessSize = static_cast<u8>(accessSize / 8);
    info.offset = offset;
    info.registersInUse = registersInUse;
    info.flags = static_cast<u8>(flags);
    RecordBackpatch(mov, info);
    return;
  }

  if (offset != 0)
  {
    if ((flags & SAFE_LOADSTORE_CLOBBER_RSCRATCH_INSTEAD_OF_ADDR) != 0)
    {
      LEA(32, RSCRATCH, MDisp(reg_addr, offset));
      reg_addr = RSCRATCH;
    }
    else
    {
      ADD(32, R(reg_addr), Imm32(static_cast<u32>(offset)));
    }
  }

  const bool inline_check = CanInlineBATCheck(flags);
  FixupBranch skip_slow{};
  if (inline_check)
  {
    const FixupBranch to_slow = CheckIfSafeAddress(reg_value, reg_addr, registersInUse);
    UnsafeWriteRegToReg(reg_value, reg_addr, accessSize, 0, swap);
    skip_slow = BeginSlowPath(to_slow);
  }

  if ((flags & SAFE_LOADSTORE_NO_UPDATE_PC) == 0)
    FlushPCBeforeSlowAccess();

  const size_t stack_offset = (flags & SAFE_LOADSTORE_NO_PROLOG) != 0 ? 8 : 0;
  ABI_PushRegistersAndAdjustStack(registersInUse, stack_offset);

  // The accessor takes the value in a register; an immediate goes into the
  // first parameter register unless the address already occupies it.
  X64Reg value_reg;
  if (reg_value.IsImm())
  {
    value_reg = reg_addr == ABI_PARAM1 ? RSCRATCH : ABI_PARAM1;
    MOV(accessSize, R(value_reg), reg_value);
  }
  else
  {
    value_reg = reg_value.GetSimpleReg();
  }

  WithAccessType(accessSize, [&](auto type) {
    using T = decltype(type);
    ABI_CallFunctionPRR(StoreAccessor<T>(swap), &m_jit.m_mmu, value_reg, reg_addr);
  });
  ABI_PopRegistersAndAdjustStack(registersInUse, stack_offset);

  MemoryExceptionCheck();

  if (inline_check)
    EndSlowPath(skip_slow);
}

void EmuCodeBlock::SafeWriteRegToReg(X64Reg reg_value, X64Reg reg_addr, int accessSize,
                                     s32 offset, BitSet32 registersInUse, int flags)
{
  SafeWriteRegToReg(R(reg_value), reg_addr, accessSize, offset, registersInUse, flags);
}

bool EmuCodeBlock::WriteClobbersRegValue(int accessSize, bool swap)
{
  // Without MOVBE a swapping store BSWAPs its source in place; bytes need no swap.
  return swap && accessSize > 8 && !cpu_info.bMOVBE;
}

bool EmuCodeBlock::WriteToConstAddress(int accessSize, OpArg arg, u32 address,
                                       BitSet32 registersInUse)
{
  arg = ResizeImm(accessSize, arg);
  const int bytes = accessSize / 8;

  if (m_jit.jo.optimizeGatherPipe && m_jit.m_mmu.IsOptimizableGatherPipeWrite(address))
  {
    // Append straight to the gather pipe buffer. MOVBE can store from the
    // source register itself; otherwise go through RSCRATCH.
    const X64Reg value_reg =
        arg.IsSimpleReg() && cpu_info.bMOVBE ? arg.GetSimpleReg() : RSCRATCH;
    if (!arg.IsSimpleReg(value_reg))
      MOV(accessSize, R(value_reg), arg);

    MOV(64, R(RSCRATCH2), PPCSTATE(gather_pipe_ptr));
    SwapAndStore(accessSize, MatR(RSCRATCH2), value_reg);
    ADD(64, R(RSCRATCH2), Imm8(static_cast<u8>(bytes)));
    MOV(64, PPCSTATE(gather_pipe_ptr), R(RSCRATCH2));

    m_jit.js.fifoBytesSinceCheck += static_cast<u32>(bytes);
    return false;
  }

  if (m_jit.jo.fastmem_arena && m_jit.m_mmu.IsOptimizableRAMAddress(address, static_cast<u32>(accessSize)))
  {
    WriteToConstRamAddress(accessSize, arg, address);
    return false;
  }

  FlushPCBeforeSlowAccess();
  ABI_PushRegistersAndAdjustStack(registersInUse, 0);
  WithAccessType(accessSize, [&](auto type) {
    using T = decltype(type);
    ABI_CallFunctionPAC(accessSize, &PowerPC::WriteFromJit<T>, &m_jit.m_mmu, arg, address);
  });
  ABI_PopRegistersAndAdjustStack(registersInUse, 0);
  return true;
}

void EmuCodeBlock::WriteToConstRamAddress(int accessSize, OpArg arg, u32 address, bool swap)
{
  if (arg.IsImm())
  {
    // Immediates are always stored byte-reversed here, whatever `swap` says.
    MOV(32, R(RSCRATCH), Imm32(address));
    MOV(accessSize, MRegSum(RMEM, RSCRATCH), ByteReversedImm(accessSize, arg));
    return;
  }

  // A swapping store without MOVBE clobbers its source, so it needs a copy
  // unless the value already lives in RSCRATCH.
  const bool needs_copy =
      !arg.IsSimpleReg() || (swap && !cpu_info.bMOVBE && arg.GetSimpleReg() != RSCRATCH);
  X64Reg value_reg = RSCRATCH;
  if (needs_copy)
    MOV(accessSize, R(RSCRATCH), arg);
  else
    value_reg = arg.GetSimpleReg();

  MOV(32, R(RSCRATCH2), Imm32(address));
  const OpArg dest = MRegSum(RMEM, RSCRATCH2);
  if (swap)
    SwapAndStore(accessSize, dest, value_reg);
  else
    MOV(accessSize, dest, R(value_reg));
}

void EmuCodeBlock::JitGetAndClearCAOV(bool oe)
{
  if (oe)
    AND(8, PPCSTATE(xer_so_ov), Imm8(static_cast<u8>(~XER_OV_MASK & 0xFF)));
  // xer_ca holds 0 or 1, so shifting it right moves CA into the carry flag
  // and leaves zero behind.
  SHR(8, PPCSTATE(xer_ca), Imm8(1));
}

void EmuCodeBlock::JitSetCA()
{
  MOV(8, PPCSTATE(xer_ca), Imm8(1));
}

void EmuCodeBlock::JitSetCAIf(CCFlags conditionCode)
{
  // A SETcc is cheaper on average than branching around a store.
  SETcc(conditionCode, PPCSTATE(xer_ca));
}

void EmuCodeBlock::JitClearCA()
{
  MOV(8, PPCSTATE(xer_ca), Imm8(0));
}

void EmuCodeBlock::avx_op(void (XEmitter::*avxOp)(X64Reg, X64Reg, const OpArg&),
                          void (XEmitter::*sseOp)(X64Reg, const OpArg&), X64Reg regOp,
                          const OpArg& arg1, const OpArg& arg2, bool packed, bool reversible,
                          X64Reg scratch)
{
  const auto copy = [&](X64Reg dst, const OpArg& src) {
    if (packed)
      MOVAPD(dst, src);
    else
      MOVSD(dst, src);
  };

  // Already in two-operand shape.
  if (arg1.IsSimpleReg(regOp))
  {
    (this->*sseOp)(regOp, arg2);
    return;
  }
  if (reversible && arg2.IsSimpleReg(regOp))
  {
    (this->*sseOp)(regOp, arg1);
    return;
  }

  // AVX can take a separate destination as long as the first source is a register.
  if (cpu_info.bAVX)
  {
    if (arg1.IsSimpleReg())
    {
      (this->*avxOp)(regOp, arg1.GetSimpleReg(), arg2);
      return;
    }
    if (reversible && arg2.IsSimpleReg())
    {
      (this->*avxOp)(regOp, arg2.GetSimpleReg(), arg1);
      return;
    }
  }

  // Copying arg1 into the destination is safe while arg2 does not live there.
  if (!arg2.IsSimpleReg(regOp))
  {
    copy(regOp, arg1);
    (this->*sseOp)(regOp, arg1 == arg2 ? R(regOp) : arg2);
    return;
  }

  // regOp holds arg2 and the operation is not commutative: compute in scratch.
  if (!arg1.IsSimpleReg(scratch))
    MOVAPD(scratch, arg1);
  if (cpu_info.bAVX)
  {
    (this->*avxOp)(regOp, scratch, arg2);
    return;
  }
  (this->*sseOp)(scratch, arg2);
  copy(regOp, R(scratch));
}

void EmuCodeBlock::avx_op(void (XEmitter::*avxOp)(X64Reg, X64Reg, const OpArg&, u8),
                          void (XEmitter::*sseOp)(X64Reg, const OpArg&, u8), X64Reg regOp,
                          const OpArg& arg1, const OpArg& arg2, u8 imm, X64Reg scratch)
{
  if (arg1.IsSimpleReg(regOp))
  {
    (this->*sseOp)(regOp, arg2, imm);
    return;
  }
  if (cpu_info.bAVX && arg1.IsSimpleReg())
  {
    (this->*avxOp)(regOp, arg1.GetSimpleReg(), arg2, imm);
    return;
  }
  if (!arg2.IsSimpleReg(regOp))
  {
    MOVAPD(regOp, arg1);
    (this->*sseOp)(regOp, arg1 == arg2 ? R(regOp) : arg2, imm);
    return;
  }

  // regOp holds arg2: go through scratch.
  if (!arg1.IsSimpleReg(scratch))
    MOVAPD(scratch, arg1);
  if (cpu_info.bAVX)
  {
    (this->*avxOp)(regOp, scratch, arg2, imm);
    return;
  }
  (this->*sseOp)(scratch, arg2, imm);
  if (scratch != regOp)
    MOVAPD(regOp, R(scratch));
}

void EmuCodeBlock::avx_op(void (XEmitter::*avxOp)(X64Reg, X64Reg, u8),
                          void (XEmitter::*sseOp)(X64Reg, u8), X64Reg regOp1, X64Reg regOp2, u8 imm)
{
  if (regOp1 != regOp2)
  {
    if (cpu_info.bAVX)
    {
      (this->*avxOp)(regOp1, regOp2, imm);
      return;
    }
    MOVAPD(regOp1, R(regOp2));
  }
  (this->*sseOp)(regOp1, imm);
}

// The Gekko rounds the right-hand operand of a single-precision multiply to
// 25 significant bits first: the low 27 mantissa bits are dropped, rounding to
// nearest via bit 27 (mantissa = (mantissa & ~0x7FFFFFF) + ((mantissa & 0x8000000) << 1)).
void EmuCodeBlock::Force25BitPrecision(X64Reg output, const OpArg& input, X64Reg tmp)
{
  if (!m_jit.jo.accurateSinglePrecision)
  {
    if (!input.IsSimpleReg(output))
      MOVAPD(output, input);
    return;
  }

  DEBUG_ASSERT(output != tmp);
  if (cpu_info.bAVX && input.IsSimpleReg() && !input.IsSimpleReg(tmp))
  {
    const X64Reg src = input.GetSimpleReg();
    VPAND(tmp, src, MConst(MANTISSA_ROUND_BIT));
    VPAND(output, src, MConst(MANTISSA_KEEP_HIGH));
  }
  else
  {
    if (!input.IsSimpleReg(output))
      MOVAPD(output, input);
    avx_op(&XEmitter::VPAND, &XEmitter::PAND, tmp, R(output), MConst(MANTISSA_ROUND_BIT), true,
           true);
    PAND(output, MConst(MANTISSA_KEEP_HIGH));
  }
  PADDQ(output, R(tmp));
}

// Every single-precision value, denormals included, is a normal double, so
// CVTSS2SD is exact except for NaNs: it always sets the quiet bit, while the
// guest keeps a signalling NaN signalling. That case is fixed up in far code.
void EmuCodeBlock::ConvertSingleToDouble(X64Reg dst, X64Reg src, bool src_is_gpr)
{
  const X64Reg bits_reg = src_is_gpr ? src : RSCRATCH;
  if (src_is_gpr)
  {
    MOVD_xmm(dst, R(src));
  }
  else
  {
    if (dst != src)
      MOVAPS(dst, R(src));
    MOVD_xmm(R(RSCRATCH), src);
  }

  UCOMISS(dst, R(dst));
  CVTSS2SD(dst, R(dst));
  const FixupBranch is_nan = J_CC(CC_P, Jump::Near);

  SwitchToFarCode();
  SetJumpTarget(is_nan);
  TEST(32, R(bits_reg), Imm32(0x00400000));
  const FixupBranch was_quiet = J_CC(CC_NZ, Jump::Near);
  ANDPD(dst, MConst(DOUBLE_QUIET_CLEAR));
  const FixupBranch back = J(Jump::Near);
  SwitchToNearCode();

  SetJumpTarget(was_quiet);
  SetJumpTarget(back);
  MOVDDUP(dst, R(dst));
}

// Classifies the low lane of `xmm` and stores the class in FPSCR[FPRF].
// The class codes come in negative/positive pairs whose difference is 4, so
// with RSCRATCH = sign bit one LEA picks the right one.
void EmuCodeBlock::SetFPRF(X64Reg xmm, bool single)
{
  const int width = single ? 32 : 64;
  const u8 sign_shift = static_cast<u8>(width - 1);

  const auto pick_by_sign = [&](Common::PPCFpClass negative, Common::PPCFpClass positive) {
    LEA(32, RSCRATCH,
        MScaled(RSCRATCH, static_cast<int>(negative) - static_cast<int>(positive),
                static_cast<int>(positive)));
  };
  // Zeroes differ by 0x10, which no scale reaches.
  const auto pick_zero_by_sign = [&] {
    SHL(32, R(RSCRATCH), Imm8(4));
    ADD(32, R(RSCRATCH), Imm8(Common::PPC_FPCLASS_PZ));
  };

  AND(32, PPCSTATE(fpscr), Imm32(~static_cast<u32>(FPRF_MASK)));

  std::array<FixupBranch, 4> done;
  if (cpu_info.bSSE4_1)
  {
    // PTEST against field masks: ZF = field all zero, CF = field all ones.
    MOVQ_xmm(R(RSCRATCH), xmm);
    SHR(width, R(RSCRATCH), Imm8(sign_shift));
    PTEST(xmm, single ? MConst(FLOAT_EXP_LANE) : MConst(DOUBLE_EXP_LANE));
    const FixupBranch exp_all_ones = J_CC(CC_C);
    const FixupBranch exp_zero = J_CC(CC_Z);

    pick_by_sign(Common::PPC_FPCLASS_NN, Common::PPC_FPCLASS_PN);
    done[0] = J();

    SetJumpTarget(exp_all_ones);
    PTEST(xmm, single ? MConst(FLOAT_FRAC_LANE) : MConst(DOUBLE_FRAC_LANE));
    const FixupBranch infinite = J_CC(CC_Z);
    MOV(32, R(RSCRATCH), Imm32(Common::PPC_FPCLASS_QNAN));
    done[1] = J();

    SetJumpTarget(infinite);
    pick_by_sign(Common::PPC_FPCLASS_NINF, Common::PPC_FPCLASS_PINF);
    done[2] = J();

    SetJumpTarget(exp_zero);
    PTEST(xmm, single ? MConst(FLOAT_MAGNITUDE_LANE) : MConst(DOUBLE_MAGNITUDE_LANE));
    const FixupBranch zero = J_CC(CC_Z);
    pick_by_sign(Common::PPC_FPCLASS_ND, Common::PPC_FPCLASS_PD);
    done[3] = J();

    SetJumpTarget(zero);
    pick_zero_by_sign();
  }
  else
  {
    const auto load_sign = [&] {
      MOVQ_xmm(R(RSCRATCH), xmm);
      SHR(width, R(RSCRATCH), Imm8(sign_shift));
    };

    MOVQ_xmm(R(RSCRATCH), xmm);
    if (single)
      TEST(32, R(RSCRATCH), Imm32(Common::FLOAT_EXP));
    else
      TEST(64, R(RSCRATCH), MConst(DOUBLE_EXP_LANE));
    const FixupBranch exp_zero = J_CC(CC_Z);

    // Compare the magnitude with the infinity pattern: above it is a NaN.
    if (single)
    {
      AND(32, R(RSCRATCH), Imm32(~Common::FLOAT_SIGN));
      CMP(32, R(RSCRATCH), Imm32(Common::FLOAT_EXP));
    }
    else
    {
      AND(64, R(RSCRATCH), MConst(DOUBLE_MAGNITUDE_LANE));
      CMP(64, R(RSCRATCH), MConst(DOUBLE_EXP_LANE));
    }
    const FixupBranch nan = J_CC(CC_G);
    const FixupBranch infinite = J_CC(CC_E);

    load_sign();
    pick_by_sign(Common::PPC_FPCLASS_NN, Common::PPC_FPCLASS_PN);
    done[0] = J();

    SetJumpTarget(nan);
    MOV(32, R(RSCRATCH), Imm32(Common::PPC_FPCLASS_QNAN));
    done[1] = J();

    SetJumpTarget(infinite);
    load_sign();
    pick_by_sign(Common::PPC_FPCLASS_NINF, Common::PPC_FPCLASS_PINF);
    done[2] = J();

    // RSCRATCH still holds the raw bits here.
    SetJumpTarget(exp_zero);
    if (single)
      TEST(width, R(RSCRATCH), Imm32(~Common::FLOAT_SIGN));
    else
      TEST(width, R(RSCRATCH), MConst(DOUBLE_MAGNITUDE_LANE));
    const FixupBranch zero = J_CC(CC_Z);
    SHR(width, R(RSCRATCH), Imm8(sign_shift));
    pick_by_sign(Common::PPC_FPCLASS_ND, Common::PPC_FPCLASS_PD);
    done[3] = J();

    SetJumpTarget(zero);
    SHR(width, R(RSCRATCH), Imm8(sign_shift));
    pick_zero_by_sign();
  }

  for (const FixupBranch& branch : done)
    SetJumpTarget(branch);
  SHL(32, R(RSCRATCH), Imm8(FPRF_SHIFT));
  OR(32, PPCSTATE(fpscr), R(RSCRATCH));
}

void EmuCodeBlock::Clear()
{
  m_back_patch_info.clear();
  m_exception_handler_at_loc.clear();
}
