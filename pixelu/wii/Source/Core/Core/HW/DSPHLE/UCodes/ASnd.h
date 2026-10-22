// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP::HLE
{
class DSPHLE;

// The libasnd mixer ucode from libogc (written by Hermes, BSD-3-Clause), used by older homebrew.
// The CPU hands it one voice at a time; the ucode mixes 1024 stereo samples of that voice into
// its internal 48 kHz output buffer, which the CPU then asks to have copied out.
class ASndUCode final : public UCodeInterface
{
public:
  ASndUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

  // Known builds (size of the image in parentheses).
  // libogc, November 2008, first release (0x5a0, padded).
  static constexpr u32 HASH_2008 = 0x8d69a19b;
  // libogc, February 2009: adds MAIL_TERMINATE (0x5c0, padded).
  static constexpr u32 HASH_2009 = 0xcc2fd441;
  // libogc, June 2011: more sample formats, so the flag bits moved (0x620, padded). The
  // committed binary differs a little from the committed source, without changing behaviour.
  static constexpr u32 HASH_2011 = 0xa81582e2;
  // libogc, June 2020: built from source at library build time (0x606); behaves like 2011.
  static constexpr u32 HASH_2020 = 0xdbbeeb61;
  // The same code padded to 0x620, as shipped by libogc2 and libogc-rice from December 2020.
  static constexpr u32 HASH_2020_PAD = 0xbad876ef;
  // Desert Bus 1.04: the 2011 code with the input channels in left-then-right order (0x620).
  static constexpr u32 HASH_DESERT_BUS_2011 = 0xfa9c576f;
  // Desert Bus 1.05: as 1.04 plus four do-nothing instructions at the start (0x620).
  static constexpr u32 HASH_DESERT_BUS_2012 = 0x614dd145;
  // libogc, March 2024: input channels fixed to left-then-right (0x606).
  static constexpr u32 HASH_2024 = 0x5dbf8bf1;
  // The March 2024 code padded to 0x620 (libogc2, libogc-rice).
  static constexpr u32 HASH_2024_PAD = 0x373a950e;

private:
  // libasnd's t_sound_data as seen by the DSP: 0x20 big-endian words.
  struct VoiceData
  {
    u32 out_buf;        // output buffer (0x1000 bytes, 32-byte aligned)
    u32 delay_samples;  // samples of silence before the voice starts (48000 = 1 s)
    // (step << 16) | (loop << 2) | (format & 3) in the 2008/2009 builds,
    // (step << 16) | (status << 8) | (format & 7) from 2011 on. The step is the size in bytes of
    // one input sample frame.
    u32 flags;
    u32 start_addr;  // current read address
    u32 end_addr;    // end of the current buffer (read in 32-byte blocks)
    u32 freq;        // input sample rate
    s16 left;        // last output samples, volume already applied
    s16 right;
    u32 counter;  // rate conversion accumulator
    u16 volume_l;
    u16 volume_r;
    u32 start_addr2;  // next buffer, for double buffering
    u32 end_addr2;
    u16 volume2_l;
    u16 volume2_r;
    u32 backup_addr;   // start of the buffer being played
    u32 tick_counter;  // CPU-side only
    u32 cb;            // CPU-side only
    u32 _pad;
  };
  static_assert(sizeof(VoiceData) == 0x40);

  // Input sample block: 0x20 bytes are fetched at a time. The ucode reserves twice that, and one
  // stereo 16-bit read can reach one word past the block, so the buffer has the reserved size.
  static constexpr u32 INPUT_BLOCK_BYTES = 0x20;
  static constexpr u32 INPUT_BLOCK_WORDS = INPUT_BLOCK_BYTES / 2;
  static constexpr u32 NUM_OUTPUT_SAMPLES = 1024;

  bool UsesNewFlagLayout() const;
  bool HasRightBeforeLeftInput() const;

  void LoadVoice();
  void SaveVoice();
  void FetchInputBlock(u32 address);
  void SwitchToNextBuffer();
  void MixVoice();
  void MixAndReply();
  std::pair<s16, s16> ReadInputFrame(u32 format) const;

  bool m_next_mail_is_voice_addr = false;
  u32 m_voice_addr = 0;
  VoiceData m_voice{};
  std::array<u16, INPUT_BLOCK_WORDS * 2> m_input_block{};
  // Interleaved right, left.
  std::array<u16, NUM_OUTPUT_SAMPLES * 2> m_output{};
};
}  // namespace DSP::HLE
