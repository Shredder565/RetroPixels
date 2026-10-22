// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Voice rendering shared by the GameCube and Wii AX ucodes: fetching samples through the
// accelerator, sample rate conversion, filters, volume and mixing to the buses. The code is
// templated on the parameter block type (AXPB or AXPBWii).

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

#include "Common/CommonTypes.h"
#include "Core/DSP/DSPAccelerator.h"
#include "Core/DolphinAnalytics.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/UCodes/AX.h"
#include "Core/HW/DSPHLE/UCodes/AXStructs.h"
#include "Core/HW/Memmap.h"

namespace DSP::HLE::AXVoice
{
template <typename PB>
inline constexpr bool IS_WII_PB = std::is_same_v<PB, AXPBWii>;

constexpr u32 Join16(u16 hi, u16 lo)
{
  return (u32{hi} << 16) | lo;
}

constexpr s16 Clamp16(s64 value)
{
  return static_cast<s16>(std::clamp<s64>(value, -0x8000, 0x7FFF));
}

// Adds with two's complement wrap-around, as the 32-bit mixing buffers of the ucode do.
constexpr int AddWrapped(int a, s64 b)
{
  return static_cast<int>(static_cast<u32>(a) + static_cast<u32>(b));
}

// ---- Mid-frame PB updates ----------------------------------------------------------------------

template <typename PB>
PBUpdateData FetchUpdateList(const Memory::MemoryManager& memory, const PB& pb)
{
  PBUpdateData list{};
  memory.CopyFromEmuSwapped(reinterpret_cast<u16*>(list.data()),
                            Join16(pb.updates.data_hi, pb.updates.data_lo), sizeof(list));
  return list;
}

// Applies the updates scheduled for millisecond `ms` of the frame. The list holds the updates of
// all milliseconds back to back; a millisecond whose entries would not fit is skipped.
template <typename PB>
void ApplyUpdatesForMillisecond(PB& pb, u32 ms, const PBUpdateData& list)
{
  u32 first = 0;
  for (u32 i = 0; i < ms; ++i)
    first += pb.updates.num_updates[i];
  if (first >= list.size())
    return;

  const u32 count = pb.updates.num_updates[ms];
  if (count > list.size() - first)
    return;

  u8* const pb_bytes = reinterpret_cast<u8*>(&pb);
  for (u32 i = first; i < first + count; ++i)
  {
    const PBUpdate& update = list[i];
    if (update.pb_offset < sizeof(PB) / sizeof(u16))
      std::memcpy(pb_bytes + update.pb_offset * sizeof(u16), &update.new_value, sizeof(u16));
  }
}

// ---- Accelerator -----------------------------------------------------------------------------

// The accelerator as AX drives it for one voice. Its end-of-sample exception does what the AX
// exception handler does: loop (restoring the loop's ADPCM state, or for streams keeping the
// current one) or stop the voice.
template <typename PB>
class VoiceAccelerator final : public Accelerator
{
public:
  explicit VoiceAccelerator(DSPManager& dsp) : m_dsp(dsp) {}
  VoiceAccelerator(const VoiceAccelerator&) = delete;
  VoiceAccelerator(VoiceAccelerator&&) = delete;
  VoiceAccelerator& operator=(const VoiceAccelerator&) = delete;
  VoiceAccelerator& operator=(VoiceAccelerator&&) = delete;
  ~VoiceAccelerator() override = default;

  // Loads the accelerator registers from the voice.
  void Attach(PB& pb)
  {
    m_pb = &pb;
    const PBAudioAddr& addr = pb.audio_addr;
    SetStartAddress(Join16(addr.loop_addr_hi, addr.loop_addr_lo));
    SetEndAddress(Join16(addr.end_addr_hi, addr.end_addr_lo));
    SetCurrentAddress(Join16(addr.cur_addr_hi, addr.cur_addr_lo));
    SetSampleFormat(addr.sample_format);
    SetYn1(pb.adpcm.yn1);
    SetYn2(pb.adpcm.yn2);
    SetGain(static_cast<s16>(pb.adpcm.gain));
    SetPredScale(pb.adpcm.pred_scale);
  }

  // Stores the accelerator registers that change while reading back into the voice.
  void Detach()
  {
    const u32 current = GetCurrentAddress();
    m_pb->audio_addr.cur_addr_hi = static_cast<u16>(current >> 16);
    m_pb->audio_addr.cur_addr_lo = static_cast<u16>(current);
    m_pb->adpcm.yn1 = GetYn1();
    m_pb->adpcm.yn2 = GetYn2();
    m_pb->adpcm.pred_scale = GetPredScale();
  }

  s16 NextSample() { return static_cast<s16>(ReadSample(m_pb->adpcm.coefs)); }

protected:
  void OnRawReadEndException() override {}
  void OnRawWriteEndException() override {}

  void OnSampleReadEndException() override
  {
    PB& pb = *m_pb;
    if (!pb.audio_addr.looping)
    {
      pb.running = 0;
      return;
    }

    SetPredScale(pb.adpcm_loop_info.pred_scale);
    if (pb.is_stream == 1)
    {
      // Streams keep their history; rewriting it lets the accelerator read on.
      SetYn1(GetYn1());
      SetYn2(GetYn2());
      if constexpr (!IS_WII_PB<PB>)
        ++pb.loop_counter;
    }
    else
    {
      SetYn1(static_cast<s16>(pb.adpcm_loop_info.yn1));
      SetYn2(static_cast<s16>(pb.adpcm_loop_info.yn2));
    }
  }

  u8 ReadMemory(u32 address) override { return m_dsp.ReadARAM(address); }
  void WriteMemory(u32 address, u8 value) override { m_dsp.WriteARAM(value, address); }

private:
  DSPManager& m_dsp;
  PB* m_pb = nullptr;
};

// ---- Sample rate conversion ------------------------------------------------------------------

// Produces `count` output samples from the input `next_input()` provides, stepping `ratio` (16.16
// input samples per output sample) from position `pos` (16.16, only the fraction matters).
// `history` holds the last four input samples, oldest first, and is updated. Returns the new
// position.
//
// Polyphase conversion needs the coefficient table (from the DSP ROM); without it, it falls
// back to linear interpolation. Note that both interpolate between the oldest samples of the
// history, so the output lags the input by a few samples, like the ucode's does.
template <typename Source>
u32 ConvertSampleRate(Source&& next_input, s16* out, u32 count, s16* history, u32 pos, u32 ratio,
                      int src_type, const s16* coef_table)
{
  const bool polyphase = coef_table != nullptr && src_type == SRCTYPE_POLYPHASE;
  const bool linear = !polyphase && (src_type == SRCTYPE_LINEAR || src_type == SRCTYPE_POLYPHASE);

  if (!polyphase && !linear)
  {
    // No conversion: one input sample per output sample.
    for (u32 i = 0; i < count; ++i)
      out[i] = next_input();
    std::memcpy(history, out + count - 4, 4 * sizeof(s16));
    return pos;
  }

  std::array<s16, 4> h{history[0], history[1], history[2], history[3]};
  for (u32 i = 0; i < count; ++i)
  {
    pos += ratio;
    for (; pos >= 0x10000; pos -= 0x10000)
      h = {h[1], h[2], h[3], next_input()};

    const u32 frac = pos & 0xFFFF;
    if (polyphase)
    {
      // 128 phases of 4 taps, chosen by the top 7 bits of the fraction.
      const s16* taps = coef_table + (frac >> 9) * 4;
      const s64 sum = s64{h[0]} * taps[0] + s64{h[1]} * taps[1] + s64{h[2]} * taps[2] +
                      s64{h[3]} * taps[3];
      out[i] = Clamp16(sum >> 15);
    }
    else if (frac != 0)
    {
      out[i] = static_cast<s16>((s32{h[0]} * static_cast<s32>(0x10000 - frac) +
                                 s32{h[1]} * static_cast<s32>(frac)) >>
                                16);
    }
    else
    {
      out[i] = h[0];
    }
  }

  std::copy(h.begin(), h.end(), history);
  return pos;
}

// ---- Filters -----------------------------------------------------------------------------------

inline void ApplyLowPass(s16* samples, u32 count, PBLowPassFilter& f)
{
  for (u32 i = 0; i < count; ++i)
  {
    const s32 sum = AddWrapped(f.a0 * samples[i], f.b0 * f.yn1);
    const s16 y = Clamp16(sum >> 15);
    samples[i] = y;
    f.yn1 = y;
  }
}

inline void ApplyBiquad(s16* samples, u32 count, PBBiquadFilter& f)
{
  for (u32 i = 0; i < count; ++i)
  {
    const s16 x = samples[i];
    s64 sum = s64{f.b0 * x} + s64{f.b1 * f.xn1} + s64{f.b2 * f.xn2} + s64{f.a1 * f.yn1} +
              s64{f.a2 * f.yn2};
    // 2.14 coefficients; the ucode rounds with its "round to even-ish" accumulator mode.
    sum *= 4;
    sum += (sum & 0x10000) != 0 ? 0x8000 : 0x7FFF;
    const s16 y = Clamp16(sum >> 16);

    f.xn2 = f.xn1;
    f.xn1 = x;
    f.yn2 = f.yn1;
    f.yn1 = y;
    samples[i] = y;
  }
}

// ---- Mixing ------------------------------------------------------------------------------------

// Adds the samples, scaled by the send level (1.15), to a bus, ramping the level when asked.
// The last value sent is kept for depopping.
inline void MixToBus(int* bus, const s16* samples, u32 count, VolumeData& level, s16& last_sent,
                     bool ramp)
{
  const u16 step = ramp ? level.volume_delta : u16{0};
  for (u32 i = 0; i < count; ++i)
  {
    const s16 scaled = Clamp16((s64{samples[i]} * level.volume) >> 15);
    bus[i] = AddWrapped(bus[i], scaled);
    level.volume = static_cast<u16>(level.volume + step);
    last_sent = scaled;
  }
}

// The voice's sends, in AXMixControl order (main L/R/S, auxA L/R/S, auxB L/R/S[, auxC L/R/S]).
template <typename PB>
struct Sends;

template <>
struct Sends<AXPB>
{
  static constexpr std::array<VolumeData PBMixer::*, 9> level{
      &PBMixer::main_left,  &PBMixer::main_right,    &PBMixer::main_surround,
      &PBMixer::auxA_left,  &PBMixer::auxA_right,    &PBMixer::auxA_surround,
      &PBMixer::auxB_left,  &PBMixer::auxB_right,    &PBMixer::auxB_surround,
  };
  static constexpr std::array<s16 PBDpop::*, 9> last{
      &PBDpop::main_left, &PBDpop::main_right, &PBDpop::main_surround,
      &PBDpop::auxA_left, &PBDpop::auxA_right, &PBDpop::auxA_surround,
      &PBDpop::auxB_left, &PBDpop::auxB_right, &PBDpop::auxB_surround,
  };
};

template <>
struct Sends<AXPBWii>
{
  static constexpr std::array<VolumeData PBMixerWii::*, 12> level{
      &PBMixerWii::main_left, &PBMixerWii::main_right, &PBMixerWii::main_surround,
      &PBMixerWii::auxA_left, &PBMixerWii::auxA_right, &PBMixerWii::auxA_surround,
      &PBMixerWii::auxB_left, &PBMixerWii::auxB_right, &PBMixerWii::auxB_surround,
      &PBMixerWii::auxC_left, &PBMixerWii::auxC_right, &PBMixerWii::auxC_surround,
  };
  static constexpr std::array<s16 PBDpopWii::*, 12> last{
      &PBDpopWii::main_left, &PBDpopWii::main_right, &PBDpopWii::main_surround,
      &PBDpopWii::auxA_left, &PBDpopWii::auxA_right, &PBDpopWii::auxA_surround,
      &PBDpopWii::auxB_left, &PBDpopWii::auxB_right, &PBDpopWii::auxB_surround,
      &PBDpopWii::auxC_left, &PBDpopWii::auxC_right, &PBDpopWii::auxC_surround,
  };
  // Wii Remote sends: main0, aux0, main1, aux1, ..., matching the remote mixer control bit pairs.
  static constexpr std::array<VolumeData PBMixerWM::*, 8> remote_level{
      &PBMixerWM::main0, &PBMixerWM::aux0, &PBMixerWM::main1, &PBMixerWM::aux1,
      &PBMixerWM::main2, &PBMixerWM::aux2, &PBMixerWM::main3, &PBMixerWM::aux3,
  };
  static constexpr std::array<s16 PBDpopWM::*, 8> remote_last{
      &PBDpopWM::main0, &PBDpopWM::aux0, &PBDpopWM::main1, &PBDpopWM::aux1,
      &PBDpopWM::main2, &PBDpopWM::aux2, &PBDpopWM::main3, &PBDpopWM::aux3,
  };
};

// Where a voice mixes to: one pointer per send (see Sends), then for Wii PBs the eight Wii
// Remote buses.
template <typename PB>
struct VoiceTargets
{
  std::array<int*, Sends<PB>::level.size()> buses{};
  std::array<int*, IS_WII_PB<PB> ? 8 : 0> remote{};
};

// The largest number of samples one voice call renders (a 3 ms Wii frame).
constexpr u32 MAX_VOICE_SAMPLES = 96;
// The Wii Remote speaker runs at 6 kHz: 18 samples per 3 ms frame, 6 per millisecond.
constexpr u32 MAX_REMOTE_SAMPLES = 18;

// Renders `count` samples of a voice and mixes them as its mixer control says. `coef_tables` is
// the polyphase table set, or nullptr. `new_filters` enables the biquad and Wii Remote filters
// of late AXWii builds.
template <typename PB>
void RenderVoice(VoiceAccelerator<PB>& accelerator, PB& pb, const VoiceTargets<PB>& targets,
                 u32 count, u32 mix_control, const s16* coef_tables, bool new_filters)
{
  if (pb.running != 1)
    return;

  // Input samples, through the sample rate converter.
  std::array<s16, MAX_VOICE_SAMPLES> samples;
  accelerator.Attach(pb);
  const s16* voice_table = coef_tables != nullptr ? coef_tables + pb.coef_select * 0x200 : nullptr;
  const u32 pos =
      ConvertSampleRate([&accelerator] { return accelerator.NextSample(); }, samples.data(), count,
                        pb.src.last_samples, pb.src.cur_addr_frac,
                        Join16(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type, voice_table);
  pb.src.cur_addr_frac = static_cast<u16>(pos);
  accelerator.Detach();

  // Voice volume envelope (1.15; read as unsigned on the Wii).
  for (u32 i = 0; i < count; ++i)
  {
    const s32 volume = IS_WII_PB<PB> ? s32{static_cast<u16>(pb.vol_env.cur_volume)} :
                                       s32{pb.vol_env.cur_volume};
    samples[i] = Clamp16((s32{samples[i]} * volume) >> 15);
    pb.vol_env.cur_volume = static_cast<s16>(pb.vol_env.cur_volume + pb.vol_env.cur_volume_delta);
  }

  if (pb.lpf.on != 0)
    ApplyLowPass(samples.data(), count, pb.lpf);
  if constexpr (IS_WII_PB<PB>)
  {
    if (new_filters && pb.biquad.on != 0)
      ApplyBiquad(samples.data(), count, pb.biquad);
  }

  // Each send has an enable bit and a ramp bit, two bits per send.
  for (size_t send = 0; send < Sends<PB>::level.size(); ++send)
  {
    const u32 bits = mix_control >> (2 * send);
    if ((bits & 1) == 0)
      continue;
    MixToBus(targets.buses[send], samples.data(), count, pb.mixer.*Sends<PB>::level[send],
             pb.dpop.*Sends<PB>::last[send], (bits & 2) != 0);
  }

  if (pb.initial_time_delay.on != 0)
  {
    // Not emulated.
    DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesAXInitialTimeDelay);
  }

  if constexpr (IS_WII_PB<PB>)
  {
    if (pb.remote == 0)
      return;

    if (new_filters && pb.remote_iir.on != 0)
    {
      // At most one filter for the remote.
      if (pb.remote_iir.on == 2)
      {
        DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesAXWiimoteBiquad);
        ApplyBiquad(samples.data(), count, pb.remote_iir.biquad);
      }
      else
      {
        DolphinAnalytics::Instance().ReportGameQuirk(GameQuirk::UsesAXWiimoteLowPass);
        ApplyLowPass(samples.data(), count, pb.remote_iir.lpf);
      }
    }

    // Down to 6 kHz with polyphase conversion at 0x55555 (5.333, as close to 96/18 as 16.16
    // gets). Old AXWii builds render a millisecond at a time.
    const u32 remote_count = count == MAX_VOICE_SAMPLES ? MAX_REMOTE_SAMPLES : 6;
    std::array<s16, MAX_REMOTE_SAMPLES> remote_samples;
    u32 cursor = 0;
    const u32 remote_pos = ConvertSampleRate(
        [&samples, &cursor] { return samples[cursor++]; }, remote_samples.data(), remote_count,
        pb.remote_src.last_samples, pb.remote_src.cur_addr_frac, 0x55555, SRCTYPE_POLYPHASE,
        coef_tables);
    pb.remote_src.cur_addr_frac = static_cast<u16>(remote_pos);

    for (size_t send = 0; send < Sends<PB>::remote_level.size(); ++send)
    {
      const u32 bits = (u32{pb.remote_mixer_control} >> (2 * send)) & 3;
      if (bits == 0)
        continue;
      MixToBus(targets.remote[send], remote_samples.data(), remote_count,
               pb.remote_mixer.*Sends<PB>::remote_level[send],
               pb.remote_dpop.*Sends<PB>::remote_last[send], (bits & 2) != 0);
    }
  }
}
}  // namespace DSP::HLE::AXVoice
