// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/IOPorts.h"

#include <algorithm>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Triforce/ICCardReader.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Triforce
{

namespace
{
struct ButtonBit
{
  u16 button;
  u8 bit;
};

template <std::size_t N>
void MapButtons(u16 buttons, u8& target, const std::array<ButtonBit, N>& map)
{
  for (const ButtonBit& entry : map)
  {
    if ((buttons & entry.button) != 0)
      target |= entry.bit;
  }
}

void SetBit(u8& target, u8 bit, bool on)
{
  if (on)
    target |= bit;
  else
    target &= static_cast<u8>(0xFF ^ bit);
}

u16 StickToAnalog(u8 value)
{
  return Common::ExpandValue<u16>(value, 8);
}
}  // namespace

// ---- IOPorts ---------------------------------------------------------------------------------

void IOPorts::Update()
{
  m_system_inputs = 0;
  m_switch_inputs.fill(0);
  m_analog_inputs.fill(NEUTRAL_ANALOG_VALUE);
  m_coin_inputs.fill(false);

  for (const auto& adapter : m_adapters)
    adapter->Update();
}

std::span<u8> IOPorts::GetSwitchInputs(u32 player_index)
{
  ASSERT(player_index < PLAYER_COUNT);
  return std::span<u8>(m_switch_inputs).subspan(player_index * BYTES_PER_PLAYER,
                                                BYTES_PER_PLAYER);
}

std::span<const u8> IOPorts::GetSwitchInputs(u32 player_index) const
{
  ASSERT(player_index < PLAYER_COUNT);
  return std::span<const u8>(m_switch_inputs)
      .subspan(player_index * BYTES_PER_PLAYER, BYTES_PER_PLAYER);
}

void IOPorts::SetGenericOutputs(std::span<const u8> bytes)
{
  if (bytes.size() > OUTPUT_BYTES)
  {
    WARN_LOG_FMT(SERIALINTERFACE_JVSIO, "SetGenericOutputs: Unexpected byte count: {}",
                 bytes.size());
  }

  const std::size_t count = std::min(bytes.size(), OUTPUT_BYTES);
  if (std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(count),
                 m_generic_outputs.begin()))
  {
    return;
  }

  std::array<u8, OUTPUT_BYTES> turned_on{};
  std::array<u8, OUTPUT_BYTES> turned_off{};
  for (std::size_t i = 0; i != count; ++i)
  {
    const u8 before = m_generic_outputs[i];
    const u8 after = bytes[i];
    const u8 diff = static_cast<u8>(before ^ after);
    turned_on[i] = static_cast<u8>(diff & after);
    turned_off[i] = static_cast<u8>(diff & before);
    m_generic_outputs[i] = after;
    DEBUG_LOG_FMT(SERIALINTERFACE_JVSIO, "SetGenericOutputs: set:{:02x} clr:{:02x} ({:02x})",
                  turned_on[i], turned_off[i], after);
  }

  for (const auto& adapter : m_adapters)
    adapter->HandleGenericOutputsChanged(turned_on, turned_off);
}

void IOPorts::ResetGenericOutputs()
{
  static constexpr std::array<u8, OUTPUT_BYTES> all_off{};
  SetGenericOutputs(all_off);
}

void IOPorts::AddIOAdapter(std::unique_ptr<IOAdapter> adapter)
{
  adapter->SetIOPorts(this);
  m_adapters.push_back(std::move(adapter));
}

void IOPorts::DoState(PointerWrap& p)
{
  p.DoArray(m_status_switches);
  p.DoArray(m_generic_outputs);
  for (const auto& adapter : m_adapters)
    adapter->DoState(p);
}

// ---- IOAdapter -------------------------------------------------------------------------------

IOAdapter::IOAdapter() = default;
IOAdapter::~IOAdapter() = default;

void IOAdapter::Update()
{
}

void IOAdapter::DoState(PointerWrap&)
{
}

void IOAdapter::HandleGenericOutputsChanged(std::span<const u8>, std::span<const u8>)
{
}

// ---- Common ----------------------------------------------------------------------------------

void Common_IOAdapter::Update()
{
  IOPorts& ports = *GetIOPorts();
  const auto& si = m_system.GetSerialInterface();

  for (int channel = 0; channel != 2; ++channel)
  {
    if (si.GetDeviceType(channel) != SerialInterface::SIDEVICE_AM_BASEBOARD)
      continue;

    const GCPadStatus pad = Pad::GetStatus(channel);
    if ((pad.switches & SWITCH_TEST) != 0)
    {
      if (AMMediaboard::GetTestMenu())
      {
        *ports.GetSystemInputs() |= 0x80;
      }
      else
      {
        OSD::AddMessage("Test menu is disabled due to missing SegaBoot.", OSD::Duration::NORMAL,
                        OSD::Color::RED);
      }
    }
    if ((pad.switches & SWITCH_SERVICE) != 0)
      ports.GetSwitchInputs(static_cast<u32>(channel))[0] |= 0x40;
    if ((pad.switches & SWITCH_COIN) != 0)
      ports.GetCoinInputs()[static_cast<std::size_t>(channel)] = true;
  }
}

// ---- Virtua Striker 3 ------------------------------------------------------------------------

void VirtuaStriker3_IOAdapter::Update()
{
  static constexpr std::array<ButtonBit, 7> first_byte{{
      {PAD_BUTTON_START, 0x80},
      {PAD_BUTTON_UP, 0x20},
      {PAD_BUTTON_DOWN, 0x10},
      {PAD_BUTTON_LEFT, 0x08},
      {PAD_BUTTON_RIGHT, 0x04},
      {PAD_BUTTON_X, 0x02},
      {PAD_BUTTON_B, 0x01},
  }};
  static constexpr std::array<ButtonBit, 1> second_byte{{{PAD_BUTTON_A, 0x80}}};

  IOPorts& ports = *GetIOPorts();
  for (u32 player = 0; player != IOPorts::PLAYER_COUNT; ++player)
  {
    const GCPadStatus pad = Pad::GetStatus(static_cast<int>(player));
    const std::span<u8> switches = ports.GetSwitchInputs(player);
    MapButtons(pad.button, switches[0], first_byte);
    MapButtons(pad.button, switches[1], second_byte);
  }
}

// ---- Virtua Striker 4 ------------------------------------------------------------------------

void VirtuaStriker4Common_IOAdapter::Update()
{
  static constexpr std::array<ButtonBit, 6> first_byte{{
      {PAD_BUTTON_START, 0x80},
      {PAD_BUTTON_LEFT, 0x20},
      {PAD_BUTTON_UP, 0x08},
      {PAD_BUTTON_RIGHT, 0x04},
      {PAD_BUTTON_A, 0x02},
      {PAD_BUTTON_X, 0x01},
  }};
  static constexpr std::array<ButtonBit, 2> second_byte{{
      {PAD_BUTTON_B, 0x80},
      {PAD_BUTTON_Y, 0x40},
  }};

  IOPorts& ports = *GetIOPorts();
  const u8 locks = ports.GetGenericOutputs()[0];
  ICCardReader& reader_a = *m_readers[0];
  ICCardReader& reader_b = *m_readers[1];

  // While slot A is locked a card goes to slot B; otherwise B gives its card back.
  if ((locks & SLOT_A_LOCK_BIT) != 0)
  {
    if (reader_b.IsReadyToInsertCard())
      reader_b.InsertCard();
  }
  else if (reader_b.IsCardPresent() && (locks & SLOT_B_LOCK_BIT) == 0)
  {
    reader_b.EjectCard();
  }

  if (reader_a.IsReadyToInsertCard())
    reader_a.InsertCard();

  const std::span<u16> analog = ports.GetAnalogInputs();
  for (u32 player = 0; player != IOPorts::PLAYER_COUNT; ++player)
  {
    const ICCardReader& reader = *m_readers[player];
    const std::span<u8> switches = ports.GetSwitchInputs(player);
    SetBit(switches[0], 0x10, reader.IsCardPresent());
    SetBit(switches[1], 0x20, reader.IsEjecting());

    const GCPadStatus pad = Pad::GetStatus(static_cast<int>(player));
    MapButtons(pad.button, switches[0], first_byte);
    MapButtons(pad.button, switches[1], second_byte);

    analog[player * 2] = StickToAnalog(static_cast<u8>(0xFF - pad.stickY));
    analog[player * 2 + 1] = StickToAnalog(pad.stickX);
  }
}

void VirtuaStriker4Common_IOAdapter::HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                                                 std::span<const u8> bits_cleared)
{
  const u8 changed = static_cast<u8>(bits_set[0] | bits_cleared[0]);
  const auto state = [&](u8 bit) { return (bits_set[0] & bit) != 0 ? "Locked" : "Unlocked"; };

  if ((changed & SLOT_A_LOCK_BIT) != 0)
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "JVS-IO: Slot 1: {}", state(SLOT_A_LOCK_BIT));
  if ((changed & SLOT_B_LOCK_BIT) != 0)
    INFO_LOG_FMT(SERIALINTERFACE_JVSIO, "JVS-IO: Slot 2: {}", state(SLOT_B_LOCK_BIT));
}

void VirtuaStriker4_2006_IOAdapter::HandleGenericOutputsChanged(std::span<const u8> bits_set,
                                                                std::span<const u8>)
{
  if ((bits_set[0] & SLOT_A_EJECT_BIT) != 0)
    m_readers[0]->EjectCard();
  if ((bits_set[0] & SLOT_B_EJECT_BIT) != 0)
    m_readers[1]->EjectCard();
}

// ---- Gekitou Pro Yakyuu ----------------------------------------------------------------------

void GekitouProYakyuu_IOAdapter::Update()
{
  static constexpr std::array<ButtonBit, 7> first_byte{{
      {PAD_BUTTON_START, 0x80},
      {PAD_BUTTON_UP, 0x20},
      {PAD_BUTTON_DOWN, 0x10},
      {PAD_BUTTON_LEFT, 0x08},
      {PAD_BUTTON_RIGHT, 0x04},
      {PAD_BUTTON_A, 0x02},
      {PAD_BUTTON_B, 0x01},
  }};
  static constexpr std::array<ButtonBit, 1> second_byte{{{PAD_TRIGGER_L, 0x80}}};

  IOPorts& ports = *GetIOPorts();
  for (u32 player = 0; player != IOPorts::PLAYER_COUNT; ++player)
  {
    ICCardReader& reader = *m_readers[player];
    if (reader.IsReadyToInsertCard())
      reader.InsertCard();

    const std::span<u8> switches = ports.GetSwitchInputs(player);
    SetBit(switches[1], 0x40, reader.IsCardPresent());

    const GCPadStatus pad = Pad::GetStatus(static_cast<int>(player));
    MapButtons(pad.button, switches[0], first_byte);
    MapButtons(pad.button, switches[1], second_byte);
  }
}

void GekitouProYakyuu_IOAdapter::HandleGenericOutputsChanged(std::span<const u8>,
                                                             std::span<const u8> bits_cleared)
{
  if ((bits_cleared[0] & SLOT_A_LOCK_BIT) != 0)
    m_readers[0]->EjectCard();
  if ((bits_cleared[0] & SLOT_B_LOCK_BIT) != 0)
    m_readers[1]->EjectCard();
}

// ---- The Key of Avalon -----------------------------------------------------------------------

void KeyOfAvalon_IOAdapter::Update()
{
  static constexpr std::array<ButtonBit, 3> first_byte{{
      {PAD_BUTTON_START, 0x80},
      {PAD_BUTTON_B, 0x08},
      {PAD_BUTTON_A, 0x04},
  }};

  // The game polls InsertCheck while it waits for the card to be taken.
  if (m_reader->GetCardPresentInsertCheckCount() >= 10)
    m_reader->EjectCard();
  if (m_reader->IsReadyToInsertCard())
    m_reader->InsertCard();

  const GCPadStatus pad = Pad::GetStatus(0);
  MapButtons(pad.button, GetIOPorts()->GetSwitchInputs(0)[0], first_byte);
}

}  // namespace Triforce
