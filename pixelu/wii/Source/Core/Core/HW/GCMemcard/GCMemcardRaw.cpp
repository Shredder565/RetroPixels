// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCMemcard/GCMemcardRaw.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Common/Timer.h"

#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/Sram.h"
#include "Core/System.h"

namespace
{
// One megabit of flash, in bytes.
constexpr u32 BYTES_PER_MBIT = 1024 * 1024 / 8;
// Header, two directories and two BATs: the part Format() fills in.
constexpr u32 SYSTEM_AREA_BYTES = Memcard::MC_FST_BLOCKS * Memcard::BLOCK_SIZE;

constexpr auto AUTOSAVE_PERIOD = std::chrono::seconds(15);
}  // namespace

MemoryCard::MemoryCard(std::string filename, ExpansionInterface::Slot card_slot, u16 size_mbits)
    : MemoryCardBase(card_slot, size_mbits), m_filename(std::move(filename))
{
  File::IOFile image(m_filename, "rb");
  if (image)
  {
    // Use the card as found on disk; its size decides the reported capacity.
    m_memory_card_size = static_cast<u32>(image.GetSize());
    m_nintendo_card_id = m_memory_card_size / BYTES_PER_MBIT;
    m_memcard_data = std::make_unique<u8[]>(m_memory_card_size);
    std::fill_n(m_memcard_data.get(), m_memory_card_size, u8{0xFF});

    INFO_LOG_FMT(EXPANSIONINTERFACE, "Reading memory card {}", m_filename);
    image.ReadBytes(m_memcard_data.get(), m_memory_card_size);
  }
  else
  {
    // No image yet: start from a freshly formatted, otherwise erased card,
    // formatted the way this console's IPL would do it.
    m_nintendo_card_id = size_mbits;
    m_memory_card_size = size_mbits * BYTES_PER_MBIT;
    m_memcard_data = std::make_unique<u8[]>(m_memory_card_size);

    const auto& sram = Core::System::GetInstance().GetSRAM();
    const u64 now_gc =
        Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;
    Memcard::GCMemcard::Format(m_memcard_data.get(), sram.settings_ex.flash_id[Memcard::SLOT_A],
                               size_mbits, m_filename.contains(".JAP.raw"),
                               sram.settings.rtc_bias, static_cast<u32>(sram.settings.language),
                               now_gc);
    std::fill(m_memcard_data.get() + SYSTEM_AREA_BYTES, m_memcard_data.get() + m_memory_card_size,
              u8{0xFF});

    INFO_LOG_FMT(EXPANSIONINTERFACE, "No memory card found. A new one was created instead.");
  }

  // The flush thread reads the members above, so it starts last.
  m_flush_buffer = std::make_unique<u8[]>(m_memory_card_size);
  m_flush_thread = std::thread(&MemoryCard::FlushThread, this);
}

MemoryCard::~MemoryCard()
{
  if (!m_flush_thread.joinable())
    return;

  // Waking the thread early tells it to do a final write and stop.
  m_flush_trigger.Set();
  m_flush_thread.join();
}

void MemoryCard::FlushThread()
{
  if (!Config::Get(Config::SESSION_SAVE_DATA_WRITABLE))
    return;

  Common::SetCurrentThreadName(fmt::format("Memcard {} flushing thread", m_card_slot).c_str());

  for (;;)
  {
    // A signal means shutdown (always write); a timeout means write only if
    // the game changed something since the last pass.
    const bool shutting_down = m_flush_trigger.WaitFor(AUTOSAVE_PERIOD);
    if (!shutting_down && !m_dirty.TestAndClear())
      continue;

    // Reopened every time in case the file was removed while running.
    File::IOFile image(m_filename, "r+b");
    if (!image)
    {
      std::string folder;
      SplitPath(m_filename, &folder, nullptr, nullptr);
      if (!File::IsDirectory(folder))
        File::CreateFullPath(folder);
      image.Open(m_filename, "wb");
    }

    if (!image)
    {
      PanicAlertFmtT(
          "Could not write memory card file {0}.\n\n"
          "Are you running Dolphin from a CD/DVD, or is the save file maybe write protected?\n\n"
          "Are you receiving this after moving the emulator directory?\nIf so, then you may "
          "need to re-specify your memory card location in the options.",
          m_filename);
      // Give up; nothing more is written back for this card.
      return;
    }

    // Snapshot under the lock so the disk write doesn't block the emulator.
    {
      std::lock_guard guard(m_flush_mutex);
      std::copy_n(m_memcard_data.get(), m_memory_card_size, m_flush_buffer.get());
    }
    image.WriteBytes(m_flush_buffer.get(), m_memory_card_size);

    if (shutting_down)
      return;

    const char slot_letter = m_card_slot == ExpansionInterface::Slot::A ? 'A' : 'B';
    Core::DisplayMessage(fmt::format("Wrote to Memory Card {}", slot_letter), 4000);
  }
}

void MemoryCard::MakeDirty()
{
  m_dirty.Set();
}

s32 MemoryCard::Read(u32 src_address, s32 length, u8* dest_address)
{
  if (!IsAddressInBounds(src_address, static_cast<u32>(length)))
  {
    PanicAlertFmtT("MemoryCard: Read called with invalid source address ({0:#x})", src_address);
    return -1;
  }

  std::memcpy(dest_address, m_memcard_data.get() + src_address, length);
  return length;
}

s32 MemoryCard::Write(u32 dest_address, s32 length, const u8* src_address)
{
  if (!IsAddressInBounds(dest_address, static_cast<u32>(length)))
  {
    PanicAlertFmtT("MemoryCard: Write called with invalid destination address ({0:#x})",
                   dest_address);
    return -1;
  }

  {
    std::lock_guard guard(m_flush_mutex);
    std::memcpy(m_memcard_data.get() + dest_address, src_address, length);
  }
  MakeDirty();
  return length;
}

void MemoryCard::ClearBlock(u32 address)
{
  // Only whole, sector-aligned blocks can be erased.
  const bool aligned = address % Memcard::BLOCK_SIZE == 0;
  if (!aligned || !IsAddressInBounds(address, Memcard::BLOCK_SIZE))
  {
    PanicAlertFmtT("MemoryCard: ClearBlock called on invalid address ({0:#x})", address);
    return;
  }

  {
    std::lock_guard guard(m_flush_mutex);
    std::fill_n(m_memcard_data.get() + address, Memcard::BLOCK_SIZE, u8{0xFF});
  }
  MakeDirty();
}

void MemoryCard::ClearAll()
{
  {
    std::lock_guard guard(m_flush_mutex);
    std::fill_n(m_memcard_data.get(), m_memory_card_size, u8{0xFF});
  }
  MakeDirty();
}

void MemoryCard::DoState(PointerWrap& p)
{
  p.Do(m_card_slot);
  p.Do(m_memory_card_size);
  p.DoArray(m_memcard_data.get(), m_memory_card_size);
}
