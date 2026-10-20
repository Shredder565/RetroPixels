// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <SFML/Network/Packet.hpp>

#include <type_traits>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Common/TypeUtils.h"

// sf::Packet extraction into big-endian wrappers (the packet itself is already big-endian on the
// wire; these store the value back in big-endian form).
sf::Packet& operator>>(sf::Packet& packet, Common::BigEndianValue<u16>& data);
sf::Packet& operator>>(sf::Packet& packet, Common::BigEndianValue<u32>& data);
sf::Packet& operator>>(sf::Packet& packet, Common::BigEndianValue<u64>& data);

// Enums travel as their underlying integer type.
template <Common::Enum Enum>
sf::Packet& operator<<(sf::Packet& packet, Enum e)
{
  return packet << std::to_underlying(e);
}

template <Common::Enum Enum>
sf::Packet& operator>>(sf::Packet& packet, Enum& e)
{
  std::underlying_type_t<Enum> raw{};
  packet >> raw;
  e = static_cast<Enum>(raw);
  return packet;
}

namespace Common
{
// Reads a u64 as an expression.
u64 PacketReadU64(sf::Packet& packet);
}  // namespace Common
