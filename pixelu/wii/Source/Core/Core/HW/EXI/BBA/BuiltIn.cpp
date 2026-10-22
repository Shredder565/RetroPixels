// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/BBA/BuiltIn.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>

#include "SFML/Network/IpAddress.hpp"
#include "SFML/Network/Socket.hpp"

#ifdef _WIN32
#include <ws2ipdef.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "Common/BitUtils.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

// The "built-in" network backend. Instead of passing frames to a real network, the adapter
// talks to a small emulated LAN: a router at x.y.z.1 that answers ARP and DHCP, and relays the
// console's UDP datagrams and TCP streams through ordinary host sockets. Remote peers appear to
// the console as hosts on that LAN, each with a made-up MAC address.

namespace ExpansionInterface
{
namespace
{
// TCP header flag bits.
constexpr u16 TCP_FIN = 0x01;
constexpr u16 TCP_SYN = 0x02;
constexpr u16 TCP_RST = 0x04;
constexpr u16 TCP_PSH = 0x08;
constexpr u16 TCP_ACK = 0x10;

// Largest payloads read from the host sockets for one frame.
constexpr std::size_t UDP_READ_SIZE = 1500;
constexpr std::size_t TCP_READ_SIZE = 440;

// Our side of every relayed TCP stream starts at this sequence number.
constexpr u32 INITIAL_SEQUENCE = 0x1000000;
// Unacknowledged segments are sent again after this long.
constexpr u64 RETRANSMIT_AFTER_MS = 1000;
// Remote closes are only passed on once a connection is this old.
constexpr u64 CLOSE_GRACE_MS = 3000;
// The console is fed more stream data once this long has passed since the last segment, or
// right away while its advertised window is larger than WIDE_WINDOW.
constexpr u64 FEED_INTERVAL_MS = 100;
constexpr u16 WIDE_WINDOW = 2000;
// The receive ring has 16 pages; the reader waits while this many hold unread frames.
constexpr u32 BUSY_PAGES = 8;
// Frames shorter than the Ethernet minimum are padded with zeros.
constexpr std::size_t MIN_FRAME_SIZE = 64;

constexpr u16 DHCP_SERVER_PORT = 67;
constexpr u16 DNS_PORT = 53;
// Larger SSDP datagrams sent while the SSDP port could not be bound are reflected back.
constexpr u16 SSDP_REFLECT_LENGTH = 150;

// Ports some games listen on without ever sending from them first.
constexpr u16 KIRBY_AIR_RIDE_PORT = 26502;
constexpr u16 MKDD_AND_1080_PORT = 26512;

u64 NowMs()
{
  const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count());
}

u32 AsU32(const Common::IPAddress& address)
{
  return std::bit_cast<u32>(address);
}

// A header-only segment from the remote peer to the console.
std::vector<u8> Segment(const BbaConnection& c, u16 flags)
{
  return Common::TCPPacket(c.bba_mac, c.my_mac, c.from, c.to, c.seq_num, c.ack_num, flags).Build();
}

// The segment that tears a stream down. Nothing that was waiting for an ACK is resent after it.
std::vector<u8> TearDown(BbaConnection& c)
{
  std::vector<u8> frame = Segment(c, TCP_FIN | TCP_ACK | TCP_RST);
  for (BbaTcpBuffer& pending : c.tcp_buffers)
    pending.used = false;
  return frame;
}

void ForgetPending(BbaConnection& c)
{
  for (BbaTcpBuffer& pending : c.tcp_buffers)
    pending.used = false;
}

// Every frame given to the console gets the next IP identification, so the IPv4 header's ID
// and checksum are rewritten in place.
void StampIpIdentification(std::vector<u8>& frame, u16 id)
{
  constexpr std::size_t ip_offset = Common::EthernetHeader::SIZE;
  if (frame.size() < ip_offset + Common::IPv4Header::SIZE)
    return;

  u8* const ip = frame.data() + ip_offset;
  const u8 header_length = static_cast<u8>((ip[0] & 0x0f) * 4);
  if (frame.size() < ip_offset + header_length)
    return;

  Common::BitCastPtr<u16>(ip + offsetof(Common::IPv4Header, identification)) = htons(id);
  auto checksum = Common::BitCastPtr<u16>(ip + offsetof(Common::IPv4Header, header_checksum));
  checksum = u16(0);
  checksum = htons(Common::ComputeNetworkChecksum(ip, header_length));
}

sockaddr_in Endpoint(u32 net_ip, u16 net_port)
{
  sockaddr_in endpoint{};
  endpoint.sin_family = IPPROTO_UDP;
  endpoint.sin_addr.s_addr = net_ip;
  endpoint.sin_port = net_port;
  return endpoint;
}

// Waits for nothing: reports whether a socket is writable or has an exception pending.
// Returns nullopt if select itself failed.
std::optional<bool> ConnectFinished(sf::SocketHandle handle)
{
  fd_set readable;
  fd_set writable;
  fd_set failed;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&failed);
  FD_SET(handle, &writable);
  FD_SET(handle, &failed);
  timeval no_wait{};
  // The first argument is ignored on Windows.
  if (select(static_cast<int>(handle) + 1, &readable, &writable, &failed, &no_wait) < 0)
    return std::nullopt;
  return FD_ISSET(handle, &writable) != 0 || FD_ISSET(handle, &failed) != 0;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Socket helpers

sf::Socket::Status BbaTcpSocket::Connect(const sf::IpAddress& dest, u16 port, u32 net_ip)
{
  // Pin the source address before connecting; a failure leaves the choice to the system.
  sockaddr_in source{};
  source.sin_family = AF_INET;
  source.sin_addr.s_addr = net_ip;
  source.sin_port = 0;
  (void)::bind(getNativeHandle(), reinterpret_cast<const sockaddr*>(&source), sizeof(source));

  m_state = ConnectingState::Connecting;
  return connect(dest, port);
}

sf::Socket::Status BbaTcpSocket::GetPeerName(sockaddr_in* addr) const
{
  socklen_t length = sizeof(*addr);
  if (::getpeername(getNativeHandle(), reinterpret_cast<sockaddr*>(addr), &length) == 0)
    return sf::Socket::Status::Done;
  ERROR_LOG_FMT(SP1, "getpeername failed: {}", Common::StrNetworkError());
  return sf::Socket::Status::Error;
}

sf::Socket::Status BbaTcpSocket::GetSockName(sockaddr_in* addr) const
{
  socklen_t length = sizeof(*addr);
  if (::getsockname(getNativeHandle(), reinterpret_cast<sockaddr*>(addr), &length) == 0)
    return sf::Socket::Status::Done;
  ERROR_LOG_FMT(SP1, "getsockname failed: {}", Common::StrNetworkError());
  return sf::Socket::Status::Error;
}

BbaTcpSocket::ConnectingState BbaTcpSocket::Connected(BbaConnection* connection)
{
  if (m_state != ConnectingState::Connecting)
    return m_state;

  const sf::SocketHandle handle = getNativeHandle();
  const std::optional<bool> finished = ConnectFinished(handle);
  if (!finished)
  {
    ERROR_LOG_FMT(SP1, "Failed to get BBA socket connection state: {}", Common::StrNetworkError());
    return m_state;
  }
  if (!*finished)
    return m_state;

  s32 socket_error = 0;
  socklen_t error_length = sizeof(socket_error);
  if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error),
                   &error_length) != 0)
  {
    ERROR_LOG_FMT(SP1, "Failed to get BBA socket error state: {}", Common::StrNetworkError());
    m_state = ConnectingState::Error;
    return m_state;
  }
  if (socket_error != 0)
  {
    ERROR_LOG_FMT(SP1, "BBA connect failed (err={}): {}", socket_error,
                  Common::DecodeNetworkError(socket_error));
    m_state = ConnectingState::Error;
    return m_state;
  }

  // Writable is not proof of a connection; having a peer is.
  sockaddr_in peer{};
  socklen_t peer_length = sizeof(peer);
  if (::getpeername(handle, reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0)
  {
    ERROR_LOG_FMT(SP1, "BBA connect failed to get peername: {}", Common::StrNetworkError());
    m_state = ConnectingState::Error;
    return m_state;
  }

  m_state = ConnectingState::Connected;
  INFO_LOG_FMT(SP1, "BBA connect succeeded");

  // Answer the console's SYN. It waits in the first retransmission slot, aged so that it goes
  // out about 100 ms from now and is resent until the console acknowledges it.
  Common::TCPPacket syn_ack(connection->bba_mac, connection->my_mac, connection->from,
                            connection->to, connection->seq_num, connection->ack_num,
                            TCP_SYN | TCP_ACK);
  syn_ack.tcp_options = {0x02, 0x04, 0x05, 0xb4,   // MSS 1460
                         0x01, 0x01, 0x01, 0x01};  // padding
  BbaTcpBuffer& slot = connection->tcp_buffers[0];
  slot.data = syn_ack.Build();
  slot.seq_id = connection->seq_num;
  slot.tick = NowMs() - (RETRANSMIT_AFTER_MS - 100);
  slot.used = true;
  ++connection->seq_num;
  return m_state;
}

sf::Socket::Status BbaUdpSocket::Bind(u16 port, u32 net_ip)
{
  if (port != Common::SSDP_PORT)
    return bind(port, sf::IpAddress(ntohl(net_ip)));

  // SSDP shares its port with other programs and needs the multicast group, so the socket is
  // set up by hand (sf::UdpSocket::bind would recreate it and lose the options).
  create();
  const sf::SocketHandle handle = getNativeHandle();
  const auto set_option = [handle](int level, int name, const void* value, int size) {
    return ::setsockopt(handle, level, name, static_cast<const char*>(value),
                        static_cast<socklen_t>(size)) == 0;
  };

  const int enable = 1;
  if (!set_option(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)))
    ERROR_LOG_FMT(SP1, "setsockopt failed to reuse SSDP address: {}", Common::StrNetworkError());
#ifdef SO_REUSEPORT
  if (!set_option(SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)))
    ERROR_LOG_FMT(SP1, "setsockopt failed to reuse SSDP port: {}", Common::StrNetworkError());
#endif
  const char loopback = 1;
  if (!set_option(IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback)))
    ERROR_LOG_FMT(SP1, "setsockopt failed to set SSDP loopback: {}", Common::StrNetworkError());

  Common::ScopeGuard close_on_failure([this] { close(); });

  // Any address is preferred; the console's host address is the fallback.
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(Common::SSDP_PORT);
  local.sin_addr.s_addr = INADDR_ANY;
  const auto bind_local = [&] {
    return ::bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
  };
  const bool bound_any = bind_local();
  local.sin_addr.s_addr = net_ip;
  if (!bound_any)
  {
    WARN_LOG_FMT(SP1, "bind with SSDP port and INADDR_ANY failed: {}", Common::StrNetworkError());
    if (!bind_local())
    {
      ERROR_LOG_FMT(SP1, "bind with SSDP port failed: {}", Common::StrNetworkError());
      return sf::Socket::Status::Error;
    }
  }
  INFO_LOG_FMT(SP1, "SSDP bind successful");

  // Multicast leaves through the console's host interface.
  if (!set_option(IPPROTO_IP, IP_MULTICAST_IF, &local.sin_addr, sizeof(local.sin_addr)))
  {
    ERROR_LOG_FMT(SP1, "setsockopt failed to bind to the network interface: {}",
                  Common::StrNetworkError());
    return sf::Socket::Status::Error;
  }

  // Only the SSDP group is joined; the emulated LAN has no other multicast.
  ip_mreq membership{};
  membership.imr_multiaddr.s_addr = AsU32(Common::IP_ADDR_SSDP);
  membership.imr_interface.s_addr = net_ip;
  if (!set_option(IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)))
  {
    ERROR_LOG_FMT(SP1, "setsockopt failed to subscribe to SSDP multicast group: {}",
                  Common::StrNetworkError());
    return sf::Socket::Status::Error;
  }

  close_on_failure.Dismiss();
  INFO_LOG_FMT(SP1, "SSDP multicast membership successful");
  return sf::Socket::Status::Done;
}

BbaConnection* BbaConnections::ForLocalPort(u16 port)
{
  const auto in_use_on_port = [port](const BbaConnection& c) {
    return c.ip != 0 && c.local == port;
  };
  const auto is_free = [](const BbaConnection& c) { return c.ip == 0; };

  if (port != 0)
  {
    if (const auto it = std::ranges::find_if(m_slots, in_use_on_port); it != m_slots.end())
      return &*it;
  }
  const auto it = std::ranges::find_if(m_slots, is_free);
  return it != m_slots.end() ? &*it : nullptr;
}

BbaConnection* BbaConnections::FindTcp(u16 local_port, u16 remote_port, u32 ip)
{
  const auto it = std::ranges::find_if(m_slots, [&](const BbaConnection& c) {
    return c.ip == ip && c.remote == remote_port && c.local == local_port;
  });
  return it != m_slots.end() ? &*it : nullptr;
}

void BbaConnections::CloseAll()
{
  for (BbaConnection& c : m_slots)
  {
    if (c.ip != 0)
    {
      if (c.type == IPPROTO_TCP)
        c.tcp_socket.disconnect();
      else
        c.udp_socket.unbind();
    }
    c.ip = 0;
  }
}

// ---------------------------------------------------------------------------------------------
// Backend

bool CEXIETHERNET::BuiltInBBAInterface::Activate()
{
  if (IsActivated())
    return true;

  m_active = true;
  m_stop_reading.Clear();
  for (std::vector<u8>& slot : m_queue)
    slot.reserve(BBA_RECV_SIZE);

  // The host address the console's traffic leaves from: the configured one if it parses, else
  // a guess at the machine's address.
  const sf::IpAddress host =
      sf::IpAddress::resolve(m_local_ip)
          .value_or(sf::IpAddress::getLocalAddress().value_or(sf::IpAddress::Any));
  m_console_ip = htonl(host.toInteger());
  m_console_mac = m_eth_ref->MacAddress();
  m_arp[m_console_ip] = m_console_mac;

  // The router takes .1 on the console's /24.
  m_gateway_ip = (m_console_ip & 0x00ffffff) | 0x01000000;
  m_gateway_mac = Common::GenerateMacAddress(Common::MACConsumer::BBA);
  m_arp[m_gateway_ip] = m_gateway_mac;

  m_connections.CloseAll();

  // Remote UPnP clients connect to the SSDP port over TCP.
  (void)m_upnp_listener.listen(Common::SSDP_PORT, host);
  m_upnp_listener.setBlocking(false);

  return RecvInit();
}

void CEXIETHERNET::BuiltInBBAInterface::Deactivate()
{
  if (!IsActivated())
    return;

  m_receiving.Clear();
  m_stop_reading.Set();
  m_active = false;
  if (m_read_thread.joinable())
    m_read_thread.join();

  m_connections.CloseAll();
  m_arp.clear();
  m_upnp_listener.close();
}

bool CEXIETHERNET::BuiltInBBAInterface::IsActivated()
{
  return m_active;
}

bool CEXIETHERNET::BuiltInBBAInterface::RecvInit()
{
  m_read_thread = std::thread(&BuiltInBBAInterface::ReadLoop, this);
  return true;
}

void CEXIETHERNET::BuiltInBBAInterface::RecvStart()
{
  if (m_receiving.IsSet())
    return;
  {
    std::lock_guard lock(m_lock);
    OpenListeningUdpPort(KIRBY_AIR_RIDE_PORT);
    OpenListeningUdpPort(MKDD_AND_1080_PORT);
  }
  m_receiving.Set();
}

void CEXIETHERNET::BuiltInBBAInterface::RecvStop()
{
  m_receiving.Clear();
  std::lock_guard lock(m_lock);
  m_connections.CloseAll();
  m_queue_head = 0;
  m_queue_tail = 0;
}

// ---------------------------------------------------------------------------------------------
// Queue of frames for the console. When it is full the newest slot is overwritten.

void CEXIETHERNET::BuiltInBBAInterface::Enqueue(std::vector<u8> frame)
{
  m_queue[m_queue_tail] = std::move(frame);
  if (QueueFull())
  {
    WARN_LOG_FMT(SP1, "BBA queue overrun, data might be lost");
    return;
  }
  m_queue_tail = static_cast<u8>((m_queue_tail + 1) % QUEUE_LENGTH);
}

bool CEXIETHERNET::BuiltInBBAInterface::QueueFull() const
{
  return (m_queue_tail + 1) % QUEUE_LENGTH == m_queue_head;
}

bool CEXIETHERNET::BuiltInBBAInterface::Dequeue(std::vector<u8>* frame)
{
  if (m_queue_head == m_queue_tail)
    return false;
  *frame = m_queue[m_queue_head];
  m_queue_head = static_cast<u8>((m_queue_head + 1) % QUEUE_LENGTH);
  return true;
}

// ---------------------------------------------------------------------------------------------
// Reader

void CEXIETHERNET::BuiltInBBAInterface::ReadLoop()
{
  while (!m_stop_reading.IsSet())
  {
    // Idle passes yield for a millisecond; busy ones go straight on.
    if (!PumpOnce())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

bool CEXIETHERNET::BuiltInBBAInterface::PumpOnce()
{
  if (!m_receiving.IsSet() || m_eth_ref->ReceivePagesInUse() >= BUSY_PAGES)
    return false;

  std::lock_guard lock(m_lock);

  // Queued frames go first; socket data fills in when the queue is empty.
  std::vector<u8> frame;
  Dequeue(&frame);
  CollectSocketData(&frame);
  AcceptUPnPClient();

  if (frame.empty())
    return false;

  if (frame.size() > BBA_RECV_SIZE)
  {
    ERROR_LOG_FMT(SP1, "Dropping a {}-byte frame, larger than the adapter accepts", frame.size());
    return true;
  }

  const Common::PacketView view(frame.data(), frame.size());
  if (view.GetEtherType() == Common::IPV4_ETHERTYPE)
    StampIpIdentification(frame, ++m_ip_id);
  if (frame.size() < MIN_FRAME_SIZE)
    frame.resize(MIN_FRAME_SIZE, 0);

  m_eth_ref->ReceiveFrame(frame.data(), static_cast<u32>(frame.size()));
  return true;
}

// Resends timed-out TCP segments and reads from every open host socket. The first data read
// becomes *frame if that is still empty; the rest is queued.
void CEXIETHERNET::BuiltInBBAInterface::CollectSocketData(std::vector<u8>* frame)
{
  for (BbaConnection& c : m_connections)
  {
    if (c.ip == 0)
      continue;

    if (c.type == IPPROTO_TCP)
    {
      for (BbaTcpBuffer& pending : c.tcp_buffers)
      {
        if (QueueFull())
          break;
        if (pending.used && NowMs() - pending.tick > RETRANSMIT_AFTER_MS)
        {
          pending.tick = NowMs();
          Enqueue(pending.data);
        }
      }
    }

    if (frame->empty())
    {
      if (std::optional<std::vector<u8>> data = ReceiveFromSocket(c))
        *frame = std::move(*data);
      continue;
    }
    if (QueueFull())
    {
      WARN_LOG_FMT(SP1, "BBA queue might overrun, can't poll more data");
      return;
    }
    if (std::optional<std::vector<u8>> data = ReceiveFromSocket(c))
      Enqueue(std::move(*data));
  }
}

std::optional<std::vector<u8>> CEXIETHERNET::BuiltInBBAInterface::ReceiveFromSocket(BbaConnection& c)
{
  switch (c.type)
  {
  case IPPROTO_UDP:
    return ReceiveUdp(c);
  case IPPROTO_TCP:
    return ReceiveTcp(c);
  default:
    return std::nullopt;
  }
}

std::optional<std::vector<u8>> CEXIETHERNET::BuiltInBBAInterface::ReceiveUdp(BbaConnection& c)
{
  std::array<u8, UDP_READ_SIZE> payload;
  std::size_t received = 0;
  std::optional<sf::IpAddress> sender;
  unsigned short sender_port = 0;
  (void)c.udp_socket.receive(payload.data(), payload.size(), received, sender, sender_port);
  if (received == 0)
    return std::nullopt;

  // The datagram appears to come from the remote host itself.
  const u32 sender_ip = htonl(sender->toInteger());
  c.from.sin_port = htons(sender_port);
  c.from.sin_addr.s_addr = sender_ip;
  c.my_mac = MacFor(sender_ip);
  const std::vector<u8> data(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(received));
  return Common::UDPPacket(c.bba_mac, c.my_mac, c.from, c.to, data).Build();
}

std::optional<std::vector<u8>> CEXIETHERNET::BuiltInBBAInterface::ReceiveTcp(BbaConnection& c)
{
  switch (c.tcp_socket.Connected(&c))
  {
  case BbaTcpSocket::ConnectingState::Connected:
    break;
  case BbaTcpSocket::ConnectingState::Error:
    // The host could not reach the peer: refuse the console's connection.
    Enqueue(Segment(c, TCP_RST | TCP_ACK));
    c.ip = 0;
    c.tcp_socket.disconnect();
    return std::nullopt;
  default:
    return std::nullopt;
  }

  const auto free_slot = std::ranges::find_if(c.tcp_buffers, [](const BbaTcpBuffer& b) {
    return !b.used;
  });
  const bool may_feed = NowMs() - c.poke_time > FEED_INTERVAL_MS || c.window_size > WIDE_WINDOW;

  std::array<u8, TCP_READ_SIZE> payload;
  std::size_t received = 0;
  sf::Socket::Status status = sf::Socket::Status::Done;
  if (free_slot != c.tcp_buffers.end() && c.ready && may_feed)
    status = c.tcp_socket.receive(payload.data(), payload.size(), received);

  if (received != 0)
  {
    Common::TCPPacket segment(c.bba_mac, c.my_mac, c.from, c.to, c.seq_num, c.ack_num,
                              TCP_ACK | TCP_PSH);
    segment.data.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(received));

    // Kept until the console acknowledges it.
    free_slot->seq_id = c.seq_num;
    free_slot->tick = NowMs();
    free_slot->data = segment.Build();
    free_slot->used = true;
    c.seq_num += static_cast<u32>(received);
    c.poke_time = NowMs();
    return free_slot->data;
  }

  const bool closed =
      status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error;
  if (closed && NowMs() - c.delay > CLOSE_GRACE_MS)
  {
    c.ip = 0;
    c.tcp_socket.disconnect();
    return TearDown(c);
  }
  return std::nullopt;
}

// A remote host connecting to the SSDP port over TCP is offered to the console as a new
// incoming connection.
void CEXIETHERNET::BuiltInBBAInterface::AcceptUPnPClient()
{
  BbaConnection* const c = m_connections.ForLocalPort(0);
  if (c == nullptr || m_upnp_listener.accept(c->tcp_socket) != sf::Socket::Status::Done)
    return;

  if (c->tcp_socket.GetPeerName(&c->from) != sf::Socket::Status::Done ||
      c->tcp_socket.GetSockName(&c->to) != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(SP1, "Failed to accept new UPnP client: {}", Common::StrNetworkError());
    return;
  }
  if (c->from.sin_addr.s_addr == m_console_ip)
  {
    c->tcp_socket.disconnect();
    WARN_LOG_FMT(SP1, "Ignoring UPnP request to itself");
    return;
  }

  c->ip = c->from.sin_addr.s_addr;
  c->local = c->to.sin_port;
  c->remote = c->from.sin_port;
  c->type = IPPROTO_TCP;
  c->delay = NowMs();
  c->seq_num = INITIAL_SEQUENCE;
  c->ack_num = 0;
  c->ack_base = 0;
  c->window_size = 8192;
  c->ready = false;
  ForgetPending(*c);
  c->bba_mac = m_console_mac;
  c->my_mac = MacFor(c->ip);
  c->tcp_socket.setBlocking(false);

  // The options a real broadband adapter peer sends in Mario Kart: Double Dash!!.
  Common::TCPPacket syn(c->bba_mac, c->my_mac, c->from, c->to, c->seq_num, c->ack_num, TCP_SYN);
  syn.tcp_options = {0x02, 0x04, 0x05, 0xb4,  // MSS 1460
                     0x01,                    // padding
                     0x03, 0x03, 0x08,        // window scale 8
                     0x01, 0x01,              // padding
                     0x04, 0x02};             // SACK permitted
  Enqueue(syn.Build());
}

const Common::MACAddress& CEXIETHERNET::BuiltInBBAInterface::MacFor(u32 inet_ip)
{
  // Hosts we have not seen yet get a fresh made-up address.
  auto [it, added] = m_arp.try_emplace(inet_ip);
  if (added)
    it->second = Common::GenerateMacAddress(Common::MACConsumer::BBA);
  return it->second;
}

// ---------------------------------------------------------------------------------------------
// Frames from the console

bool CEXIETHERNET::BuiltInBBAInterface::SendFrame(const u8* frame, u32 size)
{
  std::lock_guard lock(m_lock);
  const Common::PacketView view(frame, size);

  const std::optional<u16> ethertype = view.GetEtherType();
  if (!ethertype)
  {
    ERROR_LOG_FMT(SP1, "Unable to send frame with invalid ethernet header");
    return false;
  }

  if (*ethertype == Common::ARP_ETHERTYPE)
  {
    const std::optional<Common::ARPPacket> arp = view.GetARPPacket();
    if (!arp)
    {
      ERROR_LOG_FMT(SP1, "Unable to send frame with invalid ARP header");
      return false;
    }
    OnARP(*arp);
  }
  else if (*ethertype == Common::IPV4_ETHERTYPE)
  {
    const std::optional<u8> protocol = view.GetIPProto();
    if (!protocol)
    {
      ERROR_LOG_FMT(SP1, "Unable to send frame with invalid IP header");
      return false;
    }

    if (*protocol == IPPROTO_UDP)
    {
      const std::optional<Common::UDPPacket> udp = view.GetUDPPacket();
      if (!udp)
      {
        ERROR_LOG_FMT(SP1, "Unable to send frame with invalid UDP header");
        return false;
      }
      if (ntohs(udp->udp_header.destination_port) == DHCP_SERVER_PORT)
        OnDHCP(*udp);
      else
        OnUDP(*udp);
    }
    else if (*protocol == IPPROTO_TCP)
    {
      const std::optional<Common::TCPPacket> tcp = view.GetTCPPacket();
      if (!tcp)
      {
        ERROR_LOG_FMT(SP1, "Unable to send frame with invalid TCP header");
        return false;
      }
      OnTCP(*tcp);
    }
    else if (*protocol == IPPROTO_IGMP)
    {
      // Membership reports are simply echoed back.
      Enqueue(std::vector<u8>(frame, frame + size));
    }
    else
    {
      ERROR_LOG_FMT(SP1, "Unsupported IP protocol {}", *protocol);
    }
  }
  else
  {
    ERROR_LOG_FMT(SP1, "Unsupported EtherType {:#06x}", *ethertype);
    return false;
  }

  m_eth_ref->SendComplete();
  return true;
}

void CEXIETHERNET::BuiltInBBAInterface::OnARP(const Common::ARPPacket& packet)
{
  const Common::ARPHeader& request = packet.arp_header;

  // A probe for the console's own address (RFC 5227) gets no answer.
  const bool self_probe = request.sender_address == m_console_mac && request.sender_ip == 0 &&
                          request.target_ip == m_console_ip;
  if (self_probe)
    return;

  // Every address is on the LAN and answers.
  Common::ARPPacket reply(m_console_mac, m_gateway_mac);
  reply.arp_header =
      Common::ARPHeader(request.target_ip, MacFor(request.target_ip), m_console_ip, m_console_mac);
  Enqueue(reply.Build());
}

void CEXIETHERNET::BuiltInBBAInterface::OnDHCP(const Common::UDPPacket& packet)
{
  const Common::DHCPPacket request(packet.data);

  // The first option is the message type: a discover (1) gets an offer (2), anything else an
  // acknowledgement (5). A missing or empty type is treated as a discover.
  const std::vector<std::vector<u8>>& options = request.options;
  const bool discover = options.empty() || options[0].size() < 3 || options[0][2] == 1;

  const u8* const gateway_bytes = reinterpret_cast<const u8*>(&m_gateway_ip);
  const std::vector<u8> gateway(gateway_bytes, gateway_bytes + sizeof(m_gateway_ip));
  constexpr std::array<u8, 4> one_day = {0x00, 0x01, 0x51, 0x80};

  Common::DHCPPacket reply;
  reply.body =
      Common::DHCPBody(request.body.transaction_id, m_console_mac, m_console_ip, m_gateway_ip);
  reply.AddOption(53, {discover ? u8(2) : u8(5)});                 // message type
  reply.AddOption(54, gateway);                                    // server
  reply.AddOption(51, one_day);                                    // lease
  reply.AddOption(58, one_day);                                    // renewal
  reply.AddOption(59, one_day);                                    // rebinding
  reply.AddOption(1, {255, 255, 255, 0});                          // netmask
  reply.AddOption(28, {gateway[0], gateway[1], gateway[2], 255});  // broadcast
  reply.AddOption(6, gateway);                                     // DNS
  reply.AddOption(15, {'l', 'a', 'n'});                            // domain
  reply.AddOption(3, gateway);                                     // router
  reply.AddOption(255, {});                                        // end

  const sockaddr_in server = Endpoint(m_gateway_ip, htons(DHCP_SERVER_PORT));
  const sockaddr_in client = Endpoint(m_console_ip, packet.udp_header.source_port);
  Enqueue(Common::UDPPacket(m_console_mac, m_gateway_mac, server, client, reply.Build()).Build());
}

void CEXIETHERNET::BuiltInBBAInterface::OnTCP(const Common::TCPPacket& packet)
{
  const Common::TCPHeader& header = packet.tcp_header;
  const u32 peer_ip = AsU32(packet.ip_header.destination_addr);
  BbaConnection* const c = m_connections.FindTcp(header.source_port, header.destination_port, peer_ip);
  const u16 flags = static_cast<u16>(ntohs(header.properties) & 0xfff);

  if ((flags & (TCP_FIN | TCP_RST)) != 0)
  {
    if (c == nullptr)
      return;
    // The console closes: acknowledge, pass on any last data, and drop the stream.
    c->ack_num += 1 + static_cast<u32>(packet.data.size());
    Enqueue(TearDown(*c));
    c->ip = 0;
    if (!packet.data.empty())
      (void)c->tcp_socket.send(packet.data.data(), packet.data.size());
    c->tcp_socket.disconnect();
    return;
  }

  if (flags == (TCP_SYN | TCP_ACK))
  {
    // The console accepts a connection offered by AcceptUPnPClient.
    if (c == nullptr)
      return;
    ++c->seq_num;
    c->ack_num = ntohl(header.sequence_number) + 1;
    c->ready = true;
    Enqueue(Segment(*c, TCP_ACK));
    return;
  }

  if ((flags & TCP_SYN) != 0)
  {
    if (c == nullptr)
      OpenTCP(packet);
    return;
  }

  if (c != nullptr)
    OnTCPData(*c, packet);
}

void CEXIETHERNET::BuiltInBBAInterface::OpenTCP(const Common::TCPPacket& packet)
{
  BbaConnection* const c = m_connections.ForLocalPort(0);
  if (c == nullptr)
  {
    WARN_LOG_FMT(SP1, "BBA: no free connection for a new TCP stream");
    return;
  }

  const Common::TCPHeader& header = packet.tcp_header;
  const u32 peer_ip = AsU32(packet.ip_header.destination_addr);

  c->delay = NowMs();
  c->local = header.source_port;
  c->remote = header.destination_port;
  c->ack_num = ntohl(header.sequence_number) + 1;
  c->ack_base = c->ack_num;
  c->seq_num = INITIAL_SEQUENCE;
  c->window_size = ntohs(header.window_size);
  c->type = IPPROTO_TCP;
  ForgetPending(*c);
  c->from.sin_addr.s_addr = peer_ip;
  c->from.sin_port = header.destination_port;
  c->to.sin_addr.s_addr = AsU32(packet.ip_header.source_addr);
  c->to.sin_port = header.source_port;
  c->bba_mac = m_console_mac;
  c->my_mac = MacFor(peer_ip);
  c->tcp_socket.setBlocking(false);
  c->ready = false;
  c->ip = peer_ip;

  // The SYN-ACK is produced once the host connection is up (BbaTcpSocket::Connected).
  c->tcp_socket.Connect(sf::IpAddress(ntohl(peer_ip)), ntohs(header.destination_port),
                        m_console_ip);
}

void CEXIETHERNET::BuiltInBBAInterface::OnTCPData(BbaConnection& c, const Common::TCPPacket& packet)
{
  const Common::TCPHeader& header = packet.tcp_header;
  const int payload_size = int(ntohs(packet.ip_header.total_len)) -
                           int(packet.ip_header.DefinedSize()) - int(header.GetHeaderSize());

  if (payload_size > 0)
  {
    // In-order (or repeated) data is relayed; either way the console gets an ACK.
    const u32 sequence = ntohl(header.sequence_number);
    const bool not_behind = static_cast<s32>(sequence - c.ack_num) >= 0;
    if (not_behind && packet.data.size() >= static_cast<std::size_t>(payload_size))
    {
      (void)c.tcp_socket.send(packet.data.data(), static_cast<std::size_t>(payload_size));
      c.ack_num += static_cast<u32>(payload_size);
    }
    Enqueue(Segment(c, TCP_ACK));
  }

  c.window_size = ntohs(header.window_size);

  if ((ntohs(header.properties) & TCP_ACK) == 0)
    return;

  // Retire what the console acknowledged, and trim partly acknowledged segments.
  const u32 acked = ntohl(header.acknowledgement_number);
  for (BbaTcpBuffer& pending : c.tcp_buffers)
  {
    if (!pending.used || pending.seq_id >= acked)
      continue;

    std::optional<Common::TCPPacket> segment =
        Common::PacketView(pending.data.data(), pending.data.size()).GetTCPPacket();
    if (!segment)
      continue;

    const u32 segment_end = pending.seq_id + static_cast<u32>(segment->data.size());
    if (segment_end <= acked)
    {
      pending.used = false;
      // Once the SYN-ACK is acknowledged the stream may carry data.
      if (!c.ready && !c.tcp_buffers[0].used)
        c.ready = true;
      continue;
    }

    const u16 acked_part = static_cast<u16>(acked - pending.seq_id);
    segment->data.erase(segment->data.begin(), segment->data.begin() + acked_part);
    pending.seq_id += acked_part;
    segment->tcp_header.sequence_number = htonl(pending.seq_id);
    pending.data = segment->Build();
  }
}

// Some games listen on a UDP port without sending from it first, so it is opened up front.
void CEXIETHERNET::BuiltInBBAInterface::OpenListeningUdpPort(u16 port)
{
  BbaConnection* const c = m_connections.ForLocalPort(htons(port));
  if (c == nullptr || c->ip != 0)
    return;

  c->ip = m_gateway_ip;
  c->local = htons(port);
  c->remote = htons(port);
  c->type = IPPROTO_UDP;
  c->bba_mac = m_console_mac;
  c->my_mac = m_gateway_mac;
  c->from.sin_addr.s_addr = 0;
  c->from.sin_port = htons(port);
  c->to.sin_addr.s_addr = m_console_ip;
  c->to.sin_port = htons(port);
  c->udp_socket.setBlocking(false);
  if (c->udp_socket.Bind(port, m_console_ip) != sf::Socket::Status::Done)
  {
    ERROR_LOG_FMT(SP1, "Couldn't open UDP socket");
    PanicAlertFmtT(
        "Couldn't open port {0}. This might stop the game's LAN mode from working properly.", port);
  }
}

void CEXIETHERNET::BuiltInBBAInterface::OnUDP(const Common::UDPPacket& packet)
{
  const Common::IPv4Header& ip = packet.ip_header;
  const Common::UDPHeader& header = packet.udp_header;
  // Datagrams to 0.0.0.0 (DNS lookups before configuration) are taken by the router.
  const u32 peer_ip =
      ip.destination_addr == Common::IP_ADDR_ANY ? m_gateway_ip : AsU32(ip.destination_addr);

  BbaConnection* const c = m_connections.ForLocalPort(header.source_port);
  if (c == nullptr)
  {
    WARN_LOG_FMT(SP1, "BBA: no free connection for UDP port {}", ntohs(header.source_port));
    return;
  }

  if (c->ip == 0)
  {
    c->ip = peer_ip;
    c->local = header.source_port;
    c->remote = header.destination_port;
    c->type = IPPROTO_UDP;
    c->bba_mac = m_console_mac;
    c->my_mac = m_gateway_mac;
    c->from.sin_addr.s_addr = peer_ip;
    c->from.sin_port = header.destination_port;
    c->to.sin_addr.s_addr = AsU32(ip.source_addr);
    c->to.sin_port = header.source_port;
    c->udp_socket.setBlocking(false);

    // The game's own port is preferred so that peers can answer it directly.
    if (c->udp_socket.Bind(ntohs(header.source_port), m_console_ip) != sf::Socket::Status::Done)
    {
      PanicAlertFmtT(
          "Port {0} is already in use. This might stop the game's LAN mode from working properly.",
          ntohs(header.source_port));
      if (c->udp_socket.Bind(sf::Socket::AnyPort, m_console_ip) != sf::Socket::Status::Done)
      {
        ERROR_LOG_FMT(SP1, "Couldn't open UDP socket");
        return;
      }

      // Without the SSDP port a large SSDP request would go unanswered; bouncing it back to
      // the console lets it carry on.
      if (ntohs(header.destination_port) == Common::SSDP_PORT &&
          ntohs(header.length) > SSDP_REFLECT_LENGTH)
      {
        Common::UDPPacket bounced = packet;
        std::swap(bounced.eth_header.source, bounced.eth_header.destination);
        bounced.ip_header.destination_addr = ip.source_addr;
        bounced.ip_header.source_addr = std::bit_cast<Common::IPAddress>(peer_ip);
        Enqueue(bounced.Build());
      }
    }
  }

  const u16 port = ntohs(header.destination_port);
  const sf::IpAddress target =
      port == DNS_PORT ? sf::IpAddress::resolve(m_dns_ip).value_or(sf::IpAddress::Any) :
                         sf::IpAddress(ntohl(AsU32(ip.destination_addr)));
  (void)c->udp_socket.send(packet.data.data(), packet.data.size(), target, port);
}
}  // namespace ExpansionInterface
