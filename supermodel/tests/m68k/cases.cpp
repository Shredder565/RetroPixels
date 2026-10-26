// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Runs RetroPixels' C++ 68000 (Src/CPU/68K/M68000.cpp) on the random
// one-instruction cases cores/m68k's tests make, so the Go test
// TestMatchesSupermodelCpp can compare it with the Go 68000.
//
//   cases <cases.bin>   writes each case's result to stdout
//
// A case (little-endian 32-bit words): seed, D0-D7, A0-A7, PC, USP, SSP,
// SR, irq, interrupt-acknowledge vector (0 = autovector), flags (1
// stopped, 2 NMI seen), steps, word count, words (placed at PC). Memory
// not written holds a pattern made from the seed and the address. A
// result: cycles, D0-D7, A0-A7, PC, USP, SSP, SR, flags, irq, access
// count, then three words per bus access (1 read8, 2 read16, 3 write8,
// 4 write16, 5 acknowledge, 6 RESET instruction; address; value). Long
// accesses are logged as two word accesses, high word first.

#include "CPU/68K/M68000.h"

#include <cstdio>
#include <unordered_map>
#include <vector>

namespace
{
  struct TestBus : IBus
  {
    uint32_t seed = 0;
    int ackVector = 0;
    std::unordered_map<uint32_t, uint8_t> mem;
    std::vector<uint32_t> log;

    uint8_t Pattern(uint32_t a) const
    {
      uint32_t h = a * 0x9E3779B1u + seed;
      return uint8_t((h >> 13) ^ (h >> 5) ^ (h >> 24));
    }
    uint8_t Byte(uint32_t a) const
    {
      auto it = mem.find(a);
      return it != mem.end() ? it->second : Pattern(a);
    }
    void Log(uint32_t kind, uint32_t a, uint32_t v)
    {
      log.push_back(kind);
      log.push_back(a);
      log.push_back(v);
    }

    UINT8 Read8(UINT32 a) override
    {
      uint8_t v = Byte(a);
      Log(1, a, v);
      return v;
    }
    UINT16 Read16(UINT32 a) override
    {
      uint16_t v = uint16_t((Byte(a) << 8) | Byte(a + 1));
      Log(2, a, v);
      return v;
    }
    UINT32 Read32(UINT32 a) override
    {
      uint32_t hi = Read16(a);
      return (hi << 16) | Read16((a + 2) & 0xFFFFFE);
    }
    void Write8(UINT32 a, UINT8 v) override
    {
      mem[a] = v;
      Log(3, a, v);
    }
    void Write16(UINT32 a, UINT16 v) override
    {
      mem[a] = uint8_t(v >> 8);
      mem[a + 1] = uint8_t(v);
      Log(4, a, v);
    }
    void Write32(UINT32 a, UINT32 v) override
    {
      Write16(a, UINT16(v >> 16));
      Write16((a + 2) & 0xFFFFFE, UINT16(v));
    }
  };

  int Ack(void *user, int level)
  {
    TestBus *bus = static_cast<TestBus *>(user);
    bus->Log(5, uint32_t(level), uint32_t(bus->ackVector));
    return bus->ackVector ? bus->ackVector : kM68KAutoVector;
  }

  void OnReset(void *user)
  {
    static_cast<TestBus *>(user)->Log(6, 0, 0);
  }

  bool Word(FILE *f, uint32_t *v)
  {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4)
      return false;
    *v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
  }

  void Put(std::vector<uint8_t> &out, uint32_t v)
  {
    for (int i = 0; i < 4; i++)
      out.push_back(uint8_t(v >> (8 * i)));
  }
}

int main(int argc, char **argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: cases <cases.bin>\n");
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f)
  {
    perror(argv[1]);
    return 1;
  }
  std::vector<uint8_t> out;
  uint32_t seed;
  while (Word(f, &seed))
  {
    uint32_t w[25];
    for (uint32_t &v : w)
    {
      if (!Word(f, &v))
        return 1;
    }
    TestBus bus;
    bus.seed = seed;
    bus.ackVector = int(w[21]);
    const uint32_t pc = w[16], count = w[24];
    for (uint32_t i = 0; i < count; i++)
    {
      uint32_t word;
      if (!Word(f, &word))
        return 1;
      uint32_t ad = (pc + i * 2) & 0xFFFFFF;
      bus.mem[ad] = uint8_t(word >> 8);
      bus.mem[(ad + 1) & 0xFFFFFF] = uint8_t(word);
    }

    M68000 cpu;
    cpu.bus = &bus;
    cpu.ack = Ack;
    cpu.onReset = OnReset;
    cpu.user = &bus;
    for (int i = 0; i < 8; i++)
    {
      cpu.st.d[i] = w[i];
      cpu.st.a[i] = w[8 + i];
    }
    cpu.st.pc = pc;
    cpu.st.usp = w[17];
    cpu.st.ssp = w[18];
    const uint32_t sr = w[19];
    cpu.st.s = (sr >> 13) & 1;
    cpu.st.t = (sr >> 15) & 1;
    cpu.st.ipl = (sr >> 8) & 7;
    cpu.st.x = (sr >> 4) & 1;
    cpu.st.n = (sr >> 3) & 1;
    cpu.st.z = (sr >> 2) & 1;
    cpu.st.v = (sr >> 1) & 1;
    cpu.st.c = sr & 1;
    cpu.st.irq = int(w[20]);
    cpu.st.stopped = (w[22] & 1) != 0;
    cpu.st.nmiSeen = (w[22] & 2) != 0;
    for (uint32_t i = 0; i < w[23]; i++)
      cpu.Step();

    Put(out, uint32_t(cpu.st.cycles));
    for (uint32_t v : cpu.st.d)
      Put(out, v);
    for (uint32_t v : cpu.st.a)
      Put(out, v);
    Put(out, cpu.st.pc);
    Put(out, cpu.st.usp);
    Put(out, cpu.st.ssp);
    Put(out, cpu.SR());
    Put(out, (cpu.st.stopped ? 1u : 0u) | (cpu.st.nmiSeen ? 2u : 0u));
    Put(out, uint32_t(cpu.st.irq));
    Put(out, uint32_t(bus.log.size() / 3));
    for (uint32_t v : bus.log)
      Put(out, v);
    if (out.size() > (1 << 20))
    {
      fwrite(out.data(), 1, out.size(), stdout);
      out.clear();
    }
  }
  fwrite(out.data(), 1, out.size(), stdout);
  fclose(f);
  return 0;
}
