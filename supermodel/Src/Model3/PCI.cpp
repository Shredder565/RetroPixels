// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "PCI.h"

#include "Supermodel.h"

IPCIDevice *CPCIBus::Find(unsigned device) const
{
  for (const Slot &s : m_slots)
  {
    if (s.number == device)
      return s.dev;
  }
  return nullptr;
}

UINT32 CPCIBus::ReadConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
  if (IPCIDevice *dev = Find(device))
    return dev->ReadPCIConfigSpace(device, reg, bits, offset);
  DebugLog("PCI: config read from empty slot %u, register %02X\n", device, reg);
  return 0;
}

void CPCIBus::WriteConfigSpace(unsigned device, unsigned reg, unsigned bits, unsigned offset, UINT32 data)
{
  if (IPCIDevice *dev = Find(device))
    dev->WritePCIConfigSpace(device, reg, bits, offset, data);
  else
    DebugLog("PCI: config write to empty slot %u, register %02X = %08X\n", device, reg, data);
}

void CPCIBus::Reset(void)
{
  // The bus itself holds no state that a reset would clear.
}

void CPCIBus::AttachDevice(unsigned device, IPCIDevice *DeviceObjectPtr)
{
  m_slots.push_back(Slot{ device, DeviceObjectPtr });
}

void CPCIBus::Init(void)
{
  m_slots.clear();
}

CPCIBus::CPCIBus(void)
{
}

CPCIBus::~CPCIBus(void)
{
}
