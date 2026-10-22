// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#if defined(__linux__) || defined(__HAIKU__)
#include <sys/socket.h>
#endif

#include "Common/CommonTypes.h"

struct sockaddr_in;

// Network helpers: MAC/IP/Bluetooth address formatting and parsing, and raw Ethernet, IPv4,
// TCP, UDP, ARP and DHCP frames as the emulated network adapters see them. Header fields are
// kept in network byte order unless noted otherwise.

namespace Common
{
enum class MACConsumer
{
  BBA,
  IOS
};

enum
{
  BBA_HARDWARE_TYPE = 1,
  MAC_ADDRESS_SIZE = 6,
  IPV4_HEADER_TYPE = 8
};

enum DHCPConst
{
  MESSAGE_QUERY = 1,
  MESSAGE_REPLY = 2
};

using MACAddress = std::array<u8, MAC_ADDRESS_SIZE>;

// Stored in the order the Bluetooth protocol sends it, which is the reverse of how it is
// usually written.
struct BluetoothAddress : std::array<u8, MAC_ADDRESS_SIZE>
{
};

constexpr std::size_t IPV4_ADDR_LEN = 4;
using IPAddress = std::array<u8, IPV4_ADDR_LEN>;
constexpr IPAddress IP_ADDR_ANY = {0, 0, 0, 0};
constexpr IPAddress IP_ADDR_BROADCAST = {255, 255, 255, 255};
constexpr IPAddress IP_ADDR_SSDP = {239, 255, 255, 250};
constexpr u16 SSDP_PORT = 1900;
constexpr u16 IPV4_ETHERTYPE = 0x800;
constexpr u16 ARP_ETHERTYPE = 0x806;

struct EthernetHeader
{
  EthernetHeader();
  explicit EthernetHeader(u16 ether_type);
  EthernetHeader(const MACAddress& dest, const MACAddress& src, u16 ether_type);
  u16 Size() const;

  static constexpr std::size_t SIZE = 14;

  MACAddress destination = {};
  MACAddress source = {};
  u16 ethertype = 0;
};
static_assert(sizeof(EthernetHeader) == EthernetHeader::SIZE);
static_assert(std::is_standard_layout_v<EthernetHeader>);

struct IPv4Header
{
  IPv4Header();
  IPv4Header(u16 data_size, u8 ip_proto, const sockaddr_in& from, const sockaddr_in& to);
  u16 Size() const;
  // Header length from the IHL field, in bytes.
  u8 DefinedSize() const;

  static constexpr std::size_t SIZE = 20;

  u8 version_ihl = 0;
  u8 dscp_esn = 0;
  u16 total_len = 0;
  u16 identification = 0;
  u16 flags_fragment_offset = 0;
  u8 ttl = 0;
  u8 protocol = 0;
  u16 header_checksum = 0;
  IPAddress source_addr{};
  IPAddress destination_addr{};
};
static_assert(sizeof(IPv4Header) == IPv4Header::SIZE);
static_assert(std::is_standard_layout_v<IPv4Header>);

struct TCPHeader
{
  TCPHeader();
  TCPHeader(const sockaddr_in& from, const sockaddr_in& to, u32 seq, const u8* data, u16 length);
  TCPHeader(const sockaddr_in& from, const sockaddr_in& to, u32 seq, u32 ack, u16 flags);
  // Header length from the data offset field, in bytes.
  u8 GetHeaderSize() const;
  u16 Size() const;
  u8 IPProto() const;

  static constexpr std::size_t SIZE = 20;

  u16 source_port = 0;
  u16 destination_port = 0;
  u32 sequence_number = 0;
  u32 acknowledgement_number = 0;
  u16 properties = 0;
  u16 window_size = 0;
  u16 checksum = 0;
  u16 urgent_pointer = 0;
};
static_assert(sizeof(TCPHeader) == TCPHeader::SIZE);
static_assert(std::is_standard_layout_v<TCPHeader>);

struct UDPHeader
{
  UDPHeader();
  UDPHeader(const sockaddr_in& from, const sockaddr_in& to, u16 data_length);
  u16 Size() const;
  u8 IPProto() const;

  static constexpr std::size_t SIZE = 8;

  u16 source_port = 0;
  u16 destination_port = 0;
  u16 length = 0;
  u16 checksum = 0;
};
static_assert(sizeof(UDPHeader) == UDPHeader::SIZE);
static_assert(std::is_standard_layout_v<UDPHeader>);

#pragma pack(push, 1)
struct ARPHeader
{
  ARPHeader();
  // An ARP reply from (from_ip, from_mac) to (to_ip, to_mac). IPs are in network order.
  ARPHeader(u32 from_ip, const MACAddress& from_mac, u32 to_ip, const MACAddress& to_mac);
  u16 Size() const;

  static constexpr std::size_t SIZE = 28;

  u16 hardware_type = 0;
  u16 protocol_type = 0;
  u8 hardware_size = 0;
  u8 protocol_size = 0;
  u16 opcode = 0;
  MACAddress sender_address{};
  u32 sender_ip = 0;
  MACAddress targer_address{};
  u32 target_ip = 0;
};
static_assert(sizeof(ARPHeader) == ARPHeader::SIZE);
#pragma pack(pop)

// The fixed part of a BOOTP/DHCP message (RFC 2131), up to and including the magic cookie.
struct DHCPBody
{
  DHCPBody();
  // A reply offering new_ip from server serv_ip (both in network order).
  DHCPBody(u32 transaction, const MACAddress& client_address, u32 new_ip, u32 serv_ip);
  static constexpr std::size_t SIZE = 240;
  u8 message_type = 0;
  u8 hardware_type = 0;
  u8 hardware_addr = 0;
  u8 hops = 0;
  u32 transaction_id = 0;
  u16 seconds = 0;
  u16 boot_flag = 0;
  u32 client_ip = 0;
  u32 your_ip = 0;
  u32 server_ip = 0;
  u32 relay_ip = 0;
  MACAddress client_mac{};
  unsigned char padding[10]{};
  unsigned char hostname[0x40]{};
  unsigned char boot_file[0x80]{};
  u8 magic_cookie[4] = {0x63, 0x82, 0x53, 0x63};
};
static_assert(sizeof(DHCPBody) == DHCPBody::SIZE);

struct DHCPPacket
{
  DHCPPacket();
  // Parses a message; options are kept as raw "code, length, data" byte strings.
  DHCPPacket(std::span<const u8> data);
  void AddOption(u8 fnc, std::span<const u8> params);
  void AddOption(u8 fnc, std::initializer_list<u8> params)
  {
    AddOption(fnc, {params.begin(), params.end()});
  }
  // Body, options, then an end option padded to four bytes.
  std::vector<u8> Build() const;

  DHCPBody body;
  std::vector<std::vector<u8>> options;
};

// Packed so that no padding sits between the two headers.
#pragma pack(push, 1)
struct ARPPacket
{
  ARPPacket();
  ARPPacket(const MACAddress& destination, const MACAddress& source);
  std::vector<u8> Build() const;
  u16 Size() const;

  EthernetHeader eth_header;
  ARPHeader arp_header;

  static constexpr std::size_t SIZE = EthernetHeader::SIZE + ARPHeader::SIZE;
};
static_assert(sizeof(ARPPacket) == ARPPacket::SIZE);
#pragma pack(pop)

struct TCPPacket
{
  TCPPacket();
  TCPPacket(const MACAddress& destination, const MACAddress& source, const sockaddr_in& from,
            const sockaddr_in& to, u32 seq, u32 ack, u16 flags);
  // The whole frame, with lengths, data offset and checksums filled in.
  std::vector<u8> Build() const;
  u16 Size() const;

  EthernetHeader eth_header;
  IPv4Header ip_header;
  TCPHeader tcp_header;
  std::vector<u8> ipv4_options;
  std::vector<u8> tcp_options;
  std::vector<u8> data;

  static constexpr std::size_t MIN_SIZE = EthernetHeader::SIZE + IPv4Header::SIZE + TCPHeader::SIZE;
};

struct UDPPacket
{
  UDPPacket();
  UDPPacket(const MACAddress& destination, const MACAddress& source, const sockaddr_in& from,
            const sockaddr_in& to, const std::vector<u8>& payload);
  // The whole frame, with lengths and checksums filled in.
  std::vector<u8> Build() const;
  u16 Size() const;

  EthernetHeader eth_header;
  IPv4Header ip_header;
  UDPHeader udp_header;

  std::vector<u8> ipv4_options;
  std::vector<u8> data;

  static constexpr std::size_t MIN_SIZE = EthernetHeader::SIZE + IPv4Header::SIZE + UDPHeader::SIZE;
};

// Read-only parser over a raw Ethernet frame.
class PacketView
{
public:
  PacketView(const u8* ptr, std::size_t size);

  std::optional<u16> GetEtherType() const;
  std::optional<ARPPacket> GetARPPacket() const;
  std::optional<u8> GetIPProto() const;
  std::optional<TCPPacket> GetTCPPacket() const;
  std::optional<UDPPacket> GetUDPPacket() const;

private:
  const u8* m_ptr;
  std::size_t m_size;
};

struct NetworkErrorState
{
  int error;
#ifdef _WIN32
  int wsa_error;
#endif
};

struct IPv4Port
{
  IPAddress ip_address;
  u16 port;  // network byte order

  // Host byte order.
  u32 GetIPAddressValue() const;
  u16 GetPortValue() const;
};

struct IPv4PortRange
{
  IPv4Port first;
  IPv4Port last;

  bool IsMatch(IPv4Port subject) const;
  std::string ToString() const;
};

std::string IPAddressToString(IPAddress ip_address);

// first_ip[-last_ip|/prefix_length][:first_port[-last_port]]
std::optional<IPv4PortRange> StringToIPv4PortRange(std::string_view subject);

MACAddress GenerateMacAddress(MACConsumer type);

std::string MacAddressToString(const MACAddress& mac);
std::optional<MACAddress> StringToMacAddress(std::string_view mac_string);

std::string BluetoothAddressToString(BluetoothAddress bdaddr);
std::optional<BluetoothAddress> StringToBluetoothAddress(std::string_view str);

// RFC 1071 Internet checksum (host byte order result).
u16 ComputeNetworkChecksum(const void* data, u16 length, u32 initial_value = 0);
// Checksum including the IPv4 pseudo header; the result is in network byte order.
u16 ComputeTCPNetworkChecksum(const IPAddress& from, const IPAddress& to, const void* data,
                              u16 length, u8 protocol);
NetworkErrorState SaveNetworkErrorState();
void RestoreNetworkErrorState(const NetworkErrorState& state);
const char* DecodeNetworkError(s32 error_code);
const char* StrNetworkError();

// Sets SO_NOSIGPIPE where the platform has it.
bool SetPlatformSocketOptions(int fd);

// Flags for every send() so that a closed peer does not raise SIGPIPE.
#if defined(__linux__) || defined(__HAIKU__)
static constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
static constexpr int SEND_FLAGS = 0;
#endif
}  // namespace Common
