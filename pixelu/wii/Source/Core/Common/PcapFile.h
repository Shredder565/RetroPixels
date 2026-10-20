// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Writer for libpcap capture files (the classic format: one global header, then a record header
// and the captured bytes per packet). Only creating a file and appending to it is supported.
//
//   PCAP pcap(new File::IOFile("capture.pcap", "wb"));
//   pcap.AddPacket(some_struct);           // the struct's bytes
//   pcap.AddPacket(buffer.data(), size);

#pragma once

#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace Common
{
class PCAP final
{
public:
  // Values of the header's link-layer type field.
  enum class LinkType : u32
  {
    Ethernet = 1,  // IEEE 802.3 Ethernet frames
    User = 147,    // LINKTYPE_USER0, private use
  };

  // Takes ownership of `fp`, which must already be open for writing, and writes the file header.
  explicit PCAP(File::IOFile* fp, LinkType link_type = LinkType::User) : m_fp(fp)
  {
    AddHeader(static_cast<u32>(link_type));
  }

  template <typename T>
  void AddPacket(const T& obj)
  {
    AddPacket(reinterpret_cast<const u8*>(&obj), sizeof(obj));
  }

  // Appends one packet, timestamped now. Not safe to call from several threads at once.
  void AddPacket(const u8* bytes, size_t size);

private:
  void AddHeader(u32 link_type);

  std::unique_ptr<File::IOFile> m_fp;
};
}  // namespace Common
