// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3's PCI bus as far as configuration space goes. The MPC105/106
// bridge (CMPC10x) forwards configuration cycles for device numbers other
// than its own to CPCIBus, which hands them to whichever device object was
// attached under that number (Real3D, the 53C810, and CModel3 itself for
// the board's other PCI IDs).

#ifndef INCLUDED_PCI_H
#define INCLUDED_PCI_H

#include "Types.h"

#include <vector>

class IPCIDevice
{
public:
  // A configuration space access.
  //   device  PCI device number (for objects that answer for several)
  //   reg     dword-aligned register offset
  //   bits    8, 16 or 32
  //   offset  byte offset of the access inside that dword (0-3, or 0/2
  //           for 16 bits)
  // Values are passed in big endian (PowerPC) order; little endian devices
  // swap them themselves.
  virtual UINT32 ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const = 0;
  virtual void WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data) = 0;
};

class CPCIBus
{
public:
  // Forward an access to the device attached under that number. Reads of
  // an empty slot return 0; writes to one are dropped.
  UINT32 ReadConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const;
  void WriteConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data);

  void Reset(void);

  // Puts a device on the bus. The first object attached under a number
  // answers for it.
  void AttachDevice(unsigned device, IPCIDevice *DeviceObjectPtr);

  // Empties the bus.
  void Init(void);

  CPCIBus(void);
  ~CPCIBus(void);

private:
  IPCIDevice *Find(unsigned device) const;

  struct Slot
  {
    unsigned number;
    IPCIDevice *dev;
  };
  std::vector<Slot> m_slots;
};

#endif // INCLUDED_PCI_H
