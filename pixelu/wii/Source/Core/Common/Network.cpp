// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Network.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <fmt/format.h>

#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/StringUtil.h"

namespace Common
{
namespace
{
// Raw byte helpers. Values are copied exactly as they sit in memory.
template <typename T>
void AppendRaw(std::vector<u8>* out, const T& value)
{
  static_assert(std::is_trivially_copyable_v<T>);
  const auto* bytes = reinterpret_cast<const u8*>(&value);
  out->insert(out->end(), bytes, bytes + sizeof(T));
}

template <typename T>
T ReadRaw(const u8* ptr)
{
  static_assert(std::is_trivially_copyable_v<T>);
  T value;
  std::memcpy(&value, ptr, sizeof(T));
  return value;
}

void WriteRaw16(std::vector<u8>* out, size_t offset, u16 value)
{
  std::memcpy(out->data() + offset, &value, sizeof(value));
}

u32 HostIPValue(const IPAddress& ip)
{
  return (u32{ip[0]} << 24) | (u32{ip[1]} << 16) | (u32{ip[2]} << 8) | u32{ip[3]};
}

IPAddress IPFromHostValue(u32 value)
{
  return {static_cast<u8>(value >> 24), static_cast<u8>(value >> 16), static_cast<u8>(value >> 8),
          static_cast<u8>(value)};
}

u16 NetworkOrder16(u16 host_value)
{
  const u8 bytes[2] = {static_cast<u8>(host_value >> 8), static_cast<u8>(host_value)};
  return ReadRaw<u16>(bytes);
}

// The sum of the 16-bit halves of both addresses, the protocol and the length: the IPv4
// pseudo header of TCP and UDP checksums.
u32 PseudoHeaderSum(u32 source_host, u32 destination_host, u32 protocol, u32 length)
{
  return (source_host >> 16) + (source_host & 0xFFFF) + (destination_host >> 16) +
         (destination_host & 0xFFFF) + protocol + length;
}

u32 FoldChecksum(u32 sum)
{
  while (sum > 0xFFFF)
    sum = (sum >> 16) + (sum & 0xFFFF);
  return sum;
}

// Integer parse helper for the port range syntax: decimal digits at the front of `text`.
template <typename T>
std::optional<std::pair<T, size_t>> ParsePrefix(std::string_view text)
{
  T value{};
  const auto result = Common::FromChars(text, value);
  if (result.ec != std::errc{})
    return std::nullopt;
  return std::make_pair(value, static_cast<size_t>(result.ptr - text.data()));
}
}  // namespace

u32 IPv4Port::GetIPAddressValue() const
{
  return HostIPValue(ip_address);
}

u16 IPv4Port::GetPortValue() const
{
  u8 bytes[2];
  std::memcpy(bytes, &port, sizeof(bytes));
  return static_cast<u16>((bytes[0] << 8) | bytes[1]);
}

bool IPv4PortRange::IsMatch(IPv4Port subject) const
{
  const u32 ip = subject.GetIPAddressValue();
  const u16 port = subject.GetPortValue();
  return first.GetIPAddressValue() <= ip && ip <= last.GetIPAddressValue() &&
         first.GetPortValue() <= port && port <= last.GetPortValue();
}

std::string IPv4PortRange::ToString() const
{
  const u32 low = first.GetIPAddressValue();
  const u32 high = last.GetIPAddressValue();
  const int shared_prefix = std::countl_zero(low ^ high);

  std::string text = IPAddressToString(first.ip_address);
  if (shared_prefix != 32)
  {
    // A range whose size is a power of two at the prefix boundary prints as CIDR.
    if (((high - low + 1) << shared_prefix) == 0)
      text += fmt::format("/{}", shared_prefix);
    else if (shared_prefix >= 24)
      text += fmt::format("-{}", last.ip_address[3]);
    else
      text += '-' + IPAddressToString(last.ip_address);
  }

  if (first.port == 0)
    return text;
  if (first.port == last.port)
    return fmt::format("{}:{}", text, first.GetPortValue());
  return fmt::format("{}:{}-{}", text, first.GetPortValue(), last.GetPortValue());
}

std::string IPAddressToString(IPAddress ip_address)
{
  return fmt::format("{}.{}.{}.{}", ip_address[0], ip_address[1], ip_address[2], ip_address[3]);
}

std::optional<IPv4PortRange> StringToIPv4PortRange(std::string_view subject)
{
  IPv4PortRange range{};

  // First address: four dot-separated numbers (TryParse syntax) up to the first '-', '/' or ':'.
  size_t pos = subject.find_first_of("-/:");
  if (pos == std::string_view::npos)
    pos = subject.size();

  const auto octets = SplitStringIntoArray<4>(subject.substr(0, pos), '.');
  if (!octets)
    return std::nullopt;
  for (size_t i = 0; i < 4; ++i)
  {
    if (!TryParse(std::string((*octets)[i]), &range.first.ip_address[i]))
      return std::nullopt;
  }

  const auto all_ports = [&range] {
    range.first.port = 0;
    range.last.port = 0xFFFF;
    return range;
  };

  if (pos == subject.size())
  {
    range.last.ip_address = range.first.ip_address;
    return all_ports();
  }

  if (subject[pos] == '-')
  {
    // Last address. Leading octets may be left out and are then taken from the first address
    // ("10.0.0.1-7" or "10.0.0.1-1.7").
    ++pos;
    size_t end = subject.find(':', pos);
    if (end == std::string_view::npos)
      end = subject.size();

    const std::vector<std::string> parts =
        SplitString(std::string(subject.substr(pos, end - pos)), '.');
    if (parts.empty() || parts.size() > 4)
      return std::nullopt;

    const size_t inherited = 4 - parts.size();
    for (size_t i = 0; i < inherited; ++i)
      range.last.ip_address[i] = range.first.ip_address[i];
    for (size_t i = 0; i < parts.size(); ++i)
    {
      if (!TryParse(parts[i], &range.last.ip_address[inherited + i]))
        return std::nullopt;
    }

    if (range.last.ip_address < range.first.ip_address)
      return std::nullopt;
    pos = end;
  }
  else if (subject[pos] == '/')
  {
    // Network prefix length.
    const auto parsed = ParsePrefix<u32>(subject.substr(pos + 1));
    if (!parsed || parsed->first > 32)
      return std::nullopt;
    pos += 1 + parsed->second;

    const u32 bits = parsed->first;
    const u32 mask = bits == 0 ? 0 : ~u32{0} << (32 - bits);
    const u32 network = range.first.GetIPAddressValue() & mask;
    range.first.ip_address = IPFromHostValue(network);
    range.last.ip_address = IPFromHostValue(network | ~mask);
  }
  else
  {
    range.last.ip_address = range.first.ip_address;
  }

  if (pos == subject.size())
    return all_ports();

  if (subject[pos] != ':')
    return std::nullopt;

  const auto first_port = ParsePrefix<u16>(subject.substr(pos + 1));
  if (!first_port)
    return std::nullopt;
  range.first.port = NetworkOrder16(first_port->first);
  pos += 1 + first_port->second;

  if (pos == subject.size())
  {
    range.last.port = range.first.port;
    return range;
  }

  if (subject[pos] != '-')
    return std::nullopt;

  const auto last_port = ParsePrefix<u16>(subject.substr(pos + 1));
  if (!last_port || last_port->first < first_port->first)
    return std::nullopt;
  range.last.port = NetworkOrder16(last_port->first);
  pos += 1 + last_port->second;

  if (pos != subject.size())
    return std::nullopt;
  return range;
}

MACAddress GenerateMacAddress(const MACConsumer type)
{
  // Vendor prefixes: Nintendo's broadband adapter and Nintendo's Wi-Fi.
  MACAddress mac{};
  if (type == MACConsumer::BBA)
    mac = {0x00, 0x09, 0xbf, 0, 0, 0};
  else if (type == MACConsumer::IOS)
    mac = {0x00, 0x17, 0xab, 0, 0, 0};

  Random::Generate(&mac[3], 3);
  return mac;
}

std::string MacAddressToString(const MACAddress& mac)
{
  return fmt::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3],
                     mac[4], mac[5]);
}

std::optional<MACAddress> StringToMacAddress(std::string_view mac_string)
{
  // Takes the first twelve hex digits and ignores any other characters (separators).
  MACAddress mac{};
  size_t digits = 0;
  for (const char c : mac_string)
  {
    if (digits == MAC_ADDRESS_SIZE * 2)
      break;

    int value;
    if (c >= '0' && c <= '9')
      value = c - '0';
    else if (c >= 'a' && c <= 'f')
      value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      value = c - 'A' + 10;
    else
      continue;

    mac[digits / 2] |= static_cast<u8>(digits % 2 == 0 ? value << 4 : value);
    ++digits;
  }

  if (digits != MAC_ADDRESS_SIZE * 2)
    return std::nullopt;
  return mac;
}

std::string BluetoothAddressToString(BluetoothAddress bdaddr)
{
  MACAddress written;
  std::ranges::reverse_copy(bdaddr, written.begin());
  return MacAddressToString(written);
}

std::optional<BluetoothAddress> StringToBluetoothAddress(std::string_view str)
{
  const auto mac = StringToMacAddress(str);
  if (!mac)
    return std::nullopt;
  BluetoothAddress result;
  std::ranges::reverse_copy(*mac, result.begin());
  return result;
}

EthernetHeader::EthernetHeader() = default;

u16 EthernetHeader::Size() const
{
  return static_cast<u16>(SIZE);
}

EthernetHeader::EthernetHeader(const MACAddress& dest, const MACAddress& src, u16 ether_type)
    : destination(dest), source(src), ethertype(htons(ether_type))
{
}

EthernetHeader::EthernetHeader(u16 ether_type) : ethertype(htons(ether_type))
{
}

IPv4Header::IPv4Header() = default;

IPv4Header::IPv4Header(u16 data_size, u8 ip_proto, const sockaddr_in& from, const sockaddr_in& to)
{
  version_ihl = 0x45;  // IPv4, five 32-bit words
  total_len = htons(static_cast<u16>(SIZE + data_size));
  flags_fragment_offset = htons(0x4000);  // don't fragment
  ttl = 64;
  protocol = ip_proto;
  std::memcpy(source_addr.data(), &from.sin_addr, IPV4_ADDR_LEN);
  std::memcpy(destination_addr.data(), &to.sin_addr, IPV4_ADDR_LEN);
  // The checksum field is still zero here.
  header_checksum = htons(ComputeNetworkChecksum(this, static_cast<u16>(SIZE)));
}

u16 IPv4Header::Size() const
{
  return static_cast<u16>(SIZE);
}

u8 IPv4Header::DefinedSize() const
{
  return static_cast<u8>((version_ihl & 0x0f) * 4);
}

TCPHeader::TCPHeader() = default;

TCPHeader::TCPHeader(const sockaddr_in& from, const sockaddr_in& to, u32 seq, const u8* data,
                     u16 length)
{
  std::memcpy(&source_port, &from.sin_port, sizeof(source_port));
  std::memcpy(&destination_port, &to.sin_port, sizeof(destination_port));
  sequence_number = htonl(seq);
  // Data offset 5 (no options) in the first byte; no flags.
  reinterpret_cast<u8*>(&properties)[0] = 0x50;
  window_size = 0xFFFF;

  // The header and the payload are checksummed separately and the two (complemented) sums are
  // folded together.
  const u32 initial = PseudoHeaderSum(ntohl(from.sin_addr.s_addr), ntohl(to.sin_addr.s_addr),
                                      IPProto(), Size() + u32{length});
  u32 sum = ComputeNetworkChecksum(this, Size(), initial);
  sum += ComputeNetworkChecksum(data, length);
  checksum = htons(static_cast<u16>(FoldChecksum(sum)));
}

TCPHeader::TCPHeader(const sockaddr_in& from, const sockaddr_in& to, u32 seq, u32 ack, u16 flags)
{
  source_port = from.sin_port;
  destination_port = to.sin_port;
  sequence_number = htonl(seq);
  acknowledgement_number = htonl(ack);
  properties = htons(flags);
  window_size = 0x7c;  // stored as is
  checksum = 0;
}

u8 TCPHeader::GetHeaderSize() const
{
  return static_cast<u8>((ntohs(properties) >> 12) * 4);
}

u16 TCPHeader::Size() const
{
  return static_cast<u16>(SIZE);
}

u8 TCPHeader::IPProto() const
{
  return static_cast<u8>(IPPROTO_TCP);
}

UDPHeader::UDPHeader() = default;

UDPHeader::UDPHeader(const sockaddr_in& from, const sockaddr_in& to, u16 data_length)
{
  std::memcpy(&source_port, &from.sin_port, sizeof(source_port));
  std::memcpy(&destination_port, &to.sin_port, sizeof(destination_port));
  length = htons(static_cast<u16>(SIZE + data_length));
}

u16 UDPHeader::Size() const
{
  return static_cast<u16>(SIZE);
}

u8 UDPHeader::IPProto() const
{
  return static_cast<u8>(IPPROTO_UDP);
}

ARPHeader::ARPHeader() = default;

ARPHeader::ARPHeader(u32 from_ip, const MACAddress& from_mac, u32 to_ip, const MACAddress& to_mac)
{
  hardware_type = htons(BBA_HARDWARE_TYPE);
  // These two are stored as plain little-endian values, which gives the expected wire bytes
  // 08 00 (IPv4) and 00 02 (reply).
  protocol_type = IPV4_HEADER_TYPE;
  hardware_size = MAC_ADDRESS_SIZE;
  protocol_size = IPV4_ADDR_LEN;
  opcode = 0x200;
  sender_address = from_mac;
  sender_ip = from_ip;
  targer_address = to_mac;
  target_ip = to_ip;
}

u16 ARPHeader::Size() const
{
  return static_cast<u16>(SIZE);
}

DHCPBody::DHCPBody() = default;

DHCPBody::DHCPBody(u32 transaction, const MACAddress& client_address, u32 new_ip, u32 serv_ip)
{
  message_type = DHCPConst::MESSAGE_REPLY;
  hardware_type = BBA_HARDWARE_TYPE;
  hardware_addr = MAC_ADDRESS_SIZE;
  transaction_id = transaction;
  your_ip = new_ip;
  server_ip = serv_ip;
  client_mac = client_address;
}

DHCPPacket::DHCPPacket() = default;

DHCPPacket::DHCPPacket(std::span<const u8> data)
{
  if (data.size() < DHCPBody::SIZE)
    return;

  std::memcpy(&body, data.data(), DHCPBody::SIZE);

  // Options: code 0 is padding, code 255 ends the list, anything else is code, length, data.
  // A truncated option ends the parse.
  size_t pos = DHCPBody::SIZE;
  while (pos + 1 < data.size())
  {
    const u8 code = data[pos];
    if (code == 0)
    {
      ++pos;
      continue;
    }
    if (code == 255)
      break;

    const size_t next = pos + 2 + data[pos + 1];
    if (next > data.size())
      break;
    options.emplace_back(data.begin() + pos, data.begin() + next);
    pos = next;
  }
}

void DHCPPacket::AddOption(u8 fnc, std::span<const u8> params)
{
  if (params.size() > 0xFF)
    return;
  std::vector<u8> option;
  option.reserve(params.size() + 2);
  option.push_back(fnc);
  option.push_back(static_cast<u8>(params.size()));
  option.insert(option.end(), params.begin(), params.end());
  options.push_back(std::move(option));
}

std::vector<u8> DHCPPacket::Build() const
{
  std::vector<u8> out;
  AppendRaw(&out, body);
  for (const std::vector<u8>& option : options)
    out.insert(out.end(), option.begin(), option.end());
  // End option, padded.
  out.insert(out.end(), {u8{255}, u8{0}, u8{0}, u8{0}});
  return out;
}

u16 ComputeNetworkChecksum(const void* data, u16 length, u32 initial_value)
{
  // One's complement sum of big-endian 16-bit words; an odd final byte is the high half.
  const auto* bytes = static_cast<const u8*>(data);
  u32 sum = initial_value;
  for (u16 i = 0; i < length; ++i)
    sum += (i % 2 == 0) ? u32{bytes[i]} << 8 : u32{bytes[i]};
  return static_cast<u16>(~FoldChecksum(sum));
}

u16 ComputeTCPNetworkChecksum(const IPAddress& from, const IPAddress& to, const void* data,
                              u16 length, u8 protocol)
{
  const u32 initial = PseudoHeaderSum(HostIPValue(from), HostIPValue(to), protocol, length);
  return htons(ComputeNetworkChecksum(data, length, initial));
}

ARPPacket::ARPPacket() = default;

ARPPacket::ARPPacket(const MACAddress& destination, const MACAddress& source)
    : eth_header(destination, source, ARP_ETHERTYPE)
{
}

u16 ARPPacket::Size() const
{
  return static_cast<u16>(SIZE);
}

std::vector<u8> ARPPacket::Build() const
{
  std::vector<u8> out;
  out.reserve(SIZE);
  AppendRaw(&out, eth_header);
  AppendRaw(&out, arp_header);
  return out;
}

TCPPacket::TCPPacket() = default;

TCPPacket::TCPPacket(const MACAddress& destination, const MACAddress& source,
                     const sockaddr_in& from, const sockaddr_in& to, u32 seq, u32 ack, u16 flags)
    : eth_header(destination, source, IPV4_ETHERTYPE),
      ip_header(static_cast<u16>(TCPHeader::SIZE), IPPROTO_TCP, from, to),
      tcp_header(from, to, seq, ack, flags)
{
}

std::vector<u8> TCPPacket::Build() const
{
  std::vector<u8> out;
  out.reserve(MIN_SIZE + ipv4_options.size() + tcp_options.size() + data.size());

  AppendRaw(&out, eth_header);
  const size_t ip_at = out.size();
  AppendRaw(&out, ip_header);
  out.insert(out.end(), ipv4_options.begin(), ipv4_options.end());
  const size_t tcp_at = out.size();
  AppendRaw(&out, tcp_header);
  out.insert(out.end(), tcp_options.begin(), tcp_options.end());
  out.insert(out.end(), data.begin(), data.end());

  // Data offset in the top four bits of the properties word, flags kept.
  const u16 tcp_length = static_cast<u16>(TCPHeader::SIZE + tcp_options.size() + data.size());
  const u16 header_words = static_cast<u16>((TCPHeader::SIZE + tcp_options.size()) & 0x3c) / 4;
  const u16 properties =
      static_cast<u16>((ntohs(tcp_header.properties) & 0x0fff) | (header_words << 12));
  WriteRaw16(&out, tcp_at + offsetof(TCPHeader, properties), htons(properties));

  const u16 ip_length = static_cast<u16>(IPv4Header::SIZE + ipv4_options.size());
  WriteRaw16(&out, ip_at + offsetof(IPv4Header, total_len),
             htons(static_cast<u16>(ip_length + tcp_length)));

  WriteRaw16(&out, ip_at + offsetof(IPv4Header, header_checksum), 0);
  WriteRaw16(&out, ip_at + offsetof(IPv4Header, header_checksum),
             htons(ComputeNetworkChecksum(out.data() + ip_at, ip_length)));

  WriteRaw16(&out, tcp_at + offsetof(TCPHeader, checksum), 0);
  WriteRaw16(&out, tcp_at + offsetof(TCPHeader, checksum),
             ComputeTCPNetworkChecksum(ip_header.source_addr, ip_header.destination_addr,
                                       out.data() + tcp_at, tcp_length, IPPROTO_TCP));
  return out;
}

u16 TCPPacket::Size() const
{
  return static_cast<u16>(MIN_SIZE + data.size() + ipv4_options.size() + tcp_options.size());
}

UDPPacket::UDPPacket() = default;

UDPPacket::UDPPacket(const MACAddress& destination, const MACAddress& source,
                     const sockaddr_in& from, const sockaddr_in& to, const std::vector<u8>& payload)
    : eth_header(destination, source, IPV4_ETHERTYPE),
      ip_header(static_cast<u16>(payload.size() + UDPHeader::SIZE), IPPROTO_UDP, from, to),
      udp_header(from, to, static_cast<u16>(payload.size())), data(payload)
{
}

std::vector<u8> UDPPacket::Build() const
{
  std::vector<u8> out;
  out.reserve(MIN_SIZE + ipv4_options.size() + data.size());

  AppendRaw(&out, eth_header);
  const size_t ip_at = out.size();
  AppendRaw(&out, ip_header);
  out.insert(out.end(), ipv4_options.begin(), ipv4_options.end());
  const size_t udp_at = out.size();
  AppendRaw(&out, udp_header);
  out.insert(out.end(), data.begin(), data.end());

  const u16 udp_length = static_cast<u16>(UDPHeader::SIZE + data.size());
  WriteRaw16(&out, udp_at + offsetof(UDPHeader, length), htons(udp_length));

  const u16 ip_length = static_cast<u16>(IPv4Header::SIZE + ipv4_options.size());
  WriteRaw16(&out, ip_at + offsetof(IPv4Header, total_len),
             htons(static_cast<u16>(ip_length + udp_length)));

  WriteRaw16(&out, ip_at + offsetof(IPv4Header, header_checksum), 0);
  WriteRaw16(&out, ip_at + offsetof(IPv4Header, header_checksum),
             htons(ComputeNetworkChecksum(out.data() + ip_at, ip_length)));

  WriteRaw16(&out, udp_at + offsetof(UDPHeader, checksum), 0);
  WriteRaw16(&out, udp_at + offsetof(UDPHeader, checksum),
             ComputeTCPNetworkChecksum(ip_header.source_addr, ip_header.destination_addr,
                                       out.data() + udp_at, udp_length, IPPROTO_UDP));
  return out;
}

u16 UDPPacket::Size() const
{
  return static_cast<u16>(MIN_SIZE + data.size() + ipv4_options.size());
}

PacketView::PacketView(const u8* ptr, std::size_t size) : m_ptr(ptr), m_size(size)
{
}

std::optional<u16> PacketView::GetEtherType() const
{
  if (m_size < EthernetHeader::SIZE)
    return std::nullopt;
  return ntohs(ReadRaw<u16>(m_ptr + offsetof(EthernetHeader, ethertype)));
}

std::optional<ARPPacket> PacketView::GetARPPacket() const
{
  if (m_size < ARPPacket::SIZE)
    return std::nullopt;
  return ReadRaw<ARPPacket>(m_ptr);
}

std::optional<u8> PacketView::GetIPProto() const
{
  if (m_size < EthernetHeader::SIZE + IPv4Header::SIZE)
    return std::nullopt;
  return m_ptr[EthernetHeader::SIZE + offsetof(IPv4Header, protocol)];
}

std::optional<TCPPacket> PacketView::GetTCPPacket() const
{
  if (m_size < TCPPacket::MIN_SIZE)
    return std::nullopt;

  TCPPacket packet;
  packet.eth_header = ReadRaw<EthernetHeader>(m_ptr);
  packet.ip_header = ReadRaw<IPv4Header>(m_ptr + EthernetHeader::SIZE);

  const size_t ip_end = EthernetHeader::SIZE + IPv4Header::SIZE;
  const size_t tcp_at = EthernetHeader::SIZE + packet.ip_header.DefinedSize();
  // An IHL below 5 cannot be a valid header.
  if (tcp_at < ip_end || m_size < tcp_at + TCPHeader::SIZE)
    return std::nullopt;
  packet.tcp_header = ReadRaw<TCPHeader>(m_ptr + tcp_at);

  const size_t data_at = tcp_at + packet.tcp_header.GetHeaderSize();
  const size_t end = EthernetHeader::SIZE + ntohs(packet.ip_header.total_len);
  // Neither can a TCP data offset below 5.
  if (data_at < tcp_at + TCPHeader::SIZE || m_size < end || end < data_at)
    return std::nullopt;

  packet.ipv4_options.assign(m_ptr + ip_end, m_ptr + tcp_at);
  packet.tcp_options.assign(m_ptr + tcp_at + TCPHeader::SIZE, m_ptr + data_at);
  packet.data.assign(m_ptr + data_at, m_ptr + end);
  return packet;
}

std::optional<UDPPacket> PacketView::GetUDPPacket() const
{
  if (m_size < UDPPacket::MIN_SIZE)
    return std::nullopt;

  UDPPacket packet;
  packet.eth_header = ReadRaw<EthernetHeader>(m_ptr);
  packet.ip_header = ReadRaw<IPv4Header>(m_ptr + EthernetHeader::SIZE);

  const size_t ip_end = EthernetHeader::SIZE + IPv4Header::SIZE;
  const size_t udp_at = EthernetHeader::SIZE + packet.ip_header.DefinedSize();
  if (udp_at < ip_end || m_size < udp_at + UDPHeader::SIZE)
    return std::nullopt;
  packet.udp_header = ReadRaw<UDPHeader>(m_ptr + udp_at);

  // The UDP length counts the UDP header and payload.
  const size_t data_at = udp_at + UDPHeader::SIZE;
  const size_t end = udp_at + ntohs(packet.udp_header.length);
  if (m_size < end || end < data_at)
    return std::nullopt;

  packet.ipv4_options.assign(m_ptr + ip_end, m_ptr + udp_at);
  packet.data.assign(m_ptr + data_at, m_ptr + end);
  return packet;
}

NetworkErrorState SaveNetworkErrorState()
{
  NetworkErrorState state{};
  state.error = errno;
#ifdef _WIN32
  state.wsa_error = WSAGetLastError();
#endif
  return state;
}

void RestoreNetworkErrorState(const NetworkErrorState& state)
{
  errno = state.error;
#ifdef _WIN32
  WSASetLastError(state.wsa_error);
#endif
}

const char* DecodeNetworkError(s32 error_code)
{
  thread_local char message[1024];
#ifdef _WIN32
  const DWORD written = FormatMessageA(
      FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
      nullptr, static_cast<DWORD>(error_code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), message,
      sizeof(message), nullptr);
  if (written == 0)
    std::snprintf(message, sizeof(message), "Unknown error %d", error_code);
  return message;
#else
  return StrErrorWrapper(error_code, message, sizeof(message));
#endif
}

const char* StrNetworkError()
{
#ifdef _WIN32
  return DecodeNetworkError(WSAGetLastError());
#else
  return DecodeNetworkError(errno);
#endif
}

bool SetPlatformSocketOptions(int fd [[maybe_unused]])
{
#ifdef SO_NOSIGPIPE
  const int enable = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable)) < 0)
  {
    ERROR_LOG_FMT(COMMON, "Could not set SO_NOSIGPIPE: {}", StrNetworkError());
    return false;
  }
#endif
  return true;
}
}  // namespace Common
