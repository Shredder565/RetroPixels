// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Register moves and immediates, address register arithmetic and the $sr mode
// bits.

#include "Core/DSP/Interpreter/DSPInterpreter.h"

#include "Common/CommonTypes.h"

#include "Core/DSP/DSPCore.h"

namespace DSP::Interpreter
{
// MRR $D, $S
void Interpreter::mrr(const UDSPInstruction opc)
{
  const u16 value = OpReadRegister(opc & 0x1f);
  LoadRegister((opc >> 5) & 0x1f, value);
}

// LRI $D, #I (I is the second word)
void Interpreter::lri(const UDSPInstruction opc)
{
  const u16 imm = Regs().FetchInstruction();
  LoadRegister(opc & 0x1f, imm);
}

// LRIS $(0x18+D), #I (a sign-extended byte)
void Interpreter::lris(const UDSPInstruction opc)
{
  LoadRegister(DSP_REG_AXL0 + ((opc >> 8) & 7), static_cast<u16>(static_cast<s8>(opc & 0xff)));
}

// NX: nothing but the extension.
void Interpreter::nx(const UDSPInstruction)
{
  ZeroWriteBackLog();
}

// DAR / IAR $arD: step by one inside the ring
void Interpreter::dar(const UDSPInstruction opc)
{
  const int ar = opc & 3;
  Regs().r.ar[ar] = AddressMinusOne(ar);
}

void Interpreter::iar(const UDSPInstruction opc)
{
  const int ar = opc & 3;
  Regs().r.ar[ar] = AddressPlusOne(ar);
}

// SUBARN $arD: minus $ixD
void Interpreter::subarn(const UDSPInstruction opc)
{
  const int ar = opc & 3;
  Regs().r.ar[ar] = AddressMinus(ar, static_cast<s16>(Regs().r.ix[ar]));
}

// ADDARN $arD, $ixS
void Interpreter::addarn(const UDSPInstruction opc)
{
  const int ar = opc & 3;
  const int ix = (opc >> 2) & 3;
  Regs().r.ar[ar] = AddressPlus(ar, static_cast<s16>(Regs().r.ix[ix]));
}

// SBCLR / SBSET #I: clear or set $sr bit 6 + I
void Interpreter::sbclr(const UDSPInstruction opc)
{
  const u16 bit = static_cast<u16>(1U << (6 + (opc & 7)));
  Regs().r.sr &= static_cast<u16>(~bit);
}

void Interpreter::sbset(const UDSPInstruction opc)
{
  const u16 bit = static_cast<u16>(1U << (6 + (opc & 7)));
  Regs().r.sr |= bit;
}

// M2 / M0 / CLR15 / SET15 / SET16 / SET40: bits 8-10 select a mode bit (from
// 2 upwards, in pairs) and whether it is cleared (even) or set (odd).
void Interpreter::srbith(const UDSPInstruction opc)
{
  ZeroWriteBackLog();

  const int selector = (opc >> 8) & 7;
  u16 mode_bit;
  switch (selector >> 1)
  {
  case 1:
    mode_bit = SR_MUL_MODIFY;
    break;
  case 2:
    mode_bit = SR_MUL_UNSIGNED;
    break;
  case 3:
    mode_bit = SR_40_MODE_BIT;
    break;
  default:
    return;
  }

  u16& sr = Regs().r.sr;
  if ((selector & 1) != 0)
    sr |= mode_bit;
  else
    sr &= static_cast<u16>(~mode_bit);
}
}  // namespace DSP::Interpreter
