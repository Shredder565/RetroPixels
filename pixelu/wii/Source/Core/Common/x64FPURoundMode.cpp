// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/FPURoundMode.h"

#include <array>

#include "Common/CommonTypes.h"
#include "Common/Intrinsics.h"

namespace Common::FPU
{
namespace
{
// MXCSR layout: bits 7..12 mask the six exceptions, bits 13..14 are the rounding control
// (0 nearest, 1 toward -inf, 2 toward +inf, 3 toward zero), bit 15 is flush-to-zero.
constexpr u32 MXCSR_ALL_EXCEPTIONS_MASKED = 0x3Fu << 7;
constexpr u32 MXCSR_FLUSH_TO_ZERO = 1u << 15;
constexpr unsigned MXCSR_ROUNDING_SHIFT = 13;

// Indexed by the PowerPC rounding mode.
constexpr std::array<u32, 4> HOST_ROUNDING_CONTROL = {
    0,  // ROUND_NEAR
    3,  // ROUND_CHOP
    2,  // ROUND_UP
    1,  // ROUND_DOWN
};

// The state the process started with, and the one most recently saved.
const u32 s_startup_csr = _mm_getcsr();
u32 s_stored_csr = s_startup_csr;
}  // namespace

void SetSIMDMode(RoundMode rounding_mode, bool non_ieee_mode)
{
  u32 csr = MXCSR_ALL_EXCEPTIONS_MASKED |
            (HOST_ROUNDING_CONTROL[rounding_mode & 3] << MXCSR_ROUNDING_SHIFT);
  if (non_ieee_mode)
    csr |= MXCSR_FLUSH_TO_ZERO;
  _mm_setcsr(csr);
}

void SaveSIMDState()
{
  s_stored_csr = _mm_getcsr();
}

void LoadSIMDState()
{
  _mm_setcsr(s_stored_csr);
}

void LoadDefaultSIMDState()
{
  _mm_setcsr(s_startup_csr);
}
}  // namespace Common::FPU
