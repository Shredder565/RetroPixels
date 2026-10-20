// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

#include "Core/HW/GCMemcard/GCMemcard.h"

class PointerWrap;

// Interface shared by every backing store that can sit behind an EXI memory
// card device: a single raw image file, or a folder of GCI saves.
class MemoryCardBase
{
public:
  explicit MemoryCardBase(ExpansionInterface::Slot card_slot,
                          int size_mbits = Memcard::MBIT_SIZE_MEMORY_CARD_2043)
      : m_card_slot(card_slot), m_nintendo_card_id(size_mbits)
  {
  }
  virtual ~MemoryCardBase() = default;

  // Byte-addressed access to the card's flash. Both return the number of
  // bytes handled.
  virtual s32 Read(u32 src_address, s32 length, u8* dest_address) = 0;
  virtual s32 Write(u32 dest_address, s32 length, const u8* src_address) = 0;

  // Erase one sector (sets it to 0xFF) or the whole card.
  virtual void ClearBlock(u32 address) = 0;
  virtual void ClearAll() = 0;

  virtual void DoState(PointerWrap& p) = 0;

  // The card ID reported over EXI is the capacity in megabits.
  u32 GetCardId() const { return m_nintendo_card_id; }

protected:
  ExpansionInterface::Slot m_card_slot;
  u16 m_nintendo_card_id;
};
