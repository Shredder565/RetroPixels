// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64Common/Jit64Constants.h"
#include "Core/PowerPC/PowerPC.h"

// RPPCSTATE holds &ppcState + PPCSTATE_BIAS; every displacement below is
// relative to that biased pointer. The bias centres the 8-bit displacement
// range (-0x80..0x7f) on the start of the structure.
constexpr int PPCSTATE_BIAS = 0x80;

// Byte offset of a PowerPCState member as an int. PowerPCState is not
// standard-layout, so GCC and Clang warn about offsetof on it; the warning is
// silenced inside an immediately evaluated lambda, which keeps the result a
// constant expression.
#ifdef __GNUC__
#define PIXELU_PPCSTATE_OFFSETOF(member)                                                           \
  ([]() consteval {                                                                                \
    _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Winvalid-offsetof\"")        \
    const int result = static_cast<int>(offsetof(PowerPC::PowerPCState, member));                 \
    _Pragma("GCC diagnostic pop") return result;                                                   \
  }())
#else
#define PIXELU_PPCSTATE_OFFSETOF(member) static_cast<int>(offsetof(PowerPC::PowerPCState, member))
#endif

// Displacement of a member from RPPCSTATE.
#define PPCSTATE_OFF(member) (PIXELU_PPCSTATE_OFFSETOF(member) - PPCSTATE_BIAS)

// Displacement of element `index` of an array member. The index may be a
// runtime value, so only element 0 goes through offsetof.
#define PPCSTATE_OFF_ARRAY(array, index)                                                           \
  (PPCSTATE_OFF(array[0]) +                                                                        \
   static_cast<int>(sizeof(PowerPC::PowerPCState::array[0])) * static_cast<int>(index))

#define PPCSTATE_OFF_GPR(index) PPCSTATE_OFF_ARRAY(gpr, index)
#define PPCSTATE_OFF_CR(index) PPCSTATE_OFF_ARRAY(cr.fields, index)
#define PPCSTATE_OFF_SR(index) PPCSTATE_OFF_ARRAY(sr, index)
#define PPCSTATE_OFF_SPR(index) PPCSTATE_OFF_ARRAY(spr, index)

// The paired-single offsets assume ps[] is an array of PairedSingle.
static_assert(std::is_same_v<decltype(PowerPC::PowerPCState::ps[0]), PowerPC::PairedSingle&>);
#define PPCSTATE_OFF_PS0(index)                                                                    \
  (PPCSTATE_OFF_ARRAY(ps, index) + static_cast<int>(offsetof(PowerPC::PairedSingle, ps0)))
#define PPCSTATE_OFF_PS1(index)                                                                    \
  (PPCSTATE_OFF_ARRAY(ps, index) + static_cast<int>(offsetof(PowerPC::PairedSingle, ps1)))

// Memory operands for the members above.
#define PPCSTATE(member) MDisp(RPPCSTATE, PPCSTATE_OFF(member))
#define PPCSTATE_GPR(index) MDisp(RPPCSTATE, PPCSTATE_OFF_GPR(index))
#define PPCSTATE_CR(index) MDisp(RPPCSTATE, PPCSTATE_OFF_CR(index))
#define PPCSTATE_SR(index) MDisp(RPPCSTATE, PPCSTATE_OFF_SR(index))
#define PPCSTATE_SPR(index) MDisp(RPPCSTATE, PPCSTATE_OFF_SPR(index))
#define PPCSTATE_PS0(index) MDisp(RPPCSTATE, PPCSTATE_OFF_PS0(index))
#define PPCSTATE_PS1(index) MDisp(RPPCSTATE, PPCSTATE_OFF_PS1(index))

#define PPCSTATE_LR PPCSTATE_SPR(SPR_LR)
#define PPCSTATE_CTR PPCSTATE_SPR(SPR_CTR)
#define PPCSTATE_SRR0 PPCSTATE_SPR(SPR_SRR0)
#define PPCSTATE_SRR1 PPCSTATE_SPR(SPR_SRR1)
