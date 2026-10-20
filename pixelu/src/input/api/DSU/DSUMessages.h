// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Wire format of the cemuhook "DSU" UDP protocol (version 1001), which motion servers
// such as DS4Windows, BetterJoy or SteamDeckGyroDSU speak. Every packet starts with a
// 16 byte header and a 32 bit message type; everything is little endian. The CRC32 is
// taken over the whole packet with the CRC field set to zero.

#include "Common/enumFlags.h"
#include "util/math/vector3.h"

#include <array>
#include <cstdint>

enum class DsState : uint8_t
{
	Disconnected = 0x00,
	Reserved = 0x01,
	Connected = 0x02
};

enum class DsConnection : uint8_t
{
	None = 0x00,
	Usb = 0x01,
	Bluetooth = 0x02
};

enum class DsModel : uint8_t
{
	None = 0,
	DS3 = 1,
	DS4 = 2,
	Generic = 3
};

enum class DsBattery : uint8_t
{
	None = 0x00,
	Dying = 0x01,
	Low = 0x02,
	Medium = 0x03,
	High = 0x04,
	Full = 0x05,
	Charging = 0xEE,
	Charged = 0xEF
};

// which pads a data request subscribes to
enum class RegisterFlag : uint8_t
{
	AllPads = 0x00,
	Index = 0x01,
	MACAddress = 0x02
};
ENABLE_BITMASK_OPERATORS(RegisterFlag);

enum class MessageType : uint32_t
{
	Version = 0x100000,
	Information = 0x100001,
	Data = 0x100002,
	Rumble = 0x100003, // not used
};

using MACAddress_t = std::array<uint8_t, 6>;

#pragma pack(push, 1)

struct MessageHeader
{
	uint32_t magic;            // "DSUC" from clients, "DSUS" from servers
	uint16_t protocol_version;
	uint16_t packet_size;      // bytes after this header
	uint32_t crc32;
	uint32_t uid;              // sender id
	MessageType type;          // strictly the first field of the payload

	uint16_t GetSize() const { return (uint16_t)(16 + packet_size); }
	MessageType GetMessageType() const { return type; }
	bool IsClientMessage() const;
	bool IsServerMessage() const;
};
static_assert(sizeof(MessageHeader) == 0x14);

// --- sent by us ---

struct VersionRequest
{
	MessageHeader header;
};
static_assert(sizeof(VersionRequest) == 0x14);

struct ListPorts
{
	MessageHeader header;
	uint32_t count;                 // how many entries of `indices` are used
	std::array<uint8_t, 4> indices;
};
static_assert(sizeof(ListPorts) == 0x1C);

struct DataRequest
{
	MessageHeader header;
	RegisterFlag flags;
	uint8_t index;
	MACAddress_t mac_address;
};
static_assert(sizeof(DataRequest) == 0x1C);

// --- sent by the server ---

struct VersionResponse
{
	MessageHeader header;
	uint16_t version;
	uint8_t padding[2];

	bool IsValid() const;
	uint16_t GetVersion() const { return version; }
};
static_assert(sizeof(VersionResponse) == 0x18);

// slot description, at the start of both port info and data packets
struct PortInfoData
{
	uint8_t index;
	DsState state;
	DsModel model;
	DsConnection connection;
	MACAddress_t mac_address;
	DsBattery battery;
	uint8_t is_active;
};
static_assert(sizeof(PortInfoData) == 0xC);

struct PortInfo
{
	MessageHeader header;
	PortInfoData info;

	bool IsValid() const;
	const PortInfoData& GetInfo() const { return info; }
	uint8_t GetIndex() const { return info.index; }
};
static_assert(sizeof(PortInfo) == 0x20);

struct TouchPoint
{
	uint8_t active;
	uint8_t index;
	int16_t x, y; // 1920 x 942 on a DS4
};
static_assert(sizeof(TouchPoint) == 0x6);

struct DataResponseData
{
	uint32_t m_packet_index;

	uint8_t state1; // share, L3, R3, options, up, right, down, left
	uint8_t state2; // L2, R2, L1, R1, triangle, circle, cross, square
	uint8_t ps;
	uint8_t touch;

	// sticks, 128 is centred; y grows upwards
	uint8_t lx, ly;
	uint8_t rx, ry;

	// analog buttons
	uint8_t dpad_left;
	uint8_t dpad_down;
	uint8_t dpad_right;
	uint8_t dpad_up;

	uint8_t square;
	uint8_t cross;
	uint8_t circle;
	uint8_t triangle;

	uint8_t r1;
	uint8_t l1;
	uint8_t r2;
	uint8_t l2;

	TouchPoint tpad1, tpad2;

	uint64_t motion_timestamp; // microseconds
	Vector3f accel;            // g
	Vector3f gyro;             // degrees per second
};
static_assert(sizeof(DataResponseData) == 0x44);

struct DataResponse
{
	MessageHeader header;
	PortInfoData info;
	DataResponseData data;

	bool IsValid() const;
	const PortInfoData& GetInfo() const { return info; }
	const DataResponseData& GetData() const { return data; }
	uint8_t GetIndex() const { return info.index; }
	uint64_t GetMotionTimestamp() const { return data.motion_timestamp; }
	const Vector3f& GetAcceleration() const { return data.accel; }
	const Vector3f& GetGyro() const { return data.gyro; }
};
static_assert(sizeof(DataResponse) == 0x64);

#pragma pack(pop)

namespace DSUMessage
{
	// requests, ready to send
	VersionRequest Version(uint32_t uid);
	// asks for the slot info of `count` slots
	ListPorts PortList(uint32_t uid, uint32_t count, const std::array<uint8_t, 4>& indices);
	// subscribes to the data of every slot
	DataRequest Data(uint32_t uid);
	// subscribes to the data of one slot
	DataRequest Data(uint32_t uid, uint8_t index);
	DataRequest Data(uint32_t uid, const MACAddress_t& mac_address);
	DataRequest Data(uint32_t uid, uint8_t index, const MACAddress_t& mac_address);

	// CRC of a packet of `size` bytes as the protocol defines it
	uint32_t PacketCRC(const void* packet, size_t size);
}
