// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <initializer_list>
#include <memory>

#include "Common/CommonTypes.h"
#include "Common/EnumFormatter.h"
#include "Core/CoreTiming.h"

class PointerWrap;
struct Sram;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
enum class FromThread;
}  // namespace CoreTiming
namespace MMIO
{
class Mapping;
}

// The external interface: three serial channels in the Flipper/Hollywood, each with up to three
// chip selects. Channel 0 carries memory card slot A, the boot ROM/RTC/SRAM chip and serial
// port 1; channel 1 carries memory card slot B; channel 2 carries serial port 2.
namespace ExpansionInterface
{
class CEXIChannel;
class IEXIDevice;
enum class EXIDeviceType : int;

enum
{
  MAX_EXI_CHANNELS = 3
};

// The four places a user can plug something in.
enum class Slot : int
{
  A,
  B,
  SP1,
  SP2,
};
// A plain initializer_list (not auto) keeps older GCC versions from warning about it.
constexpr std::initializer_list<Slot> SLOTS = {Slot::A, Slot::B, Slot::SP1, Slot::SP2};
constexpr auto MAX_SLOT = Slot::SP2;
constexpr std::initializer_list<Slot> MEMCARD_SLOTS = {Slot::A, Slot::B};
constexpr auto MAX_MEMCARD_SLOT = Slot::B;
constexpr bool IsMemcardSlot(Slot slot)
{
  return slot == Slot::A || slot == Slot::B;
}

u8 SlotToEXIChannel(Slot slot);
u8 SlotToEXIDevice(Slot slot);

class ExpansionInterfaceManager
{
public:
  explicit ExpansionInterfaceManager(Core::System& system);
  ExpansionInterfaceManager(const ExpansionInterfaceManager&) = delete;
  ExpansionInterfaceManager(ExpansionInterfaceManager&&) = delete;
  ExpansionInterfaceManager& operator=(const ExpansionInterfaceManager&) = delete;
  ExpansionInterfaceManager& operator=(ExpansionInterfaceManager&&) = delete;
  ~ExpansionInterfaceManager();

  void Init(const Sram* override_sram);
  void Shutdown();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  void UpdateInterrupts();
  void ScheduleUpdateInterrupts(CoreTiming::FromThread from, int cycles_late);

  void ChangeDevice(Slot slot, EXIDeviceType device_type,
                    CoreTiming::FromThread from_thread = CoreTiming::FromThread::NON_CPU);
  void ChangeDevice(u8 channel, u8 device_num, EXIDeviceType device_type,
                    CoreTiming::FromThread from_thread = CoreTiming::FromThread::NON_CPU);

  CEXIChannel* GetChannel(u32 index);
  IEXIDevice* GetDevice(Slot slot);

private:
  void InsertMemcardSlotDevice(Slot slot);

  static void OnDeviceSwapEvent(Core::System& system, u64 userdata, s64 cycles_late);
  static void OnInterruptRefreshEvent(Core::System& system, u64 userdata, s64 cycles_late);

  CoreTiming::EventType* m_swap_event = nullptr;
  CoreTiming::EventType* m_irq_refresh_event = nullptr;

  std::array<std::unique_ptr<CEXIChannel>, MAX_EXI_CHANNELS> m_channels;

  // True when the SRAM came from the caller (NetPlay, movies) instead of the user's SRAM file;
  // it is then not written back at shutdown.
  bool m_sram_is_borrowed = false;

  Core::System& m_system;
};
}  // namespace ExpansionInterface

template <>
struct fmt::formatter<ExpansionInterface::Slot> : EnumFormatter<ExpansionInterface::MAX_SLOT>
{
  constexpr formatter() : EnumFormatter({"Slot A", "Slot B", "Serial Port 1", "Serial Port 2"}) {}
};
