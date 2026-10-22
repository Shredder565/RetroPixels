// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <string_view>

#include <enet/enet.h>

#include "Common/CommonTypes.h"
#include "Common/ENet.h"
#include "Common/TraversalProto.h"

namespace Common
{
// Receives the traversal client's events (implemented by the netplay client/server).
class TraversalClientClient
{
public:
  virtual ~TraversalClientClient() = default;
  virtual void OnTraversalStateChanged() = 0;
  virtual void OnConnectReady(ENetAddress addr) = 0;
  virtual void OnConnectFailed(TraversalConnectFailedReason reason) = 0;
  virtual void OnTtlDetermined(u8 ttl) = 0;
};

// Client side of the traversal protocol (TraversalProto.h). It shares the netplay ENet host's
// UDP socket: traversal datagrams are picked out of the host's traffic by an intercept hook.
// Registering gives this machine a host id that others can connect to through the server.
class TraversalClient
{
public:
  enum class State
  {
    Connecting,
    Connected,
    Failure,
  };
  enum class FailureReason
  {
    BadHost = 0x300,
    VersionTooOld,
    ServerForgotAboutUs,
    SocketSendError,
    ResendTimeout,
  };

  // Hooks `netHost` and starts registering with `server`:`port`. `port_alt` is the server's
  // second port, used for the hole-punching TTL test.
  TraversalClient(ENetHost* netHost, std::string server, const u16 port, const u16 port_alt = 0);
  ~TraversalClient();

  TraversalHostId GetHostID() const;
  TraversalInetAddress GetExternalAddress() const;
  State GetState() const;
  FailureReason GetFailureReason() const;

  bool HasFailed() const { return m_State == State::Failure; }
  bool IsConnecting() const { return m_State == State::Connecting; }
  bool IsConnected() const { return m_State == State::Connected; }

  // Forgets a pending connection request and detaches m_Client.
  void Reset();
  // Asks the server to set up a connection to the host with id `host`.
  void ConnectToClient(std::string_view host);
  // (Re)starts registration with the server.
  void ReconnectToServer();
  // Services the ENet host briefly when nothing else does, then HandleResends().
  void Update();
  // Retransmits unacknowledged requests and sends keep-alives.
  void HandleResends();

  TraversalClientClient* m_Client = nullptr;

private:
  struct OutgoingTraversalPacketInfo
  {
    TraversalPacket packet;
    int tries;
    u32 sendTime;
  };

  void HandleServerPacket(TraversalPacket* packet);
  // Handles `data` if it came from the server; returns whether it did.
  bool TestPacket(u8* data, size_t size, ENetAddress* from);
  void ResendPacket(OutgoingTraversalPacketInfo* info);
  TraversalRequestId SendTraversalPacket(const TraversalPacket& packet);
  void OnFailure(FailureReason reason);
  void HandlePing();
  static int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event);

  void NewTraversalTest();
  void HandleTraversalTest();

  ENetHost* m_NetHost;
  TraversalHostId m_HostId{};
  TraversalInetAddress m_external_address{};
  State m_State{};
  FailureReason m_FailureReason{};
  TraversalRequestId m_ConnectRequestId = 0;
  bool m_PendingConnect = false;
  std::list<OutgoingTraversalPacketInfo> m_OutgoingTraversalPackets;
  ENetAddress m_ServerAddress{};
  std::string m_Server;
  u16 m_port;
  u16 m_portAlt;
  u32 m_PingTime = 0;

  // TTL test: the smallest TTL whose datagram opens our NAT for the server's reply.
  ENetSocket m_TestSocket = ENET_SOCKET_NULL;
  TraversalRequestId m_TestRequestId = 0;
  u8 m_ttl = 2;
  bool m_ttlReady = false;
};

extern std::unique_ptr<TraversalClient> g_TraversalClient;
// The ENet host g_TraversalClient is attached to.
extern ENet::ENetHostPtr g_MainNetHost;

// Creates (or recreates, when any setting changed) g_MainNetHost and g_TraversalClient.
// Returns false if the ENet host could not be created.
bool EnsureTraversalClient(const std::string& server, u16 server_port, u16 server_port_alt = 0,
                           u16 listen_port = 0);
void ReleaseTraversalClient();
}  // namespace Common
