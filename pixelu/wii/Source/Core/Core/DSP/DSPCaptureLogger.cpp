// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPCaptureLogger.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/PcapFile.h"

namespace DSP
{
namespace
{
// Packet layouts of the capture file (little endian, no padding). The first byte
// tells the two kinds apart.
enum PacketKind : u8
{
  REGISTER_ACCESS = 0,
  DMA_TRANSFER = 1,
};

#pragma pack(push, 1)
struct RegisterAccessPacket
{
  u8 kind;
  u8 is_read;  // 1 for a read, 0 for a write
  u16 address;
  u16 value;
};

// The transferred bytes follow this header.
struct DMAPacketHeader
{
  u8 kind;
  u16 control;
  u32 gc_address;
  u16 dsp_address;
  u16 length;  // bytes
};
#pragma pack(pop)

static_assert(sizeof(RegisterAccessPacket) == 6);
static_assert(sizeof(DMAPacketHeader) == 11);
}  // namespace

PCAPDSPCaptureLogger::PCAPDSPCaptureLogger(const std::string& pcap_filename)
    : m_pcap(std::make_unique<Common::PCAP>(new File::IOFile(pcap_filename, "wb")))
{
}

PCAPDSPCaptureLogger::PCAPDSPCaptureLogger(Common::PCAP* pcap) : m_pcap(pcap)
{
}

PCAPDSPCaptureLogger::PCAPDSPCaptureLogger(std::unique_ptr<Common::PCAP>&& pcap)
    : m_pcap(std::move(pcap))
{
}

PCAPDSPCaptureLogger::~PCAPDSPCaptureLogger() = default;

void PCAPDSPCaptureLogger::LogIFXRead(u16 address, u16 read_value)
{
  WriteRegisterAccess(true, address, read_value);
}

void PCAPDSPCaptureLogger::LogIFXWrite(u16 address, u16 written_value)
{
  WriteRegisterAccess(false, address, written_value);
}

void PCAPDSPCaptureLogger::WriteRegisterAccess(bool is_read, u16 address, u16 value)
{
  const RegisterAccessPacket packet{REGISTER_ACCESS, static_cast<u8>(is_read ? 1 : 0), address,
                                    value};
  m_pcap->AddPacket(packet);
}

void PCAPDSPCaptureLogger::LogDMA(u16 control, u32 gc_address, u16 dsp_address, u16 length,
                                  const u8* data)
{
  const DMAPacketHeader header{DMA_TRANSFER, control, gc_address, dsp_address, length};
  m_packet.resize(sizeof(header) + length);
  std::memcpy(m_packet.data(), &header, sizeof(header));
  if (length != 0)
    std::memcpy(m_packet.data() + sizeof(header), data, length);
  m_pcap->AddPacket(m_packet.data(), m_packet.size());
}
}  // namespace DSP
