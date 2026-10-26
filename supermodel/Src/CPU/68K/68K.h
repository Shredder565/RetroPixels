// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The emulator's 68000s (sound board, DSB, net board) all run on one
// processor, RetroPixels' own core in M68000.h. Each board keeps its own
// CPU in an M68KCtx and loads it with M68KSetContext() before running it,
// then stores it back with M68KGetContext(). Everything below acts on
// whichever context is currently loaded.

#ifndef INCLUDED_68K_H
#define INCLUDED_68K_H

#include "Types.h"
#include "CPU/68K/M68000.h"
#include "CPU/Bus.h"
#include "BlockFile.h"

// Values an interrupt-acknowledge callback may return besides a vector number.
#define M68K_IRQ_AUTOVECTOR  kM68KAutoVector
#define M68K_IRQ_SPURIOUS    kM68KSpurious

// Everything one 68000 needs to be parked while another one runs.
struct M68KCtx
{
  M68000State cpu;             // registers and run state
  IBus *Bus = nullptr;         // where its memory accesses go
  int (*IRQAck)(int) = nullptr;  // interrupt-acknowledge hook, or null
};

// Register access on the loaded context. An index outside 0..7 reads
// register 7.
extern UINT32 M68KGetARegister(int n);
extern UINT32 M68KGetDRegister(int n);
extern void M68KSetARegister(int n, UINT32 v);
extern void M68KSetDRegister(int n, UINT32 v);
extern UINT32 M68KGetPC(void);

// Save states: one block holding 34 32-bit words.
extern void M68KSaveState(CBlockFile *StateFile, const char *name);
extern void M68KLoadState(CBlockFile *StateFile, const char *name);

// Running the loaded CPU.
extern void M68KSetIRQ(int irqLevel);     // drive the interrupt lines (0 = none)
extern int M68KRun(int numCycles);        // returns the cycles actually run
extern void M68KReset(void);

// Wiring for the loaded context. Without an acknowledge hook interrupts are
// autovectored and the request is dropped when one is taken.
extern void M68KSetIRQCallback(int (*F)(int nIRQ));
extern void M68KAttachBus(IBus *BusPtr);

// Called before a context is first used.
extern Result M68KInit(void);

// Swapping the loaded CPU with a parked one.
extern void M68KGetContext(M68KCtx *Dest);
extern void M68KSetContext(M68KCtx *Src);

#endif  // INCLUDED_68K_H
