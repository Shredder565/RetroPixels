// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/NetworkCaptureLogger.h"

#include <ctime>
#include <string>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Network.h"
#include "Common/PcapFile.h"
#include "Common/ScopeGuard.h"
#include "Common/TimeUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"

namespace Core
{
namespace
{
std::string DumpPath(std::string_view suffix)
{
  return File::GetUserPath(D_DUMPSSL_IDX) + SConfig::GetInstance().GetGameID() + std::string(suffix);
}

void AppendToFile(const std::string& path, const void* data, std::size_t length)
{
  File::IOFile(path, "ab").WriteBytes(data, length);
}

template <typename T>
void AppendBytes(std::vector<u8>* packet, const T* object, std::size_t size)
{
  const auto* bytes = reinterpret_cast<const u8*>(object);
  packet->insert(packet->end(), bytes, bytes + size);
}
}  // namespace

NetworkCaptureLogger::NetworkCaptureLogger() = default;
NetworkCaptureLogger::~NetworkCaptureLogger() = default;

void DummyNetworkCaptureLogger::OnNewSocket(s32)
{
}

void DummyNetworkCaptureLogger::LogSSLRead(const void*, std::size_t, s32)
{
}

void DummyNetworkCaptureLogger::LogSSLWrite(const void*, std::size_t, s32)
{
}

void DummyNetworkCaptureLogger::LogRead(const void*, std::size_t, s32, sockaddr*)
{
}

void DummyNetworkCaptureLogger::LogWrite(const void*, std::size_t, s32, sockaddr*)
{
}

void DummyNetworkCaptureLogger::LogBBA(const void*, std::size_t)
{
}

NetworkCaptureType DummyNetworkCaptureLogger::GetCaptureType() const
{
  return NetworkCaptureType::None;
}

void BinarySSLCaptureLogger::LogSSLRead(const void* data, std::size_t length, s32)
{
  if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ))
    AppendToFile(DumpPath("_read.bin"), data, length);
}

void BinarySSLCaptureLogger::LogSSLWrite(const void* data, std::size_t length, s32)
{
  if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE))
    AppendToFile(DumpPath("_write.bin"), data, length);
}

NetworkCaptureType BinarySSLCaptureLogger::GetCaptureType() const
{
  return NetworkCaptureType::Raw;
}

PCAPSSLCaptureLogger::PCAPSSLCaptureLogger()
{
  // "<dump dir><game id> <local date and time>.pcap"
  const std::string path =
      fmt::format("{}{} {:%Y-%m-%d %Hh%Mm%Ss}.pcap", File::GetUserPath(D_DUMPSSL_IDX),
                  SConfig::GetInstance().GetGameID(), *Common::LocalTime(std::time(nullptr)));
  m_pcap = std::make_unique<Common::PCAP>(new File::IOFile(path, "wb", File::SharedAccess::Read),
                                          Common::PCAP::LinkType::Ethernet);
}

PCAPSSLCaptureLogger::~PCAPSSLCaptureLogger() = default;

void PCAPSSLCaptureLogger::OnNewSocket(s32 socket)
{
  m_incoming_sequence[socket] = 0;
  m_outgoing_sequence[socket] = 0;
}

void PCAPSSLCaptureLogger::LogSSLRead(const void* data, std::size_t length, s32 socket)
{
  if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_READ))
    LogSocketData(Direction::Incoming, data, length, socket, nullptr);
}

void PCAPSSLCaptureLogger::LogSSLWrite(const void* data, std::size_t length, s32 socket)
{
  if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_WRITE))
    LogSocketData(Direction::Outgoing, data, length, socket, nullptr);
}

void PCAPSSLCaptureLogger::LogRead(const void* data, std::size_t length, s32 socket, sockaddr* from)
{
  LogSocketData(Direction::Incoming, data, length, socket, from);
}

void PCAPSSLCaptureLogger::LogWrite(const void* data, std::size_t length, s32 socket, sockaddr* to)
{
  LogSocketData(Direction::Outgoing, data, length, socket, to);
}

void PCAPSSLCaptureLogger::LogBBA(const void* data, std::size_t length)
{
  if (!Config::Get(Config::MAIN_NETWORK_DUMP_BBA))
    return;

  // The adapter's receive and send paths run on different threads.
  std::lock_guard lock(m_bba_mutex);
  m_pcap->AddPacket(static_cast<const u8*>(data), length);
}

void PCAPSSLCaptureLogger::LogSocketData(Direction direction, const void* data,
                                         std::size_t length, s32 socket, sockaddr* peer)
{
  // The socket queries below must not change the error the game sees for its own call.
  const auto saved_error = Common::SaveNetworkErrorState();
  Common::ScopeGuard restore_error([&saved_error] { Common::RestoreNetworkErrorState(saved_error); });

  sockaddr_in local;
  socklen_t local_length = sizeof(local);
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &local_length) != 0)
    return;

  sockaddr_in connected_peer;
  socklen_t peer_length = sizeof(connected_peer);
  if (peer == nullptr &&
      getpeername(socket, reinterpret_cast<sockaddr*>(&connected_peer), &peer_length) != 0)
  {
    return;
  }
  const sockaddr_in& remote = peer != nullptr ? *reinterpret_cast<sockaddr_in*>(peer) : connected_peer;

  const auto* bytes = static_cast<const u8*>(data);
  const auto size = static_cast<u16>(length);
  if (direction == Direction::Incoming)
    WritePacket(direction, bytes, size, socket, remote, local);
  else
    WritePacket(direction, bytes, size, socket, local, remote);
}

void PCAPSSLCaptureLogger::WritePacket(Direction direction, const u8* data, u16 length, s32 socket,
                                       const sockaddr_in& source, const sockaddr_in& destination)
{
  int type = 0;
  socklen_t type_length = sizeof(type);
  if (getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &type_length) != 0)
    return;
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    return;

  std::vector<u8> packet;

  // The console's MAC address goes on its side of the frame.
  Common::EthernetHeader ethernet(0x800);
  if (const auto mac = Common::StringToMacAddress(Config::Get(Config::MAIN_WIRELESS_MAC)))
  {
    if (direction == Direction::Outgoing)
      ethernet.source = *mac;
    else
      ethernet.destination = *mac;
  }
  AppendBytes(&packet, &ethernet, ethernet.Size());

  if (type == SOCK_STREAM)
  {
    auto& sequence = direction == Direction::Incoming ? m_incoming_sequence : m_outgoing_sequence;
    u32& next = sequence[socket];
    const Common::TCPHeader tcp(source, destination, next, data, length);
    next += length;
    const Common::IPv4Header ip(tcp.Size() + length, tcp.IPProto(), source, destination);
    AppendBytes(&packet, &ip, ip.Size());
    AppendBytes(&packet, &tcp, tcp.Size());
  }
  else
  {
    const Common::UDPHeader udp(source, destination, length);
    const Common::IPv4Header ip(udp.Size() + length, udp.IPProto(), source, destination);
    AppendBytes(&packet, &ip, ip.Size());
    AppendBytes(&packet, &udp, udp.Size());
  }

  packet.insert(packet.end(), data, data + length);
  m_pcap->AddPacket(packet.data(), packet.size());
}

NetworkCaptureType PCAPSSLCaptureLogger::GetCaptureType() const
{
  return NetworkCaptureType::PCAP;
}
}  // namespace Core
