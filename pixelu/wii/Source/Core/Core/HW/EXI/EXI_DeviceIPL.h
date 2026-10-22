// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string>

#include "Common/BitUtils.h"
#include "Core/HW/EXI/EXI_Device.h"

class PointerWrap;

namespace ExpansionInterface
{
// The "Mask ROM" chip on channel 0, chip select 1 (Macronix on the GameCube). It holds the boot
// ROM with the system fonts, the battery-backed SRAM and real time clock, a debug UART, and on
// the Wii a byte of RTC flags. Every access starts with a 32-bit command word: bit 31 selects
// write, bits 6-30 the address; the data bytes follow, and the address auto-increments within
// the ROM and SRAM.
class CEXIIPL : public IEXIDevice
{
public:
  explicit CEXIIPL(Core::System& system);
  ~CEXIIPL() override;

  void SetCS(int cs) override;
  bool IsPresent() const override;
  void DoState(PointerWrap& p) override;

  static constexpr u32 UNIX_EPOCH = 0;         // 1970-01-01 00:00:00
  static constexpr u32 GC_EPOCH = 0x386D4380;  // 2000-01-01 00:00:00

  static u32 GetEmulatedTime(Core::System& system, u32 epoch);
  static u64 NetPlay_GetEmulatedTime();

  // Removes the boot ROM's scrambling (it is its own inverse).
  static void Descrambler(u8* data, u32 size);

  static bool HasIPLDump();

private:
  void TransferByte(u8& data) override;

  void AccessROM(u8& data);
  void AccessSRAM(u8& data);
  void AccessUART(u8& data);

  bool CommandIsWrite() const { return (m_command >> 31) != 0; }
  u32 CommandAddress() const { return (m_command >> 6) & 0x1ffffff; }

  bool LoadFileToIPL(const std::string& filename, u32 offset);
  void LoadFontFile(const std::string& filename, u32 offset);
  static std::string FindIPLDump(const std::string& path_prefix);

  std::unique_ptr<u8[]> m_rom;

  // Saved in this order (after the SRAM and the RTC flags).
  u32 m_command = 0;
  u32 m_command_bytes = 0;
  // Offset from the command's address. One cursor serves whichever region the command
  // addresses; software re-selects the chip between commands.
  u32 m_cursor = 0;
  // Characters written to the UART that have not formed a full line yet.
  std::string m_uart_line;
  bool m_fonts_loaded = false;
};

// The Wii's RTC flags byte, read and cleared by __OSGetRTCFlags/__OSClearRTCFlags. Reads return
// the byte repeated in all four lanes, hence the repeated patterns.
enum class RTCFlag : u32
{
  EjectButton = 0x01010101,
  DiscChanged = 0x02020202,
};

extern Common::Flags<RTCFlag> g_rtc_flags;
}  // namespace ExpansionInterface
