// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// NCR/LSI 53C810 SCSI I/O processor. The Model 3 has no SCSI devices; games
// use the chip's SCRIPTS processor as a DMA engine (Memory Move) to copy
// display lists and textures to Real3D, and its interrupt instruction to
// signal completion. Only those two instructions are emulated; anything
// else stops the processor.
//
// The registers sit at C0000000 (Step 1.0), C1000000 or F9000000 and are
// little endian: register 0x2C is DSP bits 7-0, and so on.

#ifndef INCLUDED_53C810_H
#define INCLUDED_53C810_H

#include "IRQ.h"
#include "PCI.h"
#include "BlockFile.h"
#include "CPU/Bus.h"

class C53C810: public IPCIDevice
{
public:
  // Save state block "53C810".
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // Register file access (reg 0x00-0x5F).
  UINT8 ReadRegister(unsigned reg);
  void WriteRegister(unsigned reg, UINT8 data);

  // PCI configuration space (vendor 0x1000, device 0x0001).
  UINT32 ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const;
  void WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data);

  // The base address byte the game programmed through PCI config register 4.
  UINT8 GetBaseAddress(void) const;

  void Reset(void);

  // Bus for SCRIPTS fetches and DMA, the IRQ controller and the bit to
  // raise in it.
  void Init(IBus *BusObjectPtr, CIRQ *IRQObjectPtr, unsigned scsiIRQBit);

  C53C810(void);
  ~C53C810(void);

private:
  enum class Step
  {
    Continue,   // instruction done, go on
    Stop        // halted or illegal instruction
  };

  UINT32 FetchWord(unsigned offset) const;
  void FetchInstruction(void);
  Step Execute(void);
  Step MemoryMove(void);
  Step Interrupt(void);
  void RunScripts(void);
  void SingleStep(void);

  UINT8 m_regs[0x60];   // plain register file for everything not below

  // Registers with behaviour (the copies in m_regs are not used for them)
  UINT32 m_TEMP = 0;
  UINT32 m_DSP = 0;     // SCRIPTS pointer
  UINT32 m_DSPS = 0;    // second instruction word / source address
  UINT32 m_DBC = 0;     // byte count (24 bits)
  UINT8 m_DCMD = 0;
  UINT8 m_DCNTL = 0;
  UINT8 m_DMODE = 0;
  UINT8 m_DSTAT = 0;
  UINT8 m_DIEN = 0;
  UINT8 m_ISTAT = 0;

  bool m_halted = false;      // stopped by an interrupt instruction

  IBus *m_bus = nullptr;
  CIRQ *m_irq = nullptr;
  unsigned m_irqBit = 0;

  UINT8 m_baseAddress = 0;
};

#endif // INCLUDED_53C810_H
