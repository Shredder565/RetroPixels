// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include <SFML/Network/Packet.hpp>
#include <enet/enet.h>

#include "Common/CommonTypes.h"

namespace Common::ENet
{
struct ENetHostDeleter
{
  void operator()(ENetHost* host) const noexcept { enet_host_destroy(host); }
};
using ENetHostPtr = std::unique_ptr<ENetHost, ENetHostDeleter>;

// Makes a thread blocked in enet_host_service() on `host` return, by sending the host a one-byte
// datagram that InterceptCallback() turns into a SKIPPABLE_EVENT.
void WakeupThread(ENetHost* host);

// ENetHost::intercept hook: consumes WakeupThread() datagrams.
int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event);

// Queues `packet` reliably on `channel_id`. Returns false (and logs) on failure.
bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id);

// Event type reported for intercepted traversal and wake-up datagrams; callers skip it.
constexpr int SKIPPABLE_EVENT = 42;
}  // namespace Common::ENet
