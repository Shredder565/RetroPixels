// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USBV4.h"

#include <algorithm>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
// Offsets in the 32-byte /dev/usb/hid request block (WiiBrew, /dev/usb/hid). The first 16 bytes
// are unused and the device number sits at 16.
constexpr u32 OFFSET_PARAMS = 20;
constexpr u32 OFFSET_DATA_ADDRESS = 28;

// Control requests: bmRequestType, bRequest, wValue, wIndex, wLength.
constexpr u32 OFFSET_CTRL_REQUEST_TYPE = OFFSET_PARAMS + 0;
constexpr u32 OFFSET_CTRL_REQUEST = OFFSET_PARAMS + 1;
constexpr u32 OFFSET_CTRL_VALUE = OFFSET_PARAMS + 2;
constexpr u32 OFFSET_CTRL_INDEX = OFFSET_PARAMS + 4;
constexpr u32 OFFSET_CTRL_LENGTH = OFFSET_PARAMS + 6;
// Interrupt requests: endpoint (u32), length (u32).
constexpr u32 OFFSET_INTR_ENDPOINT = OFFSET_PARAMS + 0;
constexpr u32 OFFSET_INTR_LENGTH = OFFSET_PARAMS + 4;
// String requests: descriptor index (u8).
constexpr u32 OFFSET_STRING_INDEX = OFFSET_PARAMS + 0;

constexpr u16 DESCRIPTOR_TYPE_STRING = 0x03;
constexpr u16 LANGUAGE_ENGLISH_US = 0x0409;

Memory::MemoryManager& GuestMemory(EmulationKernel& ios)
{
  return ios.GetSystem().GetMemory();
}
}  // namespace

V4CtrlMessage::V4CtrlMessage(EmulationKernel& ios, const IOCtlRequest& ioctl)
    : CtrlMessage(ios, ioctl, 0)
{
  const Memory::MemoryManager& memory = GuestMemory(ios);
  const u32 block = ioctl.buffer_in;
  request_type = memory.Read_U8(block + OFFSET_CTRL_REQUEST_TYPE);
  request = memory.Read_U8(block + OFFSET_CTRL_REQUEST);
  value = memory.Read_U16(block + OFFSET_CTRL_VALUE);
  index = memory.Read_U16(block + OFFSET_CTRL_INDEX);
  length = memory.Read_U16(block + OFFSET_CTRL_LENGTH);
  data_address = memory.Read_U32(block + OFFSET_DATA_ADDRESS);
}

V4GetUSStringMessage::V4GetUSStringMessage(EmulationKernel& ios, const IOCtlRequest& ioctl)
    : CtrlMessage(ios, ioctl, 0)
{
  // This is a plain GET_DESCRIPTOR(string) request with a fixed language and maximum length.
  const Memory::MemoryManager& memory = GuestMemory(ios);
  const u32 block = ioctl.buffer_in;
  request_type = 0x80;
  request = REQUEST_GET_DESCRIPTOR;
  value = static_cast<u16>((DESCRIPTOR_TYPE_STRING << 8) |
                           memory.Read_U8(block + OFFSET_STRING_INDEX));
  index = LANGUAGE_ENGLISH_US;
  length = 255;
  data_address = memory.Read_U32(block + OFFSET_DATA_ADDRESS);
}

void V4GetUSStringMessage::OnTransferComplete(s32 return_value) const
{
  Memory::MemoryManager& memory = GuestMemory(m_ios);
  std::string text = memory.GetString(data_address);
  for (char& c : text)
  {
    if (!Common::IsPrintableCharacter(c))
      c = '?';
  }
  memory.CopyToEmu(data_address, text.data(), text.size());
  TransferCommand::OnTransferComplete(return_value);
}

V4IntrMessage::V4IntrMessage(EmulationKernel& ios, const IOCtlRequest& ioctl)
    : IntrMessage(ios, ioctl, 0)
{
  const Memory::MemoryManager& memory = GuestMemory(ios);
  const u32 block = ioctl.buffer_in;
  length = memory.Read_U32(block + OFFSET_INTR_LENGTH);
  endpoint = static_cast<u8>(memory.Read_U32(block + OFFSET_INTR_ENDPOINT));
  data_address = memory.Read_U32(block + OFFSET_DATA_ADDRESS);
}
}  // namespace IOS::HLE::USB
