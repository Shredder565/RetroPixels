// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/BPMemory.h"

#include <bit>
#include <limits>

// The emulated BP register file (saved in savestates).
BPMemory bpmem;

namespace
{
// Builds a float from the fog registers' reduced format: sign, 8-bit exponent and an 11-bit
// mantissa that occupies the top of the 23-bit IEEE mantissa.
float FogFloat(u32 sign, u32 exponent, u32 mantissa)
{
  return std::bit_cast<float>((sign << 31) | (exponent << 23) | (mantissa << 12));
}
}  // namespace

bool BlendMode::UseLogicOp() const
{
  // Logic ops only apply with blending off. NoOp is treated as "no logic op" (a game draws with
  // it to update destination alpha only).
  return !blend_enable && logic_op_enable && logic_mode != LogicOp::NoOp;
}

// Both A and C having the maximum exponent (infinite or NaN). Hardware output in that case
// depends only on the signs: fogged when both are positive, unfogged otherwise. GetA/GetC replace
// the values with finite/infinite ones that reproduce this without handing NaNs to the host GPU.
bool FogParams::IsNaNCase() const
{
  return a.exp == 255 && c_proj_fsel.c_exp == 255;
}

float FogParam0::FloatValue() const
{
  return FogFloat(sign, exp, mant);
}

float FogParam3::FloatValue() const
{
  return FogFloat(c_sign, c_exp, c_mant);
}

float FogParams::GetA() const
{
  return IsNaNCase() ? 0.0f : a.FloatValue();
}

float FogParams::GetC() const
{
  if (!IsNaNCase())
    return c_proj_fsel.FloatValue();

  constexpr float infinity = std::numeric_limits<float>::infinity();
  const bool both_positive = !a.sign && !c_proj_fsel.c_sign;
  return both_positive ? -infinity : infinity;
}
