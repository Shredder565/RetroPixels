// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The Model 3 sound board's pair of Yamaha SCSP (YMF292-F) sound chips; see
// SCSP.cpp. The sound board's 68000 reaches them through the Master/Slave
// accessors (offsets within each chip's 64 KB register window) and runs
// from SCSP_Update(), which renders one buffer of 44.1 kHz samples.

#ifndef INCLUDED_SCSP_H
#define INCLUDED_SCSP_H

#include "BlockFile.h"
#include "Types.h"
#include "Util/NewConfig.h"

#define MAX_SCSP 2

// Set the 68000 callbacks (run for a number of cycles, returning how many
// cycles it ran past them; set the interrupt level) and the output buffers
// (front left/right from the master chip, rear from the slave) before
// SCSP_Init(). Always use n = 2.
void SCSP_SetCB(int (*Run68k)(int cycles), void (*Int68k)(int irq));
void SCSP_SetBuffers(float *leftBufferPtr, float *rightBufferPtr, float *leftRearBufferPtr, float *rightRearBufferPtr, int bufferLength);
Result SCSP_Init(const Util::Config::Node &config, int n);
void SCSP_SetRAM(int n, UINT8 *r);
void SCSP_Deinit(void);

// Renders one buffer, running the 68000 along with it.
void SCSP_Update();

// MIDI port (the PowerPC's command channel).
void SCSP_MidiIn(UINT8);
void SCSP_MidiOutW(UINT8);
UINT8 SCSP_MidiOutR();
UINT8 SCSP_MidiOutFill();
UINT8 SCSP_MidiInFill();

// Register access on the chip addressed last ...
void SCSP_w8(UINT32 addr, UINT8 val);
void SCSP_w16(UINT32 addr, UINT16 val);
void SCSP_w32(UINT32 addr, UINT32 val);
UINT8 SCSP_r8(UINT32 addr);
UINT16 SCSP_r16(UINT32 addr);
UINT32 SCSP_r32(UINT32 addr);

// ... and on a given chip.
void SCSP_Master_w8(UINT32 addr, UINT8 val);
void SCSP_Master_w16(UINT32 addr, UINT16 val);
void SCSP_Master_w32(UINT32 addr, UINT32 val);
void SCSP_Slave_w8(UINT32 addr, UINT8 val);
void SCSP_Slave_w16(UINT32 addr, UINT16 val);
void SCSP_Slave_w32(UINT32 addr, UINT32 val);
UINT8 SCSP_Master_r8(UINT32 addr);
UINT16 SCSP_Master_r16(UINT32 addr);
UINT32 SCSP_Master_r32(UINT32 addr);
UINT8 SCSP_Slave_r8(UINT32 addr);
UINT16 SCSP_Slave_r16(UINT32 addr);
UINT32 SCSP_Slave_r32(UINT32 addr);

// Nothing to do on Model 3 (kept for callers).
void SCSP_CpuRunScanline();
void SCSP_RTECheck();

// Save states (block "SCSP x 2").
void SCSP_SaveState(CBlockFile *StateFile);
void SCSP_LoadState(CBlockFile *StateFile);

#endif // INCLUDED_SCSP_H
