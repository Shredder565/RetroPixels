// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/SFMLHelper.h"

#include <SFML/Network/Packet.hpp>

namespace
{
template <typename T>
sf::Packet& ReadInto(sf::Packet& packet, Common::BigEndianValue<T>& data)
{
  T value{};
  packet >> value;
  data = value;
  return packet;
}
}  // namespace

sf::Packet& operator>>(sf::Packet& packet, Common::BigEndianValue<u16>& data)
{
  return ReadInto(packet, data);
}

sf::Packet& operator>>(sf::Packet& packet, Common::BigEndianValue<u32>& data)
{
  return ReadInto(packet, data);
}

sf::Packet& operator>>(sf::Packet& packet, Common::BigEndianValue<u64>& data)
{
  return ReadInto(packet, data);
}

namespace Common
{
u64 PacketReadU64(sf::Packet& packet)
{
  u64 value{};
  packet >> value;
  return value;
}
}  // namespace Common
