// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Checks the M68K* layer the boards use (Src/CPU/68K/68K.cpp): two 68000s
// sharing the one core through M68KGetContext/M68KSetContext run exactly
// as each does alone; interrupts without an acknowledge hook are taken once
// and dropped; an acknowledge hook's vector is used; and a save state
// taken half way and loaded into a fresh context finishes the same way.
//
//   make -C supermodel/tests/m68k     (builds and runs it; see Makefile)

#include "CPU/68K/68K.h"
#include "BlockFile.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The emulator's log functions (OSD/Logger.cpp), here only printing errors.
void DebugLog(const char *, ...) {}
void InfoLog(const char *, ...) {}
Result ErrorLog(const char *fmt, ...)
{
  va_list vl;
  va_start(vl, fmt);
  vfprintf(stderr, fmt, vl);
  va_end(vl);
  fputc('\n', stderr);
  return Result::FAIL;
}

namespace
{
  int g_failures = 0;

  void Check(bool ok, const char *what)
  {
    if (!ok)
    {
      printf("FAIL: %s\n", what);
      g_failures++;
    }
  }

  // 64 KB of big-endian RAM, mirrored over the 24-bit space.
  struct Ram : IBus
  {
    std::vector<uint8_t> m = std::vector<uint8_t>(0x10000);
    UINT8 Read8(UINT32 a) override { return m[a & 0xFFFF]; }
    UINT16 Read16(UINT32 a) override { return UINT16((m[a & 0xFFFF] << 8) | m[(a + 1) & 0xFFFF]); }
    UINT32 Read32(UINT32 a) override { return (UINT32(Read16(a)) << 16) | Read16(a + 2); }
    void Write8(UINT32 a, UINT8 d) override { m[a & 0xFFFF] = d; }
    void Write16(UINT32 a, UINT16 d) override { Write8(a, UINT8(d >> 8)); Write8(a + 1, UINT8(d)); }
    void Write32(UINT32 a, UINT32 d) override { Write16(a, UINT16(d >> 16)); Write16(a + 2, UINT16(d)); }

    void Long(uint32_t a, uint32_t v) { Write32(a, v); }
    void Code(uint32_t a, std::initializer_list<uint16_t> words)
    {
      for (uint16_t w : words)
      {
        Write16(a, w);
        a += 2;
      }
    }
  };

  // A program: reset vectors, a main loop at 0x400 and handlers for
  // autovectored level 2 (0x500) and vector 0x40 (0x600).
  void Program(Ram &ram, uint16_t step, uint16_t mix)
  {
    ram.Long(0, 0x8000);
    ram.Long(4, 0x400);
    ram.Long(26 * 4, 0x500);
    ram.Long(0x40 * 4, 0x600);
    ram.Code(0x400, {
      0x46FC, 0x2000,   // MOVE.W #$2000,SR (interrupts on)
      step,             // ADDQ.L #n,D0
      mix,              // a register op mixing D0 into D1
      0x2F01,           // MOVE.L D1,-(A7)
      0x221F,           // MOVE.L (A7)+,D1
      0x60F6,           // BRA.S back to the ADDQ
    });
    ram.Code(0x500, { 0x5287, 0x4E73 });   // ADDQ.L #1,D7; RTE
    ram.Code(0x600, { 0x5486, 0x4E73 });   // ADDQ.L #2,D6; RTE
  }

  struct Regs
  {
    UINT32 d[8], a[8], pc;
    bool operator==(const Regs &o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
  };

  Regs Snapshot()
  {
    Regs r;
    for (int i = 0; i < 8; i++)
    {
      r.d[i] = M68KGetDRegister(i);
      r.a[i] = M68KGetARegister(i);
    }
    r.pc = M68KGetPC();
    return r;
  }

  // The same slices and interrupt requests for every run.
  struct Slice
  {
    int cycles, irq;
  };

  std::vector<Slice> Schedule(unsigned seed)
  {
    std::vector<Slice> s;
    for (int i = 0; i < 400; i++)
    {
      seed = seed * 1103515245u + 12345u;
      s.push_back({ int(50 + (seed >> 16) % 900), ((seed >> 8) % 5 == 0) ? 2 : 0 });
    }
    return s;
  }

  int g_acks = 0;
  int VectorAck(int level)
  {
    g_acks++;
    M68KSetIRQ(0);
    return level == 2 ? 0x40 : M68K_IRQ_AUTOVECTOR;
  }

  void Setup(M68KCtx *ctx, Ram *ram, int (*ack)(int))
  {
    M68KSetContext(ctx);
    M68KInit();
    M68KAttachBus(ram);
    M68KSetIRQCallback(ack);
    M68KReset();
    M68KGetContext(ctx);
  }

  void RunSlice(M68KCtx *ctx, const Slice &s, int *total)
  {
    M68KSetContext(ctx);
    if (s.irq)
      M68KSetIRQ(s.irq);
    int ran = M68KRun(s.cycles);
    Check(ran >= s.cycles && ran < s.cycles + 200, "M68KRun runs at least what was asked, finishing an instruction");
    *total += ran;
    M68KGetContext(ctx);
  }
}

int main()
{
  const std::vector<Slice> sa = Schedule(1), sb = Schedule(2);

  // Each CPU alone.
  Regs aloneA, aloneB;
  {
    Ram ram;
    Program(ram, 0x5280, 0xD280);   // ADDQ.L #1,D0; ADD.L D0,D1
    M68KCtx ctx;
    Setup(&ctx, &ram, nullptr);
    int total = 0;
    for (const Slice &s : sa)
      RunSlice(&ctx, s, &total);
    M68KSetContext(&ctx);
    aloneA = Snapshot();
    Check(aloneA.d[7] > 0, "autovectored interrupts were taken");
    Check(aloneA.d[0] > 1000, "the main loop ran");
  }
  {
    Ram ram;
    Program(ram, 0x5680, 0xB181);   // ADDQ.L #3,D0; EOR.L D0,D1
    M68KCtx ctx;
    Setup(&ctx, &ram, VectorAck);
    int total = 0;
    for (const Slice &s : sb)
      RunSlice(&ctx, s, &total);
    M68KSetContext(&ctx);
    aloneB = Snapshot();
    Check(aloneB.d[6] > 0 && aloneB.d[7] == 0, "the acknowledge hook's vector was used");
    Check(g_acks > 0, "the acknowledge hook was called");
  }

  // Interleaved, switching contexts every slice; A saved half way.
  Ram ramA, ramB, ramAtSave;
  Program(ramA, 0x5280, 0xD280);
  Program(ramB, 0x5680, 0xB181);
  M68KCtx a, b;
  Setup(&a, &ramA, nullptr);
  Setup(&b, &ramB, VectorAck);
  const std::string file = "m68k-wrapper-test.st";
  int totalA = 0, totalB = 0;
  for (size_t i = 0; i < sa.size(); i++)
  {
    RunSlice(&a, sa[i], &totalA);
    RunSlice(&b, sb[i], &totalB);
    if (i == sa.size() / 2)
    {
      CBlockFile f;
      Check(f.Create(file, "Test", "68K wrapper test") == Result::OKAY, "save state file created");
      M68KSetContext(&a);
      M68KSaveState(&f, "Test 68K");
      f.Close();
      ramAtSave = ramA;
    }
  }
  M68KSetContext(&a);
  Check(Snapshot() == aloneA, "CPU A interleaved with B runs as it does alone");
  M68KSetContext(&b);
  Check(Snapshot() == aloneB, "CPU B interleaved with A runs as it does alone");

  // From the save state, in a fresh context.
  M68KCtx c;
  Setup(&c, &ramAtSave, nullptr);
  {
    CBlockFile f;
    Check(f.Load(file) == Result::OKAY, "save state file loaded");
    M68KSetContext(&c);
    M68KLoadState(&f, "Test 68K");
    M68KGetContext(&c);
    f.Close();
  }
  int totalC = 0;
  for (size_t i = sa.size() / 2 + 1; i < sa.size(); i++)
    RunSlice(&c, sa[i], &totalC);
  M68KSetContext(&c);
  Check(Snapshot() == aloneA, "a state loaded half way finishes like the uninterrupted run");
  std::remove(file.c_str());

  // STOP: a stopped CPU uses up its time and wakes on an interrupt.
  {
    Ram ram;
    ram.Long(0, 0x8000);
    ram.Long(4, 0x400);
    ram.Long(25 * 4, 0x500);
    ram.Code(0x400, { 0x4E72, 0x2000, 0x5280, 0x60F8 });   // STOP #$2000; ADDQ.L #1,D0; BRA.S STOP
    ram.Code(0x500, { 0x5287, 0x4E73 });
    M68KCtx s;
    Setup(&s, &ram, nullptr);
    M68KSetContext(&s);
    M68KRun(100);
    Check(M68KRun(1000) == 1000, "a stopped CPU uses up the slice");
    Check(M68KGetDRegister(0) == 0, "nothing runs while stopped");
    M68KSetIRQ(1);
    M68KRun(200);
    Check(M68KGetDRegister(7) == 1 && M68KGetDRegister(0) == 1, "an interrupt wakes it once (the request is dropped on acknowledge)");
  }

  if (g_failures == 0)
    printf("68K wrapper: all checks passed (A ran %d cycles, B %d)\n", totalA, totalB);
  return g_failures ? 1 : 0;
}
