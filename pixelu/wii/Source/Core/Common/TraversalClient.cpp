// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/TraversalClient.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Random.h"
#include "Core/NetPlayProto.h"

namespace Common
{
namespace
{
constexpr u32 RESEND_INTERVAL_MS = 300;  // multiplied by the number of tries so far
constexpr int MAX_TRIES = 5;
constexpr u32 PING_INTERVAL_MS = 500;
constexpr u32 TTL_TEST_WAIT_MS = 50;
constexpr u8 MAX_TTL = 32;

ENetAddress ToENetAddress(const TraversalInetAddress& address)
{
  ENetAddress result{};
  // IPv6 is not supported; port 0 marks the address as unusable.
  if (!address.isIPV6)
  {
    result.host = address.address[0];
    result.port = ntohs(address.port);
  }
  return result;
}

bool SendDatagram(ENetSocket socket, const ENetAddress& to, const void* data, size_t size)
{
  ENetBuffer buffer;
  buffer.data = const_cast<void*>(data);
  buffer.dataLength = size;
  return enet_socket_send(socket, &to, &buffer, 1) != -1;
}

// Sends with a temporary TTL, restoring the socket's previous TTL afterwards.
bool SendDatagramWithTtl(ENetSocket socket, const ENetAddress& to, const void* data, size_t size,
                         int ttl)
{
  int previous_ttl = 0;
  enet_socket_get_option(socket, ENET_SOCKOPT_TTL, &previous_ttl);
  enet_socket_set_option(socket, ENET_SOCKOPT_TTL, ttl);
  const bool sent = SendDatagram(socket, to, data, size);
  enet_socket_set_option(socket, ENET_SOCKOPT_TTL, previous_ttl);
  return sent;
}
}  // namespace

TraversalClient::TraversalClient(ENetHost* netHost, std::string server, const u16 port,
                                 const u16 port_alt)
    : m_NetHost(netHost), m_Server(std::move(server)), m_port(port), m_portAlt(port_alt)
{
  netHost->intercept = TraversalClient::InterceptCallback;
  Reset();
  ReconnectToServer();
}

TraversalClient::~TraversalClient() = default;

TraversalClient::State TraversalClient::GetState() const
{
  return m_State;
}

TraversalClient::FailureReason TraversalClient::GetFailureReason() const
{
  return m_FailureReason;
}

TraversalHostId TraversalClient::GetHostID() const
{
  return m_HostId;
}

TraversalInetAddress TraversalClient::GetExternalAddress() const
{
  return m_external_address;
}

void TraversalClient::Reset()
{
  m_PendingConnect = false;
  m_Client = nullptr;
}

void TraversalClient::ReconnectToServer()
{
  if (enet_address_set_host(&m_ServerAddress, m_Server.c_str()) != 0)
  {
    OnFailure(FailureReason::BadHost);
    return;
  }
  m_ServerAddress.port = m_port;
  m_State = State::Connecting;

  TraversalPacket hello{};
  hello.type = TraversalPacketType::HelloFromClient;
  hello.helloFromClient.protoVersion = TraversalProtoVersion;
  SendTraversalPacket(hello);

  if (m_Client)
    m_Client->OnTraversalStateChanged();
}

void TraversalClient::ConnectToClient(std::string_view host)
{
  TraversalPacket request{};
  if (host.size() > request.connectPlease.hostId.size())
  {
    PanicAlertFmt("Host code is too long");
    return;
  }
  request.type = TraversalPacketType::ConnectPlease;
  std::copy(host.begin(), host.end(), request.connectPlease.hostId.begin());
  m_ConnectRequestId = SendTraversalPacket(request);
  m_PendingConnect = true;
}

bool TraversalClient::TestPacket(u8* data, size_t size, ENetAddress* from)
{
  if (from->host != m_ServerAddress.host || from->port != m_ServerAddress.port)
    return false;

  if (size < sizeof(TraversalPacket))
  {
    ERROR_LOG_FMT(NETPLAY, "Traversal server sent a truncated packet ({} bytes).", size);
    return false;
  }

  TraversalPacket packet;
  std::memcpy(&packet, data, sizeof(packet));
  HandleServerPacket(&packet);
  return true;
}

void TraversalClient::Update()
{
  ENetEvent event;
  if (enet_host_service(m_NetHost, &event, 4) > 0 && event.type == ENET_EVENT_TYPE_RECEIVE)
  {
    TestPacket(event.packet->data, event.packet->dataLength, &event.peer->address);
    enet_packet_destroy(event.packet);
  }
  HandleResends();
}

void TraversalClient::HandleServerPacket(TraversalPacket* packet)
{
  u8 ok = 1;

  switch (packet->type)
  {
  case TraversalPacketType::Ack:
  {
    if (!packet->ack.ok)
    {
      OnFailure(FailureReason::ServerForgotAboutUs);
      break;
    }
    const auto acked = std::ranges::find_if(m_OutgoingTraversalPackets, [&](const auto& info) {
      return info.packet.requestId == packet->requestId;
    });
    if (acked != m_OutgoingTraversalPackets.end())
    {
      if (packet->requestId == m_TestRequestId)
        HandleTraversalTest();
      m_OutgoingTraversalPackets.erase(acked);
    }
    break;
  }

  case TraversalPacketType::HelloFromServer:
  {
    // Only meaningful while connecting; the server refuses clients that are too old.
    if (!IsConnecting())
      break;
    const auto& hello = packet->helloFromServer;
    if (hello.ok)
    {
      m_HostId = hello.yourHostId;
      m_external_address = hello.yourAddress;
      NewTraversalTest();
      m_State = State::Connected;
      if (m_Client)
        m_Client->OnTraversalStateChanged();
    }
    else
    {
      OnFailure(FailureReason::VersionTooOld);
    }
    break;
  }

  case TraversalPacketType::PleaseSendPacket:
  {
    // Punch a hole towards a joining peer: any datagram will do.
    const ENetAddress peer = ToENetAddress(packet->pleaseSendPacket.address);
    if (peer.port == 0)
    {
      ok = 0;  // IPv6
      break;
    }
    static constexpr char HOLE_PUNCH[] = "Hello from Dolphin Netplay...";
    constexpr size_t length = sizeof(HOLE_PUNCH) - 1;
    if (m_ttlReady)
      SendDatagramWithTtl(m_NetHost->socket, peer, HOLE_PUNCH, length, m_ttl);
    else
      SendDatagram(m_NetHost->socket, peer, HOLE_PUNCH, length);
    break;
  }

  case TraversalPacketType::ConnectReady:
  case TraversalPacketType::ConnectFailed:
    // Both start with the request id they answer.
    if (!m_PendingConnect || packet->connectReady.requestId != m_ConnectRequestId)
      break;
    m_PendingConnect = false;
    if (!m_Client)
      break;
    if (packet->type == TraversalPacketType::ConnectReady)
      m_Client->OnConnectReady(ToENetAddress(packet->connectReady.address));
    else
      m_Client->OnConnectFailed(packet->connectFailed.reason);
    break;

  default:
    WARN_LOG_FMT(NETPLAY, "Traversal server sent an unknown packet type {}.",
                 static_cast<int>(packet->type));
    break;
  }

  // Everything but an ack gets acknowledged.
  if (packet->type == TraversalPacketType::Ack)
    return;

  TraversalPacket ack{};
  ack.type = TraversalPacketType::Ack;
  ack.requestId = packet->requestId;
  ack.ack.ok = ok;
  if (!SendDatagram(m_NetHost->socket, m_ServerAddress, &ack, sizeof(ack)))
    OnFailure(FailureReason::SocketSendError);
}

void TraversalClient::OnFailure(FailureReason reason)
{
  m_State = State::Failure;
  m_FailureReason = reason;
  if (m_Client)
    m_Client->OnTraversalStateChanged();
}

void TraversalClient::ResendPacket(OutgoingTraversalPacketInfo* info)
{
  // The TTL test request must leave through the test socket.
  const bool via_test_socket =
      m_TestSocket != ENET_SOCKET_NULL && info->packet.type == TraversalPacketType::TestPlease;
  info->sendTime = enet_time_get();
  ++info->tries;
  if (!SendDatagram(via_test_socket ? m_TestSocket : m_NetHost->socket, m_ServerAddress,
                    &info->packet, sizeof(info->packet)))
  {
    OnFailure(FailureReason::SocketSendError);
  }
}

void TraversalClient::HandleResends()
{
  const u32 now = enet_time_get();
  for (OutgoingTraversalPacketInfo& info : m_OutgoingTraversalPackets)
  {
    if (now - info.sendTime < RESEND_INTERVAL_MS * static_cast<u32>(info.tries))
      continue;
    if (info.tries >= MAX_TRIES)
    {
      OnFailure(FailureReason::ResendTimeout);
      m_OutgoingTraversalPackets.clear();
      break;
    }
    ResendPacket(&info);
  }
  HandlePing();
}

void TraversalClient::HandlePing()
{
  const u32 now = enet_time_get();
  if (!IsConnected() || now - m_PingTime < PING_INTERVAL_MS)
    return;

  TraversalPacket ping{};
  ping.type = TraversalPacketType::Ping;
  ping.ping.hostId = m_HostId;
  SendTraversalPacket(ping);
  m_PingTime = now;
}

void TraversalClient::NewTraversalTest()
{
  // The test: from a fresh socket, send a datagram with a small TTL to the server's alternate
  // port, then ask the server (TestPlease) to reply there. The reply only gets through if the
  // datagram travelled far enough to open our NAT; otherwise try again with TTL + 1.
  if (m_TestSocket != ENET_SOCKET_NULL)
    enet_socket_destroy(m_TestSocket);

  const auto abandon = [this] {
    if (m_TestSocket != ENET_SOCKET_NULL)
      enet_socket_destroy(m_TestSocket);
    m_TestSocket = ENET_SOCKET_NULL;
  };

  m_TestSocket = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  any.port = 0;
  if (m_TestSocket == ENET_SOCKET_NULL || enet_socket_bind(m_TestSocket, &any) < 0)
  {
    abandon();
    return;
  }
  enet_socket_set_option(m_TestSocket, ENET_SOCKOPT_NONBLOCK, 1);

  TraversalPacket probe{};
  probe.type = TraversalPacketType::Ping;
  probe.ping.hostId = m_HostId;
  probe.requestId = Common::Random::GenerateValue<TraversalRequestId>();

  ENetAddress alt_address = m_ServerAddress;
  alt_address.port = m_portAlt;

  int previous_ttl = 0;
  enet_socket_get_option(m_TestSocket, ENET_SOCKOPT_TTL, &previous_ttl);
  enet_socket_set_option(m_TestSocket, ENET_SOCKOPT_TTL, m_ttl);
  if (!SendDatagram(m_TestSocket, alt_address, &probe, sizeof(probe)))
  {
    abandon();
    return;
  }
  enet_socket_set_option(m_TestSocket, ENET_SOCKOPT_TTL, previous_ttl);

  probe.type = TraversalPacketType::TestPlease;
  m_TestRequestId = SendTraversalPacket(probe);
}

void TraversalClient::HandleTraversalTest()
{
  if (m_TestSocket == ENET_SOCKET_NULL)
    return;

  // Wait briefly for anything to arrive on the test socket. Any datagram counts as the reply;
  // the content is read (to drain it) but not inspected.
  bool received = false;
  const u32 deadline = enet_time_get() + TTL_TEST_WAIT_MS;
  for (;;)
  {
    const u32 now = enet_time_get();
    enet_uint32 events = ENET_SOCKET_WAIT_RECEIVE | ENET_SOCKET_WAIT_INTERRUPT;
    if (now > deadline || enet_socket_wait(m_TestSocket, &events, deadline - now) != 0)
      break;  // error or timeout

    if (events & ENET_SOCKET_WAIT_RECEIVE)
    {
      TraversalPacket reply;
      ENetAddress from;
      ENetBuffer buffer;
      buffer.data = &reply;
      buffer.dataLength = sizeof(reply);
      if (enet_socket_receive(m_TestSocket, &from, &buffer, 1) < 0)
        break;
      received = true;
    }

    if (!(events & ENET_SOCKET_WAIT_INTERRUPT))
      break;
    // Interrupted: wait again for the rest of the window (a reply seen so far is dropped).
    received = false;
  }

  enet_socket_destroy(m_TestSocket);
  m_TestSocket = ENET_SOCKET_NULL;

  if (received)
  {
    m_ttlReady = true;
    if (m_Client)
      m_Client->OnTtlDetermined(m_ttl);
  }
  else if (++m_ttl < MAX_TTL)
  {
    NewTraversalTest();
  }
}

TraversalRequestId TraversalClient::SendTraversalPacket(const TraversalPacket& packet)
{
  OutgoingTraversalPacketInfo& info = m_OutgoingTraversalPackets.emplace_back();
  info.packet = packet;
  info.packet.requestId = Common::Random::GenerateValue<TraversalRequestId>();
  info.tries = 0;
  info.sendTime = 0;
  const TraversalRequestId id = info.packet.requestId;
  ResendPacket(&info);
  return id;
}

int ENET_CALLBACK TraversalClient::InterceptCallback(ENetHost* host, ENetEvent* event)
{
  TraversalClient* const client = g_TraversalClient.get();
  const bool traversal = client != nullptr && client->TestPacket(host->receivedData,
                                                                 host->receivedDataLength,
                                                                 &host->receivedAddress);
  const bool wakeup = host->receivedDataLength == 1 && host->receivedData[0] == 0;
  if (!traversal && !wakeup)
    return 0;
  event->type = static_cast<ENetEventType>(Common::ENet::SKIPPABLE_EVENT);
  return 1;
}

std::unique_ptr<TraversalClient> g_TraversalClient;
ENet::ENetHostPtr g_MainNetHost;

namespace
{
// The settings g_TraversalClient was created with. The listen port matters as requested
// (0 = any), not as bound.
struct TraversalSettings
{
  std::string server;
  u16 server_port = 0;
  u16 server_port_alt = 0;
  u16 listen_port = 0;

  bool operator==(const TraversalSettings&) const = default;
};
TraversalSettings s_current_settings;
}  // namespace

bool EnsureTraversalClient(const std::string& server, u16 server_port, u16 server_port_alt,
                           u16 listen_port)
{
  const TraversalSettings wanted{server, server_port, server_port_alt, listen_port};
  if (g_MainNetHost && g_TraversalClient && wanted == s_current_settings)
    return true;

  s_current_settings = wanted;

  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  address.port = listen_port;
  ENet::ENetHostPtr host{enet_host_create(&address, 50, NetPlay::CHANNEL_COUNT, 0, 0)};
  if (!host)
  {
    g_MainNetHost.reset();
    return false;
  }
  host->mtu = std::min(host->mtu, NetPlay::MAX_ENET_MTU);
  g_MainNetHost = std::move(host);
  g_TraversalClient = std::make_unique<TraversalClient>(g_MainNetHost.get(), server, server_port,
                                                        server_port_alt);
  return true;
}

void ReleaseTraversalClient()
{
  if (!g_TraversalClient)
    return;
  g_TraversalClient.reset();
  g_MainNetHost.reset();
}
}  // namespace Common
