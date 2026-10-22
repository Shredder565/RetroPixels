// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/MagCard/C1231BR.h"

#include <array>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"

namespace MagCard
{

namespace
{
constexpr u8 SHUTTER_OPEN_FLAG = 0x80;
constexpr u8 SHUTTER_CLOSED_FLAG = 0x40;
constexpr u8 DISPENSER_HAS_CARDS_FLAG = 0x20;

constexpr u8 OPEN_ARGUMENT = 0x31;
}  // namespace

bool C1231BR::IsReadyForCard()
{
  return !IsCardPresent() && m_shutter_open;
}

u8 C1231BR::GetPositionValue()
{
  // Indexed by R.
  static constexpr std::array<u8, 5> positions{0x00, 0x18, 0x07, 0x1C, 0x01};

  const std::size_t index = static_cast<std::size_t>(m_status.r);
  u8 value = index < positions.size() ? positions[index] : u8{0};
  value |= m_shutter_open ? SHUTTER_OPEN_FLAG : SHUTTER_CLOSED_FLAG;
  if (!m_card_settings->report_dispenser_empty)
    value |= DISPENSER_HAS_CARDS_FLAG;
  return value;
}

void C1231BR::Command_10_Initialize()
{
  // "Eject after": open up and push the card back to the slot, data kept.
  if (!m_current_packet.empty() &&
      m_current_packet[0] == static_cast<u8>(InitMode::EjectAfter))
  {
    m_shutter_open = true;
    MoveCard(R::ENTRY_SLOT);
  }
  FinishCommand();
}

void C1231BR::Command_D0_ControlShutter()
{
  if (m_current_packet.empty())
  {
    SetPError(P::SYSTEM_ERR);
    return;
  }
  m_shutter_open = m_current_packet[0] == OPEN_ARGUMENT;
  INFO_LOG_FMT(SERIALINTERFACE_CARD, "Shutter: {}", m_shutter_open ? "Open" : "Close");
  FinishCommand();
}

void C1231BR::DoState(PointerWrap& p)
{
  MagneticCardReader::DoState(p);
  p.Do(m_shutter_open);
}

}  // namespace MagCard
