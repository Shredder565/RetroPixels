// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The network board of builds made without networking (SUPERMODEL_NO_NET,
// RetroPixels for Android): never attached, so the main board leaves it alone
// and linked play is simply not offered. Reads give 0 and writes are dropped,
// as with an empty slot.

#ifndef INCLUDED_NONETBOARD_H
#define INCLUDED_NONETBOARD_H

#include "Network/INetBoard.h"

class CNoNetBoard : public INetBoard
{
public:
  void GetGame(const Game &) override {}
  Result Init(UINT8 *, UINT8 *) override { return Result::OKAY; }
  void Reset(void) override {}
  void RunFrame(void) override {}
  bool IsAttached(void) override { return false; }
  bool IsRunning(void) override { return false; }
  UINT8 ReadCommRAM8(unsigned) override { return 0; }
  UINT16 ReadCommRAM16(unsigned) override { return 0; }
  UINT32 ReadCommRAM32(unsigned) override { return 0; }
  void WriteCommRAM8(unsigned, UINT8) override {}
  void WriteCommRAM16(unsigned, UINT16) override {}
  void WriteCommRAM32(unsigned, UINT32) override {}
  UINT16 ReadIORegister(unsigned) override { return 0; }
  void WriteIORegister(unsigned, UINT16) override {}
  void SaveState(CBlockFile *) override {}
  void LoadState(CBlockFile *) override {}
};

#endif // INCLUDED_NONETBOARD_H
