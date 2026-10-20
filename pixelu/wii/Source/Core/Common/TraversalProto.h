// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include "Common/CommonTypes.h"

// Wire format of the netplay traversal (NAT hole punching) protocol. Every datagram is one
// TraversalPacket: a type byte, a request id, then the type's payload. Multi-byte fields are in
// host order (little-endian in practice) and the structs are packed, so this layout is the
// protocol and must not change.

namespace Common
{
constexpr size_t NETPLAY_CODE_SIZE = 8;
using TraversalHostId = std::array<char, NETPLAY_CODE_SIZE>;
using TraversalRequestId = u64;

// Direction: c = client (a netplay host or joiner), s = traversal server.
enum class TraversalPacketType : u8
{
  Ack = 0,               // either way: acknowledges requestId
  Ping = 1,              // c->s: keep-alive for a registered host id
  HelloFromClient = 2,   // c->s: register, giving the protocol version
  HelloFromServer = 3,   // s->c: registration result, assigned host id, public address
  ConnectPlease = 4,     // c->s: a joiner asks to reach a host id
  PleaseSendPacket = 5,  // s->c: the host should send a datagram to this address
  ConnectReady = 6,      // s->c: the joiner may now connect to this address
  ConnectFailed = 7,     // s->c: the connection attempt failed (see the reason)
  TestPlease = 8,        // c->s: NAT test; the server answers through two different paths
};

constexpr u8 TraversalProtoVersion = 0;

enum class TraversalConnectFailedReason : u8
{
  ClientDidntRespond = 0,
  ClientFailure,
  NoSuchClient,
};

#pragma pack(push, 1)
struct TraversalInetAddress
{
  u8 isIPV6;
  u32 address[4];  // IPv4 uses address[0], in network byte order
  u16 port;        // network byte order
};

struct TraversalPacket
{
  TraversalPacketType type;
  TraversalRequestId requestId;
  union
  {
    struct
    {
      u8 ok;
    } ack;
    struct
    {
      TraversalHostId hostId;
    } ping;
    struct
    {
      u8 protoVersion;
    } helloFromClient;
    struct
    {
      u8 ok;
      TraversalHostId yourHostId;
      TraversalInetAddress yourAddress;
    } helloFromServer;
    struct
    {
      TraversalHostId hostId;
    } connectPlease;
    struct
    {
      TraversalInetAddress address;
    } pleaseSendPacket;
    struct
    {
      TraversalRequestId requestId;
      TraversalInetAddress address;
    } connectReady;
    struct
    {
      TraversalRequestId requestId;
      TraversalConnectFailedReason reason;
    } connectFailed;
    struct
    {
      TraversalHostId hostId;
    } testPlease;
  };
};
#pragma pack(pop)

// The layout above is the protocol.
static_assert(sizeof(TraversalInetAddress) == 19);
static_assert(sizeof(TraversalPacket) == 37);
static_assert(offsetof(TraversalPacket, requestId) == 1);
}  // namespace Common
