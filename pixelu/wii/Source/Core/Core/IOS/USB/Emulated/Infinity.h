// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/IOS/USB/Common.h"

namespace IOS::HLE::USB
{
// A figure is an NFC tag of 20 blocks of 16 bytes.
constexpr u8 INFINITY_NUM_BLOCKS = 0x14;
constexpr u8 INFINITY_BLOCK_SIZE = 0x10;

struct InfinityFigure final
{
  // Writes the data back to the figure file, if one is open.
  void Save();

  File::IOFile inf_file;
  std::array<u8, INFINITY_NUM_BLOCKS * INFINITY_BLOCK_SIZE> data{};
  bool present = false;
  // The order the figure was first placed in (255: never placed).
  u8 order_added = 255;
};

class InfinityBase;

// The USB side of a Disney Infinity base (0e6f:0129). The game sends 32-byte "FF" command
// packets on the interrupt OUT endpoint and reads 32-byte "AA" replies (and "AB" figure
// placement notices) from the IN endpoint.
class InfinityUSB final : public Device
{
public:
  InfinityUSB();
  ~InfinityUSB() override;
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

private:
  using Packet = std::array<u8, 32>;

  void ScheduleTransfer(std::unique_ptr<TransferCommand> command, const std::array<u8, 32>& data,
                        u64 expected_time_us);
  // Runs one FF command packet and returns its reply.
  Packet RunCommand(InfinityBase& base, u8* packet);
  // Hands a reply to a waiting read, or keeps it for the next one.
  void DeliverReply(const Packet& reply);

  u16 m_vid = 0;
  u16 m_pid = 0;
  u8 m_active_interface = 0;
  bool m_device_attached = false;
  DeviceDescriptor m_device_descriptor;
  std::vector<ConfigDescriptor> m_config_descriptor;
  std::vector<InterfaceDescriptor> m_interface_descriptor;
  std::vector<EndpointDescriptor> m_endpoint_descriptor;
  // Replies nobody has read yet, and reads waiting for a reply.
  std::queue<Packet> m_queries;
  std::queue<std::unique_ptr<IntrMessage>> m_response_list;
};

// Slots in the UI: three on the hexagon, then each player's figure and two ability discs.
enum class FigureUIPosition : u8
{
  HexagonDiscOne = 0,
  HexagonDiscTwo = 1,
  HexagonDiscThree = 2,
  PlayerOne = 3,
  P1AbilityOne = 4,
  P1AbilityTwo = 5,
  PlayerTwo = 6,
  P2AbilityOne = 7,
  P2AbilityTwo = 8
};

// The three pads of the base as the game numbers them.
enum class FigureBasePosition : u8
{
  Unknown = 0,
  HexagonSlot = 1,
  PlayerOneSlot = 2,
  PlayerTwoSlot = 3
};

class InfinityBase final
{
public:
  bool HasFigureBeenAddedRemoved() const;
  std::array<u8, 32> PopAddedRemovedResponse();
  void GetBlankResponse(u8 sequence, std::array<u8, 32>& reply_buf);
  void GetPresentFigures(u8 sequence, std::array<u8, 32>& reply_buf);
  void GetFigureIdentifier(u8 fig_num, u8 sequence, std::array<u8, 32>& reply_buf);
  void QueryBlock(u8 fig_num, u8 block, std::array<u8, 32>& reply_buf, u8 sequence);
  void WriteBlock(u8 fig_num, u8 block, const u8* to_write_buf, std::array<u8, 32>& reply_buf,
                  u8 sequence);
  void DescrambleAndSeed(u8* buf, u8 sequence, std::array<u8, 32>& reply_buf);
  void GetNextAndScramble(u8 sequence, std::array<u8, 32>& reply_buf);
  void RemoveFigure(FigureUIPosition position);
  // Returns Infinity Figure name based on data from in_file param
  std::string LoadFigure(const std::array<u8, INFINITY_NUM_BLOCKS * INFINITY_BLOCK_SIZE>& buf,
                         File::IOFile in_file, FigureUIPosition position);
  bool CreateFigure(const std::string& file_path, u32 character);
  static std::span<const std::pair<const char*, const u32>> GetFigureList();
  std::string FindFigure(u32 character) const;

protected:
  std::mutex m_infinity_mutex;
  std::array<InfinityFigure, 9> m_figures;

private:
  using Packet = std::array<u8, 32>;

  InfinityFigure& FigureInOrder(u8 order_added);
  void QueueFigureChange(FigureUIPosition position, u8 order_added, bool removed);

  // The base's random number generator, which the game seeds and then checks.
  void SeedGenerator(u32 seed);
  u32 NextRandom();

  // State of the random number generator.
  u32 m_rng_a = 0;
  u32 m_rng_b = 0;
  u32 m_rng_c = 0;
  u32 m_rng_d = 0;

  u8 m_figure_order = 0;
  std::queue<Packet> m_figure_added_removed_response;
};
}  // namespace IOS::HLE::USB
