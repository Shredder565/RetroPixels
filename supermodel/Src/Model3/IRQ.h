// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3's interrupt controller, which drives the PowerPC's external
// interrupt line. Sources raise bits in a pending word; bits 0-7 are
// maskable through the enable register at F0100014 and readable at
// F0100018 (bit 1 VBlank, bit 6 MIDI/SCSP, ...). Bits above 7 are used for
// sources that always interrupt (the SCSI controller and Real3D DMA use
// 0x100).

#ifndef INCLUDED_IRQ_H
#define INCLUDED_IRQ_H

#include "BlockFile.h"
#include "Types.h"

class CIRQ
{
public:
  // Save state block "IRQ": enable word, pending word (32 bits each).
  void SaveState(CBlockFile *SaveState);
  void LoadState(CBlockFile *SaveState);

  // Raise or clear pending bits. Raising sets the CPU's line if an enabled
  // or unmaskable bit is pending; clearing drops it when none is left.
  void Assert(unsigned irqBits);
  void Deassert(unsigned irqBits);

  // The enable register. Writing it does not change the CPU line by itself.
  UINT8 ReadIRQEnable(void) const;
  void WriteIRQEnable(UINT8 data);

  // The low 8 pending bits.
  UINT8 ReadIRQState(void) const;

  void Reset(void);
  void Init(void);

  CIRQ(void);
  ~CIRQ(void);

private:
  static const unsigned kUnmaskable = ~0xFFu;

  unsigned m_enabled = 0;
  unsigned m_pending = 0;
};

#endif // INCLUDED_IRQ_H
