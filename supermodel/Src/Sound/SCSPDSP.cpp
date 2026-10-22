// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// SCSP effect DSP: one pass of the microprogram per sample (see SCSPDSP.h).
//
// Microcode word layout (four 16-bit words per step):
//   word 0: TRA (14-8) TWT (7) TWA (6-0)
//   word 1: XSEL (15) YSEL (14-13) IRA (11-6) IWT (5) IWA (4-0)
//   word 2: TABLE (15) MWT (14) MRD (13) EWT (12) EWA (11-8) ADRL (7)
//           FRCL (6) SHFT (5-4) YRL (3) NEGB (2) ZERO (1) BSEL (0)
//   word 3: NOFL (15) COEF (14-9) MASA (6-2) ADREB (1) NXADR (0)
// Each step computes ACC = X * Y / 4096 + B, where X is an input or a TEMP
// register, Y a coefficient, the fraction register or part of the Y
// register, and B the previous ACC or a TEMP register. The shifter output
// (the previous ACC, saturated or wrapped to 24 bits) can be written to TEMP,
// to the effect outputs, and to sound RAM. Memory is accessed on odd steps.

#include "SCSPDSP.h"
#include <cstring>

namespace
{
  // Sign-extends the low 'bits' bits of v.
  inline INT32 SignExtend(INT32 v, int bits)
  {
    const int s = 32 - bits;
    return (INT32)((UINT32)v << s) >> s;
  }

  inline INT32 Saturate24(INT32 v)
  {
    if (v > 0x7FFFFF)
      return 0x7FFFFF;
    if (v < -0x800000)
      return -0x800000;
    return v;
  }

  // 24-bit value -> 16-bit memory format: sign (15), exponent = number of
  // redundant sign bits below bit 23, at most 12 (14-11), 11-bit mantissa.
  UINT16 ToMemory(INT32 value)
  {
    const UINT32 v = (UINT32)value;
    const UINT32 sign = (v >> 23) & 1;
    unsigned exponent = 0;
    while (exponent < 12 && ((v >> (23 - exponent)) & 1) == ((v >> (22 - exponent)) & 1))
      ++exponent;
    UINT32 mantissa;
    if (exponent < 12)
      mantissa = ((v << exponent) >> 11) & 0x7FF;
    else
      mantissa = v & 0x7FF;
    return (UINT16)((sign << 15) | (exponent << 11) | mantissa);
  }

  // 16-bit memory format -> 24-bit value (sign-extended to 32 bits).
  INT32 FromMemory(UINT16 word)
  {
    const UINT32 sign = (word >> 15) & 1;
    UINT32 exponent = (word >> 11) & 0xF;
    UINT32 v = (UINT32)(word & 0x7FF) << 11;
    if (exponent > 11)
    {
      // Exponents 12-15 all mean a plain 11-bit value (no hidden bit).
      exponent = 11;
      v |= sign << 22;
    }
    else
      v |= (sign ^ 1) << 22;   // the hidden bit: the inverse of the sign
    v |= sign << 23;
    return SignExtend((INT32)v, 24) >> exponent;
  }
}

void ScspDsp::Reset(State &dsp)
{
  std::memset(&dsp, 0, sizeof(dsp));
  dsp.ringWords = 8 * 1024;
  dsp.idle = true;
}

void ScspDsp::Send(State &dsp, INT32 value, int sel)
{
  dsp.mixs[sel] = (INT32)((UINT32)dsp.mixs[sel] + (UINT32)value);
}

void ScspDsp::Start(State &dsp)
{
  dsp.idle = false;
  // The program ends after the last step that has any bit set.
  int last = 127;
  const UINT16 *code = dsp.Code();
  while (last >= 0)
  {
    const UINT16 *w = &code[last * 4];
    if (w[0] | w[1] | w[2] | w[3])
      break;
    --last;
  }
  dsp.steps = last + 1;
}

void ScspDsp::RunSample(State &dsp)
{
  if (dsp.idle)
    return;

  const INT16 *coef = dsp.Coefficients();
  const UINT16 *madrs = dsp.Addresses();
  const UINT16 *code = dsp.Code();

  // Pipeline registers live for one pass only.
  INT32 acc = 0;
  INT32 shifted = 0;
  INT32 memRead = 0;
  INT32 frac = 0;
  INT32 yReg = 0;
  UINT32 addrReg = 0;

  std::memset(dsp.efreg, 0, sizeof(dsp.efreg));

  for (int step = 0; step < dsp.steps; ++step)
  {
    const UINT16 w0 = code[step * 4 + 0];
    const UINT16 w1 = code[step * 4 + 1];
    const UINT16 w2 = code[step * 4 + 2];
    const UINT16 w3 = code[step * 4 + 3];

    const unsigned tempRead = (w0 >> 8) & 0x7F;
    const bool tempWrite = (w0 >> 7) & 1;
    const unsigned tempWriteAddr = w0 & 0x7F;

    const bool xFromInput = (w1 >> 15) & 1;
    const unsigned ySelect = (w1 >> 13) & 3;
    const unsigned inputAddr = (w1 >> 6) & 0x3F;
    const bool memsWrite = (w1 >> 5) & 1;
    const unsigned memsWriteAddr = w1 & 0x1F;

    const bool tableMode = (w2 >> 15) & 1;
    const bool memWrite = (w2 >> 14) & 1;
    const bool memReadOp = (w2 >> 13) & 1;
    const bool effectWrite = (w2 >> 12) & 1;
    const unsigned effectAddr = (w2 >> 8) & 0xF;
    const bool latchAddr = (w2 >> 7) & 1;
    const bool latchFrac = (w2 >> 6) & 1;
    const unsigned shiftMode = (w2 >> 4) & 3;
    const bool latchY = (w2 >> 3) & 1;
    const bool negateB = (w2 >> 2) & 1;
    const bool zeroB = (w2 >> 1) & 1;
    const bool bFromAcc = w2 & 1;

    const bool rawMemory = (w3 >> 15) & 1;
    const unsigned coefAddr = (w3 >> 9) & 0x3F;
    const unsigned memAddrSel = (w3 >> 2) & 0x1F;
    const bool addAddrReg = (w3 >> 1) & 1;
    const bool nextAddr = w3 & 1;

    // Input bus
    INT32 input;
    if (inputAddr < 0x20)
      input = dsp.mems[inputAddr];
    else if (inputAddr < 0x30)
      input = (INT32)((UINT32)dsp.mixs[inputAddr - 0x20] << 4);
    else if (inputAddr < 0x32)
      input = (INT32)dsp.exts[inputAddr - 0x30] * 256;
    else
      return;   // undefined source: the pass stops here
    input = SignExtend(input, 24);

    if (memsWrite)
    {
      dsp.mems[memsWriteAddr] = memRead;
      if (inputAddr == memsWriteAddr)
        input = memRead;
    }

    const unsigned tempSlot = (tempRead + dsp.ringOffset) & 0x7F;

    INT32 b = 0;
    if (!zeroB)
    {
      b = bFromAcc ? acc : SignExtend(dsp.temp[tempSlot], 24);
      if (negateB)
        b = 0 - b;
    }

    const INT32 x = xFromInput ? input : SignExtend(dsp.temp[tempSlot], 24);

    INT32 y;
    switch (ySelect)
    {
    case 0:  y = frac; break;
    case 1:  y = coef[coefAddr] >> 3; break;
    case 2:  y = (yReg >> 11) & 0x1FFF; break;
    default: y = (yReg >> 4) & 0x0FFF; break;
    }

    if (latchY)
      yReg = input;

    // Shifter (works on the ACC of the previous step)
    switch (shiftMode)
    {
    case 0:  shifted = Saturate24(acc); break;
    case 1:  shifted = Saturate24(acc * 2); break;
    case 2:  shifted = SignExtend(acc * 2, 24); break;
    default: shifted = SignExtend(acc, 24); break;
    }

    y = SignExtend(y, 13);
    acc = (INT32)(((INT64)x * (INT64)y) >> 12) + b;

    if (tempWrite)
      dsp.temp[(tempWriteAddr + dsp.ringOffset) & 0x7F] = shifted;

    if (latchFrac)
      frac = (shiftMode == 3) ? (shifted & 0x0FFF) : ((shifted >> 11) & 0x1FFF);

    if (memReadOp || memWrite)
    {
      UINT32 addr = madrs[memAddrSel];
      if (!tableMode)
        addr += dsp.ringOffset;
      if (addAddrReg)
        addr += addrReg & 0x0FFF;
      if (nextAddr)
        addr++;
      addr &= tableMode ? 0xFFFF : (dsp.ringWords - 1);
      addr += dsp.ringBase << 12;
      if (addr > 0x7FFFF)
        addr = 0;
      if (step & 1)
      {
        if (memReadOp)
          memRead = rawMemory ? (INT32)((UINT32)dsp.ram[addr] << 8) : FromMemory(dsp.ram[addr]);
        if (memWrite)
          dsp.ram[addr] = rawMemory ? (UINT16)(shifted >> 8) : ToMemory(shifted);
      }
    }

    if (latchAddr)
      addrReg = (shiftMode == 3) ? (UINT32)((shifted >> 12) & 0xFFF) : (UINT32)(input >> 16);

    if (effectWrite)
      dsp.efreg[effectAddr] = (INT16)(dsp.efreg[effectAddr] + (shifted >> 8));
  }

  --dsp.ringOffset;
  std::memset(dsp.mixs, 0, sizeof(dsp.mixs));
}
