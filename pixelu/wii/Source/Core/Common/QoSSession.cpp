// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/QoSSession.h"

#if defined(_WIN32)
#include <Qos2.h>
#pragma comment(lib, "qwave")
#endif

namespace Common
{
#if defined(_WIN32)
QoSSession::QoSSession(ENetPeer* peer, int tos_val) : m_peer(peer)
{
  QOS_VERSION version{};
  version.MajorVersion = 1;
  version.MinorVersion = 0;
  if (!QOSCreateHandle(&version, &m_qos_handle))
  {
    m_qos_handle = nullptr;
    return;
  }

  // The flow is keyed on the host socket's own address.
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ENET_HOST_TO_NET_16(peer->host->address.port);
  address.sin_addr.s_addr = peer->host->address.host;

  if (!QOSAddSocketToFlow(m_qos_handle, peer->host->socket, reinterpret_cast<PSOCKADDR>(&address),
                          QOSTrafficTypeControl, QOS_NON_ADAPTIVE_FLOW, &m_qos_flow_id))
  {
    return;
  }

  // DSCP is the ToS byte without its two ECN bits, so the same marking as IP_TOS elsewhere.
  // Setting it needs administrator rights; without them the flow still exists, so it is not an
  // error.
  DWORD dscp = static_cast<DWORD>(tos_val >> 2);
  QOSSetFlow(m_qos_handle, m_qos_flow_id, QOSSetOutgoingDSCPValue, sizeof(dscp), &dscp, 0,
             nullptr);
  m_success = true;
}

void QoSSession::Release()
{
  if (m_qos_handle != nullptr)
  {
    if (m_qos_flow_id != 0)
      QOSRemoveSocketFromFlow(m_qos_handle, m_peer->host->socket, m_qos_flow_id, 0);
    QOSCloseHandle(m_qos_handle);
  }
  m_qos_handle = nullptr;
  m_qos_flow_id = 0;
  m_peer = nullptr;
  m_success = false;
}
#else
QoSSession::QoSSession(ENetPeer* peer, int tos_val)
{
  const ENetSocket socket = peer->host->socket;
#if defined(__linux__)
  constexpr int priority = 7;  // highest priority settable without CAP_NET_ADMIN
  setsockopt(socket, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
#endif
  m_success = setsockopt(socket, IPPROTO_IP, IP_TOS, &tos_val, sizeof(tos_val)) == 0;
}

void QoSSession::Release()
{
  m_success = false;
}
#endif

QoSSession::~QoSSession()
{
  Release();
}

QoSSession& QoSSession::operator=(QoSSession&& session)
{
  if (this == &session)
    return *this;

  Release();
#if defined(_WIN32)
  m_qos_handle = std::exchange(session.m_qos_handle, nullptr);
  m_qos_flow_id = std::exchange(session.m_qos_flow_id, 0);
  m_peer = std::exchange(session.m_peer, nullptr);
#endif
  m_success = std::exchange(session.m_success, false);
  return *this;
}
}  // namespace Common
