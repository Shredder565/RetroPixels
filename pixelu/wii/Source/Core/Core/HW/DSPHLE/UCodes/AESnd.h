// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Core/DSP/DSPAccelerator.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP
{
class DSPManager;
}

namespace DSP::HLE
{
class DSPHLE;

// The DSP accelerator as libaesnd uses it: PCM reads from ARAM. Its end-of-sample handler just
// re-arms the accelerator.
class AESndAccelerator final : public Accelerator
{
public:
  explicit AESndAccelerator(DSPManager& dsp);
  AESndAccelerator(const AESndAccelerator&) = delete;
  AESndAccelerator(AESndAccelerator&&) = delete;
  AESndAccelerator& operator=(const AESndAccelerator&) = delete;
  AESndAccelerator& operator=(AESndAccelerator&&) = delete;
  ~AESndAccelerator() override;

protected:
  void OnRawReadEndException() override {}
  void OnRawWriteEndException() override {}
  void OnSampleReadEndException() override;
  u8 ReadMemory(u32 address) override;
  void WriteMemory(u32 address, u8 value) override;

private:
  DSPManager& m_dsp;
};

// The libaesnd mixer ucode from libogc (Zlib licence), used by homebrew. The CPU passes one voice
// parameter block at a time; each is mixed into a 2 ms (96 sample) stereo buffer.
class AESndUCode final : public UCodeInterface
{
public:
  AESndUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

  // Known builds (image size in parentheses).
  // June 2010, first release, shipped from libogc 1.8.4 (0x3e0, padded).
  static constexpr u32 HASH_2010 = 0x008366af;
  // April 2012: input channels swapped, libogc 1.8.11 (0x3e0, padded).
  static constexpr u32 HASH_2012 = 0x078066ab;
  // EDuke32 Wii's modified 2010 build: unsigned 8-bit formats added (breaking the mono ones),
  // plus the 2012 channel swap (0x3e0, padded).
  static constexpr u32 HASH_EDUKE32 = 0x5ad4d933;
  // June 2020: unsigned formats, libogc 2.1.0 (0x3e6).
  static constexpr u32 HASH_2020 = 0x84c680a9;
  // The 2020 code padded to 0x400 (libogc-rice, libogc2; Not64, OpenTTD).
  static constexpr u32 HASH_2020_PAD = 0xa02a6131;
  // July 2022: MAIL_TERMINATE fixed, padded to 0x400 (libogc2, libogc-rice).
  static constexpr u32 HASH_2022_PAD = 0x2e5e4100;
  // March 2023: the same fix in libogc 2.4.0 (0x3e8).
  static constexpr u32 HASH_2023 = 0x002e5e41;

private:
  // The first 0x40 bytes of libaesnd's aesndpb_t, as the DSP sees them (big-endian, packed:
  // freq is only 16-bit aligned).
#pragma pack(push, 1)
  struct ParameterBlock
  {
    u32 out_buf;
    u32 buf_start;
    u32 buf_end;
    u32 buf_curr;
    u16 yn1;
    u16 yn2;
    u16 pds;
    u32 freq;  // 16.16 input samples per output sample
    u16 counter;
    s16 left;
    s16 right;
    u16 volume_l;
    u16 volume_r;
    u32 delay;
    u32 flags;
    u8 _pad[20];
  };
#pragma pack(pop)
  static_assert(sizeof(ParameterBlock) == 0x40);

  static constexpr u32 NUM_OUTPUT_SAMPLES = 96;

  bool UsesNewFlagLayout() const;
  bool HasLeftFirstInput() const;

  void LoadParameterBlock();
  void SaveParameterBlock();
  void ArmAccelerator(u16 format, u16 gain);
  void MixVoice();
  void MixAndReply();

  bool m_next_mail_is_parameter_block_addr = false;
  u32 m_parameter_block_addr = 0;
  ParameterBlock m_pb{};
  // Interleaved right, left.
  std::array<s16, NUM_OUTPUT_SAMPLES * 2> m_output{};
  AESndAccelerator m_accelerator;
  bool m_warned_about_format = false;
};
}  // namespace DSP::HLE
