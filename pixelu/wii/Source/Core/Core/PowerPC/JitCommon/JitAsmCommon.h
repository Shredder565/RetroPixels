// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// PSHUFB masks that byte-swap the first one or two 32-bit lanes and keep the rest.
alignas(16) extern const std::array<u8, 16> pbswapShuffle1x4;
alignas(16) extern const std::array<u8, 16> pbswapShuffle2x4;

// {1.0f, 0, 0, 0}
alignas(16) extern const std::array<float, 4> m_one;

// Scale factors for paired-single quantization, indexed by 2 * (6-bit GQR
// scale field) so each factor fills two adjacent lanes. The field is a signed
// exponent: quantizing multiplies by 2^scale, dequantizing by 2^-scale.
alignas(16) extern const std::array<float, 128> m_quantizeTableS;
alignas(16) extern const std::array<float, 128> m_dequantizeTableS;

// Entry points of the code shared by every block. Filled in by the backend's
// routine generator.
struct CommonAsmRoutinesBase
{
  const u8* enter_code;

  const u8* dispatcher_mispredicted_blr;
  const u8* dispatcher;
  const u8* dispatcher_no_timing_check;
  const u8* dispatcher_no_check;
  const u8* dispatcher_exit;

  const u8* do_timing;

  const u8* frsqrte;
  const u8* fres;
  const u8* mfcr;
  const u8* cdts;
  const u8* cstd;
  const u8* fprf_single;
  const u8* fprf_double;
  const u8* fmadds_eft;
  const u8* ps_madd_eft;

  // Quantized load/store helpers, one per GQR load or store type and scale.
  // Address in ECX; the value is returned in (or taken from) the low lane(s)
  // of XMM0. They clobber all three scratch registers and may raise a guest
  // exception, so the PC must be stored first when that matters.
  const u8** paired_load_quantized;
  const u8** single_load_quantized;
  const u8** paired_store_quantized;
  const u8** single_store_quantized;
};
