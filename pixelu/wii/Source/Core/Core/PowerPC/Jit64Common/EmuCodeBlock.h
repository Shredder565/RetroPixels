// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <unordered_map>

#include "Common/BitSet.h"
#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"

#include "Core/PowerPC/Jit64Common/ConstantPool.h"
#include "Core/PowerPC/Jit64Common/FarCodeCache.h"
#include "Core/PowerPC/Jit64Common/TrampolineInfo.h"

namespace MMIO
{
class Mapping;
}

class Jit64;

// An x86-64 code block with the guest-memory, carry and floating-point helpers
// shared by the Jit64 recompiler, its trampolines and its assembly routines.
class EmuCodeBlock : public Gen::X64CodeBlock
{
public:
  explicit EmuCodeBlock(Jit64& jit) : m_jit{jit} {}

  // After a slow-path memory access: branch to the instruction's exception
  // exit if the access raised a DSI.
  void MemoryExceptionCheck();

  // Redirect emission to the far code region and back again.
  void SwitchToFarCode();
  void SwitchToNearCode();

  // Constants are copied into a pool inside the code region so they are
  // always reachable RIP-relative.
  template <typename T>
  const void* GetConstantFromPool(const T& value)
  {
    return m_const_pool.GetConstant(&value, sizeof(T), 1, 0);
  }

  template <typename T>
  Gen::OpArg MConst(const T& value)
  {
    return Gen::M(GetConstantFromPool(value));
  }

  template <typename T, size_t N>
  Gen::OpArg MConst(const T (&value)[N], size_t index = 0)
  {
    return Gen::M(m_const_pool.GetConstant(&value, sizeof(T), N, index));
  }

  template <typename T, size_t N>
  Gen::OpArg MConst(const std::array<T, N>& value, size_t index = 0)
  {
    return Gen::M(m_const_pool.GetConstant(value.data(), sizeof(T), N, index));
  }

  void FlushPCBeforeSlowAccess();

  // Replaces addr with its BAT table entry (physical page in the upper 15
  // bits, flags in the lower 17); clobbers tmp. The returned branch is taken
  // when the address is unmapped.
  Gen::FixupBranch BATAddressLookup(Gen::X64Reg addr, Gen::X64Reg tmp, const void* bat_table);

  // The returned branch is taken when reg_addr is not backed by the fastmem
  // arena. Preserves every register.
  Gen::FixupBranch CheckIfSafeAddress(const Gen::OpArg& reg_value, Gen::X64Reg reg_addr,
                                      BitSet32 registers_in_use);

  // Direct accesses through RMEM. `info`, when given, receives the address of
  // the faulting instruction for backpatching.
  void UnsafeWriteRegToReg(Gen::OpArg reg_value, Gen::X64Reg reg_addr, int accessSize,
                           s32 offset = 0, bool swap = true, Gen::MovInfo* info = nullptr);
  void UnsafeWriteRegToReg(Gen::X64Reg reg_value, Gen::X64Reg reg_addr, int accessSize,
                           s32 offset = 0, bool swap = true, Gen::MovInfo* info = nullptr);

  // Returns true when the offset had to be added into reg_value, which was
  // also the address register.
  bool UnsafeLoadToReg(Gen::X64Reg reg_value, Gen::OpArg opAddress, int accessSize, s32 offset,
                       bool signExtend, Gen::MovInfo* info = nullptr);

  // Inlines the MMIO read handler for a constant address. The address must be
  // a known MMIO address (MMIO::IsMMIOAddress).
  void MMIOLoadToReg(MMIO::Mapping* mmio, Gen::X64Reg reg_value, BitSet32 registers_in_use,
                     u32 address, int access_size, bool sign_extend);

  enum SafeLoadStoreFlags
  {
    SAFE_LOADSTORE_NO_SWAP = 1,
    SAFE_LOADSTORE_NO_PROLOG = 2,
    // The access cannot be backpatched, so fastmem must not be used.
    SAFE_LOADSTORE_NO_FASTMEM = 4,
    SAFE_LOADSTORE_CLOBBER_RSCRATCH_INSTEAD_OF_ADDR = 8,
    // Always go through the C++ accessors (trampolines use this).
    SAFE_LOADSTORE_FORCE_SLOW_ACCESS = 16,
    SAFE_LOADSTORE_DR_ON = 32,
    // The caller has no instruction PC to store (shared asm routines).
    SAFE_LOADSTORE_NO_UPDATE_PC = 64,
  };

  void SafeLoadToReg(Gen::X64Reg reg_value, const Gen::OpArg& opAddress, int accessSize, s32 offset,
                     BitSet32 registersInUse, bool signExtend, int flags = 0);
  void SafeLoadToRegImmediate(Gen::X64Reg reg_value, u32 address, int accessSize,
                              BitSet32 registersInUse, bool signExtend);

  // Clobbers RSCRATCH or reg_addr (see SAFE_LOADSTORE_CLOBBER_RSCRATCH_...).
  // reg_value survives a faulting store when memchecks are on. reg_value must
  // be an immediate or a plain register.
  void SafeWriteRegToReg(Gen::OpArg reg_value, Gen::X64Reg reg_addr, int accessSize, s32 offset,
                         BitSet32 registersInUse, int flags = 0);
  void SafeWriteRegToReg(Gen::X64Reg reg_value, Gen::X64Reg reg_addr, int accessSize, s32 offset,
                         BitSet32 registersInUse, int flags = 0);

  // Whether a (safe or unsafe) store byte-swaps its source register in place.
  bool WriteClobbersRegValue(int accessSize, bool swap);

  // Returns whether the store may have raised an exception.
  bool WriteToConstAddress(int accessSize, Gen::OpArg arg, u32 address, BitSet32 registersInUse);
  void WriteToConstRamAddress(int accessSize, Gen::OpArg arg, u32 address, bool swap = true);

  // XER helpers. JitGetAndClearCAOV leaves the old CA in the host carry flag.
  void JitGetAndClearCAOV(bool oe);
  void JitSetCA();
  void JitSetCAIf(Gen::CCFlags conditionCode);
  void JitClearCA();

  // Emit regOp = arg1 op arg2 using the AVX three-operand form when it helps
  // and SSE plus moves otherwise.
  void avx_op(void (Gen::XEmitter::*avxOp)(Gen::X64Reg, Gen::X64Reg, const Gen::OpArg&),
              void (Gen::XEmitter::*sseOp)(Gen::X64Reg, const Gen::OpArg&), Gen::X64Reg regOp,
              const Gen::OpArg& arg1, const Gen::OpArg& arg2, bool packed = true,
              bool reversible = false, Gen::X64Reg scratch = Gen::XMM0);
  void avx_op(void (Gen::XEmitter::*avxOp)(Gen::X64Reg, Gen::X64Reg, const Gen::OpArg&, u8),
              void (Gen::XEmitter::*sseOp)(Gen::X64Reg, const Gen::OpArg&, u8), Gen::X64Reg regOp,
              const Gen::OpArg& arg1, const Gen::OpArg& arg2, u8 imm,
              Gen::X64Reg scratch = Gen::XMM0);
  void avx_op(void (Gen::XEmitter::*avxOp)(Gen::X64Reg, Gen::X64Reg, u8),
              void (Gen::XEmitter::*sseOp)(Gen::X64Reg, u8), Gen::X64Reg regOp1, Gen::X64Reg regOp2,
              u8 imm);

  void Force25BitPrecision(Gen::X64Reg output, const Gen::OpArg& input, Gen::X64Reg tmp);

  // May clobber RSCRATCH.
  void ConvertSingleToDouble(Gen::X64Reg dst, Gen::X64Reg src, bool src_is_gpr = false);
  void ConvertDoubleToSingle(Gen::X64Reg dst, Gen::X64Reg src);
  void SetFPRF(Gen::X64Reg xmm, bool single);
  void Clear();

protected:
  Jit64& m_jit;
  ConstantPool m_const_pool;
  FarCodeCache m_far_code;

  // Near-code emission state, saved while emitting far code.
  u8* m_near_code = nullptr;
  u8* m_near_code_end = nullptr;
  bool m_near_code_write_failed = false;

  std::unordered_map<u8*, TrampolineInfo> m_back_patch_info;
  std::unordered_map<u8*, u8*> m_exception_handler_at_loc;

private:
  // Whether a slow-path access may first try fastmem behind an inline BAT check.
  bool CanInlineBATCheck(int flags) const;
  // Leaves the inline fast path; emission continues at the slow path that
  // `to_slow` jumps to. Returns the branch EndSlowPath needs.
  Gen::FixupBranch BeginSlowPath(Gen::FixupBranch to_slow);
  void EndSlowPath(Gen::FixupBranch skip);
  // Pads a fastmem access to BACKPATCH_SIZE and records it for backpatching.
  void RecordBackpatch(const Gen::MovInfo& mov, TrampolineInfo info);
  // Moves a C++ accessor's return value into the destination register.
  void TakeReadResult(Gen::X64Reg reg_value, int accessSize, bool signExtend);
};
