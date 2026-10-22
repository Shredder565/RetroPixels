// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USBV0.h"

#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Device.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
namespace
{
// Reads the parameters that each sit at the start of one in vector.
class InParams
{
public:
  InParams(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
      : m_memory(ios.GetSystem().GetMemory()), m_ioctlv(ioctlv)
  {
  }

  u8 U8(size_t vector) const { return m_memory.Read_U8(Address(vector)); }
  u16 U16(size_t vector) const { return m_memory.Read_U16(Address(vector)); }
  u32 U32(size_t vector) const { return m_memory.Read_U32(Address(vector)); }
  // For the fields that libusb-style callers store little-endian.
  u16 U16LE(size_t vector) const { return Common::swap16(U16(vector)); }

  Memory::MemoryManager& Memory() const { return m_memory; }

private:
  u32 Address(size_t vector) const { return m_ioctlv.in_vectors[vector].address; }

  Memory::MemoryManager& m_memory;
  const IOCtlVRequest& m_ioctlv;
};
}  // namespace

V0CtrlMessage::V0CtrlMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : CtrlMessage(ios, ioctlv, ioctlv.io_vectors[0].address)
{
  const InParams in(ios, ioctlv);
  request_type = in.U8(0);
  request = in.U8(1);
  value = in.U16LE(2);
  index = in.U16LE(3);
  length = in.U16LE(4);
}

V0BulkMessage::V0BulkMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv, bool long_length)
    : BulkMessage(ios, ioctlv, ioctlv.io_vectors[0].address)
{
  const InParams in(ios, ioctlv);
  endpoint = in.U8(0);
  length = long_length ? in.U32(1) : u32{in.U16(1)};
}

V0IntrMessage::V0IntrMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : IntrMessage(ios, ioctlv, ioctlv.io_vectors[0].address)
{
  const InParams in(ios, ioctlv);
  endpoint = in.U8(0);
  length = in.U16(1);
}

V0IsoMessage::V0IsoMessage(EmulationKernel& ios, const IOCtlVRequest& ioctlv)
    : IsoMessage(ios, ioctlv, ioctlv.io_vectors[1].address)
{
  const InParams in(ios, ioctlv);
  endpoint = in.U8(0);
  length = in.U16(1);
  num_packets = in.U8(2);

  packet_sizes_addr = ioctlv.io_vectors[0].address;
  packet_sizes.reserve(num_packets);
  for (u32 packet = 0; packet < num_packets; ++packet)
    packet_sizes.push_back(in.Memory().Read_U16(packet_sizes_addr + packet * 2));
}
}  // namespace IOS::HLE::USB
