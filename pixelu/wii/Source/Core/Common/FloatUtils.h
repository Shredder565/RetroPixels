// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <bit>
#include <limits>

#include "Common/CommonTypes.h"

namespace Common
{
template <typename T>
constexpr T SNANConstant()
{
  return std::numeric_limits<T>::signaling_NaN();
}

// IEEE-754 field masks. On every host we support, the top fraction bit marks a quiet NaN.
static constexpr u64 DOUBLE_SIGN = 0x8000000000000000ULL;
static constexpr u64 DOUBLE_EXP = 0x7FF0000000000000ULL;
static constexpr u64 DOUBLE_FRAC = 0x000FFFFFFFFFFFFFULL;
static constexpr u64 DOUBLE_QBIT = 0x0008000000000000ULL;
static constexpr u64 DOUBLE_ZERO = 0;
static constexpr int DOUBLE_EXP_WIDTH = 11;
static constexpr int DOUBLE_FRAC_WIDTH = 52;

static constexpr u32 FLOAT_SIGN = 0x80000000;
static constexpr u32 FLOAT_EXP = 0x7F800000;
static constexpr u32 FLOAT_FRAC = 0x007FFFFF;
static constexpr u32 FLOAT_ZERO = 0;
static constexpr int FLOAT_EXP_WIDTH = 8;
static constexpr int FLOAT_FRAC_WIDTH = 23;

inline bool IsQNAN(double d)
{
  const u64 bits = std::bit_cast<u64>(d);
  return (bits & DOUBLE_EXP) == DOUBLE_EXP && (bits & DOUBLE_QBIT) != 0;
}

inline bool IsSNAN(double d)
{
  const u64 bits = std::bit_cast<u64>(d);
  return (bits & DOUBLE_EXP) == DOUBLE_EXP && (bits & DOUBLE_FRAC) != 0 &&
         (bits & DOUBLE_QBIT) == 0;
}

// Denormals (and zeros) become zero with the same sign.
inline float FlushToZero(float f)
{
  const u32 bits = std::bit_cast<u32>(f);
  return std::bit_cast<float>((bits & FLOAT_EXP) == 0 ? (bits & FLOAT_SIGN) : bits);
}

inline double FlushToZero(double d)
{
  const u64 bits = std::bit_cast<u64>(d);
  return std::bit_cast<double>((bits & DOUBLE_EXP) == 0 ? (bits & DOUBLE_SIGN) : bits);
}

// Sets the quiet bit (turns an SNaN into the corresponding QNaN).
inline double MakeQuiet(double d)
{
  return std::bit_cast<double>(std::bit_cast<u64>(d) | DOUBLE_QBIT);
}

// FPSCR[FPRF] class codes.
enum PPCFpClass
{
  PPC_FPCLASS_QNAN = 0x11,
  PPC_FPCLASS_NINF = 0x9,
  PPC_FPCLASS_NN = 0x8,
  PPC_FPCLASS_ND = 0x18,
  PPC_FPCLASS_NZ = 0x12,
  PPC_FPCLASS_PZ = 0x2,
  PPC_FPCLASS_PD = 0x14,
  PPC_FPCLASS_PN = 0x4,
  PPC_FPCLASS_PINF = 0x5,
};

// Returns the PPCFpClass of a value (any NaN counts as QNAN).
u32 ClassifyDouble(double dvalue);
u32 ClassifyFloat(float fvalue);

// One segment of the hardware's piecewise-linear estimate tables: the estimate is
// m_base + m_dec * (offset within the segment).
struct BaseAndDec
{
  int m_base;
  int m_dec;
};
extern const std::array<BaseAndDec, 32> frsqrte_expected;
extern const std::array<BaseAndDec, 32> fres_expected;

// Bit-exact versions of the Gekko/Broadway estimate instructions.
double ApproximateReciprocalSquareRoot(double val);  // frsqrte, ps_rsqrte
double ApproximateReciprocal(double val);            // fres, ps_res
}  // namespace Common
