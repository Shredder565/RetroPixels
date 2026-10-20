// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI.h"

#include <array>
#include <memory>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI_Channel.h"
#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Sram.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/System.h"

#include "DiscIO/Enums.h"

namespace ExpansionInterface
{
namespace
{
// Where each user-visible slot sits on the bus: {channel, device index}.
struct SlotWiring
{
  u8 channel;
  u8 device;
};
constexpr std::array<SlotWiring, 4> SLOT_WIRING = {{
    {0, 0},  // Slot A: channel 0, chip select 0
    {1, 0},  // Slot B: channel 1, chip select 0
    {0, 2},  // Serial port 1: channel 0, chip select 2
    {2, 0},  // Serial port 2: channel 2, chip select 0
}};

// Every channel owns five 32-bit registers, packed back to back.
constexpr u32 CHANNEL_REGISTER_STRIDE = 5 * sizeof(u32);

// Device swap events carry the target in their userdata.
constexpr u64 PackSwap(u8 channel, EXIDeviceType type, u8 device_num)
{
  return (u64{channel} << 32) | (u64{static_cast<u8>(type)} << 16) | u64{device_num};
}

const SlotWiring* LookUpWiring(Slot slot)
{
  const auto index = static_cast<std::size_t>(slot);
  if (index >= SLOT_WIRING.size())
  {
    PanicAlertFmt("Unhandled slot {}", slot);
    return nullptr;
  }
  return &SLOT_WIRING[index];
}
}  // namespace

u8 SlotToEXIChannel(Slot slot)
{
  const SlotWiring* wiring = LookUpWiring(slot);
  return wiring ? wiring->channel : 0;
}

u8 SlotToEXIDevice(Slot slot)
{
  const SlotWiring* wiring = LookUpWiring(slot);
  return wiring ? wiring->device : 0;
}

ExpansionInterfaceManager::ExpansionInterfaceManager(Core::System& system) : m_system(system)
{
}

ExpansionInterfaceManager::~ExpansionInterfaceManager() = default;

void ExpansionInterfaceManager::InsertMemcardSlotDevice(Slot slot)
{
  const EXIDeviceType configured = Config::Get(Config::GetInfoForEXIDevice(slot));
  EXIDeviceType chosen = configured;

  // A movie that saved its config decides whether a card is present.
  auto& movie = m_system.GetMovie();
  if (movie.IsPlayingInput() && movie.IsConfigSaved())
  {
    if (!movie.IsUsingMemcard(slot))
    {
      chosen = EXIDeviceType::None;
    }
    else if (configured != EXIDeviceType::MemoryCard &&
             configured != EXIDeviceType::MemoryCardFolder)
    {
      PanicAlertFmtT(
          "The movie indicates that a memory card should be inserted into {0:n}, but one is not "
          "currently inserted (instead, {1} is inserted).  For the movie to sync properly, "
          "please change the selected device to Memory Card or GCI Folder.",
          slot, Common::GetStringT(fmt::format("{:n}", configured).c_str()));
    }
  }

  m_channels[SlotToEXIChannel(slot)]->AddDevice(chosen, SlotToEXIDevice(slot));
}

void ExpansionInterfaceManager::Init(const Sram* override_sram)
{
  Sram& sram = m_system.GetSRAM();
  m_sram_is_borrowed = override_sram != nullptr;
  if (m_sram_is_borrowed)
    sram = *override_sram;
  else
    InitSRAM(&sram, SConfig::GetInstance().m_strSRAM);

  auto& core_timing = m_system.GetCoreTiming();
  // The memory card events have to exist before anything can be loaded from a savestate.
  CEXIMemoryCard::Init(core_timing);

  // Card size: 2043 blocks unless the user picked one of the 59 << n sizes.
  u16 card_mbits = Memcard::MBIT_SIZE_MEMORY_CARD_2043;
  const int size_choice = Config::Get(Config::MAIN_MEMORY_CARD_SIZE);
  if (size_choice >= 0 && size_choice <= 4)
    card_mbits = static_cast<u16>(Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_choice);

  const bool japanese_encoding =
      Config::ToGameCubeRegion(SConfig::GetInstance().m_region) == DiscIO::Region::NTSC_J;
  const u64 seconds_since_gc_epoch =
      Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;

  for (u32 channel = 0; channel < MAX_EXI_CHANNELS; ++channel)
  {
    // Each channel gets its own format time so that cards created on different channels differ.
    Memcard::HeaderData header;
    Memcard::InitializeHeaderData(&header, sram.settings_ex.flash_id[Memcard::SLOT_A], card_mbits,
                                  japanese_encoding, sram.settings.rtc_bias,
                                  static_cast<u32>(sram.settings.language),
                                  seconds_since_gc_epoch + channel);
    m_channels[channel] = std::make_unique<CEXIChannel>(m_system, channel, header);
  }

  for (Slot slot : MEMCARD_SLOTS)
    InsertMemcardSlotDevice(slot);

  // The boot ROM / RTC / SRAM chip is always on channel 0, chip select 1.
  m_channels[0]->AddDevice(EXIDeviceType::MaskROM, 1);

  // Triforce boards always have their baseboard on serial port 1.
  const EXIDeviceType sp1 = m_system.IsTriforce() ? EXIDeviceType::Baseboard :
                                                    Config::Get(Config::MAIN_SERIAL_PORT_1);
  m_channels[SlotToEXIChannel(Slot::SP1)]->AddDevice(sp1, SlotToEXIDevice(Slot::SP1));
  m_channels[SlotToEXIChannel(Slot::SP2)]->AddDevice(Config::Get(Config::MAIN_SERIAL_PORT_2),
                                                     SlotToEXIDevice(Slot::SP2));

  m_swap_event = core_timing.RegisterEvent("ChangeEXIDevice", OnDeviceSwapEvent);
  m_irq_refresh_event = core_timing.RegisterEvent("EXIUpdateInterrupts", OnInterruptRefreshEvent);
}

void ExpansionInterfaceManager::Shutdown()
{
  for (std::unique_ptr<CEXIChannel>& channel : m_channels)
    channel.reset();

  CEXIMemoryCard::Shutdown();

  if (m_sram_is_borrowed)
    return;

  File::IOFile sram_file(SConfig::GetInstance().m_strSRAM, "wb");
  sram_file.WriteArray(&m_system.GetSRAM(), 1);
}

void ExpansionInterfaceManager::DoState(PointerWrap& p)
{
  for (std::unique_ptr<CEXIChannel>& channel : m_channels)
    channel->DoState(p);
}

void ExpansionInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  for (u32 index = 0; index < MAX_EXI_CHANNELS; ++index)
  {
    DEBUG_ASSERT(m_channels[index] != nullptr);
    // The channel bases are 0x14 apart, so they are not page aligned; channels add their
    // register offsets instead of OR-ing them in.
    m_channels[index]->RegisterMMIO(mmio, base + index * CHANNEL_REGISTER_STRIDE);
  }
}

void ExpansionInterfaceManager::OnDeviceSwapEvent(Core::System& system, u64 userdata, s64)
{
  const u8 channel = static_cast<u8>(userdata >> 32);
  const auto type = static_cast<EXIDeviceType>(static_cast<u8>(userdata >> 16));
  const u8 device_num = static_cast<u8>(userdata);
  system.GetExpansionInterface().m_channels.at(channel)->AddDevice(type, device_num);
}

void ExpansionInterfaceManager::ChangeDevice(Slot slot, EXIDeviceType device_type,
                                             CoreTiming::FromThread from_thread)
{
  ChangeDevice(SlotToEXIChannel(slot), SlotToEXIDevice(slot), device_type, from_thread);
}

void ExpansionInterfaceManager::ChangeDevice(u8 channel, u8 device_num, EXIDeviceType device_type,
                                             CoreTiming::FromThread from_thread)
{
  // Unplug now and plug the new device in one emulated second later, so that software notices
  // the slot going empty before the new device appears.
  auto& core_timing = m_system.GetCoreTiming();
  core_timing.ScheduleEvent(0, m_swap_event, PackSwap(channel, EXIDeviceType::None, device_num),
                            from_thread);
  core_timing.ScheduleEvent(m_system.GetSystemTimers().GetTicksPerSecond(), m_swap_event,
                            PackSwap(channel, device_type, device_num), from_thread);
}

CEXIChannel* ExpansionInterfaceManager::GetChannel(u32 index)
{
  return m_channels.at(index).get();
}

IEXIDevice* ExpansionInterfaceManager::GetDevice(Slot slot)
{
  const u8 chip_select = static_cast<u8>(1 << SlotToEXIDevice(slot));
  return m_channels.at(SlotToEXIChannel(slot))->GetDevice(chip_select);
}

void ExpansionInterfaceManager::UpdateInterrupts()
{
  // Serial port 1 sits on channel 0 but raises its EXI interrupt on channel 2.
  m_channels[2]->SetEXIINT(m_channels[0]->GetDevice(4)->IsInterruptSet());

  // Every channel is polled (no short-circuit): polling latches the EXI interrupt bits.
  bool any_pending = false;
  for (std::unique_ptr<CEXIChannel>& channel : m_channels)
  {
    if (channel->IsCausingInterrupt())
      any_pending = true;
  }

  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_EXI, any_pending);
}

void ExpansionInterfaceManager::OnInterruptRefreshEvent(Core::System& system, u64, s64)
{
  system.GetExpansionInterface().UpdateInterrupts();
}

void ExpansionInterfaceManager::ScheduleUpdateInterrupts(CoreTiming::FromThread from,
                                                         int cycles_late)
{
  m_system.GetCoreTiming().ScheduleEvent(cycles_late, m_irq_refresh_event, 0, from);
}

}  // namespace ExpansionInterface
