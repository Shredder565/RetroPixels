// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/PowerPC/Jit64Common/EmuCodeBlock.h"
#include "Core/PowerPC/JitCommon/JitAsmCommon.h"

enum EQuantizeType : u32;

class Jit64;

// Emits the conversion between paired singles in XMM0 and a GQR-quantized
// memory representation. Used both for the shared routines (quantize == -1,
// scale passed in RSCRATCH2) and inline when the GQR is known at compile time.
class QuantizedMemoryRoutines : public EmuCodeBlock
{
public:
  explicit QuantizedMemoryRoutines(Jit64& jit) : EmuCodeBlock(jit) {}
  void GenQuantizedLoad(bool single, EQuantizeType type, int quantize);
  void GenQuantizedStore(bool single, EQuantizeType type, int quantize);

private:
  void GenQuantizedLoadFloat(bool single, bool isInline);
  void GenQuantizedStoreFloat(bool single, bool isInline);

  // Multiplies XMM0 by the scale factor for `quantize`, read from the
  // quantize (store) or dequantize (load) table.
  void ApplyQuantizeScale(bool single, int quantize, bool load);
};

// Shared assembly routines called from Jit64 blocks.
class CommonAsmRoutines : public CommonAsmRoutinesBase, public QuantizedMemoryRoutines
{
public:
  explicit CommonAsmRoutines(Jit64& jit) : QuantizedMemoryRoutines(jit) {}
  void GenFrsqrte();
  void GenFres();
  void GenMfcr();

protected:
  void GenConvertDoubleToSingle();
  void GenerateFmaddsEft();
  void GeneratePsMaddEft();
  const u8* GenQuantizedLoadRuntime(bool single, EQuantizeType type);
  const u8* GenQuantizedStoreRuntime(bool single, EQuantizeType type);
  void GenQuantizedLoads();
  void GenQuantizedSingleLoads();
  void GenQuantizedStores();
  void GenQuantizedSingleStores();

private:
  // Emits one routine per quantize type plus the table pointing at them.
  const u8** GenQuantizedTable(bool load, bool single);
  // Sets an FPSCR exception bit, and FX if the bit was clear before.
  void RaiseFPSCRException(u32 exception_bit);
};
