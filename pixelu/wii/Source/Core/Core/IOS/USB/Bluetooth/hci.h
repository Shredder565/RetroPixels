// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The parts of the Bluetooth Host Controller Interface that the Wii's Bluetooth module (and the
// passthrough of a real adapter) needs: command opcodes, event codes, the ACL handle layout and
// a handful of packed wire structures. Everything here follows the HCI functional
// specification of the Bluetooth Core Specification (v1.1 to v2.1). All multi-byte fields are
// little endian on the wire, which is also the host byte order of every supported build.
//
// IOS strips the leading packet indicator byte of the UART transport, so command, event and
// ACL packets start directly with their own headers.

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

// A device address as it is sent over the air and through HCI: least significant byte first.
using bdaddr_t = std::array<u8, 6>;

namespace BluetoothHCI
{
constexpr std::size_t CLASS_SIZE = 3;
constexpr std::size_t FEATURES_SIZE = 8;
constexpr std::size_t LINK_KEY_SIZE = 16;
constexpr std::size_t NAME_SIZE = 248;

// An opcode is a 6-bit group (OGF) and a 10-bit command (OCF).
constexpr u16 MakeOpcode(u16 group, u16 command)
{
  return static_cast<u16>(((group & 0x3f) << 10) | (command & 0x3ff));
}
constexpr u16 OpcodeGroup(u16 opcode)
{
  return static_cast<u16>(opcode >> 10);
}
constexpr u16 OpcodeCommand(u16 opcode)
{
  return static_cast<u16>(opcode & 0x3ff);
}

namespace Group
{
constexpr u16 LINK_CONTROL = 0x01;
constexpr u16 LINK_POLICY = 0x02;
constexpr u16 CONTROLLER = 0x03;
constexpr u16 INFORMATIONAL = 0x04;
constexpr u16 VENDOR = 0x3f;
}  // namespace Group

namespace Opcode
{
// Link control.
constexpr u16 INQUIRY = MakeOpcode(Group::LINK_CONTROL, 0x0001);
constexpr u16 INQUIRY_CANCEL = MakeOpcode(Group::LINK_CONTROL, 0x0002);
constexpr u16 CREATE_CONNECTION = MakeOpcode(Group::LINK_CONTROL, 0x0005);
constexpr u16 DISCONNECT = MakeOpcode(Group::LINK_CONTROL, 0x0006);
constexpr u16 ACCEPT_CONNECTION = MakeOpcode(Group::LINK_CONTROL, 0x0009);
constexpr u16 LINK_KEY_REPLY = MakeOpcode(Group::LINK_CONTROL, 0x000b);
constexpr u16 LINK_KEY_NEGATIVE_REPLY = MakeOpcode(Group::LINK_CONTROL, 0x000c);
constexpr u16 CHANGE_PACKET_TYPE = MakeOpcode(Group::LINK_CONTROL, 0x000f);
constexpr u16 AUTHENTICATION_REQUESTED = MakeOpcode(Group::LINK_CONTROL, 0x0011);
constexpr u16 REMOTE_NAME_REQUEST = MakeOpcode(Group::LINK_CONTROL, 0x0019);
constexpr u16 READ_REMOTE_FEATURES = MakeOpcode(Group::LINK_CONTROL, 0x001b);
constexpr u16 READ_REMOTE_VERSION = MakeOpcode(Group::LINK_CONTROL, 0x001d);
constexpr u16 READ_CLOCK_OFFSET = MakeOpcode(Group::LINK_CONTROL, 0x001f);

// Link policy.
constexpr u16 SNIFF_MODE = MakeOpcode(Group::LINK_POLICY, 0x0003);
constexpr u16 QOS_SETUP = MakeOpcode(Group::LINK_POLICY, 0x0007);
constexpr u16 WRITE_LINK_POLICY = MakeOpcode(Group::LINK_POLICY, 0x000d);

// Controller and baseband.
constexpr u16 RESET = MakeOpcode(Group::CONTROLLER, 0x0003);
constexpr u16 SET_EVENT_FILTER = MakeOpcode(Group::CONTROLLER, 0x0005);
constexpr u16 WRITE_PIN_TYPE = MakeOpcode(Group::CONTROLLER, 0x000a);
constexpr u16 READ_STORED_LINK_KEY = MakeOpcode(Group::CONTROLLER, 0x000d);
constexpr u16 WRITE_STORED_LINK_KEY = MakeOpcode(Group::CONTROLLER, 0x0011);
constexpr u16 DELETE_STORED_LINK_KEY = MakeOpcode(Group::CONTROLLER, 0x0012);
constexpr u16 WRITE_LOCAL_NAME = MakeOpcode(Group::CONTROLLER, 0x0013);
constexpr u16 WRITE_PAGE_TIMEOUT = MakeOpcode(Group::CONTROLLER, 0x0018);
constexpr u16 WRITE_SCAN_ENABLE = MakeOpcode(Group::CONTROLLER, 0x001a);
constexpr u16 WRITE_CLASS_OF_DEVICE = MakeOpcode(Group::CONTROLLER, 0x0024);
constexpr u16 HOST_BUFFER_SIZE = MakeOpcode(Group::CONTROLLER, 0x0033);
constexpr u16 WRITE_LINK_SUPERVISION_TIMEOUT = MakeOpcode(Group::CONTROLLER, 0x0037);
constexpr u16 WRITE_INQUIRY_SCAN_TYPE = MakeOpcode(Group::CONTROLLER, 0x0043);
constexpr u16 WRITE_INQUIRY_MODE = MakeOpcode(Group::CONTROLLER, 0x0045);
constexpr u16 WRITE_PAGE_SCAN_TYPE = MakeOpcode(Group::CONTROLLER, 0x0047);

// Informational.
constexpr u16 READ_LOCAL_VERSION = MakeOpcode(Group::INFORMATIONAL, 0x0001);
constexpr u16 READ_LOCAL_FEATURES = MakeOpcode(Group::INFORMATIONAL, 0x0003);
constexpr u16 READ_BUFFER_SIZE = MakeOpcode(Group::INFORMATIONAL, 0x0005);
constexpr u16 READ_BD_ADDR = MakeOpcode(Group::INFORMATIONAL, 0x0009);

// Broadcom vendor commands sent by the Wii's Bluetooth stack (WUD) during setup.
constexpr u16 VENDOR_FC4C = MakeOpcode(Group::VENDOR, 0x004c);
constexpr u16 VENDOR_FC4F = MakeOpcode(Group::VENDOR, 0x004f);
}  // namespace Opcode

namespace Event
{
constexpr u8 INQUIRY_COMPLETE = 0x01;
constexpr u8 INQUIRY_RESULT = 0x02;
constexpr u8 CONNECTION_COMPLETE = 0x03;
constexpr u8 CONNECTION_REQUEST = 0x04;
constexpr u8 DISCONNECTION_COMPLETE = 0x05;
constexpr u8 AUTHENTICATION_COMPLETE = 0x06;
constexpr u8 REMOTE_NAME_REQUEST_COMPLETE = 0x07;
constexpr u8 READ_REMOTE_FEATURES_COMPLETE = 0x0b;
constexpr u8 READ_REMOTE_VERSION_COMPLETE = 0x0c;
constexpr u8 QOS_SETUP_COMPLETE = 0x0d;
constexpr u8 COMMAND_COMPLETE = 0x0e;
constexpr u8 COMMAND_STATUS = 0x0f;
constexpr u8 ROLE_CHANGE = 0x12;
constexpr u8 NUMBER_OF_COMPLETED_PACKETS = 0x13;
constexpr u8 MODE_CHANGE = 0x14;
constexpr u8 RETURN_LINK_KEYS = 0x15;
constexpr u8 LINK_KEY_REQUEST = 0x17;
constexpr u8 LINK_KEY_NOTIFICATION = 0x18;
constexpr u8 READ_CLOCK_OFFSET_COMPLETE = 0x1c;
constexpr u8 PACKET_TYPE_CHANGED = 0x1d;
constexpr u8 VENDOR = 0xff;
}  // namespace Event

// Write Scan Enable bits.
constexpr u8 SCAN_INQUIRY = 0x01;
constexpr u8 SCAN_PAGE = 0x02;

// Link types reported in connection events.
constexpr u8 LINK_TYPE_SCO = 0x00;
constexpr u8 LINK_TYPE_ACL = 0x01;

// QoS service types.
constexpr u8 QOS_BEST_EFFORT = 0x01;
constexpr u8 QOS_GUARANTEED = 0x02;

// The 16-bit handle field of an ACL packet: a 12-bit connection handle, then the packet
// boundary flag (2 bits) and the broadcast flag (2 bits).
constexpr u16 ACL_HANDLE_MASK = 0x0fff;
constexpr u16 ACL_FIRST_FLUSHABLE = 0x2;  // packet boundary: start of an L2CAP packet
constexpr u16 ACL_POINT_TO_POINT = 0x0;   // broadcast flag: not a broadcast

constexpr u16 MakeACLHandle(u16 handle, u16 boundary, u16 broadcast)
{
  return static_cast<u16>((handle & ACL_HANDLE_MASK) | ((boundary & 3) << 12) |
                          ((broadcast & 3) << 14));
}
constexpr u16 ACLConnectionHandle(u16 field)
{
  return static_cast<u16>(field & ACL_HANDLE_MASK);
}
constexpr u16 ACLBoundaryFlag(u16 field)
{
  return static_cast<u16>((field >> 12) & 3);
}
constexpr u16 ACLBroadcastFlag(u16 field)
{
  return static_cast<u16>((field >> 14) & 3);
}

#pragma pack(push, 1)
struct CommandHeader
{
  u16 opcode;
  u8 length;  // parameter bytes that follow
};
static_assert(sizeof(CommandHeader) == 3);

struct EventHeader
{
  u8 code;
  u8 length;  // parameter bytes that follow
};
static_assert(sizeof(EventHeader) == 2);

struct ACLHeader
{
  u16 handle;  // see MakeACLHandle
  u16 length;  // payload bytes that follow
};
static_assert(sizeof(ACLHeader) == 4);

// Parameters of the Command Complete event (followed by the command's return parameters).
struct CommandCompleteParams
{
  u8 num_command_packets;
  u16 opcode;
};
static_assert(sizeof(CommandCompleteParams) == 3);

// Parameters of the Command Status event.
struct CommandStatusParams
{
  u8 status;
  u8 num_command_packets;
  u16 opcode;
};
static_assert(sizeof(CommandStatusParams) == 4);

// Return parameters of Read Buffer Size.
struct ReadBufferSizeReply
{
  u8 status;
  u16 acl_packet_length;
  u8 sco_packet_length;
  u16 acl_packet_count;
  u16 sco_packet_count;
};
static_assert(sizeof(ReadBufferSizeReply) == 8);

// Return parameters of Read Local Version Information.
struct ReadLocalVersionReply
{
  u8 status;
  u8 hci_version;
  u16 hci_revision;
  u8 lmp_version;
  u16 manufacturer;
  u16 lmp_subversion;
};
static_assert(sizeof(ReadLocalVersionReply) == 9);

// Parameters of Delete Stored Link Key.
struct DeleteStoredLinkKeyParams
{
  bdaddr_t address;
  u8 delete_all;
};
static_assert(sizeof(DeleteStoredLinkKeyParams) == 7);

// Parameters of QoS Setup.
struct QoSSetupParams
{
  u16 connection_handle;
  u8 flags;
  u8 service_type;
  u32 token_rate;
  u32 peak_bandwidth;
  u32 latency;
  u32 delay_variation;
};
static_assert(sizeof(QoSSetupParams) == 20);

// Parameters of the Link Key Notification event.
struct LinkKeyNotificationParams
{
  bdaddr_t address;
  std::array<u8, LINK_KEY_SIZE> key;
  u8 key_type;
};
static_assert(sizeof(LinkKeyNotificationParams) == 23);
#pragma pack(pop)
}  // namespace BluetoothHCI
