// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceGCController.h"

#include <array>
#include <cstring>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/System.h"
#include "InputCommon/GCPadStatus.h"

namespace SerialInterface
{
namespace
{
// The analog mode decides how C-stick, triggers and analog A/B share the
// low input word. Each layout lists its fields from bit 0 upwards; a field
// is either the whole byte (8 bits) or its top nibble (4 bits).
enum class Analog : u8
{
  SubX,
  SubY,
  TrigL,
  TrigR,
  AnalogA,
  AnalogB,
};

struct PackedField
{
  Analog source;
  u8 bits;
};

struct LowWordLayout
{
  std::array<PackedField, 6> fields;
  u8 count;
};

constexpr LowWordLayout LAYOUT_MODE0 = {{{{Analog::AnalogB, 4},
                                          {Analog::AnalogA, 4},
                                          {Analog::TrigR, 4},
                                          {Analog::TrigL, 4},
                                          {Analog::SubY, 8},
                                          {Analog::SubX, 8}}},
                                        6};
constexpr LowWordLayout LAYOUT_MODE1 = {{{{Analog::AnalogB, 4},
                                          {Analog::AnalogA, 4},
                                          {Analog::TrigR, 8},
                                          {Analog::TrigL, 8},
                                          {Analog::SubY, 4},
                                          {Analog::SubX, 4}}},
                                        6};
constexpr LowWordLayout LAYOUT_MODE2 = {{{{Analog::AnalogB, 8},
                                          {Analog::AnalogA, 8},
                                          {Analog::TrigR, 4},
                                          {Analog::TrigL, 4},
                                          {Analog::SubY, 4},
                                          {Analog::SubX, 4}}},
                                        6};
// Mode 3 has no analog A/B, mode 4 no analog triggers.
constexpr LowWordLayout LAYOUT_MODE3 = {
    {{{Analog::TrigR, 8}, {Analog::TrigL, 8}, {Analog::SubY, 8}, {Analog::SubX, 8}}}, 4};
constexpr LowWordLayout LAYOUT_MODE4 = {
    {{{Analog::AnalogB, 8}, {Analog::AnalogA, 8}, {Analog::SubY, 8}, {Analog::SubX, 8}}}, 4};

const LowWordLayout* LayoutForMode(u8 mode)
{
  switch (mode)
  {
  case 0:
  case 5:
  case 6:
  case 7:
    return &LAYOUT_MODE0;
  case 1:
    return &LAYOUT_MODE1;
  case 2:
    return &LAYOUT_MODE2;
  case 3:
    return &LAYOUT_MODE3;
  case 4:
    return &LAYOUT_MODE4;
  default:
    return nullptr;
  }
}

u8 AnalogValue(const GCPadStatus& pad, Analog source)
{
  switch (source)
  {
  case Analog::SubX:
    return pad.substickX;
  case Analog::SubY:
    return pad.substickY;
  case Analog::TrigL:
    return pad.triggerLeft;
  case Analog::TrigR:
    return pad.triggerRight;
  case Analog::AnalogA:
    return pad.analogA;
  case Analog::AnalogB:
    return pad.analogB;
  }
  return 0;
}

u32 PackLowWord(const LowWordLayout& layout, const GCPadStatus& pad)
{
  u32 word = 0;
  u32 position = 0;
  for (u8 i = 0; i < layout.count; ++i)
  {
    const PackedField& field = layout.fields[i];
    const u32 value = AnalogValue(pad, field.source);
    word |= (field.bits == 8 ? value : value >> 4) << position;
    position += field.bits;
  }
  return word;
}

void StoreWordBE(u8* dst, u32 value)
{
  dst[0] = static_cast<u8>(value >> 24);
  dst[1] = static_cast<u8>(value >> 16);
  dst[2] = static_cast<u8>(value >> 8);
  dst[3] = static_cast<u8>(value);
}
}  // namespace

CSIDevice_GCController::CSIDevice_GCController(Core::System& system, SIDevices device,
                                               int device_number)
    : ISIDevice(system, device, device_number)
{
  // Start from a centred origin rather than whatever the sticks read when the
  // pad is plugged in, so a slightly moved stick does not become "neutral".
  m_origin.origin_stick_x = GCPadStatus::MAIN_STICK_CENTER_X;
  m_origin.origin_stick_y = GCPadStatus::MAIN_STICK_CENTER_Y;
  m_origin.substick_x = GCPadStatus::C_STICK_CENTER_X;
  m_origin.substick_y = GCPadStatus::C_STICK_CENTER_Y;
}

int CSIDevice_GCController::RunBuffer(u8* buffer, int request_length)
{
  ISIDevice::RunBuffer(buffer, request_length);

  if (!GetPadStatus().isConnected)
    return -1;

  const u8 command = buffer[0];
  switch (static_cast<EBufferCommands>(command))
  {
  case EBufferCommands::CMD_STATUS:
  case EBufferCommands::CMD_RESET:
    return CreateStatusResponse(SI_GC_CONTROLLER, buffer);

  case EBufferCommands::CMD_DIRECT:
  {
    INFO_LOG_FMT(SERIALINTERFACE, "Pad {}: direct poll ({} byte request)", m_device_number,
                 request_length);
    u32 hi = 0;
    u32 lo = 0;
    GetData(hi, lo);
    StoreWordBE(buffer, hi);
    StoreWordBE(buffer + 4, lo);
    return 8;
  }

  case EBufferCommands::CMD_ORIGIN:
  case EBufferCommands::CMD_RECALIBRATE:
    INFO_LOG_FMT(SERIALINTERFACE, "Pad {}: origin request ({:#04x})", m_device_number, command);
    std::memcpy(buffer, &m_origin, sizeof(SOrigin));
    return static_cast<int>(sizeof(SOrigin));

  case EBufferCommands::CMD_SET_GAME_ID:
    // Licensed pads ignore it (third-party adapters such as BlueRetro use it
    // to pick a per-game mapping); there is no reply.
    return 0;

  default:
    ERROR_LOG_FMT(SERIALINTERFACE, "Pad {}: unhandled SI command {:#x}", m_device_number, command);
    PanicAlertFmt("SI: Unknown command ({:#x})", command);
    return 0;
  }
}

void CSIDevice_GCController::HandleMoviePadStatus(Movie::MovieManager& movie, int device_number,
                                                  GCPadStatus* pad_status)
{
  movie.SetPolledDevice();

  // NetPlay supplies the input itself; nothing else to do then.
  if (NetPlay_GetInput(device_number, pad_status))
    return;

  if (movie.IsPlayingInput())
  {
    movie.PlayController(pad_status, device_number);
    movie.InputUpdate();
  }
  else if (movie.IsRecordingInput())
  {
    movie.RecordInput(pad_status, device_number);
    movie.InputUpdate();
  }
  else
  {
    movie.CheckPadStatus(pad_status, device_number);
  }
}

GCPadStatus CSIDevice_GCController::GetPadStatus()
{
  GCPadStatus pad = {};

  // Under NetPlay every pad's state (local ones included) comes from the
  // NetPlay client inside HandleMoviePadStatus.
  if (!NetPlay::IsNetPlayRunning())
    pad = Pad::GetStatus(m_device_number);

  HandleMoviePadStatus(m_system.GetMovie(), m_device_number, &pad);

  // A freshly connected real pad (GC adapter) asks for its origin this way.
  if ((pad.button & PAD_GET_ORIGIN) != 0)
    SetOrigin(pad);

  return pad;
}

DataResponse CSIDevice_GCController::GetData(u32& hi, u32& low)
{
  GCPadStatus pad = GetPadStatus();
  if (!pad.isConnected)
    return DataResponse::ErrorNoResponse;

  if (HandleButtonCombos(pad) == COMBO_ORIGIN)
    pad.button |= PAD_GET_ORIGIN;

  // The high word does not depend on the analog mode.
  hi = MapPadStatus(pad);

  // An unknown mode leaves the low word as it was.
  if (const LowWordLayout* layout = LayoutForMode(m_mode))
    low = PackLowWord(*layout, pad);

  return DataResponse::Success;
}

u32 CSIDevice_GCController::MapPadStatus(const GCPadStatus& pad_status)
{
  const u32 buttons = static_cast<u32>(pad_status.button) | PAD_USE_ORIGIN;
  return (buttons << 16) | (u32{pad_status.stickX} << 8) | u32{pad_status.stickY};
}

CSIDevice_GCController::EButtonCombo
CSIDevice_GCController::HandleButtonCombos(const GCPadStatus& pad_status)
{
  constexpr u16 ORIGIN_COMBO = PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START;
  constexpr u16 RESET_COMBO = PAD_BUTTON_X | PAD_BUTTON_B | PAD_BUTTON_START;

  const u16 held = pad_status.button & 0xff00;
  EButtonCombo combo = COMBO_NONE;
  if (held == ORIGIN_COMBO)
    combo = COMBO_ORIGIN;
  else if (held == RESET_COMBO)
    combo = COMBO_RESET;

  auto& timing = m_system.GetCoreTiming();
  if (combo != m_last_button_combo)
  {
    m_last_button_combo = combo;
    if (combo != COMBO_NONE)
      m_timer_button_combo_start = timing.GetTicks();
  }

  if (m_last_button_combo == COMBO_NONE)
    return COMBO_NONE;

  // The combo fires once it has been held for three seconds.
  const u32 held_for = static_cast<u32>(timing.GetTicks() - m_timer_button_combo_start);
  const u32 three_seconds = m_system.GetSystemTimers().GetTicksPerSecond() * 3;
  if (held_for <= three_seconds)
    return COMBO_NONE;

  if (m_last_button_combo == COMBO_RESET)
  {
    INFO_LOG_FMT(SERIALINTERFACE, "Pad {}: reset combo", m_device_number);
    m_system.GetProcessorInterface().ResetButton_Tap();
  }
  else
  {
    INFO_LOG_FMT(SERIALINTERFACE, "Pad {}: origin combo", m_device_number);
    SetOrigin(pad_status);
  }
  m_last_button_combo = COMBO_NONE;
  return combo;
}

void CSIDevice_GCController::SetOrigin(const GCPadStatus& pad_status)
{
  m_origin.origin_stick_x = pad_status.stickX;
  m_origin.origin_stick_y = pad_status.stickY;
  m_origin.substick_x = pad_status.substickX;
  m_origin.substick_y = pad_status.substickY;
  m_origin.trigger_left = pad_status.triggerLeft;
  m_origin.trigger_right = pad_status.triggerRight;
}

void CSIDevice_GCController::SendCommand(u32 command, u8 poll)
{
  const UCommand word(command);
  const auto direct = static_cast<EDirectCommands>(word.command);

  if (direct != EDirectCommands::CMD_WRITE)
  {
    // Some demos send a zero command byte; that is harmless.
    if (word.command != 0)
    {
      ERROR_LOG_FMT(SERIALINTERFACE, "Pad {}: unhandled direct command {:#x}", m_device_number,
                    command);
      PanicAlertFmt("SI: Unknown direct command");
    }
    return;
  }

  // Parameter 1 is the motor state: 0 stop, 1 rumble, 2 hard stop.
  const int local_pad = NetPlay_InGamePadToLocalPad(m_device_number);
  if (local_pad < 4)
  {
    const SIDevices local_type = m_system.GetSerialInterface().GetDeviceType(local_pad);
    Rumble(local_pad, word.parameter1 == 1 ? 1.0 : 0.0, local_type);
  }

  // Parameter 2 selects the analog mode, but only on a write without polling.
  if (poll == 0)
  {
    m_mode = static_cast<u8>(word.parameter2);
    INFO_LOG_FMT(SERIALINTERFACE, "Pad {}: analog mode {}", m_device_number, m_mode);
  }
}

void CSIDevice_GCController::DoState(PointerWrap& p)
{
  p.Do(m_origin);
  p.Do(m_mode);
  p.Do(m_timer_button_combo_start);
  p.Do(m_last_button_combo);
}

CSIDevice_TaruKonga::CSIDevice_TaruKonga(Core::System& system, SIDevices device, int device_number)
    : CSIDevice_GCController(system, device, device_number)
{
}

DataResponse CSIDevice_TaruKonga::GetData(u32& hi, u32& low)
{
  // The bongos report no sticks and only a few buttons. They always claim
  // success, even when the underlying pad is disconnected.
  CSIDevice_GCController::GetData(hi, low);
  hi &= HI_BUTTON_MASK;
  return DataResponse::Success;
}

}  // namespace SerialInterface
