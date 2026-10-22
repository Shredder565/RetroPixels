// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/DSU/DSUMessages.h"
#include "util/crypto/crc32.h"

namespace
{
	// the magic values read as text in the packet: "DSUC" / "DSUS"
	constexpr uint32_t kClientMagic = 0x43555344;
	constexpr uint32_t kServerMagic = 0x53555344;
	constexpr uint16_t kProtocolVersion = 1001;

	constexpr size_t kCRCOffset = offsetof(MessageHeader, crc32);

	MessageHeader ClientHeader(uint32_t uid, MessageType type)
	{
		MessageHeader h{};
		h.magic = kClientMagic;
		h.protocol_version = kProtocolVersion;
		h.uid = uid;
		h.type = type;
		return h;
	}

	// fills in the size and CRC once the packet is complete
	template<typename T>
	T Seal(T packet)
	{
		packet.header.packet_size = (uint16_t)(sizeof(T) - 16);
		packet.header.crc32 = DSUMessage::PacketCRC(&packet, sizeof(T));
		return packet;
	}

	// server packets are checked over the size of the packet type we expect
	template<typename T>
	bool HasValidCRC(const T& packet)
	{
		return packet.header.crc32 == DSUMessage::PacketCRC(&packet, sizeof(T));
	}

	DataRequest MakeDataRequest(uint32_t uid, RegisterFlag flags, uint8_t index, const MACAddress_t& mac)
	{
		DataRequest r{};
		r.header = ClientHeader(uid, MessageType::Data);
		r.flags = flags;
		r.index = index;
		r.mac_address = mac;
		return Seal(r);
	}
}

bool MessageHeader::IsClientMessage() const { return magic == kClientMagic; }
bool MessageHeader::IsServerMessage() const { return magic == kServerMagic; }

bool VersionResponse::IsValid() const { return HasValidCRC(*this); }
bool PortInfo::IsValid() const { return HasValidCRC(*this); }
bool DataResponse::IsValid() const { return HasValidCRC(*this); }

uint32_t DSUMessage::PacketCRC(const void* packet, size_t size)
{
	// computed over a copy with the CRC field cleared
	uint8_t copy[sizeof(DataResponse)];
	cemu_assert(size <= sizeof(copy) && size >= kCRCOffset + 4);
	memcpy(copy, packet, size);
	memset(copy + kCRCOffset, 0, 4);
	return crc32_calc(copy, size);
}

VersionRequest DSUMessage::Version(uint32_t uid)
{
	VersionRequest r{};
	r.header = ClientHeader(uid, MessageType::Version);
	return Seal(r);
}

ListPorts DSUMessage::PortList(uint32_t uid, uint32_t count, const std::array<uint8_t, 4>& indices)
{
	ListPorts r{};
	r.header = ClientHeader(uid, MessageType::Information);
	r.count = count;
	r.indices = indices;
	return Seal(r);
}

DataRequest DSUMessage::Data(uint32_t uid)
{
	return MakeDataRequest(uid, RegisterFlag::AllPads, 0, {});
}

DataRequest DSUMessage::Data(uint32_t uid, uint8_t index)
{
	return MakeDataRequest(uid, RegisterFlag::Index, index, {});
}

DataRequest DSUMessage::Data(uint32_t uid, const MACAddress_t& mac_address)
{
	return MakeDataRequest(uid, RegisterFlag::MACAddress, 0, mac_address);
}

DataRequest DSUMessage::Data(uint32_t uid, uint8_t index, const MACAddress_t& mac_address)
{
	return MakeDataRequest(uid, RegisterFlag::Index | RegisterFlag::MACAddress, index, mac_address);
}
