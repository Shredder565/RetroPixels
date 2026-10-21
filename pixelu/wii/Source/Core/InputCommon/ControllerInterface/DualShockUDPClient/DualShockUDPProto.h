// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstring>
#include <optional>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"
#include "Common/Logging/Log.h"

// Wire format of the "DSU" (cemuhook) protocol, in which a server (DS4Windows, BetterJoy and
// others) streams controller state, including motion data, to clients over UDP. Every packet
// starts with a 16-byte header followed by a 32-bit message type; all fields are little endian
// and packed, which these structs assume of the host.
namespace ciface::DualShockUDPClient::Proto
{
constexpr u16 CEMUHOOK_PROTOCOL_VERSION = 1001;

// A server reports up to four controller slots.
constexpr int PORT_COUNT = 4;

using Magic = std::array<u8, 4>;
constexpr Magic CLIENT{'D', 'S', 'U', 'C'};
constexpr Magic SERVER{'D', 'S', 'U', 'S'};

#pragma pack(push, 1)

enum class DsState : u8
{
  Disconnected = 0x00,
  Reserved = 0x01,
  Connected = 0x02
};

enum class DsConnection : u8
{
  None = 0x00,
  Usb = 0x01,
  Bluetooth = 0x02
};

enum class DsModel : u8
{
  None = 0,
  PartialGyro = 1,
  FullGyro = 2,
  Generic = 3
};

enum class DsBattery : u8
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

// How a pad data request selects the slots to stream.
enum RegisterFlags : u8
{
  AllPads = 0x00,
  PadID = 0x01,
  PadMACAdddress = 0x02
};

struct Header
{
  Magic magic;
  u16 protocol_version;
  // Packet size minus the header.
  u16 payload_size;
  // CRC-32 of the whole packet, computed with this field zero.
  u32 crc32;
  // Random id identifying the sender for the session.
  u32 sender_id;
};
static_assert(sizeof(Header) == 16);

struct Touch
{
  u8 active;
  u8 id;
  s16 x;
  s16 y;
};

// Slot description shared by the port info and pad data replies.
struct SlotInfo
{
  u8 pad_id;
  DsState pad_state;
  DsModel model;
  DsConnection connection_type;
  std::array<u8, 6> pad_mac_address;
  DsBattery battery_status;
};

// Client: which protocol version does the server speak?
struct VersionRequest
{
  static constexpr Magic FROM = CLIENT;
  static constexpr u32 TYPE = 0x100000;
  Header header;
  u32 message_type;
};

struct VersionResponse
{
  static constexpr Magic FROM = SERVER;
  static constexpr u32 TYPE = 0x100000;
  Header header;
  u32 message_type;
  u16 max_protocol_version;
  std::array<u8, 2> padding;
};

// Client: describe these slots. The server answers with one PortInfo per connected slot.
struct ListPorts
{
  static constexpr Magic FROM = CLIENT;
  static constexpr u32 TYPE = 0x100001;
  Header header;
  u32 message_type;
  u32 pad_request_count;
  std::array<u8, 4> pad_ids;
};

struct PortInfo
{
  static constexpr Magic FROM = SERVER;
  static constexpr u32 TYPE = 0x100001;
  Header header;
  u32 message_type;
  SlotInfo slot;
  u8 padding;
};

// Client: stream this slot's state to me (for a few seconds; the request is repeated).
struct PadDataRequest
{
  static constexpr Magic FROM = CLIENT;
  static constexpr u32 TYPE = 0x100002;
  Header header;
  u32 message_type;
  RegisterFlags register_flags;
  u8 pad_id_to_register;
  std::array<u8, 6> mac_address_to_register;
};

struct PadDataResponse
{
  static constexpr Magic FROM = SERVER;
  static constexpr u32 TYPE = 0x100002;
  Header header;
  u32 message_type;
  SlotInfo slot;
  u8 active;
  u32 hid_packet_counter;
  // Share, L3, R3, Options in bits 0-3; the d-pad in bits 4-7.
  u8 button_states1;
  // L2, R2, L1, R1, Triangle, Circle, Cross, Square.
  u8 button_states2;
  u8 button_ps;
  u8 button_touch;
  u8 left_stick_x;
  u8 left_stick_y_inverted;
  u8 right_stick_x;
  u8 right_stick_y_inverted;
  u8 button_dpad_left_analog;
  u8 button_dpad_down_analog;
  u8 button_dpad_right_analog;
  u8 button_dpad_up_analog;
  u8 button_square_analog;
  u8 button_cross_analog;
  u8 button_circle_analog;
  u8 button_triangle_analog;
  u8 button_r1_analog;
  u8 button_l1_analog;
  u8 trigger_r2;
  u8 trigger_l2;
  Touch touch1;
  Touch touch2;
  u64 accelerometer_timestamp_us;
  float accelerometer_x_g;
  float accelerometer_y_g;
  float accelerometer_z_g;
  float gyro_pitch_deg_s;
  float gyro_yaw_deg_s;
  float gyro_roll_deg_s;
};
static_assert(sizeof(PadDataResponse) == 100);

#pragma pack(pop)

// A packet to send: header and type filled in for `sender_id`; call Seal() once the payload is
// written.
template <typename Msg>
Msg NewMessage(u32 sender_id)
{
  Msg msg{};
  msg.header.magic = Msg::FROM;
  msg.header.protocol_version = CEMUHOOK_PROTOCOL_VERSION;
  msg.header.payload_size = static_cast<u16>(sizeof(Msg) - sizeof(Header));
  msg.header.sender_id = sender_id;
  msg.message_type = Msg::TYPE;
  return msg;
}

template <typename Msg>
void Seal(Msg* msg)
{
  msg->header.crc32 = 0;
  msg->header.crc32 = Common::ComputeCRC32(reinterpret_cast<const u8*>(msg), sizeof(Msg));
}

// Room for the largest packet a server sends. Received packets are decoded straight from it.
struct ReceiveBuffer
{
  // Checks that the buffer holds a valid `Msg` and returns a copy of it. The checksum is taken
  // over sizeof(Msg) bytes whatever the datagram's length was, and the header's checksum field
  // is left zeroed afterwards.
  template <typename Msg>
  std::optional<Msg> Decode()
  {
    Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));

    const u32 claimed_crc = header.crc32;
    header.crc32 = 0;
    std::memcpy(bytes.data(), &header, sizeof(header));

    const u32 actual_crc = Common::ComputeCRC32(bytes.data(), sizeof(Msg));
    if (actual_crc != claimed_crc)
    {
      NOTICE_LOG_FMT(
          CONTROLLERINTERFACE,
          "DualShockUDPClient Received message with bad CRC in header: got {:08x}, expected {:08x}",
          claimed_crc, actual_crc);
      return std::nullopt;
    }

    u32 message_type;
    std::memcpy(&message_type, bytes.data() + sizeof(Header), sizeof(message_type));

    const bool acceptable = header.protocol_version <= CEMUHOOK_PROTOCOL_VERSION &&
                            header.magic == Msg::FROM && message_type == Msg::TYPE &&
                            header.payload_size + sizeof(Header) <= sizeof(Msg);
    if (!acceptable)
      return std::nullopt;

    Msg msg;
    std::memcpy(&msg, bytes.data(), sizeof(msg));
    return msg;
  }

  std::array<u8, sizeof(PadDataResponse)> bytes{};
};

}  // namespace ciface::DualShockUDPClient::Proto
