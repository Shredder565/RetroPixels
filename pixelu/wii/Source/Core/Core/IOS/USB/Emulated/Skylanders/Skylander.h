// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/USB/Common.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"

// The portal reports the state of every figure in a 32-bit word, two bits each.
constexpr u8 MAX_SKYLANDERS = 16;
constexpr u8 NUM_SKYLANDER_GAMES = 5;
constexpr u8 NUM_SKYLANDER_TYPES = 10;
constexpr u8 NUM_SKYLANDER_ELEMENTS = 11;

namespace IOS::HLE::USB
{
enum class Game
{
  SpyrosAdv,
  Giants,
  SwapForce,
  TrapTeam,
  Superchargers
};
enum class Type : u8
{
  Skylander = 1,
  Giant,
  Swapper,
  TrapMaster,
  Mini,
  Item,
  Trophy,
  Vehicle,
  Trap,
  Unknown
};
// The kinds whose saved data is understood: Skylander (all characters), Trophy, or Unknown.
Type NormalizeSkylanderType(Type type);
enum class Element
{
  Magic = 1,
  Fire,
  Air,
  Life,
  Undead,
  Earth,
  Water,
  Tech,
  Dark,
  Light,
  Other
};

struct SkyData
{
  const char* name = "";
  Game game = Game::SpyrosAdv;
  Element element = Element::Other;
  Type type = Type::Unknown;
};

// Known figures by (figure ID, variant ID).
extern const std::map<const std::pair<const u16, const u16>, SkyData> list_skylanders;

class SkylanderPortal;

// The USB side of a Skylanders portal of power (1430:0150): HID reports over control transfers
// for commands and an interrupt endpoint for replies, status and speaker audio.
class SkylanderUSB final : public Device
{
public:
  SkylanderUSB();
  ~SkylanderUSB() override;
  DeviceDescriptor GetDeviceDescriptor() const override;
  std::vector<ConfigDescriptor> GetConfigurations() const override;
  std::vector<InterfaceDescriptor> GetInterfaces(u8 config) const override;
  std::vector<EndpointDescriptor> GetEndpoints(u8 config, u8 iface, u8 alt) const override;
  bool Attach() override;
  bool AttachAndChangeInterface(u8 iface) override;
  int CancelTransfer(u8 endpoint) override;
  int ChangeInterface(u8 iface) override;
  int GetNumberOfAltSettings(u8 iface) override;
  int SetAltSetting(u8 alt_setting) override;
  int SubmitTransfer(std::unique_ptr<CtrlMessage> message) override;
  int SubmitTransfer(std::unique_ptr<BulkMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IntrMessage> message) override;
  int SubmitTransfer(std::unique_ptr<IsoMessage> message) override;
  void ScheduleTransfer(std::unique_ptr<TransferCommand> command, const std::array<u8, 64>& data,
                        s32 expected_count, u64 expected_time_us);

private:
  using Report = std::array<u8, 64>;

  // How a portal command is answered on its control transfer: the bytes echoed back and the
  // transfer length (0: the command is not accepted). Replies that come back through the
  // interrupt endpoint are queued in m_queries.
  struct CommandReply
  {
    Report control{};
    s32 length = 0;
  };
  CommandReply HandlePortalCommand(SkylanderPortal& portal, std::span<const u8> command);

  u16 m_vid = 0;
  u16 m_pid = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  DeviceDescriptor m_device_descriptor;
  std::vector<ConfigDescriptor> m_config_descriptor;
  std::vector<InterfaceDescriptor> m_interface_descriptor;
  std::vector<EndpointDescriptor> m_endpoint_descriptor;
  // Replies waiting for an interrupt transfer, oldest first.
  std::queue<Report> m_queries;
};

// One portal slot.
struct Skylander final
{
  std::unique_ptr<SkylanderFigure> figure;
  u8 status = 0;
  // States still to be reported, one per status report.
  std::queue<u8> queued_status;
  u32 last_id = 0;

  enum : u8
  {
    REMOVED = 0,
    READY = 1,
    REMOVING = 2,
    ADDED = 3
  };
};

struct SkylanderLEDColor final
{
  u8 red = 0;
  u8 green = 0;
  u8 blue = 0;
};

// The figures on the portal, shared by the emulated USB device and the UI.
class SkylanderPortal final
{
public:
  void Activate();
  void Deactivate();
  bool IsActivated();
  void UpdateStatus();
  void SetLEDs(u8 side, u8 r, u8 g, u8 b);

  std::array<u8, 64> GetStatus();
  void QueryBlock(u8 sky_num, u8 block, u8* reply_buf);
  void WriteBlock(u8 sky_num, u8 block, const u8* to_write_buf, u8* reply_buf);

  bool RemoveSkylander(u8 sky_num);
  // Returns the slot used, or 0xFF when the portal is full.
  u8 LoadSkylander(std::unique_ptr<SkylanderFigure> figure);
  Skylander* GetSkylander(u8 slot);
  std::pair<u16, u16> CalculateIDs(const std::array<u8, 0x40 * 0x10>& file_data);

private:
  std::mutex m_mutex;

  bool m_activated = true;
  bool m_status_updated = false;
  u8 m_interrupt_counter = 0;
  SkylanderLEDColor m_color_right = {};
  SkylanderLEDColor m_color_left = {};
  SkylanderLEDColor m_color_trap = {};

  std::array<Skylander, MAX_SKYLANDERS> m_slots;
};

}  // namespace IOS::HLE::USB
