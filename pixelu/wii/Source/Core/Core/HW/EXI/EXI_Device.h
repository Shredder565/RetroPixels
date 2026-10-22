// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace Memcard
{
struct HeaderData;
}

namespace ExpansionInterface
{
// Stored in config files and savestates, so the values are fixed.
enum class EXIDeviceType : int
{
  Dummy,
  MemoryCard,
  MaskROM,
  AD16,
  Microphone,
  Ethernet,
  Baseboard,
  Gecko,
  // A memory card backed by a folder of GCI files. Behaves as MemoryCard on the bus.
  MemoryCardFolder,
  AGP,
  EthernetXLink,
  EthernetTapServer,
  EthernetBuiltIn,
  ModemTapServer,
  EthernetIPC,
  None = 0xFF
};

// Base class for everything that can sit on an EXI chip select. The default implementation is
// an empty slot: nothing present, no interrupts, and transfers that move bytes through
// TransferByte one at a time.
class IEXIDevice
{
public:
  explicit IEXIDevice(Core::System& system);
  virtual ~IEXIDevice() = default;

  // Immediate transfers of 1 to 4 bytes; the data is left-aligned in the 32-bit word.
  virtual void ImmWrite(u32 data, u32 size);
  virtual u32 ImmRead(u32 size);
  virtual void ImmReadWrite(u32& data, u32 size);

  // DMA transfers to or from emulated memory.
  virtual void DMAWrite(u32 address, u32 size);
  virtual void DMARead(u32 address, u32 size);

  // True if the device signals transfer completion itself (e.g. after a delay).
  virtual bool UseDelayedTransferCompletion() const;
  virtual bool IsPresent() const;
  // Called with the channel's new chip select value when this device's line toggles.
  virtual void SetCS(int cs);
  virtual void DoState(PointerWrap& p);

  virtual bool IsInterruptSet();

  // Which kind of device this is; savestates record it to rebuild the right device on load.
  EXIDeviceType m_device_type = EXIDeviceType::None;

protected:
  Core::System& m_system;

private:
  // Exchanges one byte with the device: byte holds the byte sent and receives the reply.
  virtual void TransferByte(u8& byte);
};

std::unique_ptr<IEXIDevice> EXIDevice_Create(Core::System& system, EXIDeviceType device_type,
                                             int channel_num,
                                             const Memcard::HeaderData& memcard_header_data);
}  // namespace ExpansionInterface

template <>
struct fmt::formatter<ExpansionInterface::EXIDeviceType>
    : EnumFormatter<ExpansionInterface::EXIDeviceType::EthernetIPC>
{
  static constexpr array_type names = {
      _trans("Dummy"),
      _trans("Memory Card"),
      _trans("Mask ROM"),
      // i18n: A mysterious debugging/diagnostics peripheral for the GameCube.
      _trans("AD16"),
      _trans("Microphone"),
      _trans("Broadband Adapter (TAP)"),
      _trans("Triforce Baseboard"),
      _trans("USB Gecko"),
      _trans("GCI Folder"),
      _trans("Advance Game Port"),
      _trans("Broadband Adapter (XLink Kai)"),
      _trans("Broadband Adapter (tapserver)"),
      _trans("Broadband Adapter (HLE)"),
      _trans("Modem Adapter (tapserver)"),
      _trans("Broadband Adapter (IPC)"),
  };

  constexpr formatter() : EnumFormatter(names) {}

  // None (0xFF) is far past the table, so it is formatted here: "u" is its plain name, "s" a
  // C++ literal for generated code, "n" the name shown in the UI.
  template <typename FormatContext>
  auto format(const ExpansionInterface::EXIDeviceType& e, FormatContext& ctx) const
  {
    if (e == ExpansionInterface::EXIDeviceType::None)
    {
      if (format_type == 's')
        return fmt::format_to(ctx.out(), "0xffu /* None */");
      if (format_type == 'n')
        return fmt::format_to(ctx.out(), _trans("<Nothing>"));
      return fmt::format_to(ctx.out(), "None");
    }
    return EnumFormatter::format(e, ctx);
  }
};
