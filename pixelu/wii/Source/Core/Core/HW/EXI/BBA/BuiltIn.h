// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

#include <array>
#include <vector>

#include <SFML/Network.hpp>
#include "Common/CommonTypes.h"
#include "Common/Network.h"

// Pieces of the broadband adapter's built-in network stack (the "HLE" BBA). The console talks
// to a made-up router; its UDP and TCP traffic is relayed through host sockets.
namespace ExpansionInterface
{
struct BbaConnection;

// A TCP socket that can be bound to the host address the console's traffic should leave from,
// so that peers always see the same source address.
class BbaTcpSocket : public sf::TcpSocket
{
public:
  enum class ConnectingState
  {
    None,
    Connecting,
    Connected,
    Error
  };

  // Starts a non-blocking connect from net_ip (network byte order).
  sf::Socket::Status Connect(const sf::IpAddress& dest, u16 port, u32 net_ip);
  sf::Socket::Status GetPeerName(sockaddr_in* addr) const;
  sf::Socket::Status GetSockName(sockaddr_in* addr) const;

  // Checks on a pending connect. When it completes, the SYN-ACK for the console is put in the
  // connection's first retransmission slot.
  ConnectingState Connected(BbaConnection* connection);

private:
  ConnectingState m_state = ConnectingState::None;
};

// A UDP socket bound to a given host address. The SSDP port also joins the SSDP multicast group.
class BbaUdpSocket : public sf::UdpSocket
{
public:
  sf::Socket::Status Bind(u16 port, u32 net_ip);
};

// A TCP segment sent to the console and not acknowledged yet.
struct BbaTcpBuffer
{
  bool used = false;
  u64 tick = 0;
  u32 seq_id = 0;
  std::vector<u8> data;
};

// One relayed UDP port or TCP connection. Addresses and ports are in network byte order; "from"
// is the remote side and "to" the console.
struct BbaConnection
{
  static constexpr std::size_t RETRANSMIT_SLOTS = 4;

  u32 ip = 0;  // 0 marks a free slot
  u16 local = 0;
  u16 remote = 0;
  u16 type = 0;  // IPPROTO_TCP or IPPROTO_UDP
  u32 seq_num = 0;
  u32 ack_num = 0;
  u32 ack_base = 0;
  u16 window_size = 0;
  u64 delay = 0;
  std::array<BbaTcpBuffer, RETRANSMIT_SLOTS> tcp_buffers{};
  bool ready = false;
  sockaddr_in from{};
  sockaddr_in to{};
  Common::MACAddress bba_mac{};
  Common::MACAddress my_mac{};
  BbaUdpSocket udp_socket;
  BbaTcpSocket tcp_socket;
  u64 poke_time = 0;
};

// Up to ten at once; the GameCube network library allows eight sockets.
class BbaConnections
{
public:
  BbaConnection* begin() { return m_slots.data(); }
  BbaConnection* end() { return m_slots.data() + m_slots.size(); }

  // The active connection on this local port, else a free slot (port 0: only a free slot).
  BbaConnection* ForLocalPort(u16 port);
  BbaConnection* FindTcp(u16 local_port, u16 remote_port, u32 ip);
  // Closes every socket and frees every slot.
  void CloseAll();

private:
  std::array<BbaConnection, 10> m_slots;
};
}  // namespace ExpansionInterface
