// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <enet/enet.h>
#include <utility>

namespace Common
{
// Marks a netplay peer's traffic as latency-sensitive for as long as the session lives: a qWAVE
// flow with a DSCP value on Windows, socket priority / IP_TOS elsewhere.
class QoSSession
{
public:
  // ToS byte for Expedited Forwarding: DSCP 46 (101110) followed by two ECN bits (00).
  static constexpr int ef_tos = 0b10111000;

  QoSSession() = default;
  QoSSession(ENetPeer* peer, int tos_val = ef_tos);
  ~QoSSession();

  QoSSession& operator=(const QoSSession&) = delete;
  QoSSession(const QoSSession&) = delete;

  QoSSession& operator=(QoSSession&& session);
  QoSSession(QoSSession&& session) { *this = std::move(session); }

  bool Successful() const { return m_success; }

private:
  void Release();

#if defined(_WIN32)
  void* m_qos_handle = nullptr;
  unsigned long m_qos_flow_id = 0;
  ENetPeer* m_peer = nullptr;
#endif

  bool m_success = false;
};
}  // namespace Common
