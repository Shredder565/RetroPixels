// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// SCRIPTS behaviour games depend on:
//  - Writing the top byte of DSP restarts a halted processor and, unless
//    manual start mode is on (DMODE bit 0), starts executing at DSP.
//  - DCNTL bit 2 (STD) starts execution; with bit 4 (SSM, single step) also
//    set exactly one instruction runs, followed by a single-step interrupt.
//    Virtua Fighter 3 single-steps right after an automatic start, so a
//    single step also runs when the processor is halted.
//  - The interrupt instruction leaves DSP pointing at itself.
//  - Reading DSTAT acknowledges the interrupt.
// Automatic execution is capped at 100 instructions per start.

#include "53C810.h"

#include <cstring>
#include "Supermodel.h"

namespace
{
  // Opcode groups (DCMD bits 7-5 / 7-3).
  bool IsMemoryMove(UINT8 dcmd) { return (dcmd & 0xE0) == 0xC0; }
  bool IsInterrupt(UINT8 dcmd) { return (dcmd & 0xF8) == 0x98; }

  // Byte lane n (0 = bits 7-0) of a 32-bit register.
  UINT8 Lane(UINT32 r, unsigned n) { return UINT8(r >> (8 * n)); }
  void SetLane(UINT32 &r, unsigned n, UINT8 v)
  {
    r = (r & ~(UINT32(0xFF) << (8 * n))) | (UINT32(v) << (8 * n));
  }

  // DSTAT / DIEN bits
  const UINT8 kSIR = 0x04;    // SCRIPTS interrupt instruction
  const UINT8 kSSI = 0x08;    // single step interrupt
  // ISTAT bit
  const UINT8 kDIP = 0x01;    // DMA interrupt pending
  // DBC bit of the interrupt instruction that makes it INTFLY
  const UINT32 kIntOnTheFly = 0x100000;
}

void C53C810::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("53C810", __FILE__);
  SaveState->Write(m_regs, sizeof(m_regs));
  SaveState->Write(&m_TEMP, sizeof(m_TEMP));
  SaveState->Write(&m_DSP, sizeof(m_DSP));
  SaveState->Write(&m_DSPS, sizeof(m_DSPS));
  SaveState->Write(&m_DBC, sizeof(m_DBC));
  UINT8 bytes[6] = { m_DCMD, m_DCNTL, m_DMODE, m_DSTAT, m_DIEN, m_ISTAT };
  SaveState->Write(bytes, sizeof(bytes));
}

void C53C810::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("53C810") != Result::OKAY)
  {
    ErrorLog("Unable to load 53C810 state. Save state file is corrupt.");
    return;
  }
  SaveState->Read(m_regs, sizeof(m_regs));
  SaveState->Read(&m_TEMP, sizeof(m_TEMP));
  SaveState->Read(&m_DSP, sizeof(m_DSP));
  SaveState->Read(&m_DSPS, sizeof(m_DSPS));
  SaveState->Read(&m_DBC, sizeof(m_DBC));
  UINT8 bytes[6] = { m_DCMD, m_DCNTL, m_DMODE, m_DSTAT, m_DIEN, m_ISTAT };
  SaveState->Read(bytes, sizeof(bytes));
  m_DCMD = bytes[0];
  m_DCNTL = bytes[1];
  m_DMODE = bytes[2];
  m_DSTAT = bytes[3];
  m_DIEN = bytes[4];
  m_ISTAT = bytes[5];
}

// SCRIPTS words are little endian in memory; the bus returns big endian.
UINT32 C53C810::FetchWord(unsigned offset) const
{
  return FLIPENDIAN32(m_bus->Read32(m_DSP + offset));
}

void C53C810::FetchInstruction(void)
{
  UINT32 first = FetchWord(0);
  m_DCMD = UINT8(first >> 24);
  m_DBC = first & 0x00FFFFFF;
  m_DSPS = FetchWord(4);
}

C53C810::Step C53C810::MemoryMove(void)
{
  // Memory Move: DBC bytes from DSPS to the address in the third word.
  UINT32 from = m_DSPS;
  UINT32 to = FetchWord(8);
  UINT32 count = m_DBC;

  for (UINT32 words = count >> 2; words != 0; words--)
  {
    m_bus->Write32(to, m_bus->Read32(from));
    from += 4;
    to += 4;
  }
  for (UINT32 rest = count & 3; rest != 0; rest--)
    m_bus->Write8(to++, m_bus->Read8(from++));

  m_DBC = 0;
  m_DSPS = from;
  m_TEMP = to;
  m_DSP += 12;
  return Step::Continue;
}

C53C810::Step C53C810::Interrupt(void)
{
  m_halted = true;
  m_ISTAT |= kDIP;
  m_DSTAT |= kSIR;
  if (m_DIEN & kSIR)
    m_irq->Assert(m_irqBit);
  if (m_DBC & kIntOnTheFly)
  {
    ErrorLog("53C810 INTFLY instruction not emulated!");
    return Step::Stop;
  }
  return Step::Continue;
}

C53C810::Step C53C810::Execute(void)
{
  if (IsMemoryMove(m_DCMD))
    return MemoryMove();
  if (IsInterrupt(m_DCMD))
    return Interrupt();
  DebugLog("53C810: unemulated SCRIPTS instruction %02X%06X at %08X\n", m_DCMD, m_DBC, m_DSP);
  return Step::Stop;
}

void C53C810::RunScripts(void)
{
  for (int n = 0; n < 100 && !m_halted; n++)
  {
    FetchInstruction();
    if (Execute() != Step::Continue)
      break;
  }
}

void C53C810::SingleStep(void)
{
  FetchInstruction();
  Execute();
  m_ISTAT |= kDIP;
  m_DSTAT |= kSSI;
  if (m_DIEN & kSSI)
    m_irq->Assert(m_irqBit);
}

void C53C810::WriteRegister(unsigned reg, UINT8 data)
{
  if (reg >= 0x60)
  {
    ErrorLog("Write to invalid 53C810 register (%02X).", reg);
    return;
  }

  m_regs[reg] = data;

  switch (reg)
  {
  case 0x14: m_ISTAT = data; break;
  case 0x1C: case 0x1D: case 0x1E: case 0x1F: SetLane(m_TEMP, reg - 0x1C, data); break;
  case 0x24: case 0x25: case 0x26: SetLane(m_DBC, reg - 0x24, data); break;
  case 0x27: m_DCMD = data; break;
  case 0x2C: case 0x2D: case 0x2E: SetLane(m_DSP, reg - 0x2C, data); break;
  case 0x2F:
    SetLane(m_DSP, 3, data);
    m_halted = false;
    if ((m_DMODE & 0x01) == 0)    // automatic start
      RunScripts();
    break;
  case 0x30: case 0x31: case 0x32: case 0x33: SetLane(m_DSPS, reg - 0x30, data); break;
  case 0x38: m_DMODE = data; break;
  case 0x39: m_DIEN = data; break;
  case 0x3B:
    m_DCNTL = data;
    if ((data & 0x14) == 0x14)    // SSM + STD
      SingleStep();
    else if (data & 0x04)         // STD
      RunScripts();
    break;
  default:
    break;
  }
}

UINT8 C53C810::ReadRegister(unsigned reg)
{
  if (reg >= 0x60)
  {
    ErrorLog("Read from invalid 53C810 register (%02X).", reg);
    return 0;
  }

  switch (reg)
  {
  case 0x0C:
  {
    // DSTAT: reading it acknowledges the DMA interrupt.
    UINT8 status = m_DSTAT;
    m_ISTAT &= UINT8(~kDIP);
    m_DSTAT &= UINT8(~kSSI);
    m_irq->Deassert(m_irqBit);
    return status;
  }
  case 0x14: return m_ISTAT;
  case 0x1C: case 0x1D: case 0x1E: case 0x1F: return Lane(m_TEMP, reg - 0x1C);
  case 0x24: case 0x25: case 0x26: return Lane(m_DBC, reg - 0x24);
  case 0x27: return m_DCMD;
  case 0x2C: case 0x2D: case 0x2E: case 0x2F: return Lane(m_DSP, reg - 0x2C);
  case 0x30: case 0x31: case 0x32: case 0x33: return Lane(m_DSPS, reg - 0x30);
  case 0x38: return m_DMODE;
  case 0x39: return m_DIEN;
  case 0x3B: return m_DCNTL;
  default: return m_regs[reg];
  }
}

UINT32 C53C810::ReadPCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
  (void)device;
  if (bits == 8 || reg != 0x00)
    return 0;

  // Device/vendor ID 0x0001/0x1000, as the big endian bus presents it.
  const UINT32 id = FLIPENDIAN32(0x00011000);
  if (bits == 16)
    return (id >> ((2 - offset) * 8)) & 0xFFFF;
  return id;
}

void C53C810::WritePCIConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data)
{
  (void)device;
  (void)bits;
  (void)offset;
  if (reg == 4)
    m_baseAddress = UINT8(data);
}

UINT8 C53C810::GetBaseAddress(void) const
{
  return m_baseAddress;
}

void C53C810::Reset(void)
{
  // Register values after a chip reset (53C810 data manual).
  std::memset(m_regs, 0, sizeof(m_regs));
  m_regs[0x00] = 0xC0;  // SCNTL0
  m_regs[0x0C] = 0x80;  // DSTAT: DMA FIFO empty
  m_regs[0x0F] = 0x02;  // SSTAT2
  m_regs[0x18] = 0xFF;
  m_regs[0x19] = 0xF0;  // CTEST1
  m_regs[0x1A] = 0x01;  // CTEST2
  m_regs[0x46] = 0x60;  // MACNTL
  m_regs[0x47] = 0x0F;  // GPCNTL
  m_regs[0x4C] = 0x03;  // STEST0

  m_TEMP = 0;
  m_DSP = 0;
  m_DSPS = 0;
  m_DBC = 0;
  m_DCMD = 0;
  m_DCNTL = 0;
  m_DMODE = 0;
  m_DSTAT = 0x80;
  m_DIEN = 0;
  m_ISTAT = 0;
  m_halted = false;
}

void C53C810::Init(IBus *BusObjectPtr, CIRQ *IRQObjectPtr, unsigned scsiIRQBit)
{
  m_bus = BusObjectPtr;
  m_irq = IRQObjectPtr;
  m_irqBit = scsiIRQBit;
}

C53C810::C53C810(void)
{
  std::memset(m_regs, 0, sizeof(m_regs));
}

C53C810::~C53C810(void)
{
}
