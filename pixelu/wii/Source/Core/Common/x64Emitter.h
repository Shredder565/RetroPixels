// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// An x86-64 machine code emitter for the JITs. Not thread safe: one emitter writes to one buffer
// from one thread.

#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <tuple>
#include <type_traits>

#include "Common/Assert.h"
#include "Common/BitSet.h"
#include "Common/CodeBlock.h"
#include "Common/CommonTypes.h"
#include "Common/x64ABI.h"

namespace Gen
{
// Condition codes, numbered as in the Jcc/SETcc/CMOVcc opcodes.
enum CCFlags
{
  CC_O = 0x0,
  CC_NO = 0x1,
  CC_B = 0x2,
  CC_C = CC_B,
  CC_NAE = CC_B,
  CC_NB = 0x3,
  CC_NC = CC_NB,
  CC_AE = CC_NB,
  CC_Z = 0x4,
  CC_E = CC_Z,
  CC_NZ = 0x5,
  CC_NE = CC_NZ,
  CC_BE = 0x6,
  CC_NA = CC_BE,
  CC_NBE = 0x7,
  CC_A = CC_NBE,
  CC_S = 0x8,
  CC_NS = 0x9,
  CC_P = 0xA,
  CC_PE = CC_P,
  CC_NP = 0xB,
  CC_PO = CC_NP,
  CC_L = 0xC,
  CC_NGE = CC_L,
  CC_NL = 0xD,
  CC_GE = CC_NL,
  CC_LE = 0xE,
  CC_NG = CC_LE,
  CC_NLE = 0xF,
  CC_G = CC_NLE,
};

enum
{
  NUMGPRs = 16,
  NUMXMMs = 16,
};

// The kind of an OpArg. 1, 2, 4 and 8 are base + index * scale addressing.
enum
{
  SCALE_NONE = 0,  // a register
  SCALE_1 = 1,
  SCALE_2 = 2,
  SCALE_4 = 4,
  SCALE_8 = 8,
  SCALE_ATREG = 16,  // [base + displacement]
  // [index * scale + disp32] without a base register. A scale of 1 is just SCALE_ATREG.
  SCALE_NOBASE_2 = 0x20 | SCALE_2,
  SCALE_NOBASE_4 = 0x20 | SCALE_4,
  SCALE_NOBASE_8 = 0x20 | SCALE_8,
  SCALE_RIP = 0xFF,  // an absolute address reached RIP-relative
  SCALE_IMM8 = 0xF0,
  SCALE_IMM16 = 0xF1,
  SCALE_IMM32 = 0xF2,
  SCALE_IMM64 = 0xF3,
};

// Predicates of CMPSS/CMPSD/CMPPS/CMPPD.
enum SSECompare
{
  CMP_EQ = 0,
  CMP_LT = 1,
  CMP_LE = 2,
  CMP_UNORD = 3,
  CMP_NEQ = 4,
  CMP_NLT = 5,
  CMP_NLE = 6,
  CMP_ORD = 7,
};

class XEmitter;

// Where a byte-swapping load or store was emitted, so a fault in it can be traced back.
struct MovInfo final
{
  u8* address;
  // The store is a BSWAP of the source register followed by a plain MOV.
  bool nonAtomicSwapStore;
  // The register that BSWAP changed; only meaningful when nonAtomicSwapStore is set.
  X64Reg nonAtomicSwapStoreSrc;
};

// One operand: a register, a memory reference or an immediate.
struct OpArg
{
  friend class XEmitter;

  constexpr OpArg() = default;
  constexpr OpArg(u64 offset_, int scale_, X64Reg rm_reg = RAX, X64Reg scaled_reg = RAX)
      : m_value{offset_}, m_base{static_cast<u16>(rm_reg)},
        m_index{static_cast<u16>(scaled_reg)}, m_kind{static_cast<u8>(scale_)}
  {
  }

  constexpr bool operator==(const OpArg& other) const
  {
    return std::tie(m_kind, m_base, m_index, m_value) ==
           std::tie(other.m_kind, other.m_base, other.m_index, other.m_value);
  }

  u64 Imm64() const
  {
    DEBUG_ASSERT(m_kind == SCALE_IMM64);
    return m_value;
  }
  u32 Imm32() const
  {
    DEBUG_ASSERT(m_kind == SCALE_IMM32);
    return static_cast<u32>(m_value);
  }
  u16 Imm16() const
  {
    DEBUG_ASSERT(m_kind == SCALE_IMM16);
    return static_cast<u16>(m_value);
  }
  u8 Imm8() const
  {
    DEBUG_ASSERT(m_kind == SCALE_IMM8);
    return static_cast<u8>(m_value);
  }

  s64 SImm64() const { return static_cast<s64>(Imm64()); }
  s32 SImm32() const { return static_cast<s32>(Imm32()); }
  s16 SImm16() const { return static_cast<s16>(Imm16()); }
  s8 SImm8() const { return static_cast<s8>(Imm8()); }

  // The same immediate value, truncated to another width.
  OpArg AsImm64() const { return Resized(SCALE_IMM64, ~0ULL); }
  OpArg AsImm32() const { return Resized(SCALE_IMM32, 0xFFFFFFFFULL); }
  OpArg AsImm16() const { return Resized(SCALE_IMM16, 0xFFFFULL); }
  OpArg AsImm8() const { return Resized(SCALE_IMM8, 0xFFULL); }

  constexpr bool IsImm() const { return m_kind >= SCALE_IMM8 && m_kind <= SCALE_IMM64; }
  constexpr bool IsSimpleReg() const { return m_kind == SCALE_NONE; }
  constexpr bool IsSimpleReg(X64Reg reg) const
  {
    return IsSimpleReg() && GetSimpleReg() == reg;
  }
  constexpr bool IsZero() const { return IsImm() && m_value == 0; }

  // 8, 16, 32 or 64 for an immediate, -1 for anything else.
  constexpr int GetImmBits() const { return IsImm() ? 8 << (m_kind - SCALE_IMM8) : -1; }

  constexpr X64Reg GetSimpleReg() const
  {
    return IsSimpleReg() ? static_cast<X64Reg>(m_base) : INVALID_REG;
  }

  // Moves a memory operand's displacement (or a RIP operand's address) by val bytes.
  void AddMemOffset(int val)
  {
    DEBUG_ASSERT_MSG(DYNA_REC,
                     m_kind == SCALE_RIP || (m_kind != SCALE_NONE && m_kind <= SCALE_ATREG),
                     "AddMemOffset needs a memory operand with a displacement");
    m_value += val;
  }

private:
  OpArg Resized(int kind, u64 mask) const
  {
    DEBUG_ASSERT(IsImm());
    return OpArg(m_value & mask, kind);
  }

  // Immediate value, displacement, or the absolute address of a SCALE_RIP operand.
  u64 m_value = 0;
  // Register (SCALE_NONE) or base register.
  u16 m_base = 0;
  u16 m_index = 0;
  // One of the SCALE_* values.
  u8 m_kind = 0;
};

template <typename T>
inline OpArg M(const T* ptr)
{
  return OpArg(reinterpret_cast<u64>(static_cast<const void*>(ptr)), SCALE_RIP);
}
constexpr OpArg R(X64Reg value)
{
  return OpArg(0, SCALE_NONE, value);
}
constexpr OpArg MatR(X64Reg value)
{
  return OpArg(0, SCALE_ATREG, value);
}
constexpr OpArg MDisp(X64Reg value, int offset)
{
  return OpArg(static_cast<u32>(offset), SCALE_ATREG, value);
}
constexpr OpArg MComplex(X64Reg base, X64Reg scaled, int scale, int offset)
{
  return OpArg(offset, scale, base, scaled);
}
constexpr OpArg MScaled(X64Reg scaled, int scale, int offset)
{
  // index * 1 needs no SIB scale: it is the same as [index + offset].
  return scale == SCALE_1 ? OpArg(offset, SCALE_ATREG, scaled) :
                            OpArg(offset, 0x20 | scale, RAX, scaled);
}
constexpr OpArg MRegSum(X64Reg base, X64Reg offset)
{
  return MComplex(base, offset, SCALE_1, 0);
}

constexpr OpArg Imm8(u8 imm)
{
  return OpArg(imm, SCALE_IMM8);
}
constexpr OpArg Imm16(u16 imm)
{
  return OpArg(imm, SCALE_IMM16);
}
constexpr OpArg Imm32(u32 imm)
{
  return OpArg(imm, SCALE_IMM32);
}
constexpr OpArg Imm64(u64 imm)
{
  return OpArg(imm, SCALE_IMM64);
}
inline OpArg ImmPtr(const void* imm)
{
  return Imm64(reinterpret_cast<u64>(imm));
}

// ptr - base as a 32-bit offset; asserts (and returns 0) when it does not fit.
inline u32 PtrOffset(const void* ptr, const void* base = nullptr)
{
  const s64 distance = reinterpret_cast<s64>(ptr) - reinterpret_cast<s64>(base);
  if (distance != static_cast<s32>(distance))
  {
    ASSERT_MSG(DYNA_REC, 0, "PtrOffset: distance does not fit in 32 bits");
    return 0;
  }
  return static_cast<u32>(distance);
}

// A forward jump or call whose target is filled in later with SetJumpTarget.
struct FixupBranch
{
  enum class Type
  {
    Branch8Bit,
    Branch32Bit
  };

  // Points just past the instruction (the end of its displacement), or is null when the
  // instruction could not be written.
  u8* ptr;
  Type type;
};

class XEmitter
{
private:
  u8* code = nullptr;
  // Writes that would go past this are dropped and set m_write_failed.
  u8* m_code_end = nullptr;
  bool flags_locked = false;
  bool m_write_failed = false;

  // Instruction encoding helpers (x64Emitter.cpp).
  void CheckFlags();
  bool Fits(size_t bytes);
  void EmitRex(const OpArg& rm, u32 reg, bool wide, bool rm_is_byte, bool reg_is_byte);
  void EmitOperand(const OpArg& rm, u32 reg, int trailing_bytes = 0, bool check_rip = true);
  void EmitRegOp(int bits, const OpArg& rm, u32 reg, bool rm_is_byte, bool reg_is_byte, u8 op0,
                 int op1 = -1, int op2 = -1, int trailing_bytes = 0);
  void EmitUnaryGroup(int bits, const OpArg& rm, int ext);
  void EmitBitScan(int bits, X64Reg dest, const OpArg& src, u8 op, bool f3_prefix);
  void EmitShift(int bits, const OpArg& dest, const OpArg& shift, int ext);
  void EmitBitTest(int bits, const OpArg& dest, const OpArg& index, int ext);
  void EmitDoubleShift(int bits, const OpArg& dest, const OpArg& src, const OpArg& shift,
                       u8 op_imm);
  void EmitArith(int bits, int op, const OpArg& a1, const OpArg& a2);
  void EmitArithImm(int bits, int op, const OpArg& rm, const OpArg& imm);
  void EmitMovbe(int bits, bool store, X64Reg reg, const OpArg& mem);
  void EmitMxcsr(const OpArg& mem, int ext);
  void EmitSSE(u8 prefix, u16 op, X64Reg reg, const OpArg& rm, int trailing_bytes = 0);
  void EmitSSE_SSSE3(u8 prefix, u16 op, X64Reg reg, const OpArg& rm, int trailing_bytes = 0);
  void EmitSSE_SSE41(u8 prefix, u16 op, X64Reg reg, const OpArg& rm, int trailing_bytes = 0);
  void EmitVEX(u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, bool w = false,
               int trailing_bytes = 0);
  void EmitVEX4(u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, X64Reg is4,
                bool w = false);
  void EmitAVX(u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, int trailing_bytes = 0);
  void EmitFMA3(u8 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, bool w);
  void EmitFMA4(u8 op, X64Reg dest, X64Reg src1, X64Reg reg_src, const OpArg& rm, bool w);
  void EmitBMI(int bits, u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm,
               int trailing_bytes = 0);
  void EmitBMI1(int bits, u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm);
  void EmitBMI2(int bits, u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm,
                int trailing_bytes = 0);

  // Stack frame layout for ABI_Push/PopRegistersAndAdjustStack (x64ABI.cpp).
  struct FrameLayout
  {
    size_t shadow;       // shadow space the callee may use, at [rsp]
    size_t subtraction;  // bytes taken off rsp after the pushes
    size_t xmm_offset;   // rsp offset of the first saved XMM register
  };
  static FrameLayout CalculateFrame(BitSet32 mask, size_t rsp_alignment, size_t needed_frame_size);

protected:
  void Write8(u8 value);
  void Write16(u16 value);
  void Write32(u32 value);
  void Write64(u64 value);

public:
  XEmitter() = default;
  explicit XEmitter(u8* code_ptr, u8* code_end) : code(code_ptr), m_code_end(code_end) {}
  virtual ~XEmitter() = default;

  void SetCodePtr(u8* ptr, u8* end, bool write_failed = false);
  // Skips bytes bytes, filling them with INT3.
  void ReserveCodeSpace(int bytes);
  u8* AlignCodeTo(size_t alignment);
  u8* AlignCode4();
  u8* AlignCode16();
  u8* AlignCodePage();
  const u8* GetCodePtr() const { return code; }
  u8* GetWritableCodePtr() { return code; }
  const u8* GetCodeEnd() const { return m_code_end; }
  u8* GetWritableCodeEnd() { return m_code_end; }

  // While the flags are locked, emitting an instruction that changes them asserts.
  void LockFlags() { flags_locked = true; }
  void UnlockFlags() { flags_locked = false; }

  // True once a write did not fit. The code emitted since the last SetCodePtr must not be run.
  bool HasWriteFailed() const { return m_write_failed; }

  // Single-byte and system instructions
  void INT3();
  void NOP(size_t count = 1);  // count bytes of recommended multi-byte NOPs
  void PAUSE();
  void STC();
  void CLC();
  void CMC();
  void LAHF();
  void SAHF();

  // Stack
  void PUSH(X64Reg reg);
  void POP(X64Reg reg);
  void PUSH(int bits, const OpArg& reg);
  void POP(int bits, const OpArg& reg);
  void PUSHF();
  void POPF();

  // Control flow
  enum class Jump
  {
    Short,
    Near,
  };
  static const int SHORT_JMP_LEN = 2;
  static const int NEAR_JMP_LEN = 5;

  void RET();
  void RET_FAST();  // REP RET
  void UD2();
  [[nodiscard]] FixupBranch J(Jump jump = Jump::Short);

  // Picks the short form when the target is in range; force_near_padding then pads the
  // instruction to NEAR_JMP_LEN bytes so it can be patched to a near jump later.
  void JMP(const u8* addr, bool force_near_padding = false);
  void JMPptr(const OpArg& arg);
  void JMPself();
#ifdef CALL
#undef CALL
#endif
  void CALL(const void* fnptr);
  [[nodiscard]] FixupBranch CALL();
  void CALLptr(OpArg arg);

  [[nodiscard]] FixupBranch J_CC(CCFlags conditionCode, Jump jump = Jump::Short);
  void J_CC(CCFlags conditionCode, const u8* addr);

  // Points branch at the current code pointer.
  void SetJumpTarget(const FixupBranch& branch);

  void SETcc(CCFlags flag, OpArg dest);
  void CMOVcc(int bits, X64Reg dest, OpArg src, CCFlags flag);

  // Fences
  void LFENCE();
  void MFENCE();
  void SFENCE();

  // Bit scans
  void BSF(int bits, X64Reg dest, const OpArg& src);
  void BSR(int bits, X64Reg dest, const OpArg& src);

  // Cache control
  enum class PrefetchLevel : u8
  {
    NTA = 0,
    T0 = 1,
    T1 = 2,
    T2 = 3,
  };
  void PREFETCH(PrefetchLevel level, OpArg arg);
  void MOVNTI(int bits, const OpArg& dest, X64Reg src);
  void MOVNTDQ(const OpArg& arg, X64Reg regOp);
  void MOVNTPS(const OpArg& arg, X64Reg regOp);
  void MOVNTPD(const OpArg& arg, X64Reg regOp);

  // Multiplication and division
  void MUL(int bits, const OpArg& src);
  void IMUL(int bits, const OpArg& src);
  void IMUL(int bits, X64Reg regOp, const OpArg& src);
  void IMUL(int bits, X64Reg regOp, const OpArg& src, const OpArg& imm);
  void DIV(int bits, const OpArg& src);
  void IDIV(int bits, const OpArg& src);

  // Shifts and rotates; shift is Imm8 or R(ECX)
  void ROL(int bits, const OpArg& dest, const OpArg& shift);
  void ROR(int bits, const OpArg& dest, const OpArg& shift);
  void RCL(int bits, const OpArg& dest, const OpArg& shift);
  void RCR(int bits, const OpArg& dest, const OpArg& shift);
  void SHL(int bits, const OpArg& dest, const OpArg& shift);
  void SHR(int bits, const OpArg& dest, const OpArg& shift);
  void SAR(int bits, const OpArg& dest, const OpArg& shift);

  // Bit tests; index is Imm8 or a register
  void BT(int bits, const OpArg& dest, const OpArg& index);
  void BTS(int bits, const OpArg& dest, const OpArg& index);
  void BTR(int bits, const OpArg& dest, const OpArg& index);
  void BTC(int bits, const OpArg& dest, const OpArg& index);

  // Double-precision shifts; src is a register, shift is Imm8 or R(ECX)
  void SHRD(int bits, const OpArg& dest, const OpArg& src, const OpArg& shift);
  void SHLD(int bits, const OpArg& dest, const OpArg& src, const OpArg& shift);

  // Sign extension of the accumulator. CWD/CDQ/CQO fill DX/EDX/RDX, CBW/CWDE/CDQE widen AL/AX/EAX
  // (CBW's bits is the source width).
  void CWD(int bits = 16);
  inline void CDQ() { CWD(32); }
  inline void CQO() { CWD(64); }
  void CBW(int bits = 8);
  inline void CWDE() { CBW(16); }
  inline void CDQE() { CBW(32); }

  void LEA(int bits, X64Reg dest, OpArg src);

  // Integer arithmetic and logic
  void NEG(int bits, const OpArg& src);
  void ADD(int bits, const OpArg& a1, const OpArg& a2);
  void ADC(int bits, const OpArg& a1, const OpArg& a2);
  void SUB(int bits, const OpArg& a1, const OpArg& a2);
  void SBB(int bits, const OpArg& a1, const OpArg& a2);
  void AND(int bits, const OpArg& a1, const OpArg& a2);
  void CMP(int bits, const OpArg& a1, const OpArg& a2);
  void NOT(int bits, const OpArg& src);
  void OR(int bits, const OpArg& a1, const OpArg& a2);
  void XOR(int bits, const OpArg& a1, const OpArg& a2);
  void MOV(int bits, const OpArg& a1, const OpArg& a2);
  void TEST(int bits, const OpArg& a1, const OpArg& a2);

  // CMP, or the shorter TEST reg, reg when comparing a register with zero.
  void CMP_or_TEST(int bits, const OpArg& a1, const OpArg& a2);
  // dest = a1 + a2, using LEA or nothing at all where possible. Changes the flags.
  void MOV_sum(int bits, X64Reg dest, const OpArg& a1, const OpArg& a2);

  void XCHG(int bits, const OpArg& a1, const OpArg& a2);
  void XCHG_AHAL();

  // 32 and 64 bits use BSWAP, 16 bits a rotate by 8, 8 bits nothing.
  void BSWAP(int bits, X64Reg reg);

  // Sign/zero extension; equal sizes give a plain MOV, 32 to 64 bits MOVSXD / a 32-bit MOV.
  void MOVSX(int dbits, int sbits, X64Reg dest, OpArg src);
  void MOVZX(int dbits, int sbits, X64Reg dest, OpArg src);

  // MOVBE needs cpu_info.bMOVBE. LoadAndSwap/SwapAndStore fall back to MOV + BSWAP.
  void MOVBE(int bits, X64Reg dest, const OpArg& src);
  void MOVBE(int bits, const OpArg& dest, X64Reg src);
  void LoadAndSwap(int size, X64Reg dst, const OpArg& src, bool sign_extend = false,
                   MovInfo* info = nullptr);
  void SwapAndStore(int size, const OpArg& dst, X64Reg src, MovInfo* info = nullptr);

  // LZCNT needs cpu_info.bLZCNT, TZCNT cpu_info.bBMI1.
  void LZCNT(int bits, X64Reg dest, const OpArg& src);
  void TZCNT(int bits, X64Reg dest, const OpArg& src);

  void STMXCSR(const OpArg& memloc);
  void LDMXCSR(const OpArg& memloc);

  // Prefixes for the next instruction
  void LOCK();
  void REP();
  void REPNE();
  void FSOverride();
  void GSOverride();

  // SSE/SSE2 scalar arithmetic
  void ADDSS(X64Reg regOp, const OpArg& arg);
  void ADDSD(X64Reg regOp, const OpArg& arg);
  void SUBSS(X64Reg regOp, const OpArg& arg);
  void SUBSD(X64Reg regOp, const OpArg& arg);
  void MULSS(X64Reg regOp, const OpArg& arg);
  void MULSD(X64Reg regOp, const OpArg& arg);
  void DIVSS(X64Reg regOp, const OpArg& arg);
  void DIVSD(X64Reg regOp, const OpArg& arg);
  void MINSS(X64Reg regOp, const OpArg& arg);
  void MINSD(X64Reg regOp, const OpArg& arg);
  void MAXSS(X64Reg regOp, const OpArg& arg);
  void MAXSD(X64Reg regOp, const OpArg& arg);
  void SQRTSS(X64Reg regOp, const OpArg& arg);
  void SQRTSD(X64Reg regOp, const OpArg& arg);
  void RCPSS(X64Reg regOp, const OpArg& arg);
  void RSQRTSS(X64Reg regOp, const OpArg& arg);

  void CMPSS(X64Reg regOp, const OpArg& arg, u8 compare);
  void CMPSD(X64Reg regOp, const OpArg& arg, u8 compare);

  // SSE/SSE2 packed arithmetic
  void ADDPS(X64Reg regOp, const OpArg& arg);
  void ADDPD(X64Reg regOp, const OpArg& arg);
  void SUBPS(X64Reg regOp, const OpArg& arg);
  void SUBPD(X64Reg regOp, const OpArg& arg);
  void CMPPS(X64Reg regOp, const OpArg& arg, u8 compare);
  void CMPPD(X64Reg regOp, const OpArg& arg, u8 compare);
  void MULPS(X64Reg regOp, const OpArg& arg);
  void MULPD(X64Reg regOp, const OpArg& arg);
  void DIVPS(X64Reg regOp, const OpArg& arg);
  void DIVPD(X64Reg regOp, const OpArg& arg);
  void MINPS(X64Reg regOp, const OpArg& arg);
  void MINPD(X64Reg regOp, const OpArg& arg);
  void MAXPS(X64Reg regOp, const OpArg& arg);
  void MAXPD(X64Reg regOp, const OpArg& arg);
  void SQRTPS(X64Reg regOp, const OpArg& arg);
  void SQRTPD(X64Reg regOp, const OpArg& arg);
  void RCPPS(X64Reg regOp, const OpArg& arg);
  void RSQRTPS(X64Reg regOp, const OpArg& arg);

  // SSE/SSE2 packed logic
  void ANDPS(X64Reg regOp, const OpArg& arg);
  void ANDPD(X64Reg regOp, const OpArg& arg);
  void ANDNPS(X64Reg regOp, const OpArg& arg);
  void ANDNPD(X64Reg regOp, const OpArg& arg);
  void ORPS(X64Reg regOp, const OpArg& arg);
  void ORPD(X64Reg regOp, const OpArg& arg);
  void XORPS(X64Reg regOp, const OpArg& arg);
  void XORPD(X64Reg regOp, const OpArg& arg);

  // Shuffles
  void SHUFPS(X64Reg regOp, const OpArg& arg, u8 shuffle);
  void SHUFPD(X64Reg regOp, const OpArg& arg, u8 shuffle);

  // SSE3 duplicating moves (with SSE2 fallbacks)
  void MOVSLDUP(X64Reg regOp, const OpArg& arg);
  void MOVSHDUP(X64Reg regOp, const OpArg& arg);
  void MOVDDUP(X64Reg regOp, const OpArg& arg);

  // Interleaves
  void UNPCKLPS(X64Reg dest, const OpArg& src);
  void UNPCKHPS(X64Reg dest, const OpArg& src);
  void UNPCKLPD(X64Reg dest, const OpArg& src);
  void UNPCKHPD(X64Reg dest, const OpArg& src);

  // Scalar compares setting EFLAGS
  void COMISS(X64Reg regOp, const OpArg& arg);
  void COMISD(X64Reg regOp, const OpArg& arg);
  void UCOMISS(X64Reg regOp, const OpArg& arg);
  void UCOMISD(X64Reg regOp, const OpArg& arg);

  // Vector moves. The PD forms of MOVAP emit MOVAPS, which does the same and is shorter.
  void MOVAPS(X64Reg regOp, const OpArg& arg);
  void MOVAPD(X64Reg regOp, const OpArg& arg);
  void MOVAPS(const OpArg& arg, X64Reg regOp);
  void MOVAPD(const OpArg& arg, X64Reg regOp);

  void MOVUPS(X64Reg regOp, const OpArg& arg);
  void MOVUPD(X64Reg regOp, const OpArg& arg);
  void MOVUPS(const OpArg& arg, X64Reg regOp);
  void MOVUPD(const OpArg& arg, X64Reg regOp);

  void MOVDQA(X64Reg regOp, const OpArg& arg);
  void MOVDQA(const OpArg& arg, X64Reg regOp);
  void MOVDQU(X64Reg regOp, const OpArg& arg);
  void MOVDQU(const OpArg& arg, X64Reg regOp);

  void MOVSS(X64Reg regOp, const OpArg& arg);
  void MOVSD(X64Reg regOp, const OpArg& arg);
  void MOVSS(const OpArg& arg, X64Reg regOp);
  void MOVSD(const OpArg& arg, X64Reg regOp);

  void MOVLPS(X64Reg regOp, const OpArg& arg);
  void MOVLPD(X64Reg regOp, const OpArg& arg);
  void MOVLPS(const OpArg& arg, X64Reg regOp);
  void MOVLPD(const OpArg& arg, X64Reg regOp);

  void MOVHPS(X64Reg regOp, const OpArg& arg);
  void MOVHPD(X64Reg regOp, const OpArg& arg);
  void MOVHPS(const OpArg& arg, X64Reg regOp);
  void MOVHPD(const OpArg& arg, X64Reg regOp);

  void MOVHLPS(X64Reg regOp1, X64Reg regOp2);
  void MOVLHPS(X64Reg regOp1, X64Reg regOp2);

  // GPR <-> XMM moves. The X64Reg parameter is always the XMM register and the OpArg the general
  // purpose register or memory: MOVD_xmm(R(EAX), XMM1) is "movd eax, xmm1".
  void MOVD_xmm(X64Reg dest, const OpArg& arg);
  void MOVQ_xmm(X64Reg dest, OpArg arg);
  void MOVD_xmm(const OpArg& arg, X64Reg src);
  void MOVQ_xmm(OpArg arg, X64Reg src);

  // Sign masks into a general purpose register
  void MOVMSKPS(X64Reg dest, const OpArg& arg);
  void MOVMSKPD(X64Reg dest, const OpArg& arg);

  // Byte-masked store to [rdi]
  void MASKMOVDQU(X64Reg dest, X64Reg src);
  void LDDQU(X64Reg dest, const OpArg& src);

  // Conversions
  void CVTPS2PD(X64Reg dest, const OpArg& src);
  void CVTPD2PS(X64Reg dest, const OpArg& src);
  void CVTSS2SD(X64Reg dest, const OpArg& src);
  void CVTSI2SS(X64Reg dest, const OpArg& src);
  void CVTSD2SS(X64Reg dest, const OpArg& src);
  void CVTSI2SD(X64Reg dest, const OpArg& src);
  void CVTDQ2PD(X64Reg regOp, const OpArg& arg);
  void CVTPD2DQ(X64Reg regOp, const OpArg& arg);
  void CVTDQ2PS(X64Reg regOp, const OpArg& arg);
  void CVTPS2DQ(X64Reg regOp, const OpArg& arg);

  void CVTTPS2DQ(X64Reg regOp, const OpArg& arg);
  void CVTTPD2DQ(X64Reg regOp, const OpArg& arg);

  // These write a 32-bit general purpose register.
  void CVTSS2SI(X64Reg xregdest, const OpArg& src);
  void CVTSD2SI(X64Reg xregdest, const OpArg& src);
  void CVTTSS2SI(X64Reg xregdest, const OpArg& arg);
  void CVTTSD2SI(X64Reg xregdest, const OpArg& arg);

  // Packed integer
  void PACKSSDW(X64Reg dest, const OpArg& arg);
  void PACKSSWB(X64Reg dest, const OpArg& arg);
  void PACKUSDW(X64Reg dest, const OpArg& arg);
  void PACKUSWB(X64Reg dest, const OpArg& arg);

  void PUNPCKLBW(X64Reg dest, const OpArg& arg);
  void PUNPCKLWD(X64Reg dest, const OpArg& arg);
  void PUNPCKLDQ(X64Reg dest, const OpArg& arg);
  void PUNPCKLQDQ(X64Reg dest, const OpArg& arg);

  void PTEST(X64Reg dest, const OpArg& arg);
  void PAND(X64Reg dest, const OpArg& arg);
  void PANDN(X64Reg dest, const OpArg& arg);
  void PXOR(X64Reg dest, const OpArg& arg);
  void POR(X64Reg dest, const OpArg& arg);

  void PADDB(X64Reg dest, const OpArg& arg);
  void PADDW(X64Reg dest, const OpArg& arg);
  void PADDD(X64Reg dest, const OpArg& arg);
  void PADDQ(X64Reg dest, const OpArg& arg);

  void PADDSB(X64Reg dest, const OpArg& arg);
  void PADDSW(X64Reg dest, const OpArg& arg);
  void PADDUSB(X64Reg dest, const OpArg& arg);
  void PADDUSW(X64Reg dest, const OpArg& arg);

  void PSUBB(X64Reg dest, const OpArg& arg);
  void PSUBW(X64Reg dest, const OpArg& arg);
  void PSUBD(X64Reg dest, const OpArg& arg);
  void PSUBQ(X64Reg dest, const OpArg& arg);

  void PSUBSB(X64Reg dest, const OpArg& arg);
  void PSUBSW(X64Reg dest, const OpArg& arg);
  void PSUBUSB(X64Reg dest, const OpArg& arg);
  void PSUBUSW(X64Reg dest, const OpArg& arg);

  void PAVGB(X64Reg dest, const OpArg& arg);
  void PAVGW(X64Reg dest, const OpArg& arg);

  void PCMPEQB(X64Reg dest, const OpArg& arg);
  void PCMPEQW(X64Reg dest, const OpArg& arg);
  void PCMPEQD(X64Reg dest, const OpArg& arg);

  void PCMPGTB(X64Reg dest, const OpArg& arg);
  void PCMPGTW(X64Reg dest, const OpArg& arg);
  void PCMPGTD(X64Reg dest, const OpArg& arg);

  void PEXTRW(X64Reg dest, const OpArg& arg, u8 subreg);
  void PINSRW(X64Reg dest, const OpArg& arg, u8 subreg);
  void PINSRD(X64Reg dest, const OpArg& arg, u8 subreg);

  void PMADDWD(X64Reg dest, const OpArg& arg);
  void PSADBW(X64Reg dest, const OpArg& arg);

  void PMAXSW(X64Reg dest, const OpArg& arg);
  void PMAXUB(X64Reg dest, const OpArg& arg);
  void PMINSW(X64Reg dest, const OpArg& arg);
  void PMINUB(X64Reg dest, const OpArg& arg);

  void PMOVMSKB(X64Reg dest, const OpArg& arg);
  void PSHUFD(X64Reg dest, const OpArg& arg, u8 shuffle);
  void PSHUFB(X64Reg dest, const OpArg& arg);

  void PSHUFLW(X64Reg dest, const OpArg& arg, u8 shuffle);
  void PSHUFHW(X64Reg dest, const OpArg& arg, u8 shuffle);

  // Immediate shifts of a whole register
  void PSRLW(X64Reg reg, u8 shift);
  void PSRLD(X64Reg reg, u8 shift);
  void PSRLQ(X64Reg reg, u8 shift);
  void PSRLQ(X64Reg reg, const OpArg& arg);
  void PSRLDQ(X64Reg reg, u8 shift);

  void PSLLW(X64Reg reg, u8 shift);
  void PSLLD(X64Reg reg, u8 shift);
  void PSLLQ(X64Reg reg, u8 shift);
  void PSLLDQ(X64Reg reg, u8 shift);

  // XMM0-XMM7 only
  void PSRAW(X64Reg reg, u8 shift);
  void PSRAD(X64Reg reg, u8 shift);

  // SSE4.1 widening moves
  void PMOVSXBW(X64Reg dest, const OpArg& arg);
  void PMOVSXBD(X64Reg dest, const OpArg& arg);
  void PMOVSXBQ(X64Reg dest, const OpArg& arg);
  void PMOVSXWD(X64Reg dest, const OpArg& arg);
  void PMOVSXWQ(X64Reg dest, const OpArg& arg);
  void PMOVSXDQ(X64Reg dest, const OpArg& arg);
  void PMOVZXBW(X64Reg dest, const OpArg& arg);
  void PMOVZXBD(X64Reg dest, const OpArg& arg);
  void PMOVZXBQ(X64Reg dest, const OpArg& arg);
  void PMOVZXWD(X64Reg dest, const OpArg& arg);
  void PMOVZXWQ(X64Reg dest, const OpArg& arg);
  void PMOVZXDQ(X64Reg dest, const OpArg& arg);

  // SSE4.1 blends (the V forms without a mask operand use XMM0)
  void PBLENDVB(X64Reg dest, const OpArg& arg);
  void BLENDVPS(X64Reg dest, const OpArg& arg);
  void BLENDVPD(X64Reg dest, const OpArg& arg);
  void BLENDPS(X64Reg dest, const OpArg& arg, u8 blend);
  void BLENDPD(X64Reg dest, const OpArg& arg, u8 blend);

  void PCMPEQQ(X64Reg dest, const OpArg& arg);

  // AVX (three-operand: regOp1 = regOp2 op arg)
  void VADDSS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VSUBSS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VMULSS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VDIVSS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VADDPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VSUBPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VMULPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VDIVPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VADDSD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VSUBSD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VMULSD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VDIVSD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VADDPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VSUBPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VMULPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VDIVPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VSQRTSD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VCMPPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, u8 compare);
  void VSHUFPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, u8 shuffle);
  void VSHUFPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, u8 shuffle);
  void VUNPCKLPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VUNPCKLPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VUNPCKHPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VBLENDVPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, X64Reg mask);
  void VBLENDPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, u8 blend);
  void VBLENDPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, u8 blend);

  void VANDPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VANDPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VANDNPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VANDNPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VORPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VORPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VXORPS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VXORPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);

  void VPAND(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VPANDN(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VPOR(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void VPXOR(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);

  void VPSLLQ(X64Reg regOp1, X64Reg regOp2, u8 shift);

  void VMOVAPS(const OpArg& arg, X64Reg regOp);

  void VZEROUPPER();

  // FMA3. 132: r1 = r1 * arg + r2; 213: r1 = r2 * r1 + arg; 231: r1 = r2 * arg + r1.
#define PIXELU_FMA3_DECL(form)                                                                     \
  void VFMADD##form##PS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMADD##form##PD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMADD##form##SS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMADD##form##SD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMSUB##form##PS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMSUB##form##PD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMSUB##form##SS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFMSUB##form##SD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                           \
  void VFNMADD##form##PS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMADD##form##PD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMADD##form##SS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMADD##form##SD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMSUB##form##PS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMSUB##form##PD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMSUB##form##SS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFNMSUB##form##SD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void VFMADDSUB##form##PS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                        \
  void VFMADDSUB##form##PD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                        \
  void VFMSUBADD##form##PS(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                        \
  void VFMSUBADD##form##PD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  PIXELU_FMA3_DECL(132)
  PIXELU_FMA3_DECL(213)
  PIXELU_FMA3_DECL(231)
#undef PIXELU_FMA3_DECL

  // FMA4 (AMD). dest = regOp1 * (second source) +/- (third source); either the second or the
  // third source may be the memory operand.
#define PIXELU_FMA4_DECL(name)                                                                     \
  void name(X64Reg dest, X64Reg regOp1, X64Reg regOp2, const OpArg& arg);                          \
  void name(X64Reg dest, X64Reg regOp1, const OpArg& arg, X64Reg regOp2);
  PIXELU_FMA4_DECL(VFMADDSUBPS)
  PIXELU_FMA4_DECL(VFMADDSUBPD)
  PIXELU_FMA4_DECL(VFMSUBADDPS)
  PIXELU_FMA4_DECL(VFMSUBADDPD)
  PIXELU_FMA4_DECL(VFMADDPS)
  PIXELU_FMA4_DECL(VFMADDPD)
  PIXELU_FMA4_DECL(VFMADDSS)
  PIXELU_FMA4_DECL(VFMADDSD)
  PIXELU_FMA4_DECL(VFMSUBPS)
  PIXELU_FMA4_DECL(VFMSUBPD)
  PIXELU_FMA4_DECL(VFMSUBSS)
  PIXELU_FMA4_DECL(VFMSUBSD)
  PIXELU_FMA4_DECL(VFNMADDPS)
  PIXELU_FMA4_DECL(VFNMADDPD)
  PIXELU_FMA4_DECL(VFNMADDSS)
  PIXELU_FMA4_DECL(VFNMADDSD)
  PIXELU_FMA4_DECL(VFNMSUBPS)
  PIXELU_FMA4_DECL(VFNMSUBPD)
  PIXELU_FMA4_DECL(VFNMSUBSS)
  PIXELU_FMA4_DECL(VFNMSUBSD)
#undef PIXELU_FMA4_DECL

  // BMI1/BMI2 (VEX-encoded general purpose instructions)
  void SARX(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2);
  void SHLX(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2);
  void SHRX(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2);
  void RORX(int bits, X64Reg regOp, const OpArg& arg, u8 rotate);
  void PEXT(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void PDEP(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void MULX(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg);
  void BZHI(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2);
  void BLSR(int bits, X64Reg regOp, const OpArg& arg);
  void BLSMSK(int bits, X64Reg regOp, const OpArg& arg);
  void BLSI(int bits, X64Reg regOp, const OpArg& arg);
  void BEXTR(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2);
  void ANDN(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg);

  void RDTSC();

  // Calls func, directly when it is within a rel32 of the call, otherwise through RAX.
  template <typename FunctionPointer>
  void ABI_CallFunction(FunctionPointer func)
  {
    static_assert(std::is_pointer_v<FunctionPointer> &&
                      std::is_function_v<std::remove_pointer_t<FunctionPointer>>,
                  "ABI_CallFunction needs a function pointer");

    const void* const target = reinterpret_cast<const void*>(func);
    const s64 rel = static_cast<s64>(reinterpret_cast<u64>(target) -
                                     (reinterpret_cast<u64>(code) + NEAR_JMP_LEN));
    if (rel == static_cast<s32>(rel))
    {
      CALL(target);
    }
    else
    {
      MOV(64, R(RAX), Imm64(reinterpret_cast<u64>(target)));
      CALLptr(R(RAX));
    }
  }

  // The suffix letters give the arguments in order: C = a 32-bit (or 16-bit) constant,
  // P = a pointer constant, R = a register, A = an OpArg of the given width.
  // The all-constant forms load ABI_PARAM1, ABI_PARAM2, ... from left to right.
  template <typename FunctionPointer>
  void ABI_CallFunctionC(FunctionPointer func, u32 param1)
  {
    CallWithConstantArgs(func, param1);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionC16(FunctionPointer func, u16 param1)
  {
    CallWithConstantArgs(func, param1);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionP(FunctionPointer func, const void* param1)
  {
    CallWithConstantArgs(func, param1);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionCC(FunctionPointer func, u32 param1, u32 param2)
  {
    CallWithConstantArgs(func, param1, param2);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionCC16(FunctionPointer func, u32 param1, u16 param2)
  {
    CallWithConstantArgs(func, param1, param2);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionCP(FunctionPointer func, u32 param1, const void* param2)
  {
    CallWithConstantArgs(func, param1, param2);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionPC(FunctionPointer func, const void* param1, u32 param2)
  {
    CallWithConstantArgs(func, param1, param2);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionPP(FunctionPointer func, const void* param1, const void* param2)
  {
    CallWithConstantArgs(func, param1, param2);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionCCC(FunctionPointer func, u32 param1, u32 param2, u32 param3)
  {
    CallWithConstantArgs(func, param1, param2, param3);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionCCP(FunctionPointer func, u32 param1, u32 param2, const void* param3)
  {
    CallWithConstantArgs(func, param1, param2, param3);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionPPC(FunctionPointer func, const void* param1, const void* param2, u32 param3)
  {
    CallWithConstantArgs(func, param1, param2, param3);
  }
  template <typename FunctionPointer>
  void ABI_CallFunctionCCCP(FunctionPointer func, u32 param1, u32 param2, u32 param3,
                            const void* param4)
  {
    CallWithConstantArgs(func, param1, param2, param3, param4);
  }

  template <typename FunctionPointer>
  void ABI_CallFunctionR(FunctionPointer func, X64Reg reg1)
  {
    if (reg1 != ABI_PARAM1)
      MOV(32, R(ABI_PARAM1), R(reg1));
    ABI_CallFunction(func);
  }

  template <typename FunctionPointer>
  void ABI_CallFunctionPR(FunctionPointer func, const void* ptr, X64Reg reg1)
  {
    if (reg1 != ABI_PARAM2)
      MOV(64, R(ABI_PARAM2), R(reg1));
    MOV(64, R(ABI_PARAM1), ImmPtr(ptr));
    ABI_CallFunction(func);
  }

  template <typename FunctionPointer>
  void ABI_CallFunctionRR(FunctionPointer func, X64Reg reg1, X64Reg reg2)
  {
    MOVTwo(64, ABI_PARAM1, reg1, 0, ABI_PARAM2, reg2);
    ABI_CallFunction(func);
  }

  template <typename FunctionPointer>
  void ABI_CallFunctionPRR(FunctionPointer func, const void* ptr, X64Reg reg1, X64Reg reg2)
  {
    MOVTwo(64, ABI_PARAM2, reg1, 0, ABI_PARAM3, reg2);
    MOV(64, R(ABI_PARAM1), ImmPtr(ptr));
    ABI_CallFunction(func);
  }

  // The OpArg is moved first, so it may live in any of the argument registers the constants
  // go to afterwards.
  template <typename FunctionPointer>
  void ABI_CallFunctionA(int bits, FunctionPointer func, const Gen::OpArg& arg1)
  {
    LoadOpArg(bits, ABI_PARAM1, arg1);
    ABI_CallFunction(func);
  }

  template <typename FunctionPointer>
  void ABI_CallFunctionAC(int bits, FunctionPointer func, const Gen::OpArg& arg1, u32 param2)
  {
    LoadOpArg(bits, ABI_PARAM1, arg1);
    LoadConstantArg(ABI_PARAM2, param2);
    ABI_CallFunction(func);
  }

  template <typename FunctionPointer>
  void ABI_CallFunctionPAC(int bits, FunctionPointer func, const void* ptr1, const Gen::OpArg& arg2,
                           u32 param3)
  {
    LoadOpArg(bits, ABI_PARAM2, arg2);
    LoadConstantArg(ABI_PARAM3, param3);
    LoadConstantArg(ABI_PARAM1, ptr1);
    ABI_CallFunction(func);
  }

  // dst1 = src1 + offset, dst2 = src2, in an order (or with an XCHG) that works whichever
  // registers overlap.
  void MOVTwo(int bits, X64Reg dst1, X64Reg src1, s32 offset, X64Reg dst2, X64Reg src2);

  // Saves the registers in mask and moves RSP so that it is 16-byte aligned for a call, given
  // that RSP % 16 == rsp_alignment % 16 on entry, and leaves needed_frame_size bytes (above the
  // shadow space) free for the caller. Returns the size of the shadow space, i.e. the RSP offset
  // of that free area. Pop with the same arguments.
  size_t ABI_PushRegistersAndAdjustStack(BitSet32 mask, size_t rsp_alignment,
                                         size_t needed_frame_size = 0);
  void ABI_PopRegistersAndAdjustStack(BitSet32 mask, size_t rsp_alignment,
                                      size_t needed_frame_size = 0);

  // Calling a std::function's operator() directly from generated code is not possible (it may be
  // a thunk), so generated code calls this static trampoline instead.
  template <typename T, typename... Args>
  static T CallLambdaTrampoline(const std::function<T(Args...)>* f, Args... args)
  {
    return (*f)(args...);
  }

  template <typename T, typename... Args>
  void ABI_CallLambdaPC(const std::function<T(Args...)>* f, void* p1, u32 p2)
  {
    T (*const trampoline)(const std::function<T(Args...)>*, Args...) =
        &XEmitter::CallLambdaTrampoline<T, Args...>;
    ABI_CallFunctionPPC(trampoline, static_cast<const void*>(f), p1, p2);
  }

private:
  // One constant call argument: a 32-bit move for integers, a 64-bit one for pointers.
  void LoadConstantArg(X64Reg reg, u32 value) { MOV(32, R(reg), Imm32(value)); }
  void LoadConstantArg(X64Reg reg, const void* value) { MOV(64, R(reg), ImmPtr(value)); }
  // An OpArg argument, unless it already is that register.
  void LoadOpArg(int bits, X64Reg reg, const OpArg& arg)
  {
    if (!arg.IsSimpleReg(reg))
      MOV(bits, R(reg), arg);
  }

  template <typename FunctionPointer, typename... Args>
  void CallWithConstantArgs(FunctionPointer func, Args... args)
  {
    static_assert(sizeof...(Args) >= 1 && sizeof...(Args) <= 4);
    const X64Reg arg_regs[] = {ABI_PARAM1, ABI_PARAM2, ABI_PARAM3, ABI_PARAM4};
    size_t next = 0;
    (LoadConstantArg(arg_regs[next++], args), ...);
    ABI_CallFunction(func);
  }
};

class X64CodeBlock : public Common::CodeBlock<XEmitter>
{
private:
  // INT3 everywhere
  void PoisonMemory() override { std::memset(region, 0xCC, region_size); }
};

}  // namespace Gen
