// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// What the Model 3 board needs from a network board, whichever kind it is:
// the emulated 68000 board (CNetBoard) or the simulation of it that talks to
// the other machines directly (CSimNetBoard).

#ifndef INCLUDED_INETBOARD_H
#define INCLUDED_INETBOARD_H

#include "Types.h"
#include "Game.h"
#include "BlockFile.h"

class INetBoard
{
public:
  virtual ~INetBoard() = default;

  // Setup. GetGame comes first, so Init knows whether the game is linkable.
  virtual void GetGame(const Game &game) = 0;
  virtual Result Init(UINT8 *netRAMPtr, UINT8 *netBufferPtr) = 0;
  virtual void Reset(void) = 0;

  // Once a frame.
  virtual void RunFrame(void) = 0;

  virtual bool IsAttached(void) = 0;
  virtual bool IsRunning(void) = 0;

  // The shared RAM, as the main board sees it.
  virtual UINT8 ReadCommRAM8(unsigned addr) = 0;
  virtual UINT16 ReadCommRAM16(unsigned addr) = 0;
  virtual UINT32 ReadCommRAM32(unsigned addr) = 0;
  virtual void WriteCommRAM8(unsigned addr, UINT8 data) = 0;
  virtual void WriteCommRAM16(unsigned addr, UINT16 data) = 0;
  virtual void WriteCommRAM32(unsigned addr, UINT32 data) = 0;

  // The board's registers, as the main board sees them.
  virtual UINT16 ReadIORegister(unsigned reg) = 0;
  virtual void WriteIORegister(unsigned reg, UINT16 data) = 0;

  virtual void SaveState(CBlockFile *state) = 0;
  virtual void LoadState(CBlockFile *state) = 0;
};

#endif // INCLUDED_INETBOARD_H
