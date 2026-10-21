// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// The base's packet format and the figure format follow public community documentation of the
// Disney Infinity base. Figures carry a 7-byte tag ID in block 0 and the character number in
// block 1, which is AES-128 encrypted with a key derived (SHA-1) from a fixed string and the tag
// ID.

#include "Core/IOS/USB/Emulated/Infinity.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Common/Assert.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/StringUtil.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace IOS::HLE::USB
{
static constexpr std::array<u8, 32> SHA1_CONSTANT = {
    0xAF, 0x62, 0xD2, 0xEC, 0x04, 0x91, 0x96, 0x8C, 0xC5, 0x2A, 0x1A, 0x71, 0x65, 0xF8, 0x65, 0xFE,
    0x28, 0x63, 0x29, 0x20, 0x44, 0x69, 0x73, 0x6e, 0x65, 0x79, 0x20, 0x32, 0x30, 0x31, 0x33};

namespace
{
// Character numbers (https://disneyinfinity.fandom.com/wiki/Model_Numbers).
struct InfinityToy
{
  u32 number;
  const char* name;
};

// clang-format off
constexpr InfinityToy TOYS[] = {
    {0x0F4241, "The Incredibles - Mr. Incredible"},
    {0x0F4242, "Monsters University - Sulley"},
    {0x0F4243, "Pirates of the Caribbean - Jack Sparrow"},
    {0x0F4244, "The Lone Ranger - The Lone Ranger"},
    {0x0F4245, "The Lone Ranger - Tonto"},
    {0x0F4246, "Cars - Lightning McQueen"},
    {0x0F4247, "Cars - Holley Shiftwell"},
    {0x0F4248, "Toy Story - Buzz Lightyear"},
    {0x0F4249, "Toy Story - Jessie"},
    {0x0F424A, "Monsters University - Mike Wazowski"},
    {0x0F424B, "The Incredibles - Mrs. Incredible"},
    {0x0F424C, "Pirates of the Caribbean - Barbossa"},
    {0x0F424D, "Pirates of the Caribbean - Davy Jones"},
    {0x0F424E, "Monsters University - Randy"},
    {0x0F424F, "The Incredibles - Syndrome"},
    {0x0F4250, "Toy Story - Woody"},
    {0x0F4251, "Cars - Mater"},
    {0x0F4252, "The Incredibles - Dash"},
    {0x0F4253, "The Incredibles - Violet"},
    {0x0F4254, "Cars - Francesco Bernoulli"},
    {0x0F4255, "Fantasia - Sorcerer's Apprentice Mickey"},
    {0x0F4256, "The Nightmare Before Christmas - Jack Skellington"},
    {0x0F4257, "Tangled - Rapunzel"},
    {0x0F4258, "Frozen - Anna"},
    {0x0F4259, "Frozen - Elsa"},
    {0x0F425A, "Phineas and Ferb - Phineas Flynn"},
    {0x0F425B, "Phineas and Ferb - Agent P"},
    {0x0F425C, "Wreck-It Ralph - Wreck-It Ralph"},
    {0x0F425D, "Wreck-It Ralph - Vanellope von Schweetz"},
    {0x0F425E, "The Incredibles - Mr. Incredible (Crystal)"},
    {0x0F425F, "Pirates of the Caribbean - Jack Sparrow (Crystal)"},
    {0x0F4260, "Monsters University - Sulley (Crystal)"},
    {0x0F4261, "Cars - Lightning McQueen (Crystal)"},
    {0x0F4262, "The Lone Ranger - The Lone Ranger (Crystal)"},
    {0x0F4263, "Toy Story - Buzz Lightyear (Crystal)"},
    {0x0F4264, "Phineas and Ferb - Agent P (Crystal)"},
    {0x0F4265, "Fantasia - Sorcerer's Apprentice Mickey (Crystal)"},
    {0x0F4266, "Toy Story - Buzz Lightyear (Glowing)"},
    {0x1E8481, "The Incredibles - Pirates of the Caribbean - Monsters University Play Set"},
    {0x1E8482, "The Lone Ranger Play Set"},
    {0x1E8483, "Cars Play Set"},
    {0x1E8484, "Toy Story in Space Play Set"},
    {0x2DC6C3, "Bolt - Bolt's Super Strength - Ability"},
    {0x2DC6C4, "Wreck-it Ralph - Ralph's Power of Destruction - Ability"},
    {0x2DC6C5, "Fantasia - Chernabog's Power - Ability"},
    {0x2DC6C6, "Cars - C.H.R.O.M.E. Damage Increaser - Ability"},
    {0x2DC6C7, "Phineas and Ferb - Dr. Doofenshmirtz's Damage-Inator! - Ability"},
    {0x2DC6C8, "Frankenweenie - Electro-Charge - Ability"},
    {0x2DC6C9, "Wreck-It Ralph - Fix-It Felix's Repair Power - Ability"},
    {0x2DC6CA, "Tangled - Rapunzel's Healing - Ability"},
    {0x2DC6CB, "Cars - C.H.R.O.M.E. Armor Shield - Ability"},
    {0x2DC6CC, "Toy Story - Star Command Shield - Ability"},
    {0x2DC6CD, "The Incredibles - Violet's Force Field - Ability"},
    {0x2DC6CE, "Pirates of the Caribbean - Pieces of Eight - Ability"},
    {0x2DC6CF, "DuckTales - Scrooge McDuck's Lucky Dime - Ability"},
    {0x2DC6D0, "TRON - User Control Disc - Ability"},
    {0x2DC6D1, "Fantasia - Mickey's Sorcerer Hat - Ability"},
    {0x2DC6FE, "Toy Story - Emperor Zurg's Wrath - Ability"},
    {0x2DC6FF, "The Sword in the Stone - Merlin's Summon - Ability"},
    {0x3D0912, "Mickey Mouse Universe - Mickey's Car - Toy (Vehicle)"},
    {0x3D0913, "Cinderella - Cinderella's Coach - Toy (Vehicle)"},
    {0x3D0914, "The Muppets - Electric Mayhem Bus - Toy (Vehicle)"},
    {0x3D0915, "101 Dalmatians - Cruella De Vil's Car - Toy (Vehicle)"},
    {0x3D0916, "Toy Story - Pizza Planet Delivery Truck - Toy (Vehicle)"},
    {0x3D0917, "Monsters, Inc. - Mike's New Car - Toy (Vehicle)"},
    {0x3D0919, "Disney Parks - Disney Parks Parking Lot Tram - Toy (Vehicle)"},
    {0x3D091A, "Peter Pan, Disney Parks - Jolly Roger - Toy (Aircraft)"},
    {0x3D091B, "Dumbo, Disney Parks - Dumbo the Flying Elephant - Toy (Aircraft)"},
    {0x3D091C, "Bolt - Calico Helicopter - Toy (Aircraft)"},
    {0x3D091D, "Tangled - Maximus - Toy (Mount)"},
    {0x3D091E, "Brave - Angus - Toy (Mount)"},
    {0x3D091F, "Aladdin - Abu the Elephant - Toy (Mount)"},
    {0x3D0920, "The Adventures of Ichabod and Mr. Toad - Headless Horseman's Horse - Toy (Mount)"},
    {0x3D0921, "Beauty and the Beast - Phillipe - Toy (Mount)"},
    {0x3D0922, "Mulan - Khan - Toy (Mount)"},
    {0x3D0923, "Tarzan - Tantor - Toy (Mount)"},
    {0x3D0924, "Mulan - Dragon Firework Cannon - Toy (Weapon)"},
    {0x3D0925, "Lilo & Stitch - Stitch's Blaster - Toy (Weapon)"},
    {0x3D0926, "Toy Story, Disney Parks - Toy Story Mania Blaster - Toy (Weapon)"},
    {0x3D0927, "Alice in Wonderland - Flamingo Croquet Mallet - Toy (Weapon)"},
    {0x3D0928, "Up - Carl Fredricksen's Cane - Toy (Weapon)"},
    {0x3D0929, "Lilo & Stitch - Hangin' Ten Stitch With Surfboard - Toy (Hoverboard)"},
    {0x3D092A, "Condorman - Condorman Glider - Toy (Glider)"},
    {0x3D092B, "WALL-E - WALL-E's Fire Extinguisher - Toy (Jetpack)"},
    {0x3D092C, "TRON - On the Grid - Customization (Terrain)"},
    {0x3D092D, "WALL-E - WALL-E's Collection - Customization (Terrain)"},
    {0x3D092E, "Wreck-It Ralph - King Candy's Dessert Toppings - Customization (Terrain)"},
    {0x3D0930, "Frankenweenie - Victor's Experiments - Customization (Terrain)"},
    {0x3D0931,
     "The Nightmare Before Christmas - Jack's Scary Decorations - Customization (Terrain)"},
    {0x3D0933, "Frozen - Frozen Flourish - Customization (Terrain)"},
    {0x3D0934, "Tangled - Rapunzel's Kingdom - Customization (Terrain)"},
    {0x3D0935, "TRON - TRON Interface - Customization (Skydome)"},
    {0x3D0936, "WALL-E - Buy N Large Atmosphere - Customization (Skydome)"},
    {0x3D0937, "Wreck-It Ralph - Sugar Rush Sky - Customization (Skydome)"},
    {0x3D093A, "The Nightmare Before Christmas - Halloween Town Sky - Customization (Skydome)"},
    {0x3D093C, "Frozen - Chill in the Air - Customization (Skydome)"},
    {0x3D093D, "Tangled - Rapunzel's Birthday Sky - Customization (Skydome)"},
    {0x3D0940, "Toy Story, Disney Parks - Astro Blasters Space Cruiser - Toy (Vehicle)"},
    {0x3D0941, "Finding Nemo - Marlin's Reef - Customization (Terrain)"},
    {0x3D0942, "Finding Nemo - Nemo's Seascape - Customization (Skydome)"},
    {0x3D0943, "Alice in Wonderland - Alice's Wonderland - Customization (Terrain)"},
    {0x3D0944, "Alice in Wonderland - Tulgey Wood - Customization (Skydome)"},
    {0x3D0945, "Phineas and Ferb - Tri-State Area Terrain - Customization (Terrain)"},
    {0x3D0946, "Phineas and Ferb - Danville Sky - Customization (Skydome)"},
};
// clang-format on

template <std::size_t... I>
constexpr auto MakeFigureList(std::index_sequence<I...>)
{
  return std::array<std::pair<const char*, const u32>, sizeof...(I)>{
      {{TOYS[I].name, TOYS[I].number}...}};
}

constexpr auto FIGURE_LIST = MakeFigureList(std::make_index_sequence<std::size(TOYS)>{});

using Packet = std::array<u8, 32>;
using FigureBytes = std::array<u8, INFINITY_NUM_BLOCKS * INFINITY_BLOCK_SIZE>;

constexpr u8 REPLY = 0xAA;
constexpr u8 FIGURE_CHANGE = 0xAB;
constexpr u8 COMMAND = 0xFF;

// Reply delays in microseconds.
constexpr u64 ECHO_DELAY_US = 500;
constexpr u64 REPLY_DELAY_US = 1000;

// The game's mask for spreading a 32-bit number over 64 bits.
constexpr u64 SCRAMBLE_MASK = 0x8E55AA1B3999E8AA;

// The low byte of the sum of the first `count` bytes.
u8 Checksum(const Packet& packet, std::size_t count)
{
  u32 sum = 0;
  for (std::size_t i = 0; i < count; ++i)
    sum += packet[i];
  return static_cast<u8>(sum);
}

// The figure block that the game's block number refers to: 0 is block 1, n is block 4n.
// (Computed in eight bits, as the base does.)
u8 FigureBlock(u8 block)
{
  return block == 0 ? u8{1} : static_cast<u8>(block * 4);
}

// CRC-32 (reflected polynomial 0xEDB88320) without the usual initial value and final XOR, over
// the first 12 bytes of a block.
constexpr std::array<u32, 256> MakeCRCTable()
{
  std::array<u32, 256> table{};
  for (u32 n = 0; n < 256; ++n)
  {
    u32 value = n;
    for (int bit = 0; bit < 8; ++bit)
      value = (value & 1) != 0 ? (value >> 1) ^ 0xEDB88320 : value >> 1;
    table[n] = value;
  }
  return table;
}

u32 BlockCRC(const std::array<u8, 16>& block)
{
  static constexpr auto TABLE = MakeCRCTable();
  u32 crc = 0;
  for (std::size_t i = 0; i < 12; ++i)
    crc = (crc >> 8) ^ TABLE[(crc ^ block[i]) & 0xFF];
  return crc;
}

// The AES key of a figure: SHA-1 over the fixed string (without its last byte) and the tag ID;
// the first 16 bytes of the digest with each 32-bit word byte-swapped.
std::array<u8, 16> FigureKey(std::span<const u8> tag_id)
{
  std::vector<u8> message(SHA1_CONSTANT.begin(), SHA1_CONSTANT.end() - 1);
  message.insert(message.end(), tag_id.begin(), tag_id.end());
  const Common::SHA1::Digest digest = Common::SHA1::CalculateDigest(message);

  std::array<u8, 16> key{};
  for (std::size_t word = 0; word < 4; ++word)
  {
    for (std::size_t b = 0; b < 4; ++b)
      key[4 * word + b] = digest[4 * word + 3 - b];
  }
  return key;
}

// Block 1 of a new figure: the character number, a manufacturing date (13/08/18, the release of
// Infinity 1.0), fixed bytes and the CRC.
std::array<u8, 16> CharacterBlock(u32 number)
{
  std::array<u8, 16> block{};
  block[1] = static_cast<u8>(number >> 16);
  block[2] = static_cast<u8>(number >> 8);
  block[3] = static_cast<u8>(number);
  block[4] = 0x0D;
  block[5] = 0x08;
  block[6] = 0x12;
  block[9] = 0x01;
  block[10] = 0xD1;
  block[11] = 0x1F;
  const u32 crc = BlockCRC(block);
  for (std::size_t b = 0; b < 4; ++b)
    block[12 + b] = static_cast<u8>(crc >> (24 - 8 * b));
  return block;
}

// Puts the bits of `value` (lowest first) where the mask has ones, read from its lowest bit and
// written from the result's highest bit.
u64 Scramble(u32 value)
{
  u64 result = 0;
  for (int i = 0; i < 64; ++i)
  {
    if (((SCRAMBLE_MASK >> i) & 1) != 0)
    {
      result |= u64{value & 1} << (63 - i);
      value >>= 1;
    }
  }
  return result;
}

// Collects the bits of `value` (lowest first) where the mask, read from its highest bit, has
// ones.
u32 Descramble(u64 value)
{
  u32 result = 0;
  for (int i = 0; i < 64; ++i)
  {
    if (((SCRAMBLE_MASK >> (63 - i)) & 1) != 0)
      result = (result << 1) | static_cast<u32>((value >> i) & 1);
  }
  return result;
}

FigureBasePosition PadOf(FigureUIPosition position)
{
  const auto index = static_cast<u8>(position);
  if (index < 3)
    return FigureBasePosition::HexagonSlot;
  if (index < 6)
    return FigureBasePosition::PlayerOneSlot;
  if (index < 9)
    return FigureBasePosition::PlayerTwoSlot;
  return FigureBasePosition::Unknown;
}

void ReplyHeader(Packet& packet, u8 length, u8 sequence)
{
  packet[0] = REPLY;
  packet[1] = length;
  packet[2] = sequence;
}
}  // namespace

InfinityUSB::InfinityUSB()
{
  m_vid = 0x0E6F;
  m_pid = 0x0129;
  m_id = (u64{m_vid} << 32) | (u64{m_pid} << 16) | (u64{9} << 8) | u64{1};
  m_device_descriptor = DeviceDescriptor{0x12,   0x1,    0x200, 0x0, 0x0, 0x0, 0x20,
                                         0x0E6F, 0x0129, 0x200, 0x1, 0x2, 0x3, 0x1};
  m_config_descriptor.emplace_back(ConfigDescriptor{0x9, 0x2, 0x29, 0x1, 0x1, 0x0, 0x80, 0xFA});
  m_interface_descriptor.emplace_back(
      InterfaceDescriptor{0x9, 0x4, 0x0, 0x0, 0x2, 0x3, 0x0, 0x0, 0x0});
  // Interrupt IN 0x81 and OUT 0x01, 32 bytes, every frame.
  m_endpoint_descriptor.emplace_back(EndpointDescriptor{0x7, 0x5, 0x81, 0x3, 0x20, 0x1});
  m_endpoint_descriptor.emplace_back(EndpointDescriptor{0x7, 0x5, 0x1, 0x3, 0x20, 0x1});
}

InfinityUSB::~InfinityUSB() = default;

DeviceDescriptor InfinityUSB::GetDeviceDescriptor() const
{
  return m_device_descriptor;
}

std::vector<ConfigDescriptor> InfinityUSB::GetConfigurations() const
{
  return m_config_descriptor;
}

std::vector<InterfaceDescriptor> InfinityUSB::GetInterfaces(u8 /*config*/) const
{
  return m_interface_descriptor;
}

std::vector<EndpointDescriptor> InfinityUSB::GetEndpoints(u8 /*config*/, u8 /*iface*/,
                                                          u8 /*alt*/) const
{
  return m_endpoint_descriptor;
}

bool InfinityUSB::Attach()
{
  if (!m_device_attached)
  {
    DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x}] Opening device", m_vid, m_pid);
    m_device_attached = true;
  }
  return true;
}

bool InfinityUSB::AttachAndChangeInterface(const u8 iface)
{
  if (!Attach())
    return false;
  return iface == m_active_interface || ChangeInterface(iface) == 0;
}

int InfinityUSB::CancelTransfer(const u8 endpoint)
{
  INFO_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Cancelling transfers (endpoint {:#x})", m_vid, m_pid,
               m_active_interface, endpoint);
  return IPC_SUCCESS;
}

int InfinityUSB::ChangeInterface(const u8 iface)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Changing interface to {}", m_vid, m_pid,
                m_active_interface, iface);
  m_active_interface = iface;
  return 0;
}

int InfinityUSB::GetNumberOfAltSettings(u8 /*iface*/)
{
  return 0;
}

int InfinityUSB::SetAltSetting(u8 /*alt_setting*/)
{
  return 0;
}

int InfinityUSB::SubmitTransfer(std::unique_ptr<CtrlMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}] Control: bRequestType={:02x} bRequest={:02x} wValue={:04x}"
                " wIndex={:04x} wLength={:04x}",
                m_vid, m_pid, m_active_interface, cmd->request_type, cmd->request, cmd->value,
                cmd->index, cmd->length);
  return 0;
}

int InfinityUSB::SubmitTransfer(std::unique_ptr<BulkMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Bulk: length={:04x} endpoint={:02x}", m_vid, m_pid,
                m_active_interface, cmd->length, cmd->endpoint);
  return 0;
}

// Packets in: FF <length> <command> <sequence> <data...> <checksum> runs a command; 00 (the
// first read) and AA/AB wait for a reply. Other packets are ignored.
int InfinityUSB::SubmitTransfer(std::unique_ptr<IntrMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB, "[{:04x}:{:04x} {}] Interrupt: length={:04x} endpoint={:02x}", m_vid,
                m_pid, m_active_interface, cmd->length, cmd->endpoint);

  auto& system = cmd->GetEmulationKernel().GetSystem();
  u8* const packet = system.GetMemory().GetPointerForRange(cmd->data_address, cmd->length);
  if (cmd->length != 32 || packet == nullptr)
  {
    ERROR_LOG_FMT(IOS_USB, "Infinity Base command invalid");
    return IPC_EINVAL;
  }

  switch (packet[0])
  {
  case 0x00:
    m_response_list.push(std::move(cmd));
    break;

  case REPLY:
  case FIGURE_CHANGE:
  {
    // Figure placement notices go first, then replies to earlier commands.
    InfinityBase& base = system.GetInfinityBase();
    if (base.HasFigureBeenAddedRemoved())
    {
      ScheduleTransfer(std::move(cmd), base.PopAddedRemovedResponse(), REPLY_DELAY_US);
    }
    else if (!m_queries.empty())
    {
      ScheduleTransfer(std::move(cmd), m_queries.front(), REPLY_DELAY_US);
      m_queries.pop();
    }
    else
    {
      m_response_list.push(std::move(cmd));
    }
    break;
  }

  case COMMAND:
  {
    // The packet itself is echoed back, and the reply goes to the next read.
    const Packet reply = RunCommand(system.GetInfinityBase(), packet);
    Packet echo;
    std::copy_n(packet, echo.size(), echo.begin());
    ScheduleTransfer(std::move(cmd), echo, ECHO_DELAY_US);
    DeliverReply(reply);
    break;
  }

  default:
    break;
  }
  return 0;
}

auto InfinityUSB::RunCommand(InfinityBase& base, u8* packet) -> Packet
{
  const u8 command = packet[2];
  const u8 sequence = packet[3];
  Packet reply{};

  switch (command)
  {
  case 0x80:
    // Activate: a fixed description of the base.
    reply = {0xaa, 0x15, 0x00, 0x00, 0x0f, 0x01, 0x00, 0x03, 0x02, 0x09, 0x09, 0x43,
             0x20, 0x32, 0x62, 0x36, 0x36, 0x4b, 0x34, 0x99, 0x67, 0x31, 0x93, 0x8c};
    break;
  case 0x81:
    // Seed the random number generator with the scrambled number the game sends.
    base.DescrambleAndSeed(packet, sequence, reply);
    break;
  case 0x83:
    // The next random number, scrambled.
    base.GetNextAndScramble(sequence, reply);
    break;
  case 0x90:
  case 0x92:
  case 0x93:
  case 0x95:
  case 0x96:
    // Lights (not emulated) and a status query only need an acknowledgement.
  case 0xB5:
    base.GetBlankResponse(sequence, reply);
    break;
  case 0xA1:
    // Which figures are present.
    base.GetPresentFigures(sequence, reply);
    break;
  case 0xA2:
    // Read a block: figure (order placed), block, unknown.
    base.QueryBlock(packet[4], packet[5], reply, sequence);
    break;
  case 0xA3:
    // Write a block: figure, block, unknown, 16 bytes.
    base.WriteBlock(packet[4], packet[5], packet + 7, reply, sequence);
    break;
  case 0xB4:
    // The tag ID of a figure.
    base.GetFigureIdentifier(packet[4], sequence, reply);
    break;
  default:
    ERROR_LOG_FMT(IOS_USB, "Unhandled Infinity Base Command: {}", command);
    break;
  }
  return reply;
}

void InfinityUSB::DeliverReply(const Packet& reply)
{
  if (m_response_list.empty())
  {
    m_queries.push(reply);
    return;
  }
  ScheduleTransfer(std::move(m_response_list.front()), reply, REPLY_DELAY_US);
  m_response_list.pop();
}

int InfinityUSB::SubmitTransfer(std::unique_ptr<IsoMessage> cmd)
{
  DEBUG_LOG_FMT(IOS_USB,
                "[{:04x}:{:04x} {}] Isochronous: length={:04x} endpoint={:02x} num_packets={:02x}",
                m_vid, m_pid, m_active_interface, cmd->length, cmd->endpoint, cmd->num_packets);
  return 0;
}

void InfinityUSB::ScheduleTransfer(std::unique_ptr<TransferCommand> command,
                                   const std::array<u8, 32>& data, u64 expected_time_us)
{
  command->FillBuffer(data.data(), data.size());
  command->ScheduleTransferCompletion(32, static_cast<u32>(expected_time_us));
}

// ---- the base ----

bool InfinityBase::HasFigureBeenAddedRemoved() const
{
  return !m_figure_added_removed_response.empty();
}

std::array<u8, 32> InfinityBase::PopAddedRemovedResponse()
{
  const Packet notice = m_figure_added_removed_response.front();
  m_figure_added_removed_response.pop();
  return notice;
}

void InfinityBase::GetBlankResponse(u8 sequence, std::array<u8, 32>& reply_buf)
{
  ReplyHeader(reply_buf, 0x01, sequence);
  reply_buf[3] = Checksum(reply_buf, 3);
}

// Two bytes per present figure, in slot order: pad (10 hexagon, 20 player one, 30 player two)
// plus the order it was placed in, then 09.
void InfinityBase::GetPresentFigures(u8 sequence, std::array<u8, 32>& reply_buf)
{
  std::size_t end = 3;
  for (std::size_t i = 0; i < m_figures.size(); ++i)
  {
    const InfinityFigure& figure = m_figures[i];
    if (!figure.present)
      continue;
    const u8 pad = i == 0 ? 0x10 : i < 4 ? 0x20 : 0x30;
    reply_buf[end] = static_cast<u8>(pad + figure.order_added);
    reply_buf[end + 1] = 0x09;
    end += 2;
  }
  ReplyHeader(reply_buf, static_cast<u8>(end - 2), sequence);
  reply_buf[end] = Checksum(reply_buf, end);
}

void InfinityBase::GetFigureIdentifier(u8 fig_num, u8 sequence, std::array<u8, 32>& reply_buf)
{
  std::lock_guard lock(m_infinity_mutex);
  const InfinityFigure& figure = FigureInOrder(fig_num);

  ReplyHeader(reply_buf, 0x09, sequence);
  reply_buf[3] = 0x00;
  if (figure.present)
    std::copy_n(figure.data.begin(), 7, reply_buf.begin() + 4);
  reply_buf[11] = Checksum(reply_buf, 11);
}

void InfinityBase::QueryBlock(u8 fig_num, u8 block, std::array<u8, 32>& reply_buf, u8 sequence)
{
  std::lock_guard lock(m_infinity_mutex);
  const InfinityFigure& figure = FigureInOrder(fig_num);

  ReplyHeader(reply_buf, 0x12, sequence);
  reply_buf[3] = 0x00;
  const u8 file_block = FigureBlock(block);
  if (figure.present && file_block < INFINITY_NUM_BLOCKS)
  {
    std::copy_n(figure.data.begin() + INFINITY_BLOCK_SIZE * file_block, INFINITY_BLOCK_SIZE,
                reply_buf.begin() + 4);
  }
  reply_buf[20] = Checksum(reply_buf, 20);
}

void InfinityBase::WriteBlock(u8 fig_num, u8 block, const u8* to_write_buf,
                              std::array<u8, 32>& reply_buf, u8 sequence)
{
  std::lock_guard lock(m_infinity_mutex);
  InfinityFigure& figure = FigureInOrder(fig_num);

  ReplyHeader(reply_buf, 0x02, sequence);
  reply_buf[3] = 0x00;
  const u8 file_block = FigureBlock(block);
  if (figure.present && file_block < INFINITY_NUM_BLOCKS)
  {
    std::copy_n(to_write_buf, INFINITY_BLOCK_SIZE,
                figure.data.begin() + INFINITY_BLOCK_SIZE * file_block);
    figure.Save();
  }
  reply_buf[4] = Checksum(reply_buf, 4);
}

// AB 04 <pad> 09 <order> <00 placed / 01 removed> <checksum>
void InfinityBase::QueueFigureChange(FigureUIPosition position, u8 order_added, bool removed)
{
  Packet notice{FIGURE_CHANGE, 0x04, static_cast<u8>(PadOf(position)), 0x09, order_added,
                static_cast<u8>(removed ? 0x01 : 0x00)};
  notice[6] = Checksum(notice, 6);
  m_figure_added_removed_response.push(notice);
}

std::string InfinityBase::LoadFigure(const FigureBytes& buf, File::IOFile in_file,
                                     FigureUIPosition position)
{
  std::lock_guard lock(m_infinity_mutex);

  // The character number is in the encrypted block 1.
  const auto key = FigureKey(std::span{buf}.first(7));
  std::array<u8, 16> character{};
  Common::AES::CreateContextDecrypt(key.data())
      ->CryptIvZero(buf.data() + INFINITY_BLOCK_SIZE, character.data(), character.size());
  const u32 number = (u32{character[1]} << 16) | (u32{character[2]} << 8) | u32{character[3]};
  DEBUG_LOG_FMT(IOS_USB, "Toy Number: {}", number);

  InfinityFigure& figure = m_figures[static_cast<u8>(position)];
  figure.inf_file = std::move(in_file);
  figure.data = buf;
  figure.present = true;
  // A figure keeps the order number of its slot.
  if (figure.order_added == 255)
    figure.order_added = m_figure_order++;

  if (PadOf(position) == FigureBasePosition::Unknown)
  {
    ERROR_LOG_FMT(IOS_USB, "Invalid Position for Infinity Figure");
    return "Unknown Figure";
  }
  QueueFigureChange(position, figure.order_added, false);
  return FindFigure(number);
}

void InfinityBase::RemoveFigure(FigureUIPosition position)
{
  std::lock_guard lock(m_infinity_mutex);
  InfinityFigure& figure = m_figures[static_cast<u8>(position)];

  if (figure.inf_file.IsOpen())
  {
    figure.Save();
    figure.inf_file.Close();
  }

  if (!figure.present)
    return;
  if (PadOf(position) == FigureBasePosition::Unknown)
  {
    ERROR_LOG_FMT(IOS_USB, "Invalid Position for Infinity Figure");
    return;
  }
  figure.present = false;
  QueueFigureChange(position, figure.order_added, true);
}

// A new figure: a random tag ID, the character block and four blank data blocks (encrypted),
// and the NFC access bits in the sector trailers.
bool InfinityBase::CreateFigure(const std::string& file_path, u32 figure_num)
{
  File::IOFile file(file_path, "w+b");
  if (!file)
    return false;

  FigureBytes bytes{};
  constexpr std::array<u8, 3> FIRST_SECTOR_ACCESS{0x17, 0x87, 0x8E};
  constexpr std::array<u8, 3> OTHER_SECTOR_ACCESS{0x77, 0x87, 0x88};
  for (std::size_t sector = 0; sector < 5; ++sector)
  {
    const auto& access = sector == 0 ? FIRST_SECTOR_ACCESS : OTHER_SECTOR_ACCESS;
    std::ranges::copy(access, bytes.begin() + static_cast<std::ptrdiff_t>(sector * 0x40 + 0x36));
  }

  // Block 0: the tag ID, then fixed bytes.
  std::array<u8, 16> tag{};
  Common::Random::Generate(tag.data(), 7);
  tag[7] = 0x89;
  tag[8] = 0x44;
  tag[9] = 0x00;
  tag[10] = 0xC2;

  const std::array<u8, 16> character = CharacterBlock(figure_num);
  // Character numbers start at 0x0F4241, so the top byte is never zero.
  if (character[1] == 0x00)
    return false;

  const auto aes = Common::AES::CreateContextEncrypt(FigureKey(std::span{tag}.first(7)).data());
  std::array<u8, 16> encrypted_character{};
  std::array<u8, 16> encrypted_blank{};
  const std::array<u8, 16> blank{};
  aes->CryptIvZero(character.data(), encrypted_character.data(), character.size());
  aes->CryptIvZero(blank.data(), encrypted_blank.data(), blank.size());

  std::ranges::copy(tag, bytes.begin());
  std::ranges::copy(encrypted_character, bytes.begin() + 1 * INFINITY_BLOCK_SIZE);
  constexpr std::array<std::size_t, 4> BLANK_BLOCKS{4, 8, 12, 13};
  for (const std::size_t block : BLANK_BLOCKS)
  {
    std::ranges::copy(encrypted_blank,
                      bytes.begin() + static_cast<std::ptrdiff_t>(block * INFINITY_BLOCK_SIZE));
  }

  DEBUG_LOG_FMT(IOS_USB, "File Data: \n{}", HexDump(bytes.data(), bytes.size()));
  file.WriteBytes(bytes.data(), bytes.size());
  return true;
}

std::span<const std::pair<const char*, const u32>> InfinityBase::GetFigureList()
{
  return FIGURE_LIST;
}

std::string InfinityBase::FindFigure(u32 number) const
{
  for (const auto& [name, toy_number] : FIGURE_LIST)
  {
    if (toy_number == number)
      return name;
  }
  return "Unknown Figure";
}

InfinityFigure& InfinityBase::FigureInOrder(u8 order_added)
{
  for (InfinityFigure& figure : m_figures)
  {
    if (figure.order_added == order_added)
      return figure;
  }
  ASSERT_MSG(IOS_USB, false, "Unable to find Disney Figure requested by game");
  return m_figures[0];
}

// 81 <8 bytes, big endian>: the scrambled seed.
void InfinityBase::DescrambleAndSeed(u8* buf, u8 sequence, std::array<u8, 32>& reply_buf)
{
  u64 scrambled = 0;
  for (std::size_t i = 0; i < 8; ++i)
    scrambled = (scrambled << 8) | buf[4 + i];
  SeedGenerator(Descramble(scrambled));
  GetBlankResponse(sequence, reply_buf);
}

void InfinityBase::GetNextAndScramble(u8 sequence, std::array<u8, 32>& reply_buf)
{
  const u64 scrambled = Scramble(NextRandom());
  reply_buf = {};
  ReplyHeader(reply_buf, 0x09, sequence);
  for (std::size_t i = 0; i < 8; ++i)
    reply_buf[3 + i] = static_cast<u8>(scrambled >> (56 - 8 * i));
  reply_buf[11] = Checksum(reply_buf, 11);
}

// Bob Jenkins' small fast generator (JSF32), seeded the usual way.
void InfinityBase::SeedGenerator(u32 seed)
{
  m_rng_a = 0xF1EA5EED;
  m_rng_b = seed;
  m_rng_c = seed;
  m_rng_d = seed;
  for (int i = 0; i < 23; ++i)
    NextRandom();
}

u32 InfinityBase::NextRandom()
{
  const u32 e = m_rng_a - std::rotl(m_rng_b, 27);
  m_rng_a = m_rng_b ^ std::rotl(m_rng_c, 17);
  m_rng_b = m_rng_c + m_rng_d;
  m_rng_c = m_rng_d + e;
  m_rng_d = e + m_rng_a;
  return m_rng_d;
}

void InfinityFigure::Save()
{
  if (!inf_file)
    return;
  inf_file.Seek(0, File::SeekOrigin::Begin);
  inf_file.WriteBytes(data.data(), data.size());
}
}  // namespace IOS::HLE::USB
