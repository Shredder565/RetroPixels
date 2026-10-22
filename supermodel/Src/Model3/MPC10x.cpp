// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Only big endian mode is emulated (PICR1 bit 5 set by a game is reported).
// Only bus 0 exists. Accesses to the bridge's own registers go byte by byte
// to the register file, most significant byte at the lowest offset as the
// PowerPC sees it.

#include "MPC10x.h"

#include <cstring>
#include "Supermodel.h"

namespace
{
  struct PowerOnValue
  {
    UINT8 reg;
    UINT32 value;
  };

  // Power-on register values (MPC106 user's manual, register summary).
  const PowerOnValue kMPC106Defaults[] =
  {
    { 0x04, 0x00800006 },   // PCI command / status
    { 0x08, 0x00060000 },   // class code, revision
    { 0x0C, 0x00000800 },   // cache line size, latency timer
    { 0x70, 0x00CD0000 },   // output driver control
    { 0xA8, 0x0010FF00 },   // PICR1
    { 0xAC, 0x060C000C },   // PICR2
    { 0xB8, 0x04000000 },
    { 0xC0, 0x00000100 },   // error enabling 1
    { 0xE0, 0x00420FFF },   // emulation support 1
    { 0xE8, 0x00200000 },   // emulation support 2
    { 0xF0, 0x0000FF02 },   // MCCR1
    { 0xF4, 0x00030000 },   // MCCR2
    { 0xFC, 0x00000010 },   // MCCR4
  };

  // The MPC105 subset.
  const PowerOnValue kMPC105Defaults[] =
  {
    { 0x04, 0x00800006 },
    { 0x08, 0x00060000 },
    { 0xA8, 0x0010FF00 },
    { 0xAC, 0x060C000C },
    { 0xB8, 0x04000000 },
    { 0xF0, 0x0000FF02 },
    { 0xF4, 0x00030000 },
    { 0xFC, 0x00000010 },
  };
}

void CMPC10x::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("MPC10x", __FILE__);
  SaveState->Write(m_regs, sizeof(m_regs));
  uint32_t sel[4] = { m_selBus, m_selDevice, m_selFunction, m_selRegister };
  SaveState->Write(sel, sizeof(sel));
}

void CMPC10x::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("MPC10x") != Result::OKAY)
  {
    ErrorLog("Unable to load MPC%X state. Save state file is corrupt.", m_model);
    return;
  }
  SaveState->Read(m_regs, sizeof(m_regs));
  uint32_t sel[4] = { m_selBus, m_selDevice, m_selFunction, m_selRegister };
  SaveState->Read(sel, sizeof(sel));
  m_selBus = sel[0];
  m_selDevice = sel[1];
  m_selFunction = sel[2];
  m_selRegister = sel[3];
}

void CMPC10x::WritePCIConfigAddress(UINT32 data)
{
  // CONFIG_ADDR (little endian): bus 23-16, device 15-11, function 10-8,
  // register 7-0.
  UINT32 a = FLIPENDIAN32(data);
  m_selBus = (a >> 16) & 0xFF;
  m_selDevice = (a >> 11) & 0x1F;
  m_selFunction = (a >> 8) & 0x07;
  m_selRegister = a & 0xFF;
  if (m_selBus != 0)
    DebugLog("MPC10x: configuration cycle for PCI bus %u (only bus 0 exists)\n", m_selBus);
}

UINT32 CMPC10x::ReadPCIConfigData(unsigned bits, unsigned offset) const
{
  if (m_selDevice == 0)
  {
    unsigned at = m_selRegister + offset;
    if (bits == 8 || bits == 16 || bits == 32)
    {
      UINT32 v = 0;
      for (unsigned i = 0; i < bits / 8; i++)
        v = (v << 8) | Reg(at + i);
      return v;
    }
    ErrorLog("MPC10x internal error: invalid access size (%d-bits)", bits);
  }
  // Other devices: the register number is passed on as the bus expects it.
  return m_bus->ReadConfigSpace(m_selDevice, (m_selRegister >> 2) & 0x3C, bits, offset);
}

void CMPC10x::WritePCIConfigData(unsigned bits, unsigned offset, UINT32 data)
{
  if (m_selDevice != 0)
  {
    m_bus->WriteConfigSpace(m_selDevice, (m_selRegister >> 2) & 0x3C, bits, offset, data);
    return;
  }
  if (bits != 8 && bits != 16 && bits != 32)
  {
    ErrorLog("MPC10x internal error: invalid access size (%d-bits)", bits);
    return;
  }
  unsigned n = bits / 8;
  unsigned at = m_selRegister + offset;
  for (unsigned i = 0; i < n; i++)
    Reg(at + i) = UINT8(data >> (8 * (n - 1 - i)));
}

void CMPC10x::WriteRegister(unsigned reg, UINT8 data)
{
  Reg(reg) = data;
  if ((reg & 0xFF) == 0xA8 && (data & 0x20) != 0)
    ErrorLog("MPC10x little endian mode not yet implemented!");
}

void CMPC10x::Put32LE(unsigned index, UINT32 value)
{
  for (unsigned i = 0; i < 4; i++)
    Reg(index + i) = UINT8(value >> (8 * i));
}

void CMPC10x::Reset(void)
{
  std::memset(m_regs, 0, sizeof(m_regs));

  // Vendor 0x1057 (Motorola), device 0x0001 (MPC105) or 0x0002 (MPC106).
  Reg(0x00) = 0x57;
  Reg(0x01) = 0x10;
  Reg(0x02) = (m_model == 0x106) ? 0x02 : 0x01;
  Reg(0x03) = 0x00;

  if (m_model == 0x106)
  {
    for (const PowerOnValue &d : kMPC106Defaults)
      Put32LE(d.reg, d.value);
  }
  else
  {
    for (const PowerOnValue &d : kMPC105Defaults)
      Put32LE(d.reg, d.value);
  }

  m_selBus = 0;
  m_selDevice = 0;
  m_selFunction = 0;
  m_selRegister = 0;
}

void CMPC10x::AttachPCIBus(CPCIBus *BusObjectPtr)
{
  m_bus = BusObjectPtr;
}

void CMPC10x::SetModel(int modelNum)
{
  if (modelNum == 0x105 || modelNum == 0x106)
    m_model = modelNum;
  else
  {
    ErrorLog("%s:%d: Invalid MPC10x model number (%X).", __FILE__, __LINE__, modelNum);
    m_model = 0x105;
  }
}

int CMPC10x::GetModel() const
{
  return m_model;
}

void CMPC10x::Init(void)
{
}

CMPC10x::CMPC10x(void)
{
}

CMPC10x::~CMPC10x(void)
{
}
