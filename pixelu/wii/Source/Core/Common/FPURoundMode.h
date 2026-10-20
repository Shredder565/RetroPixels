// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Host SIMD floating-point control (MXCSR on x86-64), used to mirror the guest's FPSCR.
namespace Common::FPU
{
// Values match the PowerPC FPSCR[RN] field.
enum RoundMode : u32
{
  ROUND_NEAR = 0,
  ROUND_CHOP = 1,
  ROUND_UP = 2,
  ROUND_DOWN = 3
};

// Selects the rounding mode; non_ieee_mode additionally flushes denormal results to zero.
// All floating-point exceptions stay masked.
void SetSIMDMode(RoundMode rounding_mode, bool non_ieee_mode);

// Note that only the rounding conversions (e.g. cvtps2dq) follow the mode set here; the
// truncating ones (cvttps2dq) always round toward zero.
void SaveSIMDState();
void LoadSIMDState();
void LoadDefaultSIMDState();
}  // namespace Common::FPU
