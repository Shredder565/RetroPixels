// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "InputCommon/GCPadStatus.h"

namespace Movie
{
class MovieManager;
}

namespace SerialInterface
{
// The standard GameCube controller. Several other devices (adapter ports,
// bongos, dance mat, wheel) reuse its command handling.
class CSIDevice_GCController : public ISIDevice
{
protected:
  // Reply to the origin/recalibrate commands. It is sent and saved as raw
  // bytes, so the layout must not change.
  struct SOrigin
  {
    u16 button;
    u8 origin_stick_x;
    u8 origin_stick_y;
    u8 substick_x;
    u8 substick_y;
    u8 trigger_left;
    u8 trigger_right;
    u8 unk_4;
    u8 unk_5;
  };

  enum EButtonCombo
  {
    COMBO_NONE = 0,
    COMBO_ORIGIN,
    COMBO_RESET
  };

  // Neutral position the game calibrates against. It starts perfectly
  // centred (a real pad samples its sticks at power-on instead); holding
  // X+Y+Start re-samples it.
  SOrigin m_origin = {};

  // Analog mode chosen by the game (how the low input word is packed).
  u8 m_mode = 0x3;

  // When the current button combo started being held, and which one it is.
  // X+Y+Start held for 3 s resets the origin, B+X+Start presses Reset.
  u64 m_timer_button_combo_start = 0;
  EButtonCombo m_last_button_combo = COMBO_NONE;

public:
  CSIDevice_GCController(Core::System& system, SIDevices device, int device_number);

  int RunBuffer(u8* buffer, int request_length) override;

  DataResponse GetData(u32& hi, u32& low) override;

  void SendCommand(u32 command, u8 poll) override;

  void DoState(PointerWrap& p) override;

  virtual GCPadStatus GetPadStatus();
  virtual u32 MapPadStatus(const GCPadStatus& pad_status);
  virtual EButtonCombo HandleButtonCombos(const GCPadStatus& pad_status);

  // NetPlay input (implemented by the NetPlay client).
  static bool NetPlay_GetInput(int pad_num, GCPadStatus* status);
  static int NetPlay_InGamePadToLocalPad(int pad_num);

  // Sends rumble to the local device behind a pad number.
  static void Rumble(int pad_num, ControlState strength, SIDevices device);

  // Routes a polled pad state through NetPlay and movie playback/recording.
  static void HandleMoviePadStatus(Movie::MovieManager& movie, int device_number,
                                   GCPadStatus* pad_status);

protected:
  void SetOrigin(const GCPadStatus& pad_status);
};

// The DK Bongos ("TaruKonga"): a controller that only reports some buttons.
class CSIDevice_TaruKonga final : public CSIDevice_GCController
{
public:
  CSIDevice_TaruKonga(Core::System& system, SIDevices device, int device_number);

  DataResponse GetData(u32& hi, u32& low) override;

  // Buttons kept in the high input word: A, B, X, Y, Start and R.
  static const u32 HI_BUTTON_MASK =
      (PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y | PAD_BUTTON_START | PAD_TRIGGER_R)
      << 16;
};
}  // namespace SerialInterface
