// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Common.h"

#include <algorithm>
#include <initializer_list>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
Memory::MemoryManager& GuestMemory(EmulationKernel& kernel)
{
  return kernel.GetSystem().GetMemory();
}
}  // namespace

EmulationKernel& TransferCommand::GetEmulationKernel() const
{
  return m_ios;
}

void TransferCommand::OnTransferComplete(s32 return_value) const
{
  // Completion callbacks come from backend threads, never from the CPU thread.
  m_ios.EnqueueIPCReply(ios_request, return_value, 0, CoreTiming::FromThread::NON_CPU);
}

void TransferCommand::ScheduleTransferCompletion(s32 return_value, u32 expected_time_us) const
{
  const u64 ticks_per_second = m_ios.GetSystem().GetSystemTimers().GetTicksPerSecond();
  const u64 delay_ticks = ticks_per_second * expected_time_us / 1'000'000;
  m_ios.EnqueueIPCReply(ios_request, return_value, static_cast<s64>(delay_ticks),
                        CoreTiming::FromThread::ANY);
}

std::unique_ptr<u8[]> TransferCommand::MakeBuffer(const size_t size) const
{
  ASSERT_MSG(IOS_USB, data_address != 0, "Invalid data_address");
  std::unique_ptr<u8[]> copy = std::make_unique<u8[]>(size);
  GuestMemory(m_ios).CopyFromEmu(copy.get(), data_address, size);
  return copy;
}

void TransferCommand::FillBuffer(const u8* src, const size_t size) const
{
  ASSERT_MSG(IOS_USB, size == 0 || data_address != 0, "Invalid data_address");
  GuestMemory(m_ios).CopyToEmu(data_address, src, size);
}

void IsoMessage::SetPacketReturnValue(const size_t packet_num, const u16 return_value) const
{
  const u32 slot = static_cast<u32>(packet_sizes_addr + packet_num * sizeof(u16));
  GuestMemory(m_ios).Write_U16(return_value, slot);
}

u16 IsoMessage::SetPacketsReturnValueFromSize(u16 data_size) const
{
  u16 remaining = data_size;
  for (size_t packet = 0; packet < num_packets; ++packet)
  {
    const u16 used = std::min(remaining, packet_sizes[packet]);
    SetPacketReturnValue(packet, used);
    remaining = static_cast<u16>(remaining - used);
  }
  return remaining;
}

Device::~Device() = default;

u64 Device::GetId() const
{
  return m_id;
}

u16 Device::GetVid() const
{
  const DeviceDescriptor descriptor = GetDeviceDescriptor();
  return descriptor.idVendor;
}

u16 Device::GetPid() const
{
  const DeviceDescriptor descriptor = GetDeviceDescriptor();
  return descriptor.idProduct;
}

bool Device::HasClass(const u8 device_class) const
{
  // The device's own class, or that of any interface of the first configuration.
  return GetDeviceDescriptor().bDeviceClass == device_class ||
         std::ranges::any_of(GetInterfaces(0), [device_class](const InterfaceDescriptor& desc) {
           return desc.bInterfaceClass == device_class;
         });
}

std::string Device::GetErrorName(const int error_code) const
{
  return fmt::format("unknown error {}", error_code);
}

void DeviceDescriptor::Swap()
{
  for (u16* field : {&bcdUSB, &idVendor, &idProduct, &bcdDevice})
    *field = Common::swap16(*field);
}

void ConfigDescriptor::Swap()
{
  wTotalLength = Common::swap16(wTotalLength);
}

void InterfaceDescriptor::Swap()
{
  // Only single-byte fields.
}

void EndpointDescriptor::Swap()
{
  wMaxPacketSize = Common::swap16(wMaxPacketSize);
}
}  // namespace IOS::HLE::USB
