// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Parameter block (PB) layouts of the AX ucodes, as the game's AX library lays them out in RAM
// (all fields are big-endian 16-bit words there). The HLE ucodes copy a PB into one of these
// structs, byte-swapping each word, and write it back after processing.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

namespace DSP::HLE
{
// A volume and its per-sample change.
struct VolumeData
{
  u16 volume;
  u16 volume_delta;
};

// GameCube voice send levels. The surround sends come in the order auxB, main, auxA in RAM.
struct PBMixer
{
  VolumeData main_left;
  VolumeData main_right;
  VolumeData auxA_left;
  VolumeData auxA_right;
  VolumeData auxB_left;
  VolumeData auxB_right;
  VolumeData auxB_surround;
  VolumeData main_surround;
  VolumeData auxA_surround;
};

// Wii voice send levels, with a third aux bus.
struct PBMixerWii
{
  VolumeData main_left;
  VolumeData main_right;
  VolumeData auxA_left;
  VolumeData auxA_right;
  VolumeData auxB_left;
  VolumeData auxB_right;
  VolumeData auxC_left;
  VolumeData auxC_right;
  VolumeData main_surround;
  VolumeData auxA_surround;
  VolumeData auxB_surround;
  VolumeData auxC_surround;
};

// Send levels to the four Wii Remote speakers (main and aux each).
struct PBMixerWM
{
  VolumeData main0;
  VolumeData aux0;
  VolumeData main1;
  VolumeData aux1;
  VolumeData main2;
  VolumeData aux2;
  VolumeData main3;
  VolumeData aux3;
};

// Interaural time delay (not emulated).
struct PBInitialTimeDelay
{
  u16 on;
  u16 addrMemHigh;
  u16 addrMemLow;
  u16 offsetLeft;
  u16 offsetRight;
  u16 targetLeft;
  u16 targetRight;
};

// One mid-frame PB change: write `new_value` to word `pb_offset` of the PB.
struct PBUpdate
{
  u16 pb_offset;
  u16 new_value;
};
using PBUpdateData = std::array<PBUpdate, 32>;

// Mid-frame changes per millisecond (5 per GameCube frame), from a list in RAM. Games use them
// for sample-accurate note timing.
struct PBUpdates
{
  u16 num_updates[5];
  u16 data_hi;
  u16 data_lo;
};

// The Wii version, for 3 ms frames (only some AXWii builds have it).
struct PBUpdatesWii
{
  u16 num_updates[3];
  u16 data_hi;
  u16 data_lo;
};

// Last sample sent to each bus in the frame. The AX library uses these to fade out voices it
// stops ("depop").
struct PBDpop
{
  s16 main_left;
  s16 auxA_left;
  s16 auxB_left;
  s16 main_right;
  s16 auxA_right;
  s16 auxB_right;
  s16 main_surround;
  s16 auxA_surround;
  s16 auxB_surround;
};

struct PBDpopWii
{
  s16 main_left;
  s16 auxA_left;
  s16 auxB_left;
  s16 auxC_left;
  s16 main_right;
  s16 auxA_right;
  s16 auxB_right;
  s16 auxC_right;
  s16 main_surround;
  s16 auxA_surround;
  s16 auxB_surround;
  s16 auxC_surround;
};

struct PBDpopWM
{
  s16 main0;
  s16 main1;
  s16 main2;
  s16 main3;
  s16 aux0;
  s16 aux1;
  s16 aux2;
  s16 aux3;
};

// Voice volume: the value at the start of the frame and its per-sample change.
struct PBVolumeEnvelope
{
  s16 cur_volume;
  s16 cur_volume_delta;
};

struct PBUnknown2
{
  u16 unknown_reserved[3];
};

// Sample addresses for the accelerator. One-shot voices point the loop at a shared silent
// buffer; the end address is inclusive.
struct PBAudioAddr
{
  u16 looping;
  u16 sample_format;
  u16 loop_addr_hi;
  u16 loop_addr_lo;
  u16 end_addr_hi;
  u16 end_addr_lo;
  u16 cur_addr_hi;
  u16 cur_addr_lo;
};

struct PBADPCMInfo
{
  s16 coefs[16];
  u16 gain;
  u16 pred_scale;
  s16 yn1;
  s16 yn2;
};

// Sample rate conversion: ratio is input samples per output sample in 16.16 fixed point (1/512
// to 4.0), cur_addr_frac the fractional input position, last_samples the input history.
struct PBSampleRateConverter
{
  u16 ratio_hi;
  u16 ratio_lo;
  u16 cur_addr_frac;
  s16 last_samples[4];
};

struct PBSampleRateConverterWM
{
  u16 cur_addr_frac;
  s16 last_samples[4];
};

// ADPCM state to restore when a voice loops.
struct PBADPCMLoopInfo
{
  u16 pred_scale;
  u16 yn1;
  u16 yn2;
};

// One-pole low-pass filter: y = (a0 * x + b0 * y[-1]) >> 15.
struct PBLowPassFilter
{
  u16 on;
  s16 yn1;
  u16 a0;
  s16 b0;
};

// GameCube PB. The oldest AX build (CRC 0x4e8a8b21) has no lpf field in RAM.
struct AXPB
{
  u16 next_pb_hi;
  u16 next_pb_lo;
  u16 this_pb_hi;
  u16 this_pb_lo;

  u16 src_type;  // see SRCTYPE_*
  u16 coef_select;
  u16 mixer_control;

  u16 running;    // 1 = playing
  u16 is_stream;  // 1 = streaming (looping ring buffer)

  PBMixer mixer;
  PBInitialTimeDelay initial_time_delay;
  PBUpdates updates;
  PBDpop dpop;
  PBVolumeEnvelope vol_env;
  PBUnknown2 unknown3;
  PBAudioAddr audio_addr;
  PBADPCMInfo adpcm;
  PBSampleRateConverter src;
  PBADPCMLoopInfo adpcm_loop_info;
  PBLowPassFilter lpf;
  u16 loop_counter;

  u16 padding[24];
};

struct PBHighPassFilter
{
  u16 on;
  u16 unk[3];
};

// y = (b0 x + b1 x[-1] + b2 x[-2] + a1 y[-1] + a2 y[-2]), coefficients in 2.14.
struct PBBiquadFilter
{
  u16 on;
  s16 xn1;
  s16 xn2;
  s16 yn1;
  s16 yn2;
  s16 b0;
  s16 b1;
  s16 b2;
  s16 a1;
  s16 a2;
};

// Wii Remote filter: off (0), biquad (2) or low-pass (anything else).
union PBInfImpulseResponseWM
{
  u16 on;
  PBLowPassFilter lpf;
  PBBiquadFilter biquad;
};

// Wii PB, in its largest form. Some builds lack the updates field, and older ones have a
// high-pass filter where newer ones have a (larger) biquad filter; see AXWiiUCode::ReadPB.
struct AXPBWii
{
  u16 next_pb_hi;
  u16 next_pb_lo;
  u16 this_pb_hi;
  u16 this_pb_lo;

  u16 src_type;     // see SRCTYPE_*
  u16 coef_select;  // which polyphase coefficient table
  u16 mixer_control_hi;
  u16 mixer_control_lo;

  u16 running;    // 1 = playing
  u16 is_stream;  // 1 = streaming

  PBMixerWii mixer;
  PBInitialTimeDelay initial_time_delay;
  PBDpopWii dpop;
  PBUpdatesWii updates;
  PBVolumeEnvelope vol_env;
  PBAudioAddr audio_addr;
  PBADPCMInfo adpcm;
  PBSampleRateConverter src;
  PBADPCMLoopInfo adpcm_loop_info;
  PBLowPassFilter lpf;
  union
  {
    PBHighPassFilter hpf;
    PBBiquadFilter biquad;
  };

  // Wii Remote speaker output.
  u16 remote;
  u16 remote_mixer_control;
  PBMixerWM remote_mixer;
  PBDpopWM remote_dpop;
  PBSampleRateConverterWM remote_src;
  PBInfImpulseResponseWM remote_iir;

  u16 pad[2];
};

// Accelerator sample formats.
enum
{
  AUDIOFORMAT_ADPCM = 0,
  AUDIOFORMAT_PCM8 = 0x19,
  AUDIOFORMAT_PCM16 = 0xA,
};

// Sample rate converters.
enum
{
  SRCTYPE_POLYPHASE = 0,
  SRCTYPE_LINEAR = 1,
  SRCTYPE_NEAREST = 2,
};

// Voice filters (may be combined).
enum
{
  FILTER_LOWPASS = 1,
  FILTER_BIQUAD = 2,
};

static_assert(sizeof(AXPB) == 244);
static_assert(sizeof(AXPBWii) == 310);
}  // namespace DSP::HLE
