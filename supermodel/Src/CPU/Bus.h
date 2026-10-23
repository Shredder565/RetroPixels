// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Bus.h: the interface every emulated CPU uses to reach memory and I/O.
//
// A board implements IBus and hands it to its CPU. Addresses are 32 bits;
// a CPU passes addresses aligned to the access size where it needs them to
// be. Which byte order the values use is up to the board. The defaults make
// an unconnected bus: reads float high, writes go nowhere.

#ifndef INCLUDED_BUS_H
#define INCLUDED_BUS_H

#include "Types.h"

class IBus
{
public:
  // Memory space
  virtual UINT8   Read8(UINT32 addr)  { (void) addr; return 0xFF; }
  virtual UINT16  Read16(UINT32 addr) { (void) addr; return 0xFFFF; }
  virtual UINT32  Read32(UINT32 addr) { (void) addr; return 0xFFFFFFFF; }
  virtual UINT64  Read64(UINT32 addr) { (void) addr; return 0xFFFFFFFFFFFFFFFFULL; }

  virtual void    Write8(UINT32 addr, UINT8 data)   { (void) addr; (void) data; }
  virtual void    Write16(UINT32 addr, UINT16 data) { (void) addr; (void) data; }
  virtual void    Write32(UINT32 addr, UINT32 data) { (void) addr; (void) data; }
  virtual void    Write64(UINT32 addr, UINT64 data) { (void) addr; (void) data; }

  // I/O space (8-bit ports, used by the Z80s)
  virtual UINT8   IORead8(UINT32 addr)              { (void) addr; return 0xFF; }
  virtual void    IOWrite8(UINT32 addr, UINT8 data) { (void) addr; (void) data; }
};

#endif  // INCLUDED_BUS_H
