// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/x64Emitter.h"

#include <cstring>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/x64Reg.h"

// Encoding reference: Intel 64 and IA-32 Architectures Software Developer's Manual, volume 2
// (chapter 2, "Instruction Format", and the opcode maps in appendix A), and the AMD64
// Architecture Programmer's Manual, volume 3 (FMA4).
//
// An instruction is written as
//   [0x66] [F2/F3] [REX] opcode... ModRM [SIB] [displacement] [immediate]
// or, for VEX-encoded instructions, as
//   C5 xx / C4 xx xx  opcode ModRM [SIB] [displacement] [immediate]

namespace Gen
{
namespace
{
// The register number fields: the low three bits go into ModRM/SIB/opcode, bit 3 into REX/VEX.
constexpr u32 Low3(u32 reg)
{
  return reg & 7;
}
constexpr bool Ext(u32 reg)
{
  return (reg & 8) != 0;
}

// SPL, BPL, SIL and DIL are numbers 4-7 like AH, CH, DH and BH; a REX prefix (even an empty one)
// is what selects them. The high-byte registers carry the 0x100 flag and must not get a REX.
constexpr bool NeedsRexForByte(u32 reg)
{
  return (reg & 0x10C) == 4;
}

constexpr bool FitsS8(s64 value)
{
  return value >= -128 && value <= 127;
}
constexpr bool FitsS32(s64 value)
{
  return value == static_cast<s32>(value);
}

// The integer ALU operations. The eight classic ones are numbered as their ModRM /digit in the
// 80/81/83 immediate group; their register forms are at opcode (digit << 3) + 0..3.
enum ArithOp
{
  kAdd = 0,
  kOr = 1,
  kAdc = 2,
  kSbb = 3,
  kAnd = 4,
  kSub = 5,
  kXor = 6,
  kCmp = 7,
  kMov,
  kTest,
  kXchg,
};

constexpr bool IsClassicAlu(int op)
{
  return op <= kCmp;
}

// The opcode printed for an encoding that does not exist (an operand combination that has
// already been reported with an assert). 0xCC traps if it is ever executed.
constexpr u8 kNoEncoding = 0xCC;

// ModRM.reg value of an immediate form.
constexpr int ImmDigit(int op)
{
  if (IsClassicAlu(op))
    return op;
  if (op == kXchg)
    return 7;
  return 0;  // MOV C6/C7 /0, TEST F6/F7 /0
}

// Opcode of "op r/m8, imm8" / "op r/m, imm16/32".
constexpr u8 ImmGroupOpcode(int op, bool byte)
{
  if (IsClassicAlu(op))
    return byte ? 0x80 : 0x81;
  if (op == kMov)
    return byte ? 0xC6 : 0xC7;
  if (op == kTest)
    return byte ? 0xF6 : 0xF7;
  return kNoEncoding;
}

// Opcode of "op r/m, sign-extended imm8", which only the classic ALU operations have.
constexpr u8 SignedImm8Opcode(int op)
{
  return IsClassicAlu(op) ? 0x83 : kNoEncoding;
}
constexpr bool HasSignedImm8(int op)
{
  return IsClassicAlu(op);
}

// Opcode of the short "op AL/AX/EAX/RAX, imm" form, or kNoEncoding.
constexpr u8 AccumulatorOpcode(int op, bool byte)
{
  if (IsClassicAlu(op))
    return static_cast<u8>((op << 3) + (byte ? 4 : 5));
  if (op == kTest)
    return byte ? 0xA8 : 0xA9;
  return kNoEncoding;
}

// Opcode of the register/memory form: to_rm is "op r/m, reg", otherwise "op reg, r/m".
constexpr u8 RegFormOpcode(int op, bool byte, bool to_rm)
{
  const u8 width = byte ? 0 : 1;
  switch (op)
  {
  case kMov:
    return static_cast<u8>(0x88 + width + (to_rm ? 0 : 2));
  case kTest:  // symmetric, only one direction exists
    return static_cast<u8>(0x84 + width);
  case kXchg:
    return static_cast<u8>(0x86 + width);
  default:
    return static_cast<u8>((op << 3) + width + (to_rm ? 0 : 2));
  }
}

// VEX.pp and VEX.mmmmm for a legacy prefix and an opcode written as 0x38xx / 0x3Axx / 0xxx.
constexpr int VexPP(u8 prefix)
{
  switch (prefix)
  {
  case 0x66:
    return 1;
  case 0xF3:
    return 2;
  case 0xF2:
    return 3;
  default:
    return 0;
  }
}
constexpr int VexMap(u16 op)
{
  switch (op >> 8)
  {
  case 0x38:
    return 2;
  case 0x3A:
    return 3;
  default:
    return 1;
  }
}

constexpr bool IsYmm(X64Reg reg)
{
  return reg != INVALID_REG && (reg & 0x100) != 0;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Output buffer

void XEmitter::SetCodePtr(u8* ptr, u8* end, bool write_failed)
{
  code = ptr;
  m_code_end = end;
  m_write_failed = write_failed;
}

bool XEmitter::Fits(size_t bytes)
{
  if (static_cast<size_t>(m_code_end - code) >= bytes && code <= m_code_end)
    return true;
  code = m_code_end;
  m_write_failed = true;
  return false;
}

void XEmitter::Write8(u8 value)
{
  if (Fits(1))
    *code++ = value;
}

void XEmitter::Write16(u16 value)
{
  if (!Fits(sizeof(value)))
    return;
  std::memcpy(code, &value, sizeof(value));
  code += sizeof(value);
}

void XEmitter::Write32(u32 value)
{
  if (!Fits(sizeof(value)))
    return;
  std::memcpy(code, &value, sizeof(value));
  code += sizeof(value);
}

void XEmitter::Write64(u64 value)
{
  if (!Fits(sizeof(value)))
    return;
  std::memcpy(code, &value, sizeof(value));
  code += sizeof(value);
}

void XEmitter::ReserveCodeSpace(int bytes)
{
  if (bytes <= 0)
    return;
  if (!Fits(static_cast<size_t>(bytes)))
    return;
  std::memset(code, 0xCC, static_cast<size_t>(bytes));
  code += bytes;
}

u8* XEmitter::AlignCodeTo(size_t alignment)
{
  ASSERT_MSG(DYNA_REC, alignment != 0 && (alignment & (alignment - 1)) == 0,
             "AlignCodeTo: {} is not a power of two", alignment);
  const size_t misalignment = reinterpret_cast<uintptr_t>(code) & (alignment - 1);
  if (misalignment != 0)
    ReserveCodeSpace(static_cast<int>(alignment - misalignment));
  return code;
}

u8* XEmitter::AlignCode4()
{
  return AlignCodeTo(4);
}

u8* XEmitter::AlignCode16()
{
  return AlignCodeTo(16);
}

u8* XEmitter::AlignCodePage()
{
  return AlignCodeTo(4096);
}

void XEmitter::CheckFlags()
{
  ASSERT_MSG(DYNA_REC, !flags_locked, "An instruction that changes the flags was emitted while "
                                      "the flags were locked");
}

// ---------------------------------------------------------------------------------------------
// Operand encoding

// REX = 0100WRXB. It is needed for 64-bit operand size, for any register number above 7, and to
// reach SPL/BPL/SIL/DIL in byte operations.
void XEmitter::EmitRex(const OpArg& rm, u32 reg, bool wide, bool rm_is_byte, bool reg_is_byte)
{
  u8 rex = 0x40;
  if (wide)
    rex |= 8;
  if (Ext(reg))
    rex |= 4;
  if (Ext(rm.m_index))
    rex |= 2;
  if (Ext(rm.m_base))
    rex |= 1;

  const bool byte_reg_needs_rex = (rm_is_byte && rm.m_kind == SCALE_NONE &&
                                   NeedsRexForByte(rm.m_base)) ||
                                  (reg_is_byte && NeedsRexForByte(reg));
  if (rex == 0x40 && !byte_reg_needs_rex)
    return;

  Write8(rex);
  // With a REX prefix the numbers 4-7 mean SPL..DIL, so AH..BH cannot be used.
  DEBUG_ASSERT((rm.m_base & 0x100) == 0);
  DEBUG_ASSERT((reg & 0x100) == 0);
}

// ModRM, SIB and displacement for rm, with reg in ModRM.reg. trailing_bytes is the size of
// whatever follows the displacement (an immediate); RIP-relative displacements are measured from
// the end of the instruction, so it has to be known here.
void XEmitter::EmitOperand(const OpArg& rm, u32 reg, int trailing_bytes, bool check_rip)
{
  const u32 reg_field = Low3(reg) << 3;
  const u8 kind = rm.m_kind;

  if (kind == SCALE_NONE)
  {
    Write8(static_cast<u8>(0xC0 | reg_field | Low3(rm.m_base)));
    return;
  }

  if (kind == SCALE_RIP)
  {
    // mod 00, rm 101: [rip + disp32]
    Write8(static_cast<u8>(0x05 | reg_field));
    const u64 next_ip = reinterpret_cast<u64>(code) + 4 + trailing_bytes;
    const s64 rel = static_cast<s64>(rm.m_value - next_ip);
    ASSERT_MSG(DYNA_REC, FitsS32(rel) || !check_rip,
               "RIP-relative operand {:#x} is out of reach of the code at {:#x}", rm.m_value,
               next_ip);
    Write32(static_cast<u32>(rel));
    return;
  }

  // SIB.ss for a scale
  const auto scale_bits = [](u32 scale) -> u32 {
    switch (scale)
    {
    case 2:
      return 1;
    case 4:
      return 2;
    case 8:
      return 3;
    default:
      return 0;
    }
  };

  if (kind == SCALE_NOBASE_2 || kind == SCALE_NOBASE_4 || kind == SCALE_NOBASE_8)
  {
    // mod 00 with SIB base 101: [index * scale + disp32]
    Write8(static_cast<u8>(0x04 | reg_field));
    Write8(static_cast<u8>((scale_bits(kind & 0x0F) << 6) | (Low3(rm.m_index) << 3) | 5));
    Write32(static_cast<u32>(rm.m_value));
    return;
  }

  const bool indexed = kind != SCALE_ATREG;
  ASSERT_MSG(DYNA_REC, !indexed || kind == SCALE_1 || kind == SCALE_2 || kind == SCALE_4 ||
                           kind == SCALE_8,
             "Operand kind {} cannot be encoded in ModRM", kind);

  const u32 base = Low3(rm.m_base);
  // rm 100 means "SIB follows", so RSP/R12 as a base always takes a SIB byte.
  const bool sib = indexed || base == 4;

  // rm (or SIB base) 101 with mod 00 means "no base", so RBP/R13 always take a displacement.
  const s32 disp = static_cast<s32>(rm.m_value);
  u32 mod;
  if (disp == 0 && base != 5)
    mod = 0;
  else if (FitsS8(disp))
    mod = 1;
  else
    mod = 2;

  Write8(static_cast<u8>((mod << 6) | reg_field | (sib ? 4 : base)));
  if (sib)
  {
    // index 100 means "no index"
    const u32 index = indexed ? Low3(rm.m_index) : 4;
    const u32 ss = indexed ? scale_bits(kind) : 0;
    Write8(static_cast<u8>((ss << 6) | (index << 3) | base));
  }
  if (mod == 1)
    Write8(static_cast<u8>(disp));
  else if (mod == 2)
    Write32(static_cast<u32>(disp));
}

// [66] [REX] op0 [op1 [op2]] ModRM... for the common legacy-encoded register/memory forms.
void XEmitter::EmitRegOp(int bits, const OpArg& rm, u32 reg, bool rm_is_byte, bool reg_is_byte,
                         u8 op0, int op1, int op2, int trailing_bytes)
{
  if (bits == 16)
    Write8(0x66);
  EmitRex(rm, reg, bits == 64, rm_is_byte, reg_is_byte);
  Write8(op0);
  if (op1 >= 0)
    Write8(static_cast<u8>(op1));
  if (op2 >= 0)
    Write8(static_cast<u8>(op2));
  EmitOperand(rm, reg, trailing_bytes);
}

// ---------------------------------------------------------------------------------------------
// Control flow

void XEmitter::JMP(const u8* addr, bool force_near_padding)
{
  const u64 target = reinterpret_cast<u64>(addr);
  const u64 here = reinterpret_cast<u64>(code);
  const s64 short_rel = static_cast<s64>(target - (here + SHORT_JMP_LEN));
  if (FitsS8(short_rel))
  {
    Write8(0xEB);
    Write8(static_cast<u8>(short_rel));
    // Pad to the size of a near jump with INT3, which also stops speculation past the jump.
    if (force_near_padding)
    {
      for (int i = SHORT_JMP_LEN; i < NEAR_JMP_LEN; i++)
        INT3();
    }
    return;
  }

  const s64 near_rel = static_cast<s64>(target - (here + NEAR_JMP_LEN));
  ASSERT_MSG(DYNA_REC, FitsS32(near_rel), "JMP target is too far away ({})", near_rel);
  Write8(0xE9);
  Write32(static_cast<u32>(near_rel));
}

void XEmitter::JMPptr(const OpArg& arg)
{
  ASSERT_MSG(DYNA_REC, !arg.IsImm(), "JMPptr needs a register or memory operand");
  EmitRex(arg, 0, false, false, false);
  Write8(0xFF);
  EmitOperand(arg, 4);
}

void XEmitter::JMPself()
{
  Write8(0xEB);
  Write8(0xFE);
}

void XEmitter::CALLptr(OpArg arg)
{
  ASSERT_MSG(DYNA_REC, !arg.IsImm(), "CALLptr needs a register or memory operand");
  EmitRex(arg, 0, false, false, false);
  Write8(0xFF);
  EmitOperand(arg, 2);
}

void XEmitter::CALL(const void* fnptr)
{
  const s64 rel =
      static_cast<s64>(reinterpret_cast<u64>(fnptr) - (reinterpret_cast<u64>(code) + 5));
  ASSERT_MSG(DYNA_REC, FitsS32(rel), "CALL target {} is out of reach of {}", fmt::ptr(fnptr),
             fmt::ptr(code));
  Write8(0xE8);
  Write32(static_cast<u32>(rel));
}

FixupBranch XEmitter::CALL()
{
  FixupBranch branch{code + 5, FixupBranch::Type::Branch32Bit};
  Write8(0xE8);
  Write32(0);
  // A branch that was not written completely must never be patched.
  if (m_write_failed)
    branch.ptr = nullptr;
  return branch;
}

FixupBranch XEmitter::J(Jump jump)
{
  FixupBranch branch;
  if (jump == Jump::Near)
  {
    branch = {code + 5, FixupBranch::Type::Branch32Bit};
    Write8(0xE9);
    Write32(0);
  }
  else
  {
    branch = {code + 2, FixupBranch::Type::Branch8Bit};
    Write8(0xEB);
    Write8(0);
  }
  if (m_write_failed)
    branch.ptr = nullptr;
  return branch;
}

FixupBranch XEmitter::J_CC(CCFlags conditionCode, Jump jump)
{
  FixupBranch branch;
  if (jump == Jump::Near)
  {
    branch = {code + 6, FixupBranch::Type::Branch32Bit};
    Write8(0x0F);
    Write8(static_cast<u8>(0x80 + conditionCode));
    Write32(0);
  }
  else
  {
    branch = {code + 2, FixupBranch::Type::Branch8Bit};
    Write8(static_cast<u8>(0x70 + conditionCode));
    Write8(0);
  }
  if (m_write_failed)
    branch.ptr = nullptr;
  return branch;
}

void XEmitter::J_CC(CCFlags conditionCode, const u8* addr)
{
  const u64 target = reinterpret_cast<u64>(addr);
  const u64 here = reinterpret_cast<u64>(code);
  const s64 short_rel = static_cast<s64>(target - (here + 2));
  if (FitsS8(short_rel))
  {
    Write8(static_cast<u8>(0x70 + conditionCode));
    Write8(static_cast<u8>(short_rel));
    return;
  }

  const s64 near_rel = static_cast<s64>(target - (here + 6));
  ASSERT_MSG(DYNA_REC, FitsS32(near_rel), "Jcc target is too far away ({})", near_rel);
  Write8(0x0F);
  Write8(static_cast<u8>(0x80 + conditionCode));
  Write32(static_cast<u32>(near_rel));
}

void XEmitter::SetJumpTarget(const FixupBranch& branch)
{
  if (branch.ptr == nullptr)
    return;

  const s64 rel = code - branch.ptr;
  if (branch.type == FixupBranch::Type::Branch8Bit)
  {
    ASSERT_MSG(DYNA_REC, FitsS8(rel), "Short jump cannot reach its target ({}); use Jump::Near",
               rel);
    branch.ptr[-1] = static_cast<u8>(rel);
  }
  else
  {
    ASSERT_MSG(DYNA_REC, FitsS32(rel), "Near jump cannot reach its target ({})", rel);
    const s32 rel32 = static_cast<s32>(rel);
    std::memcpy(branch.ptr - 4, &rel32, sizeof(rel32));
  }
}

void XEmitter::RET()
{
  Write8(0xC3);
}

// REP RET: a two-byte return, better than a plain RET as a branch target on old AMD CPUs.
void XEmitter::RET_FAST()
{
  Write8(0xF3);
  Write8(0xC3);
}

void XEmitter::UD2()
{
  Write8(0x0F);
  Write8(0x0B);
}

void XEmitter::INT3()
{
  Write8(0xCC);
}

// ---------------------------------------------------------------------------------------------
// Simple instructions

// The multi-byte NOPs recommended by the Intel SDM (NOP r/m, opcode 0F 1F /0). Longer ones are a
// shorter form with 0x66 prefixes in front; more than three prefixes are avoided because some CPUs
// decode them slowly, so a long run is split into 11-byte pieces.
void XEmitter::NOP(size_t size)
{
  DEBUG_ASSERT(static_cast<int>(size) > 0);
  while (size != 0)
  {
    const size_t piece = size > 11 ? 11 : size;
    size -= piece;

    size_t prefixes;
    size_t body;
    switch (piece)
    {
    case 2:
    case 6:
      prefixes = 1;
      body = piece - 1;
      break;
    case 9:
    case 10:
    case 11:
      prefixes = piece - 8;
      body = 8;
      break;
    default:
      prefixes = 0;
      body = piece;
      break;
    }

    for (size_t i = 0; i < prefixes; i++)
      Write8(0x66);
    if (body == 1)
    {
      Write8(0x90);
      continue;
    }

    // ModRM (and SIB) of the memory operand, then a zero displacement
    u8 modrm;
    size_t zeros;
    switch (body)
    {
    case 3:  // nop [rax]
      modrm = 0x00;
      zeros = 0;
      break;
    case 4:  // nop [rax + disp8]
      modrm = 0x40;
      zeros = 1;
      break;
    case 5:  // nop [rax + rax*1 + disp8]
      modrm = 0x44;
      zeros = 2;
      break;
    case 7:  // nop [rax + disp32]
      modrm = 0x80;
      zeros = 4;
      break;
    default:  // 8: nop [rax + rax*1 + disp32]
      modrm = 0x84;
      zeros = 5;
      break;
    }
    Write8(0x0F);
    Write8(0x1F);
    Write8(modrm);
    for (size_t i = 0; i < zeros; i++)
      Write8(0x00);
  }
}

void XEmitter::PAUSE()
{
  Write8(0xF3);
  Write8(0x90);
}

void XEmitter::CLC()
{
  CheckFlags();
  Write8(0xF8);
}

void XEmitter::CMC()
{
  CheckFlags();
  Write8(0xF5);
}

void XEmitter::STC()
{
  CheckFlags();
  Write8(0xF9);
}

// xchg ah, al
void XEmitter::XCHG_AHAL()
{
  Write8(0x86);
  Write8(0xE0);
}

void XEmitter::LAHF()
{
  Write8(0x9F);
}

void XEmitter::SAHF()
{
  CheckFlags();
  Write8(0x9E);
}

void XEmitter::PUSHF()
{
  Write8(0x9C);
}

void XEmitter::POPF()
{
  CheckFlags();
  Write8(0x9D);
}

void XEmitter::LFENCE()
{
  Write8(0x0F);
  Write8(0xAE);
  Write8(0xE8);
}

void XEmitter::MFENCE()
{
  Write8(0x0F);
  Write8(0xAE);
  Write8(0xF0);
}

void XEmitter::SFENCE()
{
  Write8(0x0F);
  Write8(0xAE);
  Write8(0xF8);
}

void XEmitter::RDTSC()
{
  Write8(0x0F);
  Write8(0x31);
}

void XEmitter::CWD(int bits)
{
  if (bits == 16)
    Write8(0x66);
  else if (bits == 64)
    Write8(0x48);
  Write8(0x99);
}

void XEmitter::CBW(int bits)
{
  if (bits == 8)
    Write8(0x66);
  else if (bits == 32)
    Write8(0x48);
  Write8(0x98);
}

void XEmitter::LOCK()
{
  Write8(0xF0);
}

void XEmitter::REP()
{
  Write8(0xF3);
}

void XEmitter::REPNE()
{
  Write8(0xF2);
}

void XEmitter::FSOverride()
{
  Write8(0x64);
}

void XEmitter::GSOverride()
{
  Write8(0x65);
}

// ---------------------------------------------------------------------------------------------
// Stack

// PUSH/POP r64 (50+r / 58+r) default to 64-bit operands and need REX only for R8-R15.
void XEmitter::PUSH(X64Reg reg)
{
  if (Ext(reg))
    Write8(0x41);
  Write8(static_cast<u8>(0x50 + Low3(reg)));
}

void XEmitter::POP(X64Reg reg)
{
  if (Ext(reg))
    Write8(0x41);
  Write8(static_cast<u8>(0x58 + Low3(reg)));
}

void XEmitter::PUSH(int bits, const OpArg& reg)
{
  if (reg.IsSimpleReg())
  {
    PUSH(reg.GetSimpleReg());
    return;
  }

  if (reg.IsImm())
  {
    switch (reg.GetImmBits())
    {
    case 8:  // sign-extended
      Write8(0x6A);
      Write8(static_cast<u8>(reg.m_value));
      break;
    case 16:
      Write8(0x66);
      Write8(0x68);
      Write16(static_cast<u16>(reg.m_value));
      break;
    case 32:  // sign-extended
      Write8(0x68);
      Write32(static_cast<u32>(reg.m_value));
      break;
    default:
      ASSERT_MSG(DYNA_REC, 0, "PUSH cannot push a {}-bit immediate", reg.GetImmBits());
      break;
    }
    return;
  }

  // FF /6
  EmitRegOp(bits, reg, 6, bits == 8, false, 0xFF);
}

void XEmitter::POP(int /*bits*/, const OpArg& reg)
{
  if (reg.IsSimpleReg())
    POP(reg.GetSimpleReg());
  else
    ASSERT_MSG(DYNA_REC, 0, "POP only supports register operands");
}

// ---------------------------------------------------------------------------------------------
// Miscellaneous integer instructions

void XEmitter::BSWAP(int bits, X64Reg reg)
{
  if (bits >= 32)
  {
    // 0F C8+r
    if (bits == 64 || Ext(reg))
      Write8(static_cast<u8>(0x40 | (bits == 64 ? 8 : 0) | (Ext(reg) ? 1 : 0)));
    Write8(0x0F);
    Write8(static_cast<u8>(0xC8 + Low3(reg)));
  }
  else if (bits == 16)
  {
    ROL(16, R(reg), Imm8(8));
  }
  else if (bits != 8)  // a single byte has nothing to swap
  {
    ASSERT_MSG(DYNA_REC, 0, "BSWAP: bad operand size {}", bits);
  }
}

void XEmitter::PREFETCH(PrefetchLevel level, OpArg arg)
{
  ASSERT_MSG(DYNA_REC, !arg.IsImm(), "PREFETCH needs a memory operand");
  // 0F 18 /hint
  EmitRex(arg, 0, false, false, false);
  Write8(0x0F);
  Write8(0x18);
  EmitOperand(arg, static_cast<u32>(level));
}

void XEmitter::SETcc(CCFlags flag, OpArg dest)
{
  ASSERT_MSG(DYNA_REC, !dest.IsImm(), "SETcc needs a register or memory operand");
  EmitRex(dest, 0, false, true, false);
  Write8(0x0F);
  Write8(static_cast<u8>(0x90 + flag));
  EmitOperand(dest, 0);
}

void XEmitter::CMOVcc(int bits, X64Reg dest, OpArg src, CCFlags flag)
{
  ASSERT_MSG(DYNA_REC, !src.IsImm(), "CMOVcc cannot take an immediate");
  ASSERT_MSG(DYNA_REC, bits != 8, "CMOVcc has no 8-bit form");
  EmitRegOp(bits, src, dest, bits == 8, bits == 8, 0x0F, 0x40 + flag);
}

// The F6/F7 group: TEST, NOT, NEG, MUL, IMUL, DIV, IDIV with one r/m operand (ext = /digit).
void XEmitter::EmitUnaryGroup(int bits, const OpArg& rm, int ext)
{
  ASSERT_MSG(DYNA_REC, !rm.IsImm(), "This instruction cannot take an immediate");
  CheckFlags();
  if (bits == 16)
    Write8(0x66);
  EmitRex(rm, 0, bits == 64, bits == 8, false);
  Write8(bits == 8 ? 0xF6 : 0xF7);
  EmitOperand(rm, static_cast<u32>(ext));
}

void XEmitter::NOT(int bits, const OpArg& src)
{
  EmitUnaryGroup(bits, src, 2);
}

void XEmitter::NEG(int bits, const OpArg& src)
{
  EmitUnaryGroup(bits, src, 3);
}

void XEmitter::MUL(int bits, const OpArg& src)
{
  EmitUnaryGroup(bits, src, 4);
}

void XEmitter::IMUL(int bits, const OpArg& src)
{
  EmitUnaryGroup(bits, src, 5);
}

void XEmitter::DIV(int bits, const OpArg& src)
{
  EmitUnaryGroup(bits, src, 6);
}

void XEmitter::IDIV(int bits, const OpArg& src)
{
  EmitUnaryGroup(bits, src, 7);
}

// [66] [F3] [REX] 0F op /r with dest in ModRM.reg: BSF, BSR, TZCNT, LZCNT (and MOVNTI).
void XEmitter::EmitBitScan(int bits, X64Reg dest, const OpArg& src, u8 op, bool f3_prefix)
{
  ASSERT_MSG(DYNA_REC, !src.IsImm(), "This instruction cannot take an immediate");
  CheckFlags();
  if (bits == 16)
    Write8(0x66);
  if (f3_prefix)
    Write8(0xF3);
  EmitRex(src, dest, bits == 64, bits == 8, bits == 8);
  Write8(0x0F);
  Write8(op);
  EmitOperand(src, dest);
}

void XEmitter::BSF(int bits, X64Reg dest, const OpArg& src)
{
  EmitBitScan(bits, dest, src, 0xBC, false);
}

void XEmitter::BSR(int bits, X64Reg dest, const OpArg& src)
{
  EmitBitScan(bits, dest, src, 0xBD, false);
}

void XEmitter::TZCNT(int bits, X64Reg dest, const OpArg& src)
{
  CheckFlags();
  if (!cpu_info.bBMI1)
    PanicAlertFmt("TZCNT was emitted, but this CPU does not support BMI1.");
  EmitBitScan(bits, dest, src, 0xBC, true);
}

void XEmitter::LZCNT(int bits, X64Reg dest, const OpArg& src)
{
  CheckFlags();
  if (!cpu_info.bLZCNT)
    PanicAlertFmt("LZCNT was emitted, but this CPU does not support it.");
  EmitBitScan(bits, dest, src, 0xBD, true);
}

void XEmitter::MOVNTI(int bits, const OpArg& dest, X64Reg src)
{
  ASSERT_MSG(DYNA_REC, bits > 16, "MOVNTI only has 32- and 64-bit forms");
  EmitBitScan(bits, src, dest, 0xC3, false);
}

void XEmitter::MOVSX(int dbits, int sbits, X64Reg dest, OpArg src)
{
  ASSERT_MSG(DYNA_REC, !src.IsImm(), "MOVSX cannot take an immediate");
  if (dbits == sbits)
  {
    MOV(dbits, R(dest), src);
    return;
  }

  if (dbits == 16)
    Write8(0x66);
  EmitRex(src, dest, dbits == 64, sbits == 8, dbits == 8);
  if (sbits == 8)
  {
    Write8(0x0F);
    Write8(0xBE);
  }
  else if (sbits == 16)
  {
    Write8(0x0F);
    Write8(0xBF);
  }
  else if (sbits == 32 && dbits == 64)
  {
    Write8(0x63);  // MOVSXD
  }
  else
  {
    Crash();
  }
  EmitOperand(src, dest);
}

void XEmitter::MOVZX(int dbits, int sbits, X64Reg dest, OpArg src)
{
  ASSERT_MSG(DYNA_REC, !src.IsImm(), "MOVZX cannot take an immediate");
  if (dbits == sbits)
  {
    MOV(dbits, R(dest), src);
    return;
  }

  if (dbits == 16)
    Write8(0x66);
  // A 32-bit destination is zero-extended to 64 bits anyway, so REX.W is never needed.
  EmitRex(src, dest, false, sbits == 8, dbits == 8);
  if (sbits == 8)
  {
    Write8(0x0F);
    Write8(0xB6);
  }
  else if (sbits == 16)
  {
    Write8(0x0F);
    Write8(0xB7);
  }
  else if (sbits == 32 && dbits == 64)
  {
    Write8(0x8B);  // plain 32-bit MOV
  }
  else
  {
    ASSERT_MSG(DYNA_REC, 0, "MOVZX from {} to {} bits is not possible", sbits, dbits);
  }
  EmitOperand(src, dest);
}

// MOVBE: 0F 38 F0 (load) / F1 (store)
void XEmitter::EmitMovbe(int bits, bool store, X64Reg reg, const OpArg& mem)
{
  ASSERT_MSG(DYNA_REC, cpu_info.bMOVBE, "MOVBE was emitted, but this CPU does not support it.");
  if (bits == 8)
  {
    // Nothing to swap in a single byte.
    if (store)
      MOV(8, mem, R(reg));
    else
      MOV(8, R(reg), mem);
    return;
  }

  if (bits == 16)
    Write8(0x66);
  ASSERT_MSG(DYNA_REC, !mem.IsSimpleReg() && !mem.IsImm(), "MOVBE needs a memory operand");
  EmitRex(mem, reg, bits == 64, false, false);
  Write8(0x0F);
  Write8(0x38);
  Write8(store ? 0xF1 : 0xF0);
  EmitOperand(mem, reg);
}

void XEmitter::MOVBE(int bits, X64Reg dest, const OpArg& src)
{
  EmitMovbe(bits, false, dest, src);
}

void XEmitter::MOVBE(int bits, const OpArg& dest, X64Reg src)
{
  EmitMovbe(bits, true, src, dest);
}

void XEmitter::LoadAndSwap(int size, X64Reg dst, const OpArg& src, bool sign_extend, MovInfo* info)
{
  if (info != nullptr)
  {
    info->address = code;
    info->nonAtomicSwapStore = false;
  }

  if (size == 8)
  {
    if (sign_extend)
      MOVSX(32, 8, dst, src);
    else
      MOVZX(32, 8, dst, src);
  }
  else if (size == 16)
  {
    MOVZX(32, 16, dst, src);
    if (sign_extend)
    {
      // Swapped halfword ends up in the top 16 bits; an arithmetic shift brings it down signed.
      BSWAP(32, dst);
      SAR(32, R(dst), Imm8(16));
    }
    else
    {
      ROL(16, R(dst), Imm8(8));
    }
  }
  else if (size == 32 || size == 64)
  {
    if (cpu_info.bMOVBE)
    {
      MOVBE(size, dst, src);
    }
    else
    {
      MOV(size, R(dst), src);
      BSWAP(size, dst);
    }
  }
}

void XEmitter::SwapAndStore(int size, const OpArg& dst, X64Reg src, MovInfo* info)
{
  if (cpu_info.bMOVBE)
  {
    if (info != nullptr)
    {
      info->address = code;
      info->nonAtomicSwapStore = false;
    }
    MOVBE(size, dst, src);
    return;
  }

  // The faulting instruction is the MOV; src has already been swapped by then.
  BSWAP(size, src);
  if (info != nullptr)
  {
    info->address = code;
    info->nonAtomicSwapStore = true;
    info->nonAtomicSwapStoreSrc = src;
  }
  MOV(size, dst, R(src));
}

void XEmitter::LEA(int bits, X64Reg dest, OpArg src)
{
  ASSERT_MSG(DYNA_REC, !src.IsImm(), "LEA needs a memory operand");
  if (bits == 16)
    Write8(0x66);
  EmitRex(src, dest, bits == 64, bits == 8, bits == 8);
  Write8(0x8D);
  // A 32-bit LEA of a RIP operand may compute an address that does not fit; that is allowed
  // since only the low half is kept.
  EmitOperand(src, dest, 0, bits == 64);
}

// ---------------------------------------------------------------------------------------------
// Shifts and bit tests

// C0/C1 /ext ib, D0/D1 /ext (by one) or D2/D3 /ext (by CL)
void XEmitter::EmitShift(int bits, const OpArg& dest, const OpArg& shift, int ext)
{
  CheckFlags();
  ASSERT_MSG(DYNA_REC, !dest.IsImm(), "Cannot shift an immediate");
  ASSERT_MSG(DYNA_REC,
             !(shift.IsSimpleReg() && shift.GetSimpleReg() != ECX) &&
                 !(shift.IsImm() && shift.GetImmBits() != 8),
             "The shift amount must be an Imm8 or CL");

  const bool byte = bits == 8;
  const bool by_imm = shift.GetImmBits() == 8;
  const u8 amount = static_cast<u8>(shift.m_value);

  if (bits == 16)
    Write8(0x66);
  EmitRex(dest, 0, bits == 64, byte, false);
  if (!by_imm)
  {
    Write8(byte ? 0xD2 : 0xD3);
    EmitOperand(dest, static_cast<u32>(ext));
  }
  else if (amount == 1)
  {
    Write8(byte ? 0xD0 : 0xD1);
    EmitOperand(dest, static_cast<u32>(ext));
  }
  else
  {
    Write8(byte ? 0xC0 : 0xC1);
    EmitOperand(dest, static_cast<u32>(ext), 1);
    Write8(amount);
  }
}

void XEmitter::ROL(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 0);
}

void XEmitter::ROR(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 1);
}

void XEmitter::RCL(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 2);
}

void XEmitter::RCR(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 3);
}

void XEmitter::SHL(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 4);
}

void XEmitter::SHR(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 5);
}

void XEmitter::SAR(int bits, const OpArg& dest, const OpArg& shift)
{
  EmitShift(bits, dest, shift, 7);
}

// 0F BA /ext ib (immediate bit number) or 0F A3/AB/B3/BB /r (register bit number)
void XEmitter::EmitBitTest(int bits, const OpArg& dest, const OpArg& index, int ext)
{
  CheckFlags();
  ASSERT_MSG(DYNA_REC, !dest.IsImm(), "Cannot bit-test an immediate");
  ASSERT_MSG(DYNA_REC, !index.IsImm() || index.GetImmBits() == 8,
             "The bit index must be an Imm8 or a register");

  if (bits == 16)
    Write8(0x66);
  if (index.IsImm())
  {
    EmitRex(dest, 0, bits == 64, bits == 8, false);
    Write8(0x0F);
    Write8(0xBA);
    EmitOperand(dest, static_cast<u32>(ext), 1);
    Write8(static_cast<u8>(index.m_value));
  }
  else
  {
    const X64Reg bit_reg = index.GetSimpleReg();
    EmitRex(dest, bit_reg, bits == 64, bits == 8, bits == 8);
    Write8(0x0F);
    Write8(static_cast<u8>(0x83 + 8 * ext));
    EmitOperand(dest, bit_reg);
  }
}

void XEmitter::BT(int bits, const OpArg& dest, const OpArg& index)
{
  EmitBitTest(bits, dest, index, 4);
}

void XEmitter::BTS(int bits, const OpArg& dest, const OpArg& index)
{
  EmitBitTest(bits, dest, index, 5);
}

void XEmitter::BTR(int bits, const OpArg& dest, const OpArg& index)
{
  EmitBitTest(bits, dest, index, 6);
}

void XEmitter::BTC(int bits, const OpArg& dest, const OpArg& index)
{
  EmitBitTest(bits, dest, index, 7);
}

// SHLD 0F A4 (ib) / A5 (CL), SHRD 0F AC / AD
void XEmitter::EmitDoubleShift(int bits, const OpArg& dest, const OpArg& src, const OpArg& shift,
                               u8 op_imm)
{
  CheckFlags();
  ASSERT_MSG(DYNA_REC, !dest.IsImm(), "SHLD/SHRD cannot write to an immediate");
  ASSERT_MSG(DYNA_REC, src.IsSimpleReg(), "SHLD/SHRD need a register source");
  ASSERT_MSG(DYNA_REC,
             !(shift.IsSimpleReg() && shift.GetSimpleReg() != ECX) &&
                 !(shift.IsImm() && shift.GetImmBits() != 8),
             "The shift amount must be an Imm8 or CL");

  const X64Reg reg = src.GetSimpleReg();
  if (bits == 16)
    Write8(0x66);
  EmitRex(dest, reg, bits == 64, bits == 8, bits == 8);
  Write8(0x0F);
  if (shift.GetImmBits() == 8)
  {
    Write8(op_imm);
    EmitOperand(dest, reg, 1);
    Write8(static_cast<u8>(shift.m_value));
  }
  else
  {
    Write8(static_cast<u8>(op_imm + 1));
    EmitOperand(dest, reg);
  }
}

void XEmitter::SHRD(int bits, const OpArg& dest, const OpArg& src, const OpArg& shift)
{
  EmitDoubleShift(bits, dest, src, shift, 0xAC);
}

void XEmitter::SHLD(int bits, const OpArg& dest, const OpArg& src, const OpArg& shift)
{
  EmitDoubleShift(bits, dest, src, shift, 0xA4);
}

// ---------------------------------------------------------------------------------------------
// Two-operand integer instructions (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP, MOV, TEST, XCHG)

void XEmitter::EmitArithImm(int bits, int op, const OpArg& rm, const OpArg& imm)
{
  const bool to_reg = rm.IsSimpleReg();
  const u32 reg_num = rm.m_base;
  const u64 value = imm.m_value;
  const int imm_bits = imm.GetImmBits();

  if (bits == 16)
    Write8(0x66);
  EmitRex(rm, 0, bits == 64, bits == 8, false);

  // How many immediate bytes follow the ModRM-encoded operand.
  int imm_size;
  u8 opcode;

  if (imm_bits == 8 && bits == 8)
  {
    if (to_reg && reg_num == AL && AccumulatorOpcode(op, true) != kNoEncoding)
    {
      Write8(AccumulatorOpcode(op, true));
      Write8(static_cast<u8>(value));
      return;
    }
    if (to_reg && op == kMov)
    {
      Write8(static_cast<u8>(0xB0 + Low3(reg_num)));  // mov r8, imm8
      Write8(static_cast<u8>(value));
      return;
    }
    opcode = ImmGroupOpcode(op, true);
    imm_size = 1;
  }
  else if ((imm_bits == 16 && bits == 16) || (imm_bits == 32 && (bits == 32 || bits == 64)))
  {
    const bool small = imm_bits == 16 ? FitsS8(static_cast<s16>(value)) :
                                        FitsS8(static_cast<s32>(value));
    if (HasSignedImm8(op) && small)
    {
      opcode = SignedImm8Opcode(op);
      imm_size = 1;
    }
    else if (to_reg && op == kMov && bits != 64)
    {
      // mov r16/r32, imm (B8+r)
      Write8(static_cast<u8>(0xB8 + Low3(reg_num)));
      if (bits == 16)
        Write16(static_cast<u16>(value));
      else
        Write32(static_cast<u32>(value));
      return;
    }
    else if (to_reg && reg_num == EAX && AccumulatorOpcode(op, false) != kNoEncoding)
    {
      Write8(AccumulatorOpcode(op, false));
      if (bits == 16)
        Write16(static_cast<u16>(value));
      else
        Write32(static_cast<u32>(value));
      return;
    }
    else
    {
      opcode = ImmGroupOpcode(op, false);
      imm_size = bits == 16 ? 2 : 4;
    }
  }
  else if (imm_bits == 8)
  {
    // An Imm8 with a wider operation is sign-extended.
    opcode = SignedImm8Opcode(op);
    imm_size = 1;
  }
  else if (imm_bits == 64 && bits == 64)
  {
    if (!to_reg)
    {
      ASSERT_MSG(DYNA_REC, 0, "A 64-bit immediate can only be moved into a register");
      EmitOperand(rm, static_cast<u32>(ImmDigit(op)));
      return;
    }
    if (op != kMov)
    {
      ASSERT_MSG(DYNA_REC, 0, "Only MOV can take a 64-bit immediate");
      EmitOperand(rm, static_cast<u32>(ImmDigit(op)));
      return;
    }
    if (!FitsS32(static_cast<s64>(value)))
    {
      // movabs r64, imm64
      Write8(static_cast<u8>(0xB8 + Low3(reg_num)));
      Write64(value);
      return;
    }
    // mov r/m64, sign-extended imm32
    opcode = ImmGroupOpcode(kMov, false);
    imm_size = 4;
  }
  else
  {
    ASSERT_MSG(DYNA_REC, 0, "No encoding for a {}-bit immediate with a {}-bit operation",
               imm_bits, bits);
    EmitOperand(rm, static_cast<u32>(ImmDigit(op)));
    return;
  }

  Write8(opcode);
  EmitOperand(rm, static_cast<u32>(ImmDigit(op)), imm_size);
  switch (imm_size)
  {
  case 1:
    Write8(static_cast<u8>(value));
    break;
  case 2:
    Write16(static_cast<u16>(value));
    break;
  case 4:
    Write32(static_cast<u32>(value));
    break;
  }
}

void XEmitter::EmitArith(int bits, int op, const OpArg& a1, const OpArg& a2)
{
  if (a1.IsImm())
  {
    ASSERT_MSG(DYNA_REC, 0, "The destination of this instruction cannot be an immediate");
    return;
  }

  if (a2.IsImm())
  {
    EmitArithImm(bits, op, a1, a2);
    return;
  }

  // One side has to be a register; it goes into ModRM.reg. With two registers the "reg, r/m"
  // direction is used, with the destination in ModRM.reg.
  const bool to_rm = !a1.IsSimpleReg();
  const OpArg& rm = to_rm ? a1 : a2;
  const OpArg& reg_arg = to_rm ? a2 : a1;
  ASSERT_MSG(DYNA_REC, !to_rm || a2.IsSimpleReg(), "Both operands cannot be in memory");
  const u32 reg = reg_arg.m_base;

  EmitRegOp(bits, rm, reg, bits == 8, bits == 8, RegFormOpcode(op, bits == 8, to_rm));
}

void XEmitter::ADD(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kAdd, a1, a2);
}

void XEmitter::ADC(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kAdc, a1, a2);
}

void XEmitter::SUB(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kSub, a1, a2);
}

void XEmitter::SBB(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kSbb, a1, a2);
}

void XEmitter::AND(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kAnd, a1, a2);
}

void XEmitter::OR(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kOr, a1, a2);
}

void XEmitter::XOR(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kXor, a1, a2);
}

void XEmitter::CMP(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kCmp, a1, a2);
}

void XEmitter::TEST(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  EmitArith(bits, kTest, a1, a2);
}

void XEmitter::XCHG(int bits, const OpArg& a1, const OpArg& a2)
{
  EmitArith(bits, kXchg, a1, a2);
}

void XEmitter::MOV(int bits, const OpArg& a1, const OpArg& a2)
{
  // A 32-bit MOV zero-extends into the whole register, so a 64-bit constant whose upper half is
  // zero can use the 5-byte B8+r form.
  if (bits == 64 && a1.IsSimpleReg())
  {
    const bool zero_extendable =
        (a2.m_kind == SCALE_IMM64 && (a2.m_value >> 32) == 0) ||
        (a2.m_kind == SCALE_IMM32 && static_cast<s32>(a2.m_value) >= 0);
    if (zero_extendable)
    {
      EmitArith(32, kMov, a1, a2.AsImm32());
      return;
    }
  }

  if (a1.IsSimpleReg() && a2.IsSimpleReg() && a1.GetSimpleReg() == a2.GetSimpleReg())
    ERROR_LOG_FMT(DYNA_REC, "MOV of a register to itself at {} (JIT bug?)", fmt::ptr(code));
  EmitArith(bits, kMov, a1, a2);
}

void XEmitter::CMP_or_TEST(int bits, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  if (a1.IsSimpleReg() && a2.IsZero())
    EmitArith(bits, kTest, a1, a1);
  else
    EmitArith(bits, kCmp, a1, a2);
}

void XEmitter::MOV_sum(int bits, X64Reg dest, const OpArg& a1, const OpArg& a2)
{
  DEBUG_ASSERT(!flags_locked);

  // x + 0: at most a move (none at all when x is already dest)
  if (a1.IsZero() || a2.IsZero())
  {
    const OpArg& other = a1.IsZero() ? a2 : a1;
    if (!other.IsSimpleReg(dest))
      MOV(bits, R(dest), other);
    return;
  }

  // dest += x
  if (a1.IsSimpleReg(dest))
  {
    ADD(bits, R(dest), a2);
    return;
  }
  if (a2.IsSimpleReg(dest))
  {
    ADD(bits, R(dest), a1);
    return;
  }

  if (bits == 32)
  {
    if (a1.IsImm() && a2.IsImm())
    {
      MOV(32, R(dest), Imm32(a1.Imm32() + a2.Imm32()));
      return;
    }
    if (a1.IsSimpleReg() && a2.IsSimpleReg())
    {
      LEA(32, dest, MRegSum(a1.GetSimpleReg(), a2.GetSimpleReg()));
      return;
    }
    if (a1.IsSimpleReg() && a2.IsImm())
    {
      LEA(32, dest, MDisp(a1.GetSimpleReg(), a2.Imm32()));
      return;
    }
    if (a1.IsImm() && a2.IsSimpleReg())
    {
      LEA(32, dest, MDisp(a2.GetSimpleReg(), a1.Imm32()));
      return;
    }
  }

  MOV(bits, R(dest), a1);
  ADD(bits, R(dest), a2);
}

// IMUL reg, r/m, imm: 6B /r ib (sign-extended imm8) or 69 /r iw/id
void XEmitter::IMUL(int bits, X64Reg regOp, const OpArg& a1, const OpArg& a2)
{
  CheckFlags();
  if (bits == 8)
  {
    ASSERT_MSG(DYNA_REC, 0, "IMUL has no 8-bit three-operand form");
    return;
  }
  if (a1.IsImm())
  {
    ASSERT_MSG(DYNA_REC, 0, "IMUL: the second operand cannot be an immediate");
    return;
  }
  if (!a2.IsImm())
  {
    ASSERT_MSG(DYNA_REC, 0, "IMUL: the third operand must be an immediate");
    return;
  }

  if (bits == 16)
    Write8(0x66);
  EmitRex(a1, regOp, bits == 64, false, false);

  const int imm_bits = a2.GetImmBits();
  const u64 value = a2.m_value;
  const bool short_form = imm_bits == 8 ||
                          (imm_bits == 16 && FitsS8(static_cast<s16>(value))) ||
                          (imm_bits == 32 && FitsS8(static_cast<s32>(value)));
  if (short_form)
  {
    Write8(0x6B);
    EmitOperand(a1, regOp, 1);
    Write8(static_cast<u8>(value));
    return;
  }

  Write8(0x69);
  if (imm_bits == 16 && bits == 16)
  {
    EmitOperand(a1, regOp, 2);
    Write16(static_cast<u16>(value));
  }
  else if (imm_bits == 32 && (bits == 32 || bits == 64))
  {
    EmitOperand(a1, regOp, 4);
    Write32(static_cast<u32>(value));
  }
  else
  {
    ASSERT_MSG(DYNA_REC, 0, "IMUL: a {}-bit immediate does not fit a {}-bit operation", imm_bits,
               bits);
  }
}

// IMUL reg, r/m: 0F AF /r
void XEmitter::IMUL(int bits, X64Reg regOp, const OpArg& a)
{
  CheckFlags();
  if (bits == 8)
  {
    ASSERT_MSG(DYNA_REC, 0, "IMUL has no 8-bit two-operand form");
    return;
  }
  if (a.IsImm())
  {
    IMUL(bits, regOp, R(regOp), a);
    return;
  }
  EmitRegOp(bits, a, regOp, false, false, 0x0F, 0xAF);
}

// ---------------------------------------------------------------------------------------------
// SSE (legacy encoding): [prefix] [REX] 0F [38/3A] op /r

void XEmitter::EmitSSE(u8 prefix, u16 op, X64Reg reg, const OpArg& rm, int trailing_bytes)
{
  if (prefix != 0)
    Write8(prefix);
  EmitRex(rm, reg, false, false, false);
  Write8(0x0F);
  if (op > 0xFF)
    Write8(static_cast<u8>(op >> 8));
  Write8(static_cast<u8>(op));
  EmitOperand(rm, reg, trailing_bytes);
}

void XEmitter::EmitSSE_SSSE3(u8 prefix, u16 op, X64Reg reg, const OpArg& rm, int trailing_bytes)
{
  if (!cpu_info.bSSSE3)
    PanicAlertFmt("An SSSE3 instruction was emitted, but this CPU does not support SSSE3.");
  EmitSSE(prefix, op, reg, rm, trailing_bytes);
}

void XEmitter::EmitSSE_SSE41(u8 prefix, u16 op, X64Reg reg, const OpArg& rm, int trailing_bytes)
{
  if (!cpu_info.bSSE4_1)
    PanicAlertFmt("An SSE4.1 instruction was emitted, but this CPU does not support SSE4.1.");
  EmitSSE(prefix, op, reg, rm, trailing_bytes);
}

// Plain two-operand SSE instructions: name(X64Reg, const OpArg&) -> prefix 0F op
#define PIXELU_SSE_OPS(X)                                                                          \
  X(ADDSS, 0xF3, 0x58)                                                                             \
  X(ADDSD, 0xF2, 0x58)                                                                             \
  X(SUBSS, 0xF3, 0x5C)                                                                             \
  X(SUBSD, 0xF2, 0x5C)                                                                             \
  X(MULSS, 0xF3, 0x59)                                                                             \
  X(MULSD, 0xF2, 0x59)                                                                             \
  X(DIVSS, 0xF3, 0x5E)                                                                             \
  X(DIVSD, 0xF2, 0x5E)                                                                             \
  X(MINSS, 0xF3, 0x5D)                                                                             \
  X(MINSD, 0xF2, 0x5D)                                                                             \
  X(MAXSS, 0xF3, 0x5F)                                                                             \
  X(MAXSD, 0xF2, 0x5F)                                                                             \
  X(SQRTSS, 0xF3, 0x51)                                                                            \
  X(SQRTSD, 0xF2, 0x51)                                                                            \
  X(RCPSS, 0xF3, 0x53)                                                                             \
  X(RSQRTSS, 0xF3, 0x52)                                                                           \
  X(ADDPS, 0x00, 0x58)                                                                             \
  X(ADDPD, 0x66, 0x58)                                                                             \
  X(SUBPS, 0x00, 0x5C)                                                                             \
  X(SUBPD, 0x66, 0x5C)                                                                             \
  X(MULPS, 0x00, 0x59)                                                                             \
  X(MULPD, 0x66, 0x59)                                                                             \
  X(DIVPS, 0x00, 0x5E)                                                                             \
  X(DIVPD, 0x66, 0x5E)                                                                             \
  X(MINPS, 0x00, 0x5D)                                                                             \
  X(MINPD, 0x66, 0x5D)                                                                             \
  X(MAXPS, 0x00, 0x5F)                                                                             \
  X(MAXPD, 0x66, 0x5F)                                                                             \
  X(SQRTPS, 0x00, 0x51)                                                                            \
  X(SQRTPD, 0x66, 0x51)                                                                            \
  X(RCPPS, 0x00, 0x53)                                                                             \
  X(RSQRTPS, 0x00, 0x52)                                                                           \
  X(ANDPS, 0x00, 0x54)                                                                             \
  X(ANDPD, 0x66, 0x54)                                                                             \
  X(ANDNPS, 0x00, 0x55)                                                                            \
  X(ANDNPD, 0x66, 0x55)                                                                            \
  X(ORPS, 0x00, 0x56)                                                                              \
  X(ORPD, 0x66, 0x56)                                                                              \
  X(XORPS, 0x00, 0x57)                                                                             \
  X(XORPD, 0x66, 0x57)                                                                             \
  X(COMISS, 0x00, 0x2F)                                                                            \
  X(COMISD, 0x66, 0x2F)                                                                            \
  X(UCOMISS, 0x00, 0x2E)                                                                           \
  X(UCOMISD, 0x66, 0x2E)                                                                           \
  X(UNPCKLPS, 0x00, 0x14)                                                                          \
  X(UNPCKHPS, 0x00, 0x15)                                                                          \
  X(UNPCKLPD, 0x66, 0x14)                                                                          \
  X(UNPCKHPD, 0x66, 0x15)                                                                          \
  X(MOVUPS, 0x00, 0x10)                                                                            \
  X(MOVUPD, 0x66, 0x10)                                                                            \
  X(MOVDQA, 0x66, 0x6F)                                                                            \
  X(MOVDQU, 0xF3, 0x6F)                                                                            \
  X(MOVSS, 0xF3, 0x10)                                                                             \
  X(MOVSD, 0xF2, 0x10)                                                                             \
  X(MOVLPS, 0x00, 0x12)                                                                            \
  X(MOVLPD, 0x66, 0x12)                                                                            \
  X(MOVHPS, 0x00, 0x16)                                                                            \
  X(MOVHPD, 0x66, 0x16)                                                                            \
  X(MOVD_xmm, 0x66, 0x6E)                                                                          \
  X(MOVMSKPS, 0x00, 0x50)                                                                          \
  X(MOVMSKPD, 0x66, 0x50)                                                                          \
  X(LDDQU, 0xF2, 0xF0)                                                                             \
  X(CVTPS2PD, 0x00, 0x5A)                                                                          \
  X(CVTPD2PS, 0x66, 0x5A)                                                                          \
  X(CVTSD2SS, 0xF2, 0x5A)                                                                          \
  X(CVTSS2SD, 0xF3, 0x5A)                                                                          \
  X(CVTSD2SI, 0xF2, 0x2D)                                                                          \
  X(CVTSS2SI, 0xF3, 0x2D)                                                                          \
  X(CVTSI2SD, 0xF2, 0x2A)                                                                          \
  X(CVTSI2SS, 0xF3, 0x2A)                                                                          \
  X(CVTDQ2PD, 0xF3, 0xE6)                                                                          \
  X(CVTDQ2PS, 0x00, 0x5B)                                                                          \
  X(CVTPD2DQ, 0xF2, 0xE6)                                                                          \
  X(CVTPS2DQ, 0x66, 0x5B)                                                                          \
  X(CVTTSD2SI, 0xF2, 0x2C)                                                                         \
  X(CVTTSS2SI, 0xF3, 0x2C)                                                                         \
  X(CVTTPS2DQ, 0xF3, 0x5B)                                                                         \
  X(CVTTPD2DQ, 0x66, 0xE6)                                                                         \
  X(PACKSSDW, 0x66, 0x6B)                                                                          \
  X(PACKSSWB, 0x66, 0x63)                                                                          \
  X(PACKUSWB, 0x66, 0x67)                                                                          \
  X(PUNPCKLBW, 0x66, 0x60)                                                                         \
  X(PUNPCKLWD, 0x66, 0x61)                                                                         \
  X(PUNPCKLDQ, 0x66, 0x62)                                                                         \
  X(PUNPCKLQDQ, 0x66, 0x6C)                                                                        \
  X(PSRLQ, 0x66, 0xD3)                                                                             \
  X(PAND, 0x66, 0xDB)                                                                              \
  X(PANDN, 0x66, 0xDF)                                                                             \
  X(PXOR, 0x66, 0xEF)                                                                              \
  X(POR, 0x66, 0xEB)                                                                               \
  X(PADDB, 0x66, 0xFC)                                                                             \
  X(PADDW, 0x66, 0xFD)                                                                             \
  X(PADDD, 0x66, 0xFE)                                                                             \
  X(PADDQ, 0x66, 0xD4)                                                                             \
  X(PADDSB, 0x66, 0xEC)                                                                            \
  X(PADDSW, 0x66, 0xED)                                                                            \
  X(PADDUSB, 0x66, 0xDC)                                                                           \
  X(PADDUSW, 0x66, 0xDD)                                                                           \
  X(PSUBB, 0x66, 0xF8)                                                                             \
  X(PSUBW, 0x66, 0xF9)                                                                             \
  X(PSUBD, 0x66, 0xFA)                                                                             \
  X(PSUBQ, 0x66, 0xFB)                                                                             \
  X(PSUBSB, 0x66, 0xE8)                                                                            \
  X(PSUBSW, 0x66, 0xE9)                                                                            \
  X(PSUBUSB, 0x66, 0xD8)                                                                           \
  X(PSUBUSW, 0x66, 0xD9)                                                                           \
  X(PAVGB, 0x66, 0xE0)                                                                             \
  X(PAVGW, 0x66, 0xE3)                                                                             \
  X(PCMPEQB, 0x66, 0x74)                                                                           \
  X(PCMPEQW, 0x66, 0x75)                                                                           \
  X(PCMPEQD, 0x66, 0x76)                                                                           \
  X(PCMPGTB, 0x66, 0x64)                                                                           \
  X(PCMPGTW, 0x66, 0x65)                                                                           \
  X(PCMPGTD, 0x66, 0x66)                                                                           \
  X(PMADDWD, 0x66, 0xF5)                                                                           \
  X(PSADBW, 0x66, 0xF6)                                                                            \
  X(PMAXSW, 0x66, 0xEE)                                                                            \
  X(PMAXUB, 0x66, 0xDE)                                                                            \
  X(PMINSW, 0x66, 0xEA)                                                                            \
  X(PMINUB, 0x66, 0xDA)                                                                            \
  X(PMOVMSKB, 0x66, 0xD7)

#define PIXELU_DEFINE_SSE(name, prefix, op)                                                        \
  void XEmitter::name(X64Reg reg, const OpArg& arg) { EmitSSE(prefix, op, reg, arg); }
PIXELU_SSE_OPS(PIXELU_DEFINE_SSE)
#undef PIXELU_DEFINE_SSE
#undef PIXELU_SSE_OPS

// Stores: name(const OpArg& dest, X64Reg src)
#define PIXELU_SSE_STORES(X)                                                                       \
  X(MOVNTDQ, 0x66, 0xE7)                                                                           \
  X(MOVNTPS, 0x00, 0x2B)                                                                           \
  X(MOVNTPD, 0x66, 0x2B)                                                                           \
  X(MOVAPS, 0x00, 0x29)                                                                            \
  X(MOVUPS, 0x00, 0x11)                                                                            \
  X(MOVUPD, 0x66, 0x11)                                                                            \
  X(MOVDQA, 0x66, 0x7F)                                                                            \
  X(MOVDQU, 0xF3, 0x7F)                                                                            \
  X(MOVSS, 0xF3, 0x11)                                                                             \
  X(MOVSD, 0xF2, 0x11)                                                                             \
  X(MOVLPS, 0x00, 0x13)                                                                            \
  X(MOVLPD, 0x66, 0x13)                                                                            \
  X(MOVHPS, 0x00, 0x17)                                                                            \
  X(MOVHPD, 0x66, 0x17)                                                                            \
  X(MOVD_xmm, 0x66, 0x7E)

#define PIXELU_DEFINE_SSE_STORE(name, prefix, op)                                                  \
  void XEmitter::name(const OpArg& arg, X64Reg reg) { EmitSSE(prefix, op, reg, arg); }
PIXELU_SSE_STORES(PIXELU_DEFINE_SSE_STORE)
#undef PIXELU_DEFINE_SSE_STORE
#undef PIXELU_SSE_STORES

// With an immediate byte: name(X64Reg, const OpArg&, u8)
#define PIXELU_SSE_IMM_OPS(X)                                                                      \
  X(CMPSS, 0xF3, 0xC2)                                                                             \
  X(CMPSD, 0xF2, 0xC2)                                                                             \
  X(CMPPS, 0x00, 0xC2)                                                                             \
  X(CMPPD, 0x66, 0xC2)                                                                             \
  X(SHUFPS, 0x00, 0xC6)                                                                            \
  X(SHUFPD, 0x66, 0xC6)                                                                            \
  X(PSHUFD, 0x66, 0x70)                                                                            \
  X(PSHUFLW, 0xF2, 0x70)                                                                           \
  X(PSHUFHW, 0xF3, 0x70)                                                                           \
  X(PEXTRW, 0x66, 0xC5)                                                                            \
  X(PINSRW, 0x66, 0xC4)

#define PIXELU_DEFINE_SSE_IMM(name, prefix, op)                                                    \
  void XEmitter::name(X64Reg reg, const OpArg& arg, u8 imm)                                        \
  {                                                                                                \
    EmitSSE(prefix, op, reg, arg, 1);                                                              \
    Write8(imm);                                                                                   \
  }
PIXELU_SSE_IMM_OPS(PIXELU_DEFINE_SSE_IMM)
#undef PIXELU_DEFINE_SSE_IMM
#undef PIXELU_SSE_IMM_OPS

// MOVAPD does exactly what MOVAPS does and is a byte longer, so MOVAPS is emitted for it.
void XEmitter::MOVAPS(X64Reg regOp, const OpArg& arg)
{
  EmitSSE(0x00, 0x28, regOp, arg);
}

void XEmitter::MOVAPD(X64Reg regOp, const OpArg& arg)
{
  MOVAPS(regOp, arg);
}

void XEmitter::MOVAPD(const OpArg& arg, X64Reg regOp)
{
  MOVAPS(arg, regOp);
}

void XEmitter::MOVHLPS(X64Reg regOp1, X64Reg regOp2)
{
  EmitSSE(0x00, 0x12, regOp1, R(regOp2));
}

void XEmitter::MOVLHPS(X64Reg regOp1, X64Reg regOp2)
{
  EmitSSE(0x00, 0x16, regOp1, R(regOp2));
}

void XEmitter::MASKMOVDQU(X64Reg dest, X64Reg src)
{
  EmitSSE(0x66, 0xF7, dest, R(src));
}

// movq xmm, r/m64 (66 REX.W 0F 6E)
void XEmitter::MOVQ_xmm(X64Reg dest, OpArg arg)
{
  Write8(0x66);
  EmitRex(arg, dest, true, false, false);
  Write8(0x0F);
  Write8(0x6E);
  EmitOperand(arg, dest);
}

void XEmitter::MOVQ_xmm(OpArg arg, X64Reg src)
{
  if (src > XMM7 || arg.IsSimpleReg())
  {
    // movq r/m64, xmm (66 REX.W 0F 7E)
    Write8(0x66);
    EmitRex(arg, src, true, false, false);
    Write8(0x0F);
    Write8(0x7E);
  }
  else
  {
    // movq m64, xmm (66 0F D6)
    Write8(0x66);
    EmitRex(arg, src, false, false, false);
    Write8(0x0F);
    Write8(0xD6);
  }
  EmitOperand(arg, src);
}

// 0F AE /2 (LDMXCSR), /3 (STMXCSR)
void XEmitter::EmitMxcsr(const OpArg& mem, int ext)
{
  ASSERT_MSG(DYNA_REC, !mem.IsImm() && !mem.IsSimpleReg(), "MXCSR access needs a memory operand");
  EmitRex(mem, 0, false, false, false);
  Write8(0x0F);
  Write8(0xAE);
  EmitOperand(mem, static_cast<u32>(ext));
}

void XEmitter::STMXCSR(const OpArg& memloc)
{
  EmitMxcsr(memloc, 3);
}

void XEmitter::LDMXCSR(const OpArg& memloc)
{
  EmitMxcsr(memloc, 2);
}

// The SSE3 duplicating moves, done with an unpack of the register with itself on CPUs without
// SSE3.
void XEmitter::MOVSLDUP(X64Reg regOp, const OpArg& arg)
{
  if (cpu_info.bSSE3)
  {
    EmitSSE(0xF3, 0x12, regOp, arg);
    return;
  }
  if (!arg.IsSimpleReg(regOp))
    MOVAPD(regOp, arg);
  UNPCKLPS(regOp, R(regOp));
}

void XEmitter::MOVSHDUP(X64Reg regOp, const OpArg& arg)
{
  if (cpu_info.bSSE3)
  {
    EmitSSE(0xF3, 0x16, regOp, arg);
    return;
  }
  if (!arg.IsSimpleReg(regOp))
    MOVAPD(regOp, arg);
  UNPCKHPS(regOp, R(regOp));
}

void XEmitter::MOVDDUP(X64Reg regOp, const OpArg& arg)
{
  if (cpu_info.bSSE3)
  {
    EmitSSE(0xF2, 0x12, regOp, arg);
    return;
  }
  // Only the low double is needed, so a memory source is loaded with MOVSD.
  if (!arg.IsSimpleReg())
    MOVSD(regOp, arg);
  else if (arg.GetSimpleReg() != regOp)
    MOVAPD(regOp, arg);
  UNPCKLPD(regOp, R(regOp));
}

// Shifts by an immediate: 66 0F 71/72/73 /ext ib with the register in ModRM.rm.
#define PIXELU_SSE_SHIFT_OPS(X)                                                                    \
  X(PSRLW, 0x71, 2)                                                                                \
  X(PSRLD, 0x72, 2)                                                                                \
  X(PSRLQ, 0x73, 2)                                                                                \
  X(PSRLDQ, 0x73, 3)                                                                               \
  X(PSLLW, 0x71, 6)                                                                                \
  X(PSLLD, 0x72, 6)                                                                                \
  X(PSLLQ, 0x73, 6)                                                                                \
  X(PSLLDQ, 0x73, 7)

#define PIXELU_DEFINE_SSE_SHIFT(name, op, ext)                                                     \
  void XEmitter::name(X64Reg reg, u8 shift)                                                        \
  {                                                                                                \
    EmitSSE(0x66, op, static_cast<X64Reg>(ext), R(reg));                                           \
    Write8(shift);                                                                                 \
  }
PIXELU_SSE_SHIFT_OPS(PIXELU_DEFINE_SSE_SHIFT)
#undef PIXELU_DEFINE_SSE_SHIFT
#undef PIXELU_SSE_SHIFT_OPS

// These two are written without REX support, so only XMM0-XMM7 work.
void XEmitter::PSRAW(X64Reg reg, u8 shift)
{
  if (reg > XMM7)
    PanicAlertFmt("PSRAW can only shift XMM0-XMM7");
  Write8(0x66);
  Write8(0x0F);
  Write8(0x71);
  Write8(static_cast<u8>(0xE0 | reg));  // /4
  Write8(shift);
}

void XEmitter::PSRAD(X64Reg reg, u8 shift)
{
  if (reg > XMM7)
    PanicAlertFmt("PSRAD can only shift XMM0-XMM7");
  Write8(0x66);
  Write8(0x0F);
  Write8(0x72);
  Write8(static_cast<u8>(0xE0 | reg));  // /4
  Write8(shift);
}

void XEmitter::PSHUFB(X64Reg dest, const OpArg& arg)
{
  EmitSSE_SSSE3(0x66, 0x3800, dest, arg);
}

// SSE4.1, 66 0F 38 xx
#define PIXELU_SSE41_OPS(X)                                                                        \
  X(PTEST, 0x3817)                                                                                 \
  X(PACKUSDW, 0x382B)                                                                              \
  X(PMOVSXBW, 0x3820)                                                                              \
  X(PMOVSXBD, 0x3821)                                                                              \
  X(PMOVSXBQ, 0x3822)                                                                              \
  X(PMOVSXWD, 0x3823)                                                                              \
  X(PMOVSXWQ, 0x3824)                                                                              \
  X(PMOVSXDQ, 0x3825)                                                                              \
  X(PMOVZXBW, 0x3830)                                                                              \
  X(PMOVZXBD, 0x3831)                                                                              \
  X(PMOVZXBQ, 0x3832)                                                                              \
  X(PMOVZXWD, 0x3833)                                                                              \
  X(PMOVZXWQ, 0x3834)                                                                              \
  X(PMOVZXDQ, 0x3835)                                                                              \
  X(PBLENDVB, 0x3810)                                                                              \
  X(BLENDVPS, 0x3814)                                                                              \
  X(BLENDVPD, 0x3815)                                                                              \
  X(PCMPEQQ, 0x3829)

#define PIXELU_DEFINE_SSE41(name, op)                                                              \
  void XEmitter::name(X64Reg dest, const OpArg& arg) { EmitSSE_SSE41(0x66, op, dest, arg); }
PIXELU_SSE41_OPS(PIXELU_DEFINE_SSE41)
#undef PIXELU_DEFINE_SSE41
#undef PIXELU_SSE41_OPS

void XEmitter::BLENDPS(X64Reg dest, const OpArg& arg, u8 blend)
{
  EmitSSE_SSE41(0x66, 0x3A0C, dest, arg, 1);
  Write8(blend);
}

void XEmitter::BLENDPD(X64Reg dest, const OpArg& arg, u8 blend)
{
  EmitSSE_SSE41(0x66, 0x3A0D, dest, arg, 1);
  Write8(blend);
}

void XEmitter::PINSRD(X64Reg dest, const OpArg& arg, u8 subreg)
{
  EmitSSE_SSE41(0x66, 0x3A22, dest, arg, 1);
  Write8(subreg);
}

// ---------------------------------------------------------------------------------------------
// VEX encoding
//
// Two-byte form:   C5 [R vvvv L pp]                      (map 0F, no W/X/B)
// Three-byte form: C4 [R X B mmmmm] [W vvvv L pp]
// R, X, B and vvvv are stored inverted.

void XEmitter::EmitVEX(u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, bool w,
                       int trailing_bytes)
{
  const u32 not_r = Ext(reg) ? 0 : 1;
  const u32 not_x = Ext(rm.m_index) ? 0 : 1;
  const u32 not_b = Ext(rm.m_base) ? 0 : 1;
  const u32 map = static_cast<u32>(VexMap(op));
  const u32 pp = static_cast<u32>(VexPP(prefix));
  const u32 l = (IsYmm(reg) || IsYmm(vvvv)) ? 1 : 0;
  const u32 not_vvvv = vvvv == INVALID_REG ? 0xF : (~static_cast<u32>(vvvv) & 0xF);
  const u32 tail = (not_vvvv << 3) | (l << 2) | pp;

  if (not_x && not_b && !w && map == 1)
  {
    Write8(0xC5);
    Write8(static_cast<u8>((not_r << 7) | tail));
  }
  else
  {
    Write8(0xC4);
    Write8(static_cast<u8>((not_r << 7) | (not_x << 6) | (not_b << 5) | map));
    Write8(static_cast<u8>((w ? 0x80 : 0) | tail));
  }
  Write8(static_cast<u8>(op));
  EmitOperand(rm, reg, trailing_bytes);
}

// With a fourth register operand in the high nibble of a trailing immediate byte (is4).
void XEmitter::EmitVEX4(u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, X64Reg is4,
                        bool w)
{
  EmitVEX(prefix, op, reg, vvvv, rm, w, 1);
  Write8(static_cast<u8>(static_cast<u32>(is4) << 4));
}

void XEmitter::EmitAVX(u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm,
                       int trailing_bytes)
{
  if (!cpu_info.bAVX)
    PanicAlertFmt("An AVX instruction was emitted, but this CPU does not support AVX.");
  EmitVEX(prefix, op, reg, vvvv, rm, false, trailing_bytes);
}

// AVX three-operand instructions: name(X64Reg dest, X64Reg src1, const OpArg& src2)
#define PIXELU_AVX_OPS(X)                                                                          \
  X(VADDSS, 0xF3, 0x58)                                                                            \
  X(VSUBSS, 0xF3, 0x5C)                                                                            \
  X(VMULSS, 0xF3, 0x59)                                                                            \
  X(VDIVSS, 0xF3, 0x5E)                                                                            \
  X(VADDPS, 0x00, 0x58)                                                                            \
  X(VSUBPS, 0x00, 0x5C)                                                                            \
  X(VMULPS, 0x00, 0x59)                                                                            \
  X(VDIVPS, 0x00, 0x5E)                                                                            \
  X(VADDSD, 0xF2, 0x58)                                                                            \
  X(VSUBSD, 0xF2, 0x5C)                                                                            \
  X(VMULSD, 0xF2, 0x59)                                                                            \
  X(VDIVSD, 0xF2, 0x5E)                                                                            \
  X(VADDPD, 0x66, 0x58)                                                                            \
  X(VSUBPD, 0x66, 0x5C)                                                                            \
  X(VMULPD, 0x66, 0x59)                                                                            \
  X(VDIVPD, 0x66, 0x5E)                                                                            \
  X(VSQRTSD, 0xF2, 0x51)                                                                           \
  X(VUNPCKLPS, 0x00, 0x14)                                                                         \
  X(VUNPCKLPD, 0x66, 0x14)                                                                         \
  X(VUNPCKHPD, 0x66, 0x15)                                                                         \
  X(VANDPS, 0x00, 0x54)                                                                            \
  X(VANDPD, 0x66, 0x54)                                                                            \
  X(VANDNPS, 0x00, 0x55)                                                                           \
  X(VANDNPD, 0x66, 0x55)                                                                           \
  X(VORPS, 0x00, 0x56)                                                                             \
  X(VORPD, 0x66, 0x56)                                                                             \
  X(VXORPS, 0x00, 0x57)                                                                            \
  X(VXORPD, 0x66, 0x57)                                                                            \
  X(VPAND, 0x66, 0xDB)                                                                             \
  X(VPANDN, 0x66, 0xDF)                                                                            \
  X(VPOR, 0x66, 0xEB)                                                                              \
  X(VPXOR, 0x66, 0xEF)

#define PIXELU_DEFINE_AVX(name, prefix, op)                                                        \
  void XEmitter::name(X64Reg regOp1, X64Reg regOp2, const OpArg& arg)                              \
  {                                                                                                \
    EmitAVX(prefix, op, regOp1, regOp2, arg);                                                      \
  }
PIXELU_AVX_OPS(PIXELU_DEFINE_AVX)
#undef PIXELU_DEFINE_AVX
#undef PIXELU_AVX_OPS

// With an immediate byte
#define PIXELU_AVX_IMM_OPS(X)                                                                      \
  X(VCMPPD, 0x66, 0xC2)                                                                            \
  X(VSHUFPS, 0x00, 0xC6)                                                                           \
  X(VSHUFPD, 0x66, 0xC6)                                                                           \
  X(VBLENDPS, 0x66, 0x3A0C)                                                                        \
  X(VBLENDPD, 0x66, 0x3A0D)

#define PIXELU_DEFINE_AVX_IMM(name, prefix, op)                                                    \
  void XEmitter::name(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, u8 imm)                      \
  {                                                                                                \
    EmitAVX(prefix, op, regOp1, regOp2, arg, 1);                                                   \
    Write8(imm);                                                                                   \
  }
PIXELU_AVX_IMM_OPS(PIXELU_DEFINE_AVX_IMM)
#undef PIXELU_DEFINE_AVX_IMM
#undef PIXELU_AVX_IMM_OPS

void XEmitter::VBLENDVPD(X64Reg regOp1, X64Reg regOp2, const OpArg& arg, X64Reg mask)
{
  if (!cpu_info.bAVX)
    PanicAlertFmt("An AVX instruction was emitted, but this CPU does not support AVX.");
  EmitVEX4(0x66, 0x3A4B, regOp1, regOp2, arg, mask);
}

// vpsllq dest, src, imm8: VEX.vvvv is the destination, ModRM.rm the source, /6.
void XEmitter::VPSLLQ(X64Reg regOp1, X64Reg regOp2, u8 shift)
{
  EmitAVX(0x66, 0x73, static_cast<X64Reg>(6), regOp1, R(regOp2));
  Write8(shift);
}

void XEmitter::VMOVAPS(const OpArg& arg, X64Reg regOp)
{
  EmitAVX(0x00, 0x29, regOp, INVALID_REG, arg);
}

void XEmitter::VZEROUPPER()
{
  if (!cpu_info.bAVX)
    PanicAlertFmt("An AVX instruction was emitted, but this CPU does not support AVX.");
  Write8(0xC5);
  Write8(0xF8);
  Write8(0x77);
}

// FMA3: VEX.66.0F38 with W selecting double precision.
void XEmitter::EmitFMA3(u8 op, X64Reg reg, X64Reg vvvv, const OpArg& rm, bool w)
{
  if (!cpu_info.bFMA)
    PanicAlertFmt("An FMA3 instruction was emitted, but this CPU does not support FMA3.");
  EmitVEX(0x66, static_cast<u16>(0x3800 | op), reg, vvvv, rm, w);
}

// The FMA3 opcode is 0x90/0xA0/0xB0 (operand order 132/213/231) plus 6 (fmaddsub), 7 (fmsubadd),
// 8 (fmadd), 0xA (fmsub), 0xC (fnmadd) or 0xE (fnmsub), plus 1 for the scalar forms.
#define PIXELU_DEFINE_FMA3(name, op, w)                                                            \
  void XEmitter::name(X64Reg regOp1, X64Reg regOp2, const OpArg& arg)                              \
  {                                                                                                \
    EmitFMA3(op, regOp1, regOp2, arg, w);                                                          \
  }
#define PIXELU_DEFINE_FMA3_FORM(form, base)                                                        \
  PIXELU_DEFINE_FMA3(VFMADD##form##PS, base + 0x8, false)                                          \
  PIXELU_DEFINE_FMA3(VFMADD##form##PD, base + 0x8, true)                                           \
  PIXELU_DEFINE_FMA3(VFMADD##form##SS, base + 0x9, false)                                          \
  PIXELU_DEFINE_FMA3(VFMADD##form##SD, base + 0x9, true)                                           \
  PIXELU_DEFINE_FMA3(VFMSUB##form##PS, base + 0xA, false)                                          \
  PIXELU_DEFINE_FMA3(VFMSUB##form##PD, base + 0xA, true)                                           \
  PIXELU_DEFINE_FMA3(VFMSUB##form##SS, base + 0xB, false)                                          \
  PIXELU_DEFINE_FMA3(VFMSUB##form##SD, base + 0xB, true)                                           \
  PIXELU_DEFINE_FMA3(VFNMADD##form##PS, base + 0xC, false)                                         \
  PIXELU_DEFINE_FMA3(VFNMADD##form##PD, base + 0xC, true)                                          \
  PIXELU_DEFINE_FMA3(VFNMADD##form##SS, base + 0xD, false)                                         \
  PIXELU_DEFINE_FMA3(VFNMADD##form##SD, base + 0xD, true)                                          \
  PIXELU_DEFINE_FMA3(VFNMSUB##form##PS, base + 0xE, false)                                         \
  PIXELU_DEFINE_FMA3(VFNMSUB##form##PD, base + 0xE, true)                                          \
  PIXELU_DEFINE_FMA3(VFNMSUB##form##SS, base + 0xF, false)                                         \
  PIXELU_DEFINE_FMA3(VFNMSUB##form##SD, base + 0xF, true)                                          \
  PIXELU_DEFINE_FMA3(VFMADDSUB##form##PS, base + 0x6, false)                                       \
  PIXELU_DEFINE_FMA3(VFMADDSUB##form##PD, base + 0x6, true)                                        \
  PIXELU_DEFINE_FMA3(VFMSUBADD##form##PS, base + 0x7, false)                                       \
  PIXELU_DEFINE_FMA3(VFMSUBADD##form##PD, base + 0x7, true)
PIXELU_DEFINE_FMA3_FORM(132, 0x90)
PIXELU_DEFINE_FMA3_FORM(213, 0xA0)
PIXELU_DEFINE_FMA3_FORM(231, 0xB0)
#undef PIXELU_DEFINE_FMA3_FORM
#undef PIXELU_DEFINE_FMA3

// FMA4 (VEX.66.0F3A, register source in imm8[7:4]). W = 1 puts the third source in ModRM.rm,
// W = 0 the second.
void XEmitter::EmitFMA4(u8 op, X64Reg dest, X64Reg src1, X64Reg reg_src, const OpArg& rm, bool w)
{
  if (!cpu_info.bFMA4)
    PanicAlertFmt("An FMA4 instruction was emitted, but this CPU does not support FMA4.");
  EmitVEX4(0x66, static_cast<u16>(0x3A00 | op), dest, src1, rm, reg_src, w);
}

#define PIXELU_DEFINE_FMA4(name, op)                                                               \
  void XEmitter::name(X64Reg dest, X64Reg regOp1, X64Reg regOp2, const OpArg& arg)                 \
  {                                                                                                \
    EmitFMA4(op, dest, regOp1, regOp2, arg, true);                                                 \
  }                                                                                                \
  void XEmitter::name(X64Reg dest, X64Reg regOp1, const OpArg& arg, X64Reg regOp2)                 \
  {                                                                                                \
    EmitFMA4(op, dest, regOp1, regOp2, arg, false);                                                \
  }
// 0x5C-0x5F: addsub/subadd PS/PD; 0x68 + 4k: madd/msub/nmadd/nmsub PS, PD, SS, SD
PIXELU_DEFINE_FMA4(VFMADDSUBPS, 0x5C)
PIXELU_DEFINE_FMA4(VFMADDSUBPD, 0x5D)
PIXELU_DEFINE_FMA4(VFMSUBADDPS, 0x5E)
PIXELU_DEFINE_FMA4(VFMSUBADDPD, 0x5F)
PIXELU_DEFINE_FMA4(VFMADDPS, 0x68)
PIXELU_DEFINE_FMA4(VFMADDPD, 0x69)
PIXELU_DEFINE_FMA4(VFMADDSS, 0x6A)
PIXELU_DEFINE_FMA4(VFMADDSD, 0x6B)
PIXELU_DEFINE_FMA4(VFMSUBPS, 0x6C)
PIXELU_DEFINE_FMA4(VFMSUBPD, 0x6D)
PIXELU_DEFINE_FMA4(VFMSUBSS, 0x6E)
PIXELU_DEFINE_FMA4(VFMSUBSD, 0x6F)
PIXELU_DEFINE_FMA4(VFNMADDPS, 0x78)
PIXELU_DEFINE_FMA4(VFNMADDPD, 0x79)
PIXELU_DEFINE_FMA4(VFNMADDSS, 0x7A)
PIXELU_DEFINE_FMA4(VFNMADDSD, 0x7B)
PIXELU_DEFINE_FMA4(VFNMSUBPS, 0x7C)
PIXELU_DEFINE_FMA4(VFNMSUBPD, 0x7D)
PIXELU_DEFINE_FMA4(VFNMSUBSS, 0x7E)
PIXELU_DEFINE_FMA4(VFNMSUBSD, 0x7F)
#undef PIXELU_DEFINE_FMA4

// BMI1/BMI2: VEX-encoded general purpose instructions, W = 64-bit operand size.
void XEmitter::EmitBMI(int bits, u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm,
                       int trailing_bytes)
{
  if (rm.IsImm())
    PanicAlertFmt("BMI instructions cannot take an immediate operand.");
  if (bits != 32 && bits != 64)
    PanicAlertFmt("BMI instructions only have 32- and 64-bit forms.");
  EmitVEX(prefix, op, reg, vvvv, rm, bits == 64, trailing_bytes);
}

void XEmitter::EmitBMI1(int bits, u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm)
{
  CheckFlags();
  if (!cpu_info.bBMI1)
    PanicAlertFmt("A BMI1 instruction was emitted, but this CPU does not support BMI1.");
  EmitBMI(bits, prefix, op, reg, vvvv, rm);
}

void XEmitter::EmitBMI2(int bits, u8 prefix, u16 op, X64Reg reg, X64Reg vvvv, const OpArg& rm,
                        int trailing_bytes)
{
  if (!cpu_info.bBMI2)
    PanicAlertFmt("A BMI2 instruction was emitted, but this CPU does not support BMI2.");
  EmitBMI(bits, prefix, op, reg, vvvv, rm, trailing_bytes);
}

// The shift count of SARX/SHLX/SHRX is in VEX.vvvv.
void XEmitter::SARX(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2)
{
  EmitBMI2(bits, 0xF3, 0x38F7, regOp1, regOp2, arg);
}

void XEmitter::SHLX(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2)
{
  EmitBMI2(bits, 0x66, 0x38F7, regOp1, regOp2, arg);
}

void XEmitter::SHRX(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2)
{
  EmitBMI2(bits, 0xF2, 0x38F7, regOp1, regOp2, arg);
}

void XEmitter::RORX(int bits, X64Reg regOp, const OpArg& arg, u8 rotate)
{
  EmitBMI2(bits, 0xF2, 0x3AF0, regOp, INVALID_REG, arg, 1);
  Write8(rotate);
}

void XEmitter::PEXT(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg)
{
  EmitBMI2(bits, 0xF3, 0x38F5, regOp1, regOp2, arg);
}

void XEmitter::PDEP(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg)
{
  EmitBMI2(bits, 0xF2, 0x38F5, regOp1, regOp2, arg);
}

// mulx: regOp1 receives the low half (VEX.vvvv), regOp2 the high half (ModRM.reg).
void XEmitter::MULX(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg)
{
  EmitBMI2(bits, 0xF2, 0x38F6, regOp2, regOp1, arg);
}

void XEmitter::BZHI(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2)
{
  CheckFlags();
  EmitBMI2(bits, 0x00, 0x38F5, regOp1, regOp2, arg);
}

// VEX 0F38 F3 group: the destination is VEX.vvvv, ModRM.reg is /1 (BLSR), /2 (BLSMSK), /3 (BLSI).
void XEmitter::BLSR(int bits, X64Reg regOp, const OpArg& arg)
{
  EmitBMI1(bits, 0x00, 0x38F3, static_cast<X64Reg>(1), regOp, arg);
}

void XEmitter::BLSMSK(int bits, X64Reg regOp, const OpArg& arg)
{
  EmitBMI1(bits, 0x00, 0x38F3, static_cast<X64Reg>(2), regOp, arg);
}

void XEmitter::BLSI(int bits, X64Reg regOp, const OpArg& arg)
{
  EmitBMI1(bits, 0x00, 0x38F3, static_cast<X64Reg>(3), regOp, arg);
}

void XEmitter::BEXTR(int bits, X64Reg regOp1, const OpArg& arg, X64Reg regOp2)
{
  EmitBMI1(bits, 0x00, 0x38F7, regOp1, regOp2, arg);
}

void XEmitter::ANDN(int bits, X64Reg regOp1, X64Reg regOp2, const OpArg& arg)
{
  EmitBMI1(bits, 0x00, 0x38F2, regOp1, regOp2, arg);
}

}  // namespace Gen
