// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

#include "Core/HW/Triforce/SerialDevice.h"

class PointerWrap;

namespace Triforce
{

// Contactless IC card reader/writer (Virtua Striker 4, Gekitou Pro Yakyuu,
// The Key of Avalon). A card is a set of RFID tags, each with 256 pages of 8
// bytes kept in a file in the Triforce folder.
class ICCardReader final : public SerialDevice
{
public:
  explicit ICCardReader(u8 slot_index);
  ~ICCardReader() override;

  void Update() override;

  bool IsCardPresent() const;
  bool IsEjecting() const;
  bool IsReadyToInsertCard() const;

  void InsertCard();
  void EjectCard();

  u8 GetCardPresentInsertCheckCount() const { return m_insert_checks; }

  void DoState(PointerWrap& p) override;

private:
  struct Tag;

  void MakeTags(std::size_t count);
  void HandleRequest(u8 command, std::span<const u8> payload);

  std::vector<std::unique_ptr<Tag>> m_tags;

  bool m_field_on = false;
  const u8 m_slot_index;
  u8 m_eject_timer = 0;

  // The Key of Avalon keeps polling InsertCheck while it waits for the card
  // to be taken; the I/O adapter ejects the card after enough of them.
  u8 m_insert_checks = 0;
};

}  // namespace Triforce
