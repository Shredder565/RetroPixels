// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/Speaker.h"

#include <algorithm>
#include <array>

#include "AudioCommon/AudioCommon.h"

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/ConfigManager.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/System.h"

#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"

namespace WiimoteEmu
{
namespace
{
// Yamaha ADPCM (the 4-bit format also used by the Dreamcast AICA): each nibble is a sign bit
// and a 3 bit magnitude m. The sample moves by (2m + 1) / 8 of the current step, and the step
// is then scaled by a factor that grows with m.
constexpr std::array<s32, 8> STEP_SCALE = {230, 230, 230, 230, 307, 409, 512, 614};

constexpr s32 MIN_STEP = 127;
constexpr s32 MAX_STEP = 24576;

s16 DecodeNibble(ADPCMState& state, u8 nibble)
{
  const s32 magnitude = nibble & 7;
  const bool negative = (nibble & 8) != 0;

  // Divide before applying the sign so the result truncates towards zero either way.
  const s32 delta = state.step * (2 * magnitude + 1) / 8;
  state.predictor = std::clamp(state.predictor + (negative ? -delta : delta), -32768, 32767);

  const s32 scale = STEP_SCALE[static_cast<size_t>(magnitude)];
  state.step = std::clamp((state.step * scale) >> 8, MIN_STEP, MAX_STEP);

  return static_cast<s16>(state.predictor);
}

// Format parameters, from WiiBrew's speaker documentation.
struct FormatInfo
{
  u32 rate_dividend;
  u8 full_volume;
  u32 samples_per_byte;
};

constexpr FormatInfo PCM_FORMAT = {12000000, 0xff, 1};
constexpr FormatInfo ADPCM_FORMAT = {6000000, 0x7f, 2};
}  // namespace

void SpeakerLogic::SpeakerData(const u8* data, int length, float speaker_pan)
{
  if (!m_speaker_enabled)
    return;

  if (reg_data.sample_rate == 0 || length == 0)
    return;

  // A report holds at most 20 bytes, so at most 40 samples. Samples are decoded even at zero
  // volume so the ADPCM state stays right.
  std::array<s16, WiimoteCommon::OutputReportSpeakerData::DATA_SIZE * 2> samples;
  const auto byte_count = static_cast<size_t>(length);

  const FormatInfo* format = nullptr;
  if (reg_data.format == DATA_FORMAT_PCM)
  {
    format = &PCM_FORMAT;
    for (size_t i = 0; i != byte_count; ++i)
      samples[i] = static_cast<s16>(static_cast<s8>(data[i]) * 0x100);
  }
  else if (reg_data.format == DATA_FORMAT_ADPCM)
  {
    format = &ADPCM_FORMAT;
    for (size_t i = 0; i != byte_count; ++i)
    {
      // High nibble first.
      samples[i * 2] = DecodeNibble(adpcm_state, static_cast<u8>(data[i] >> 4));
      samples[i * 2 + 1] = DecodeNibble(adpcm_state, static_cast<u8>(data[i] & 0xf));
    }
  }
  else
  {
    ERROR_LOG_FMT(IOS_WIIMOTE, "Unknown speaker format {:x}", reg_data.format);
    return;
  }

  u8 full_volume = format->full_volume;
  if (reg_data.volume > full_volume)
  {
    DEBUG_LOG_FMT(IOS_WIIMOTE, "Speaker volume {} is above the expected maximum", reg_data.volume);
    full_volume = reg_data.volume;
  }

  // The mixer takes 0-255 per channel: scale by 256, truncate and cap at 255.
  const double volume = float(reg_data.volume) * 256.f / full_volume;

  // Panning only ever lowers one side, so a centered remote plays at its own volume.
  const float pan = std::clamp(speaker_pan, -1.f, 1.f);
  const u32 left = std::min(u32(std::min(1.f - pan, 1.f) * volume), 255u);
  const u32 right = std::min(u32(std::min(1.f + pan, 1.f) * volume), 255u);

  Mixer* const mixer = Core::System::GetInstance().GetSoundStream()->GetMixer();
  mixer->SetWiimoteSpeakerVolume(m_wiimote_index, left, right);

  // The ADPCM rate appears to be twice the nominal one (3000 Hz data plays at 6000 Hz).
  const u32 sample_rate = format->rate_dividend / reg_data.sample_rate;
  const auto divisor = static_cast<u32>(Mixer::FIXED_SAMPLE_RATE_DIVIDEND / (sample_rate * 2));
  mixer->PushWiimoteSpeakerSamples(m_wiimote_index, samples.data(),
                                   byte_count * format->samples_per_byte, divisor);
}

void SpeakerLogic::Reset()
{
  reg_data = {};

  adpcm_state.predictor = 0;
  adpcm_state.step = MIN_STEP;
}

void SpeakerLogic::DoState(PointerWrap& p)
{
  p.Do(adpcm_state);
  p.Do(reg_data);
}

void SpeakerLogic::SetSpeakerEnabled(bool enabled)
{
  m_speaker_enabled = enabled;
}

int SpeakerLogic::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  if (slave_addr != I2C_ADDR)
    return 0;
  return RawRead(&reg_data, addr, count, data_out);
}

int SpeakerLogic::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  if (slave_addr != I2C_ADDR)
    return 0;

  // Writes to the data register are played, not stored.
  if (addr == SPEAKER_DATA_OFFSET)
  {
    SpeakerData(data_in, count, static_cast<float>(m_speaker_pan_setting.GetValue() / 100));
    return count;
  }

  return RawWrite(&reg_data, addr, count, data_in);
}

}  // namespace WiimoteEmu
