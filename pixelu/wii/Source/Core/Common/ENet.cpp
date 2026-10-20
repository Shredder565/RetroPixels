// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/ENet.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common::ENet
{
namespace
{
constexpr enet_uint32 LOOPBACK_ADDRESS = 0x0100007f;  // 127.0.0.1 in network byte order
}

void WakeupThread(ENetHost* host)
{
  // ENet has no way to interrupt enet_host_service(), so poke the socket from outside: a single
  // zero byte sent to the host's own port on the loopback interface.
  ENetAddress target{};
  if (host->address.port != 0)
    target.port = host->address.port;
  else
    enet_socket_get_address(host->socket, &target);
  target.host = LOOPBACK_ADDRESS;

  u8 payload = 0;
  ENetBuffer buffer;
  buffer.data = &payload;
  buffer.dataLength = sizeof(payload);
  enet_socket_send(host->socket, &target, &buffer, 1);
}

int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event)
{
  const bool is_wakeup = host->receivedDataLength == 1 && host->receivedData[0] == 0;
  if (!is_wakeup)
    return 0;
  event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
  return 1;
}

bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id)
{
  if (socket == nullptr)
  {
    ERROR_LOG_FMT(NETPLAY, "Cannot send a packet: no peer.");
    return false;
  }

  ENetPacket* const enet_packet =
      enet_packet_create(packet.getData(), packet.getDataSize(), ENET_PACKET_FLAG_RELIABLE);
  if (enet_packet == nullptr)
  {
    ERROR_LOG_FMT(NETPLAY, "Could not allocate a {}-byte ENet packet.", packet.getDataSize());
    return false;
  }

  if (const int error = enet_peer_send(socket, channel_id, enet_packet); error != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "enet_peer_send failed ({}).", error);
    // A packet ENet did not queue anywhere is still ours to free.
    if (enet_packet->referenceCount == 0)
      enet_packet_destroy(enet_packet);
    return false;
  }
  return true;
}
}  // namespace Common::ENet
