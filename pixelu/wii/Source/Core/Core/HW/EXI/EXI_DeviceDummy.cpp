// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceDummy.h"

#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace ExpansionInterface
{
CEXIDummy::CEXIDummy(Core::System& system, std::string name)
    : IEXIDevice(system), m_label(std::move(name))
{
}

void CEXIDummy::ImmWrite(u32 data, u32 size)
{
  INFO_LOG_FMT(EXPANSIONINTERFACE, "EXI placeholder '{}': immediate write {:08x} ({} bytes)",
               m_label, data, size);
}

u32 CEXIDummy::ImmRead(u32 size)
{
  INFO_LOG_FMT(EXPANSIONINTERFACE, "EXI placeholder '{}': immediate read ({} bytes)", m_label,
               size);
  return 0;
}

void CEXIDummy::DMAWrite(u32 address, u32 size)
{
  INFO_LOG_FMT(EXPANSIONINTERFACE, "EXI placeholder '{}': DMA of {:#x} bytes from {:08x}", m_label,
               size, address);
}

void CEXIDummy::DMARead(u32 address, u32 size)
{
  INFO_LOG_FMT(EXPANSIONINTERFACE, "EXI placeholder '{}': DMA of {:#x} bytes to {:08x}", m_label,
               size, address);
}

bool CEXIDummy::IsPresent() const
{
  return true;
}

void CEXIDummy::TransferByte(u8&)
{
}
}  // namespace ExpansionInterface
