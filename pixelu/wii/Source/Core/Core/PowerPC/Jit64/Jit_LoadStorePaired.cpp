// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/Jit64/Jit.h"

#include "Common/CommonTypes.h"
#include "Common/x64Emitter.h"
#include "Core/PowerPC/Jit64/RegCache/JitRegCache.h"
#include "Core/PowerPC/Jit64Common/Jit64PowerPCState.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Gen;

// Quantized loads and stores call the shared routines in JitAsmCommon,
// selected by the GQR's type and scale. When the analyzer knows the GQR's
// value for the block, the routine is chosen (or the access inlined) at
// compile time; otherwise it is looked up from the GQR at run time.
//
// The run-time lookup masks the GQR half to 0x3F07 (scale in bits 8..13,
// type in bits 0..2) in RSCRATCH2, which the routine reads the scale from.
// The routine tables hold 8-byte entries and their address has a zero low
// byte, so ORing the type into that byte and shifting the byte left by 3
// gives the entry's address.

namespace
{
// Bits of a GQR half that the routines use: the 6-bit scale and the type.
constexpr u32 GQR_ROUTINE_BITS = 0x3F07;
constexpr u32 GQR_SCALE_BITS = 0x3F00;

struct PairedAccess
{
  bool indexed;
  bool update;
  u32 a;
  u32 b;
  u32 fs;
  u32 gqr;
  bool single;
  s32 offset;
};

// psq_l/psq_lu are 56/57 and psq_st/psq_stu 60/61; the X-forms are under
// opcode 4 with the update bit in SUBOP6.
PairedAccess DecodePairedAccess(UGeckoInstruction inst, u32 update_opcode)
{
  PairedAccess access{};
  access.offset = inst.SIMM_12;
  access.indexed = inst.OPCD == 4;
  if (access.indexed)
    access.update = (inst.SUBOP6 & 32) != 0;
  else
    access.update = inst.OPCD == update_opcode && access.offset != 0;
  access.a = inst.RA;
  access.b = access.indexed ? u32{inst.RB} : access.a;
  access.fs = inst.FS;
  access.gqr = access.indexed ? u32{inst.Ix} : u32{inst.I};
  access.single = (access.indexed ? u32{inst.Wx} : u32{inst.W}) != 0;
  return access;
}
}  // namespace

void Jit64::psq_stXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStorePairedOff);

  // The routines assume data translation is on.
  FALLBACK_IF(!(m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR));

  const PairedAccess access = DecodePairedAccess(inst, 61);
  FALLBACK_IF(access.a == 0);

  FlushRegistersBeforeSlowAccess();

  RCX64Reg extra = gpr.Scratch(RSCRATCH_EXTRA);
  RCOpArg Ra = access.update ? gpr.Bind(access.a, RCMode::ReadWrite) :
                               gpr.Use(access.a, RCMode::Read);
  RCOpArg Rb = access.indexed ? gpr.Use(access.b, RCMode::Read) :
                                RCOpArg::Imm32(static_cast<u32>(access.offset));
  RCOpArg Rs = fpr.Use(access.fs, RCMode::Read);
  RegCache::Realize(extra, Ra, Rb, Rs);

  // The routines take the address in RSCRATCH_EXTRA.
  MOV_sum(32, RSCRATCH_EXTRA, Ra, Rb);
  // With memchecks rA is updated only after the store succeeded.
  if (access.update && !jo.memcheck)
    MOV(32, Ra, R(RSCRATCH_EXTRA));

  if (access.single)
    CVTSD2SS(XMM0, Rs);
  else
    CVTPD2PS(XMM0, Rs);

  if (js.constantGqrValid[access.gqr])
  {
    // The store half of the GQR is its low 16 bits.
    const u32 gqr = js.constantGqr[access.gqr] & 0xffff;
    const u32 type = gqr & 0x7;
    if (gqr == 0)
    {
      // Unscaled floats: inlined.
      GenQuantizedStore(access.single, static_cast<EQuantizeType>(type),
                        static_cast<int>((gqr & GQR_SCALE_BITS) >> 8));
    }
    else
    {
      FlushPCBeforeSlowAccess();
      // The routine for the type takes the scale in RSCRATCH2.
      MOV(32, R(RSCRATCH2), Imm32(gqr & GQR_SCALE_BITS));
      CALL(access.single ? asm_routines.single_store_quantized[type] :
                           asm_routines.paired_store_quantized[type]);
    }
  }
  else
  {
    FlushPCBeforeSlowAccess();
    // Games sometimes leave junk in the unused GQR bits, so they are masked.
    MOV(32, R(RSCRATCH2), Imm32(GQR_ROUTINE_BITS));
    AND(32, R(RSCRATCH2), PPCSTATE_SPR(SPR_GQR0 + access.gqr));
    LEA(64, RSCRATCH,
        M(access.single ? asm_routines.single_store_quantized :
                          asm_routines.paired_store_quantized));
    // The table's low byte is zero, so an 8-bit OR and shift select the
    // entry without touching the upper bits.
    OR(8, R(RSCRATCH), R(RSCRATCH2));
    SHL(8, R(RSCRATCH), Imm8(3));
    CALLptr(MatR(RSCRATCH));
  }

  if (access.update && jo.memcheck)
    ADD(32, Ra, Rb);
}

void Jit64::psq_lXX(UGeckoInstruction inst)
{
  INSTRUCTION_START
  JITDISABLE(bJITLoadStorePairedOff);

  FALLBACK_IF(!(m_ppc_state.feature_flags & FEATURE_FLAG_MSR_DR));

  const PairedAccess access = DecodePairedAccess(inst, 57);
  FALLBACK_IF(access.a == 0);

  FlushRegistersBeforeSlowAccess();

  RCX64Reg extra = gpr.Scratch(RSCRATCH_EXTRA);
  RCX64Reg Ra = gpr.Bind(access.a, access.update ? RCMode::ReadWrite : RCMode::Read);
  RCOpArg Rb = access.indexed ? gpr.Use(access.b, RCMode::Read) :
                                RCOpArg::Imm32(static_cast<u32>(access.offset));
  RCX64Reg Rs = fpr.Bind(access.fs, RCMode::Write);
  RegCache::Realize(extra, Ra, Rb, Rs);

  MOV_sum(32, RSCRATCH_EXTRA, Ra, Rb);
  if (access.update && !jo.memcheck)
    MOV(32, Ra, R(RSCRATCH_EXTRA));

  if (js.constantGqrValid[access.gqr])
  {
    // The load half of the GQR is its high 16 bits.
    const u32 gqr = js.constantGqr[access.gqr] >> 16;
    GenQuantizedLoad(access.single, static_cast<EQuantizeType>(gqr & 0x7),
                     static_cast<int>((gqr & GQR_SCALE_BITS) >> 8));
  }
  else
  {
    FlushPCBeforeSlowAccess();
    OpArg load_half = PPCSTATE_SPR(SPR_GQR0 + access.gqr);
    load_half.AddMemOffset(2);
    MOV(32, R(RSCRATCH2), Imm32(GQR_ROUTINE_BITS));
    AND(32, R(RSCRATCH2), load_half);
    LEA(64, RSCRATCH,
        M(access.single ? asm_routines.single_load_quantized :
                          asm_routines.paired_load_quantized));
    OR(8, R(RSCRATCH), R(RSCRATCH2));
    SHL(8, R(RSCRATCH), Imm8(3));
    CALLptr(MatR(RSCRATCH));
  }

  // The routines leave the pair (or the single and 1.0) in XMM0 as floats.
  CVTPS2PD(Rs, R(XMM0));
  if (access.update && jo.memcheck)
    ADD(32, Ra, Rb);
}
