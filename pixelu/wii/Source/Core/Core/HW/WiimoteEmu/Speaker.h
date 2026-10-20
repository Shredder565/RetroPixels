// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/HW/WiimoteEmu/I2CBus.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

// The remote's speaker: an i2c device (slave 0x51). Sound arrives as speaker data reports, which
// the remote writes to register 0x00. The samples are 4-bit Yamaha ADPCM or 8-bit signed PCM
// (WiiBrew: "Wiimote", Speaker).

namespace WiimoteEmu
{
// Decoder state of the Yamaha ADPCM stream. Saved in savestates.
struct ADPCMState
{
  s32 predictor, step;
};

class Wiimote;

class SpeakerLogic : public I2CSlave
{
  friend class Wiimote;

public:
  static const u8 I2C_ADDR = 0x51;

  static constexpr u8 SPEAKER_DATA_OFFSET = 0x00;

  void Reset();
  void DoState(PointerWrap& p);

  void SetSpeakerEnabled(bool enabled);
  void SetWiimoteIndex(u8 index) { m_wiimote_index = index; }

private:
  // Values of the format register (0x02).
  static const u8 DATA_FORMAT_ADPCM = 0x00;
  static const u8 DATA_FORMAT_PCM = 0x40;

#pragma pack(push, 1)
  struct Register
  {
    // 0x00: sample data is written here (and played, not stored).
    u8 speaker_data;
    u8 unk_1;
    // 0x02
    u8 format;
    // 0x03: little endian. Seems not to change the ADPCM playback rate on hardware.
    u16 sample_rate;
    // 0x05
    u8 volume;
    u8 unk_5;
    u8 unk_6;
    // Real hardware returns 0x09 when this is read.
    u8 unk_7;
    u8 unk_8;
    u8 unknown[0xf6];
  };
#pragma pack(pop)

  static_assert(0x100 == sizeof(Register));

  // Decodes one data write and hands the samples to the mixer. speaker_pan is -1 to +1.
  void SpeakerData(const u8* data, int length, float speaker_pan);

  int BusRead(u8 slave_addr, u8 addr, int count, u8* data_out) override;
  int BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in) override;

  Register reg_data{};

  ADPCMState adpcm_state{};

  // Owned here, shown in the remote's Options group.
  ControllerEmu::SettingValue<double> m_speaker_pan_setting;

  bool m_speaker_enabled = false;
  u8 m_wiimote_index = 0;
};

}  // namespace WiimoteEmu
