// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// See 68K.h. One M68000 does the work; a context is its state plus the bus
// and acknowledge hook, copied in and out around each board's slice.

#include "68K.h"
#include "Supermodel.h"

namespace
{
  M68000 s_cpu;                       // the loaded context runs here
  int (*s_onAck)(int) = nullptr;      // acknowledge hook of the loaded context

  int Acknowledge(void *, int level)
  {
    if (s_onAck != nullptr)
      return s_onAck(level);
    // Nobody to ask: autovectored, and the request is dropped.
    s_cpu.SetIRQ(0);
    return M68K_IRQ_AUTOVECTOR;
  }

  // Save-state block: 34 words. 0: requested interrupt level << 8, 1: 0,
  // 2: 1 while stopped, 3-10: D0-D7, 11-18: A0-A7, 19: PC, 20: SR, 21: the
  // active stack pointer, 22: USP, 23: SSP, 24-31: 0 (registers of later
  // 680x0s), 32: address of the last instruction, 33: its opcode. The
  // layout is what existing save states hold; don't change it.
  constexpr size_t kStateWords = 34;

  int Clamp(int n)
  {
    return (n >= 0 && n < 8) ? n : 7;
  }
}

UINT32 M68KGetARegister(int n)
{
  return s_cpu.st.a[Clamp(n)];
}

UINT32 M68KGetDRegister(int n)
{
  return s_cpu.st.d[Clamp(n)];
}

void M68KSetARegister(int n, UINT32 v)
{
  s_cpu.st.a[Clamp(n)] = v;
}

void M68KSetDRegister(int n, UINT32 v)
{
  s_cpu.st.d[Clamp(n)] = v;
}

UINT32 M68KGetPC(void)
{
  return s_cpu.st.pc;
}

void M68KSaveState(CBlockFile *StateFile, const char *name)
{
  const M68000State &st = s_cpu.st;
  UINT32 words[kStateWords] = {};
  words[0] = UINT32(st.irq) << 8;
  words[2] = st.stopped ? 1 : 0;
  for (int i = 0; i < 8; i++)
  {
    words[3 + i] = st.d[i];
    words[11 + i] = st.a[i];
  }
  words[19] = st.pc;
  words[20] = s_cpu.SR();
  words[21] = st.a[7];
  words[22] = st.s ? st.usp : st.a[7];
  words[23] = st.s ? st.a[7] : st.ssp;
  words[32] = st.opPC;
  words[33] = st.op;

  StateFile->NewBlock(name, __FILE__);
  StateFile->Write(words, sizeof(words));
}

void M68KLoadState(CBlockFile *StateFile, const char *name)
{
  if (StateFile->FindBlock(name) != Result::OKAY)
  {
    ErrorLog("68K state block '%s' is missing from the save state.", name);
    return;
  }

  UINT32 words[kStateWords];
  StateFile->Read(words, sizeof(words));

  M68000State &st = s_cpu.st;
  st.irq = int((words[0] >> 8) & 7);
  st.nmiSeen = st.irq == 7;
  st.stopped = (words[2] & 1) != 0;
  for (int i = 0; i < 8; i++)
  {
    st.d[i] = words[3 + i];
    st.a[i] = words[11 + i];
  }
  st.pc = words[19];
  const UINT32 sr = words[20];
  st.t = (sr >> 15) & 1;
  st.s = (sr >> 13) & 1;
  st.ipl = (sr >> 8) & 7;
  st.x = (sr >> 4) & 1;
  st.n = (sr >> 3) & 1;
  st.z = (sr >> 2) & 1;
  st.v = (sr >> 1) & 1;
  st.c = sr & 1;
  st.a[7] = words[21];
  if (st.s)
    st.usp = words[22];
  else
    st.ssp = words[23];
  st.opPC = words[32];
  st.op = words[33] & 0xFFFF;
}

void M68KSetIRQ(int irqLevel)
{
  s_cpu.SetIRQ(irqLevel & 7);
}

int M68KRun(int numCycles)
{
  return int(s_cpu.Run(numCycles));
}

void M68KReset(void)
{
  s_cpu.Reset();
  DebugLog("68K reset\n");
}

void M68KSetIRQCallback(int (*F)(int nIRQ))
{
  s_onAck = F;
}

void M68KAttachBus(IBus *BusPtr)
{
  s_cpu.bus = BusPtr;
  DebugLog("Attached bus to 68K\n");
}

void M68KGetContext(M68KCtx *Dest)
{
  Dest->cpu = s_cpu.st;
  Dest->Bus = s_cpu.bus;
  Dest->IRQAck = s_onAck;
}

void M68KSetContext(M68KCtx *Src)
{
  s_cpu.st = Src->cpu;
  s_cpu.bus = Src->Bus;
  s_onAck = Src->IRQAck;
}

Result M68KInit(void)
{
  s_cpu.st = M68000State();
  s_cpu.ack = Acknowledge;
  s_cpu.bus = nullptr;
  s_onAck = nullptr;
  DebugLog("Initialized 68K\n");
  return Result::OKAY;
}
