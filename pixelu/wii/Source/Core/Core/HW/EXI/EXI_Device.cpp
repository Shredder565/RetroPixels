// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_Device.h"

#include <memory>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI_DeviceAD16.h"
#include "Core/HW/EXI/EXI_DeviceAGP.h"
#include "Core/HW/EXI/EXI_DeviceBaseboard.h"
#include "Core/HW/EXI/EXI_DeviceDummy.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"
#include "Core/HW/EXI/EXI_DeviceGecko.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/EXI/EXI_DeviceModem.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

#ifdef HAVE_CUBEB
#include "Core/HW/EXI/EXI_DeviceMic.h"
#else
#include "Common/MsgHandler.h"
#endif

namespace ExpansionInterface
{
IEXIDevice::IEXIDevice(Core::System& system) : m_system(system)
{
}

void IEXIDevice::ImmWrite(u32 data, u32 size)
{
  // Most significant byte goes out first.
  for (u32 index = 0; index < size; ++index)
  {
    u8 outgoing = static_cast<u8>(data >> (24 - 8 * index));
    TransferByte(outgoing);
  }
}

u32 IEXIDevice::ImmRead(u32 size)
{
  u32 word = 0;
  for (u32 index = 0; index < size; ++index)
  {
    u8 incoming = 0;
    TransferByte(incoming);
    word |= u32{incoming} << (24 - 8 * index);
  }
  return word;
}

void IEXIDevice::ImmReadWrite(u32&, u32)
{
  // Bidirectional immediate transfers are only meaningful for devices that override this.
}

void IEXIDevice::DMAWrite(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  for (u32 offset = 0; offset < size; ++offset)
  {
    u8 outgoing = memory.Read_U8(address + offset);
    TransferByte(outgoing);
  }
}

void IEXIDevice::DMARead(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  for (u32 offset = 0; offset < size; ++offset)
  {
    u8 incoming = 0;
    TransferByte(incoming);
    memory.Write_U8(incoming, address + offset);
  }
}

bool IEXIDevice::UseDelayedTransferCompletion() const
{
  return false;
}

bool IEXIDevice::IsPresent() const
{
  return false;
}

void IEXIDevice::SetCS(int)
{
}

void IEXIDevice::DoState(PointerWrap&)
{
}

bool IEXIDevice::IsInterruptSet()
{
  return false;
}

void IEXIDevice::TransferByte(u8&)
{
}

namespace
{
std::unique_ptr<IEXIDevice> CreateMicrophone(Core::System& system, int channel_num)
{
#ifdef HAVE_CUBEB
  return std::make_unique<CEXIMic>(system, channel_num);
#else
  (void)channel_num;
  PanicAlertFmtT("Dolphin was built with Cubeb disabled. The Microphone device cannot be used.");
  return std::make_unique<IEXIDevice>(system);
#endif
}

std::unique_ptr<IEXIDevice> CreateDevice(Core::System& system, EXIDeviceType device_type,
                                         int channel_num,
                                         const Memcard::HeaderData& memcard_header_data)
{
  // Only devices for the memory card slots care about their slot, and for those (channels 0 and
  // 1) the channel number is the slot.
  const auto slot = static_cast<Slot>(channel_num);

  switch (device_type)
  {
  case EXIDeviceType::Dummy:
    return std::make_unique<CEXIDummy>(system, "Dummy");
  case EXIDeviceType::MemoryCard:
    return std::make_unique<CEXIMemoryCard>(system, slot, false, memcard_header_data);
  case EXIDeviceType::MemoryCardFolder:
    return std::make_unique<CEXIMemoryCard>(system, slot, true, memcard_header_data);
  case EXIDeviceType::MaskROM:
    return std::make_unique<CEXIIPL>(system);
  case EXIDeviceType::AD16:
    return std::make_unique<CEXIAD16>(system);
  case EXIDeviceType::Microphone:
    return CreateMicrophone(system, channel_num);
  case EXIDeviceType::Ethernet:
    return std::make_unique<CEXIETHERNET>(system, BBADeviceType::TAP);
  case EXIDeviceType::EthernetTapServer:
    return std::make_unique<CEXIETHERNET>(system, BBADeviceType::TAPSERVER);
  case EXIDeviceType::EthernetXLink:
    return std::make_unique<CEXIETHERNET>(system, BBADeviceType::XLINK);
  case EXIDeviceType::EthernetBuiltIn:
    return std::make_unique<CEXIETHERNET>(system, BBADeviceType::BuiltIn);
  case EXIDeviceType::EthernetIPC:
    return std::make_unique<CEXIETHERNET>(system, BBADeviceType::IPC);
  case EXIDeviceType::ModemTapServer:
    return std::make_unique<CEXIModem>(system, ModemDeviceType::TAPSERVER);
  case EXIDeviceType::Gecko:
    return std::make_unique<CEXIGecko>(system);
  case EXIDeviceType::AGP:
    return std::make_unique<CEXIAgp>(system, slot);
  case EXIDeviceType::Baseboard:
    return std::make_unique<CEXIBaseboard>(system);
  case EXIDeviceType::None:
  default:
    return std::make_unique<IEXIDevice>(system);
  }
}
}  // namespace

std::unique_ptr<IEXIDevice> EXIDevice_Create(Core::System& system, const EXIDeviceType device_type,
                                             const int channel_num,
                                             const Memcard::HeaderData& memcard_header_data)
{
  std::unique_ptr<IEXIDevice> device =
      CreateDevice(system, device_type, channel_num, memcard_header_data);
  // Record the requested type (a GCI folder card stays MemoryCardFolder).
  device->m_device_type = device_type;
  return device;
}
}  // namespace ExpansionInterface
