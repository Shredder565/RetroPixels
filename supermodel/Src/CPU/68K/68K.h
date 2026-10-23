// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Glue between the emulator and the Musashi 68000 core. Musashi keeps one
// global CPU, so boards that own a 68K (sound board, DSB, net board) swap
// their own state in and out with M68KSetContext() / M68KGetContext() around
// each slice of execution. Everything below acts on whichever context is
// currently loaded.

#ifndef INCLUDED_68K_H
#define INCLUDED_68K_H

#include <cstring>
#include "Types.h"
#include "Musashi/m68k.h"
#include "Musashi/m68kctx.h"
#include "CPU/Bus.h"
#include "BlockFile.h"

// Musashi's memory hooks are plain C functions; no calling-convention tag.
#undef FASTCALL
#define FASTCALL

// Values an interrupt-acknowledge callback may return besides a vector number.
#define M68K_IRQ_AUTOVECTOR  M68K_INT_ACK_AUTOVECTOR
#define M68K_IRQ_SPURIOUS    M68K_INT_ACK_SPURIOUS

// Everything one 68K needs to be parked while another one runs.
typedef struct SM68KCtx
{
  m68ki_cpu_core musashiCtx;   // the core's own registers and run state
  IBus *Bus;                   // where its memory accesses go
  int (*IRQAck)(int);          // interrupt-acknowledge hook, or null

  SM68KCtx()
    : Bus(nullptr), IRQAck(nullptr)
  {
    // A fresh context must start fully zeroed: Musashi reads fields of it
    // during reset that it never initialises itself.
    std::memset(&musashiCtx, 0, sizeof(musashiCtx));
  }
} M68KCtx;

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

// Wiring for the loaded context.
extern void M68KSetIRQCallback(int (*F)(int nIRQ));
extern void M68KAttachBus(IBus *BusPtr);

// Called once at start-up, before any context is used.
extern Result M68KInit(void);

// Swapping the loaded CPU with a parked one.
extern void M68KGetContext(M68KCtx *Dest);
extern void M68KSetContext(M68KCtx *Src);

// Entry points Musashi calls directly (see Musashi/m68kconf.h).
extern "C" {
extern int M68KIRQCallback(int nIRQ);
unsigned int FASTCALL M68KFetch8(unsigned int a);
unsigned int FASTCALL M68KFetch16(unsigned int a);
unsigned int FASTCALL M68KFetch32(unsigned int a);
unsigned int FASTCALL M68KRead8(unsigned int a);
unsigned int FASTCALL M68KRead16(unsigned int a);
unsigned int FASTCALL M68KRead32(unsigned int a);
void FASTCALL M68KWrite8(unsigned int a, unsigned int d);
void FASTCALL M68KWrite16(unsigned int a, unsigned int d);
void FASTCALL M68KWrite32(unsigned int a, unsigned int d);
}

#endif  // INCLUDED_68K_H
