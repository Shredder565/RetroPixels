// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceMemoryCard.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Channel.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardDirectory.h"
#include "Core/HW/GCMemcard/GCMemcardRaw.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/Sram.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"

namespace ExpansionInterface
{
namespace
{
// The card's command bytes.
enum Opcode : int
{
  OP_GET_EXI_ID = 0x00,
  OP_READ_ARRAY = 0x52,
  OP_ARRAY_TO_BUFFER = 0x53,
  OP_SET_INTERRUPT = 0x81,
  OP_WRITE_BUFFER = 0x82,
  OP_READ_STATUS = 0x83,
  OP_READ_ID = 0x85,
  OP_READ_ERROR_BUFFER = 0x86,
  OP_WAKE_UP = 0x87,
  OP_SLEEP = 0x88,
  OP_CLEAR_STATUS = 0x89,
  OP_SECTOR_ERASE = 0xF1,
  OP_PAGE_PROGRAM = 0xF2,
  OP_EXTRA_BYTE_PROGRAM = 0xF3,
  OP_CHIP_ERASE = 0xF4,
};

constexpr bool IsKnownOpcode(int opcode)
{
  switch (opcode)
  {
  case OP_GET_EXI_ID:
  case OP_READ_ARRAY:
  case OP_ARRAY_TO_BUFFER:
  case OP_SET_INTERRUPT:
  case OP_WRITE_BUFFER:
  case OP_READ_STATUS:
  case OP_READ_ID:
  case OP_READ_ERROR_BUFFER:
  case OP_WAKE_UP:
  case OP_SLEEP:
  case OP_CLEAR_STATUS:
  case OP_SECTOR_ERASE:
  case OP_PAGE_PROGRAM:
  case OP_EXTRA_BYTE_PROGRAM:
  case OP_CHIP_ERASE:
    return true;
  default:
    return false;
  }
}

// Status register bits.
constexpr int STATUS_BUSY = 0x80;
constexpr int STATUS_UNLOCKED = 0x40;
constexpr int STATUS_ERASE_ERROR = 0x10;
constexpr int STATUS_PROGRAM_ERROR = 0x08;
constexpr int STATUS_READY = 0x01;

// Bytes per megabit of card capacity.
constexpr u32 BYTES_PER_MBIT = 1024 * 8 * 16;

// Sustained transfer speeds, in bytes per second.
constexpr u32 READ_BYTES_PER_SECOND = 512 * 1024;
constexpr u32 WRITE_BYTES_PER_SECOND = 98432;  // 96.125 KiB/s

// Erases and page programs complete this many cycles after the chip is deselected.
constexpr u64 FLASH_OPERATION_CYCLES = 5000;

// The read ID answer of a Nintendo-made card.
constexpr u16 NINTENDO_CHIP_ID = 0xc221;

// Reads and programs stay inside one 512-byte page: only the low 9 address bits advance.
constexpr u32 NextInPage(u32 address)
{
  return (address & ~u32{0x1FF}) | ((address + 1) & 0x1FF);
}

// Address phase shared by the array read and page program commands: one bit per sector-row
// byte, then page and byte offsets.
constexpr u32 AddressPhase(u32 address, u32 position, u8 value)
{
  switch (position)
  {
  case 1:
    return u32{value} << 17;
  case 2:
    return address | (u32{value} << 9);
  case 3:
    return address | (u32{value & 3u} << 7);
  case 4:
    return address | (value & 0x7Fu);
  default:
    return address;
  }
}

struct SlotEvents
{
  CoreTiming::EventType* command_done = nullptr;
  CoreTiming::EventType* transfer_done = nullptr;
};
std::array<SlotEvents, 2> s_slot_events;

char SlotLetter(Slot slot)
{
  return slot == Slot::A ? 'A' : 'B';
}

SlotEvents& EventsFor(Slot slot)
{
  return s_slot_events[static_cast<std::size_t>(slot)];
}
}  // namespace

CEXIMemoryCard* CEXIMemoryCard::FindCard(Core::System& system, u64 userdata)
{
  // The event names the slot; the card may have been swapped out in the meantime.
  IEXIDevice* device = system.GetExpansionInterface().GetDevice(static_cast<Slot>(userdata));
  if (device == nullptr)
    return nullptr;
  if (device->m_device_type != EXIDeviceType::MemoryCard &&
      device->m_device_type != EXIDeviceType::MemoryCardFolder)
  {
    return nullptr;
  }
  return static_cast<CEXIMemoryCard*>(device);
}

void CEXIMemoryCard::OnCommandDone(Core::System& system, u64 userdata, s64)
{
  if (CEXIMemoryCard* card = FindCard(system, userdata))
    card->FinishCommand();
}

void CEXIMemoryCard::OnTransferDone(Core::System& system, u64 userdata, s64)
{
  CEXIMemoryCard* card = FindCard(system, userdata);
  if (card == nullptr)
    return;
  const u8 channel = SlotToEXIChannel(card->m_card_slot);
  system.GetExpansionInterface().GetChannel(channel)->SendTransferComplete();
}

void CEXIMemoryCard::Init(CoreTiming::CoreTimingManager& core_timing)
{
  static_assert(std::size(s_slot_events) == MEMCARD_SLOTS.size());
  for (Slot slot : MEMCARD_SLOTS)
  {
    SlotEvents& events = EventsFor(slot);
    events.command_done =
        core_timing.RegisterEvent(fmt::format("memcardDone{}", SlotLetter(slot)), OnCommandDone);
    events.transfer_done = core_timing.RegisterEvent(
        fmt::format("memcardTransferComplete{}", SlotLetter(slot)), OnTransferDone);
  }
}

void CEXIMemoryCard::Shutdown()
{
  s_slot_events.fill(SlotEvents{});
}

CEXIMemoryCard::CEXIMemoryCard(Core::System& system, const Slot slot, bool gci_folder,
                               const Memcard::HeaderData& header_data)
    : IEXIDevice(system), m_card_slot(slot)
{
  ASSERT_MSG(EXPANSIONINTERFACE, IsMemcardSlot(slot), "Trying to create invalid memory card in {}.",
             slot);

  // Events restored from a savestate may arrive for this card before it has done anything.
  m_status = STATUS_BUSY | STATUS_UNLOCKED | STATUS_READY;
  m_chip_id = NINTENDO_CHIP_ID;

  if (gci_folder)
    OpenFolderCard(header_data);
  else
    OpenImageCard(header_data.m_size_mb);

  m_capacity = m_backing->GetCardId() * BYTES_PER_MBIT;

  // The IPL keeps each card's flash ID (from the card's header) in SRAM.
  std::array<u8, 20> card_header{};
  m_backing->Read(0, static_cast<s32>(card_header.size()), card_header.data());
  SetCardFlashID(&system.GetSRAM(), card_header.data(), m_card_slot);
}

std::pair<std::string /* path */, bool /* migrate */>
CEXIMemoryCard::GetGCIFolderPath(Slot card_slot, AllowMovieFolder allow_movie_folder,
                                 Movie::MovieManager& movie)
{
  std::string user_override = Config::Get(Config::GetInfoForGCIPathOverride(card_slot));
  if (!user_override.empty())
    return {std::move(user_override), false};

  const DiscIO::Region region = Config::ToGameCubeRegion(SConfig::GetInstance().m_region);

  // A movie that starts from a clean save gets its own folder, never migrated.
  const bool movie_folder = allow_movie_folder == AllowMovieFolder::Yes &&
                            movie.IsPlayingInput() && movie.IsConfigSaved() &&
                            movie.IsUsingMemcard(card_slot) && movie.IsStartingFromClearSave();
  if (movie_folder)
  {
    return {fmt::format("{}{}/Movie/Card {}", File::GetUserPath(D_GCUSER_IDX),
                        Config::GetDirectoryForRegion(region), SlotLetter(card_slot)),
            false};
  }

  return {Config::GetGCIFolderPath(card_slot, region), true};
}

void CEXIMemoryCard::OpenFolderCard(const Memcard::HeaderData& header_data)
{
  // The folder card only shows the current game's saves; the system menu and homebrew without a
  // proper game ID see everything.
  const std::string& game_id = SConfig::GetInstance().GetGameID();
  u32 filter_game_id = 0;
  const bool real_game = game_id.length() >= 4 && game_id != "00000000" &&
                         SConfig::GetInstance().GetTitleID() != Titles::SYSTEM_MENU;
  if (real_game)
    filter_game_id = Common::swap32(reinterpret_cast<const u8*>(game_id.c_str()));

  const auto [folder, migrate] =
      GetGCIFolderPath(m_card_slot, AllowMovieFolder::Yes, m_system.GetMovie());

  const auto create_folder = [&] {
    if (migrate)  // First use: import the saves from the card image.
      MigrateFromMemcardFile(folder + DIR_SEP, m_card_slot, SConfig::GetInstance().m_region);
    else
      File::CreateFullPath(folder + DIR_SEP);
  };

  const File::FileInfo info(folder);
  if (!info.Exists())
  {
    create_folder();
  }
  else if (!info.IsDirectory())
  {
    // Something else is in the way. Move it aside, or give up if that fails.
    if (!File::Rename(folder, folder + ".original"))
    {
      PanicAlertFmtT("{0} is not a directory, failed to move to *.original.\n Verify your "
                     "write permissions or move the file outside of Dolphin",
                     folder);
      std::exit(0);
    }
    PanicAlertFmtT("{0} was not a directory, moved to *.original", folder);
    create_folder();
  }

  m_backing =
      std::make_unique<GCMemcardDirectory>(folder + DIR_SEP, m_card_slot, header_data, filter_game_id);
}

void CEXIMemoryCard::OpenImageCard(u16 size_mbits)
{
  auto& movie = m_system.GetMovie();
  const bool movie_card = movie.IsPlayingInput() && movie.IsConfigSaved() &&
                          movie.IsUsingMemcard(m_card_slot) && movie.IsStartingFromClearSave();

  std::string path;
  if (movie_card)
  {
    path = File::GetUserPath(D_GCUSER_IDX) + fmt::format("Movie{}.raw", SlotLetter(m_card_slot));
  }
  else
  {
    path = Config::GetMemcardPath(m_card_slot, SConfig::GetInstance().m_region, size_mbits);
  }

  m_backing = std::make_unique<MemoryCard>(path, m_card_slot, size_mbits);
}

CEXIMemoryCard::~CEXIMemoryCard()
{
  auto& core_timing = m_system.GetCoreTiming();
  SlotEvents& events = EventsFor(m_card_slot);
  core_timing.RemoveEvent(events.command_done);
  core_timing.RemoveEvent(events.transfer_done);
}

bool CEXIMemoryCard::UseDelayedTransferCompletion() const
{
  return true;
}

bool CEXIMemoryCard::IsPresent() const
{
  return true;
}

void CEXIMemoryCard::FinishCommand()
{
  m_status = (m_status | STATUS_READY) & ~STATUS_BUSY;
  m_irq_pending = true;
  m_system.GetExpansionInterface().UpdateInterrupts();
}

void CEXIMemoryCard::FinishCommandAfter(u64 cycles)
{
  // Only one flash operation can be outstanding per card.
  auto& core_timing = m_system.GetCoreTiming();
  CoreTiming::EventType* event = EventsFor(m_card_slot).command_done;
  core_timing.RemoveEvent(event);
  core_timing.ScheduleEvent(static_cast<s64>(cycles), event, static_cast<u64>(m_card_slot));
}

u32 CEXIMemoryCard::ClampedAddress() const
{
  return m_address & (m_capacity - 1);
}

void CEXIMemoryCard::FinishErase()
{
  if (m_opcode == OP_SECTOR_ERASE)
  {
    m_backing->ClearBlock(ClampedAddress());
    m_status = (m_status | STATUS_BUSY) & ~STATUS_READY;
    FinishCommandAfter(FLASH_OPERATION_CYCLES);
  }
  else
  {
    m_backing->ClearAll();
    m_status &= ~STATUS_BUSY;
  }
}

void CEXIMemoryCard::FinishProgram()
{
  // The page buffer holds the last 128 bytes sent; they are written from the start address on,
  // one byte at a time, wrapping within the page.
  m_status &= ~STATUS_BUSY;
  const u32 bytes_sent = m_position - 5;
  for (u32 index = 0; index < bytes_sent; ++index)
  {
    m_backing->Write(m_address, 1, &m_page_buffer[index & 127]);
    m_address = NextInPage(m_address);
  }
  FinishCommandAfter(FLASH_OPERATION_CYCLES);
}

void CEXIMemoryCard::SetCS(int cs)
{
  if (cs != 0)
  {
    // Selecting the card starts a new command.
    m_position = 0;
    return;
  }

  // Erases and programs run when the chip is deselected, once their address (and data) was
  // sent completely.
  switch (m_opcode)
  {
  case OP_SECTOR_ERASE:
  case OP_CHIP_ERASE:
    if (m_position > 2)
      FinishErase();
    break;
  case OP_PAGE_PROGRAM:
    if (m_position >= 5)
      FinishProgram();
    break;
  default:
    break;
  }
}

bool CEXIMemoryCard::IsInterruptSet()
{
  return m_irq_enabled != 0 && m_irq_pending;
}

void CEXIMemoryCard::TransferByte(u8& byte)
{
  DEBUG_LOG_FMT(EXPANSIONINTERFACE, "Memory card {}: in {:02x}", m_card_slot, byte);

  if (m_position == 0)
  {
    // The first byte is the command; the card's output is undriven, read as 0xff.
    m_opcode = byte;
    byte = 0xFF;
    if (!IsKnownOpcode(m_opcode))
      WARN_LOG_FMT(EXPANSIONINTERFACE, "Memory card: unexpected command {:02x}", m_opcode);

    if (m_opcode == OP_CLEAR_STATUS)
    {
      m_status = (m_status & ~(STATUS_PROGRAM_ERROR | STATUS_ERASE_ERROR)) | STATUS_READY;
      m_irq_pending = false;
    }
  }
  else
  {
    switch (m_opcode)
    {
    case OP_GET_EXI_ID:
    {
      // A dummy byte, then the card ID as a big-endian 32-bit word, repeated.
      if (m_position == 1)
      {
        byte = 0x80;
      }
      else
      {
        const u32 shift = 24 - ((m_position - 2) & 3) * 8;
        byte = static_cast<u8>(m_backing->GetCardId() >> shift);
      }
      break;
    }

    case OP_READ_ARRAY:
      // Four address bytes, four dummy bytes, then data. The card already drives data while the
      // address arrives.
      m_address = AddressPhase(m_address, m_position, byte);
      if (m_position == 1)
      {
        byte = 0xFF;
        break;
      }
      m_backing->Read(ClampedAddress(), 1, &byte);
      if (m_position >= 9)
        m_address = NextInPage(m_address);
      break;

    case OP_READ_STATUS:
      byte = static_cast<u8>(m_status);
      break;

    case OP_READ_ID:
      // High byte first, then alternating.
      byte = static_cast<u8>((m_position != 1 && (m_position & 1) != 0) ? m_chip_id :
                                                                         m_chip_id >> 8);
      break;

    case OP_SECTOR_ERASE:
      // Only the sector part of the address is sent.
      if (m_position <= 2)
        m_address = AddressPhase(m_address, m_position, byte);
      byte = 0xFF;
      break;

    case OP_SET_INTERRUPT:
      if (m_position == 1)
        m_irq_enabled = byte;
      byte = 0xFF;
      break;

    case OP_CHIP_ERASE:
      byte = 0xFF;
      break;

    case OP_PAGE_PROGRAM:
      m_address = AddressPhase(m_address, m_position, byte);
      if (m_position >= 5)
        m_page_buffer[(m_position - 5) & 0x7F] = byte;
      byte = 0xFF;
      break;

    default:
      WARN_LOG_FMT(EXPANSIONINTERFACE, "Memory card: unhandled byte {:02x} for command {:02x}",
                   byte, m_opcode);
      byte = 0xFF;
      break;
    }
  }

  ++m_position;
  DEBUG_LOG_FMT(EXPANSIONINTERFACE, "Memory card {}: out {:02x}", m_card_slot, byte);
}

void CEXIMemoryCard::DoState(PointerWrap& p)
{
  // The card's contents and state are only part of savestates while a movie runs, so that
  // replays stay in sync. Otherwise the user's saves stay separate from savestates. On load the
  // flag in the state decides.
  bool with_contents = m_system.GetMovie().IsMovieActive();
  p.Do(with_contents);
  if (!with_contents)
    return;

  p.Do(m_irq_enabled);
  p.Do(m_irq_pending);
  p.Do(m_opcode);
  p.Do(m_status);
  p.Do(m_position);
  p.Do(m_page_buffer);
  p.Do(m_address);
  m_backing->DoState(p);
  p.Do(m_card_slot);
}

void CEXIMemoryCard::ScheduleTransferDone(u32 size, u32 bytes_per_second)
{
  const u32 cycles_per_byte = m_system.GetSystemTimers().GetTicksPerSecond() / bytes_per_second;
  m_system.GetCoreTiming().ScheduleEvent(size * cycles_per_byte,
                                         EventsFor(m_card_slot).transfer_done,
                                         static_cast<u64>(m_card_slot));
}

// The command and address were sent by immediate transfers before a DMA; the DMA moves the whole
// block at once, without advancing the card's address.
void CEXIMemoryCard::DMARead(u32 addr, u32 size)
{
  auto& memory = m_system.GetMemory();
  m_backing->Read(m_address, static_cast<s32>(size), memory.GetPointerForRange(addr, size));
  if ((m_address + size) % Memcard::BLOCK_SIZE == 0)
    INFO_LOG_FMT(EXPANSIONINTERFACE, "reading from block: {:x}", m_address / Memcard::BLOCK_SIZE);

  ScheduleTransferDone(size, READ_BYTES_PER_SECOND);
}

void CEXIMemoryCard::DMAWrite(u32 addr, u32 size)
{
  auto& memory = m_system.GetMemory();
  m_backing->Write(m_address, static_cast<s32>(size), memory.GetPointerForRange(addr, size));
  if ((m_address + size) % Memcard::BLOCK_SIZE == 0)
    INFO_LOG_FMT(EXPANSIONINTERFACE, "writing to block: {:x}", m_address / Memcard::BLOCK_SIZE);

  ScheduleTransferDone(size, WRITE_BYTES_PER_SECOND);
}
}  // namespace ExpansionInterface
