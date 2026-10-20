// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The L2CAP constants and headers used by the emulated Wii Remote (Bluetooth Core
// Specification, Logical Link Control and Adaptation Protocol). Fields are little endian.

#include "Common/CommonTypes.h"

namespace BluetoothL2CAP
{
// Channel identifiers.
constexpr u16 CID_NULL = 0x0000;
constexpr u16 CID_SIGNALING = 0x0001;
// Dynamically allocated channels start here.
constexpr u16 CID_FIRST_DYNAMIC = 0x0040;

// Protocol/service multiplexers.
constexpr u16 PSM_NONE = 0x0000;
constexpr u16 PSM_SDP = 0x0001;
constexpr u16 PSM_HID_CONTROL = 0x0011;
constexpr u16 PSM_HID_INTERRUPT = 0x0013;

// Signaling command codes.
namespace Signal
{
constexpr u8 COMMAND_REJECT = 0x01;
constexpr u8 CONNECTION_REQUEST = 0x02;
constexpr u8 CONNECTION_RESPONSE = 0x03;
constexpr u8 CONFIGURE_REQUEST = 0x04;
constexpr u8 CONFIGURE_RESPONSE = 0x05;
constexpr u8 DISCONNECTION_REQUEST = 0x06;
constexpr u8 DISCONNECTION_RESPONSE = 0x07;
}  // namespace Signal

// Connection response results and status.
constexpr u16 RESULT_SUCCESS = 0x0000;
constexpr u16 RESULT_NO_RESOURCES = 0x0004;
constexpr u16 STATUS_NONE = 0x0000;

// Configuration options.
constexpr u8 OPTION_MTU = 0x01;
constexpr u8 OPTION_FLUSH_TIMEOUT = 0x02;
constexpr u8 OPTION_MTU_LENGTH = 2;
constexpr u8 OPTION_FLUSH_TIMEOUT_LENGTH = 2;

// Values that apply when the corresponding option is absent.
constexpr u16 DEFAULT_MTU = 672;
constexpr u16 DEFAULT_FLUSH_TIMEOUT = 0xffff;

#pragma pack(push, 1)
// Basic L2CAP header in front of every packet.
struct PacketHeader
{
  u16 length;  // payload bytes that follow
  u16 cid;     // destination channel
};
static_assert(sizeof(PacketHeader) == 4);

// Header of one command on the signaling channel.
struct SignalHeader
{
  u8 code;
  u8 identifier;
  u16 length;  // parameter bytes that follow
};
static_assert(sizeof(SignalHeader) == 4);

// Header of one configuration option.
struct OptionHeader
{
  u8 type;
  u8 length;
};
static_assert(sizeof(OptionHeader) == 2);
#pragma pack(pop)
}  // namespace BluetoothL2CAP
