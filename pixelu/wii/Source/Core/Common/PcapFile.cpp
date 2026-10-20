// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/PcapFile.h"

#include <array>
#include <chrono>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

// Fields are written in host byte order; readers detect the order from the magic number.

namespace Common
{
namespace
{
constexpr u32 MAGIC = 0xa1b2c3d4;  // microsecond timestamps
constexpr u16 VERSION_MAJOR = 2;
constexpr u16 VERSION_MINOR = 4;
constexpr u32 SNAPSHOT_LENGTH = 65535;

constexpr size_t FILE_HEADER_SIZE = 24;
constexpr size_t RECORD_HEADER_SIZE = 16;

template <size_t N>
class HeaderWriter
{
public:
  template <typename T>
  HeaderWriter& Put(T value)
  {
    std::memcpy(m_bytes.data() + m_pos, &value, sizeof(T));
    m_pos += sizeof(T);
    return *this;
  }
  const std::array<u8, N>& Bytes() const { return m_bytes; }

private:
  std::array<u8, N> m_bytes{};
  size_t m_pos = 0;
};
}  // namespace

void PCAP::AddHeader(u32 link_type)
{
  HeaderWriter<FILE_HEADER_SIZE> header;
  header.Put(MAGIC)
      .Put(VERSION_MAJOR)
      .Put(VERSION_MINOR)
      .Put(s32{0})  // GMT to local time correction
      .Put(u32{0})  // timestamp accuracy
      .Put(SNAPSHOT_LENGTH)
      .Put(link_type);
  m_fp->WriteBytes(header.Bytes().data(), header.Bytes().size());
}

void PCAP::AddPacket(const u8* bytes, size_t size)
{
  using namespace std::chrono;
  const auto since_epoch = duration_cast<microseconds>(system_clock::now().time_since_epoch());
  const auto seconds = duration_cast<std::chrono::seconds>(since_epoch);
  const auto micros = since_epoch.count() % 1000000;

  HeaderWriter<RECORD_HEADER_SIZE> record;
  record.Put(static_cast<u32>(seconds.count()))
      .Put(static_cast<u32>(micros))
      .Put(static_cast<u32>(size))   // bytes stored
      .Put(static_cast<u32>(size));  // original length (nothing is truncated)
  m_fp->WriteBytes(record.Bytes().data(), record.Bytes().size());
  m_fp->WriteBytes(bytes, size);
}
}  // namespace Common
