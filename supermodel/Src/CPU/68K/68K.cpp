// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// See 68K.h. Musashi holds the CPU registers itself; this file only keeps
// the two things Musashi has no slot for (the bus and the acknowledge hook)
// for the context that is currently loaded.

#include "68K.h"
#include "Supermodel.h"

namespace
{
  IBus *g_memory = nullptr;           // bus of the loaded context
  int (*g_onAck)(int) = nullptr;      // acknowledge hook of the loaded context

  // Save-state word order after the three run-state words. Changing this
  // breaks every existing save state.
  constexpr m68k_register_t kSavedRegs[] =
  {
    M68K_REG_D0, M68K_REG_D1, M68K_REG_D2, M68K_REG_D3,
    M68K_REG_D4, M68K_REG_D5, M68K_REG_D6, M68K_REG_D7,
    M68K_REG_A0, M68K_REG_A1, M68K_REG_A2, M68K_REG_A3,
    M68K_REG_A4, M68K_REG_A5, M68K_REG_A6, M68K_REG_A7,
    M68K_REG_PC, M68K_REG_SR, M68K_REG_SP, M68K_REG_USP,
    M68K_REG_ISP, M68K_REG_MSP, M68K_REG_SFC, M68K_REG_DFC,
    M68K_REG_VBR, M68K_REG_CACR, M68K_REG_CAAR, M68K_REG_PREF_ADDR,
    M68K_REG_PREF_DATA, M68K_REG_PPC, M68K_REG_IR,
  };
  constexpr size_t kRunWords = 3;     // int_level, int_cycles, stopped
  constexpr size_t kStateWords = kRunWords + sizeof(kSavedRegs) / sizeof(kSavedRegs[0]);
  static_assert(kStateWords == 34, "68K save-state block must stay 34 words");

  // Musashi numbers D0..D7 and A0..A7 consecutively.
  static_assert(M68K_REG_D7 == M68K_REG_D0 + 7 && M68K_REG_A0 == M68K_REG_D0 + 8 &&
                M68K_REG_A7 == M68K_REG_D0 + 15, "unexpected Musashi register numbering");

  m68k_register_t DataReg(int n)
  {
    return m68k_register_t(M68K_REG_D0 + ((n >= 0 && n < 8) ? n : 7));
  }

  m68k_register_t AddrReg(int n)
  {
    return m68k_register_t(M68K_REG_A0 + ((n >= 0 && n < 8) ? n : 7));
  }
}

UINT32 M68KGetARegister(int n)
{
  return m68k_get_reg(nullptr, AddrReg(n));
}

UINT32 M68KGetDRegister(int n)
{
  return m68k_get_reg(nullptr, DataReg(n));
}

void M68KSetARegister(int n, UINT32 v)
{
  m68k_set_reg(AddrReg(n), v);
}

void M68KSetDRegister(int n, UINT32 v)
{
  m68k_set_reg(DataReg(n), v);
}

UINT32 M68KGetPC(void)
{
  return m68k_get_reg(nullptr, M68K_REG_PC);
}

void M68KSaveState(CBlockFile *StateFile, const char *name)
{
  UINT32 words[kStateWords];

  m68ki_cpu_core core;
  m68k_get_context(&core);
  words[0] = core.int_level;
  words[1] = core.int_cycles;
  words[2] = core.stopped;
  for (size_t i = 0; i < kStateWords - kRunWords; i++)
    words[kRunWords + i] = m68k_get_reg(nullptr, kSavedRegs[i]);

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

  // The interrupt and stop state goes in first so that the register writes
  // below (SR in particular) see this CPU's pending interrupt level rather
  // than whatever the previously loaded context had.
  m68ki_cpu_core core;
  m68k_get_context(&core);
  core.int_level = words[0];
  core.int_cycles = words[1];
  core.stopped = words[2];
  m68k_set_context(&core);

  for (size_t i = 0; i < kStateWords - kRunWords; i++)
    m68k_set_reg(kSavedRegs[i], words[kRunWords + i]);
}

void M68KSetIRQ(int irqLevel)
{
  m68k_set_irq(irqLevel);
}

int M68KRun(int numCycles)
{
  return m68k_execute(numCycles);
}

void M68KReset(void)
{
  m68k_pulse_reset();
  DebugLog("68K reset\n");
}

void M68KSetIRQCallback(int (*F)(int nIRQ))
{
  g_onAck = F;
}

void M68KAttachBus(IBus *BusPtr)
{
  g_memory = BusPtr;
  DebugLog("Attached bus to 68K\n");
}

void M68KGetContext(M68KCtx *Dest)
{
  Dest->Bus = g_memory;
  Dest->IRQAck = g_onAck;
  m68k_get_context(&Dest->musashiCtx);
}

void M68KSetContext(M68KCtx *Src)
{
  g_memory = Src->Bus;
  g_onAck = Src->IRQAck;
  m68k_set_context(&Src->musashiCtx);
}

Result M68KInit(void)
{
  m68k_init();
  m68k_set_cpu_type(M68K_CPU_TYPE_68000);
  m68k_set_int_ack_callback(M68KIRQCallback);
  g_memory = nullptr;
  DebugLog("Initialized 68K\n");
  return Result::OKAY;
}

extern "C" {

int M68KIRQCallback(int nIRQ)
{
  if (g_onAck)
    return g_onAck(nIRQ);
  // Nobody to ask: treat it as autovectored and drop the line ourselves.
  m68k_set_irq(0);
  return M68K_IRQ_AUTOVECTOR;
}

// Instruction fetches and data accesses go to the same bus.
unsigned int FASTCALL M68KFetch8(unsigned int a)  { return g_memory->Read8(a); }
unsigned int FASTCALL M68KFetch16(unsigned int a) { return g_memory->Read16(a); }
unsigned int FASTCALL M68KFetch32(unsigned int a) { return g_memory->Read32(a); }
unsigned int FASTCALL M68KRead8(unsigned int a)   { return g_memory->Read8(a); }
unsigned int FASTCALL M68KRead16(unsigned int a)  { return g_memory->Read16(a); }
unsigned int FASTCALL M68KRead32(unsigned int a)  { return g_memory->Read32(a); }

void FASTCALL M68KWrite8(unsigned int a, unsigned int d)  { g_memory->Write8(a, d); }
void FASTCALL M68KWrite16(unsigned int a, unsigned int d) { g_memory->Write16(a, d); }
void FASTCALL M68KWrite32(unsigned int a, unsigned int d) { g_memory->Write32(a, d); }

}
