// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Motorola MPC105 (Step 1.x) / MPC106 (Step 2.x) PCI bridge and memory
// controller, reduced to what the Model 3 uses: the CONFIG_ADDR/CONFIG_DATA
// pair for PCI configuration cycles, and the bridge's own 256-byte register
// file (PCI device 0).
//
// The register file is kept in the chip's own order: PCI configuration
// space is little endian, so a 16-bit vendor ID 0x1057 is stored as 57 10.

#ifndef INCLUDED_MPC10X_H
#define INCLUDED_MPC10X_H

#include "PCI.h"
#include "BlockFile.h"

class CMPC10x
{
public:
  // Save state block "MPC10x": the 256 registers, then the selected bus,
  // device, function and register (32 bits each).
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // CONFIG_ADDR as the PowerPC writes it (byte reversed: the bridge sees
  // a little endian word).
  void WritePCIConfigAddress(UINT32 data);

  // CONFIG_DATA. Device 0 is the bridge itself; others go to the PCI bus.
  UINT32 ReadPCIConfigData(unsigned bits, unsigned offset) const;
  void WritePCIConfigData(unsigned bits, unsigned offset, UINT32 data);

  // Direct byte access to the register file (MPC105 at F8FFF0xx).
  void WriteRegister(unsigned reg, UINT8 data);

  void Reset(void);
  void AttachPCIBus(CPCIBus *BusObjectPtr);

  // 0x105 or 0x106 (anything else is taken as 0x105).
  void SetModel(int modelNum);
  int GetModel() const;

  void Init(void);

  CMPC10x(void);
  ~CMPC10x(void);

private:
  UINT8 &Reg(unsigned index) { return m_regs[index & 0xFF]; }
  UINT8 Reg(unsigned index) const { return m_regs[index & 0xFF]; }
  void Put32LE(unsigned index, UINT32 value);

  int m_model = 0x105;
  CPCIBus *m_bus = nullptr;
  UINT8 m_regs[256] = {};
  // CONFIG_ADDR fields
  unsigned m_selBus = 0;
  unsigned m_selDevice = 0;
  unsigned m_selFunction = 0;
  unsigned m_selRegister = 0;
};

#endif // INCLUDED_MPC10X_H
