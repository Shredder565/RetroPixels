// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// HLE of the "DAC" audio ucode family used by Nintendo's first-party games (Zelda, Mario,
// Pikmin and others, plus the GameCube IPL). Each frame mixes up to 4096 voices into 0x50-sample
// buffers that live at fixed DSP addresses, which games refer to directly.

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

class PointerWrap;

namespace Core
{
class System;
}

namespace DSP::HLE
{
class DSPHLE;

// The mixing engine: voice parameter blocks (VPBs) in RAM, sample sources in ARAM (or in MRAM
// on the Wii), mixing buffers, reverb and the final output.
class ZeldaMixer
{
public:
  explicit ZeldaMixer(Core::System& system);
  ZeldaMixer(const ZeldaMixer&) = delete;
  ZeldaMixer& operator=(const ZeldaMixer&) = delete;

  void SetFlags(u32 flags) { m_flags = flags; }

  // Command 01's tables: resampling filter, constant patterns and sine table in one block at
  // `tables_addr`, the AFC predictor pairs at `afc_addr`.
  void LoadTables(u32 tables_addr, u32 afc_addr, bool with_sine_table);
  void SetVoiceBlocksAddress(u32 addr) { m_vpb_base = addr; }
  void SetReverbBlocksAddress(u32 addr) { m_reverb_base = addr; }
  void SetARAMBaseAddress(u32 addr) { m_aram_base = addr; }
  void SetOutput(u32 left_addr, u32 right_addr, u16 volume)
  {
    m_out_left = left_addr;
    m_out_right = right_addr;
    m_out_volume = volume;
  }

  // A frame is BeginFrame, the voices, then EndFrame (which uploads the output).
  void BeginFrame();
  void RenderVoice(u16 voice_id);
  void EndFrame();

  void DoState(PointerWrap& p);

private:
  class Voice;

  static constexpr size_t FRAME_SAMPLES = 0x50;
  using MixBuffer = std::array<s16, FRAME_SAMPLES>;

  // The mixing buffers, in savestate order.
  enum Buffer : size_t
  {
    FRONT_LEFT,
    FRONT_RIGHT,
    BACK_LEFT,
    BACK_RIGHT,
    FRONT_LEFT_REVERB,
    FRONT_RIGHT_REVERB,
    BACK_LEFT_REVERB,
    BACK_RIGHT_REVERB,
    UNK0_REVERB,
    UNK1_REVERB,
    UNK0,
    UNK1,
    UNK2,
    NUM_BUFFERS,
  };
  MixBuffer& Buf(Buffer which) { return m_buffers[which]; }
  // Games name buffers by their DSP address; unknown addresses give nullptr.
  MixBuffer* FindBuffer(u16 dsp_address);

  void ReadVoice(u16 voice_id, Voice& voice) const;
  void WriteVoice(u16 voice_id, Voice& voice);

  void GenerateInput(Voice& voice, MixBuffer& out);
  void Interpolate(Voice& voice, const s16* raw, MixBuffer& out) const;
  void MixDolby(Voice& voice, const MixBuffer& input);
  void MixChannels(Voice& voice, const MixBuffer& input);

  const u8* ARAMRange(u32 offset, size_t size) const;
  template <size_t BYTES>
  void FetchPCM(Voice& voice, s16* dst, u16 count);
  void FetchAFC(Voice& voice, s16* dst, u16 count);
  void DecodeAFC(Voice& voice, s16* dst, size_t blocks);
  void FetchFromMRAM(Voice& voice, s16* dst, u16 count);

  // The four reverb blocks: before mixing, the delayed signal is read back from its RAM ring
  // buffer, filtered and sent; after the frame, the reverb buffer is written to the ring.
  void ReverbInput();
  void ReverbOutput();

  u32 m_flags = 0;
  bool m_frame_prepared = false;

  u32 m_out_left = 0;
  u32 m_out_right = 0;
  u16 m_out_volume = 0;

  std::array<MixBuffer, NUM_BUFFERS> m_buffers{};

  // 64 phases of 4 taps.
  std::array<s16, 0x100> m_resample_coefs{};
  // Four 0x40-sample patterns; 2 and 3 are regenerated every frame.
  std::array<s16, 0x100> m_patterns{};
  // sin(x) over a quarter period, in 1.15, for Dolby panning.
  std::array<s16, 0x80> m_sine{};
  // 16 pairs of AFC predictor coefficients.
  std::array<s16, 0x20> m_afc_coefs{};

  u32 m_aram_base = 0;
  u32 m_vpb_base = 0;
  u32 m_reverb_base = 0;

  // Per reverb block: the current slot in its ring buffer, and the last 8 input samples of the
  // previous frame (the filter's history).
  std::array<u16, 4> m_reverb_slot{};
  std::array<std::array<s16, 8>, 4> m_reverb_tail{};

  Core::System& m_system;
};

class ZeldaUCode final : public UCodeInterface
{
public:
  ZeldaUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

private:
  // Where the incoming mail stream is. The real ucode handles mails in interrupt handlers while
  // it renders; here rendering is resumed from mail handling instead.
  enum class MailState : u32
  {
    Idle,
    Rendering,
    ReceivingCommand,
    Halted,
  };

  void OnMail(u32 mail);
  void OnLightMail(u32 mail);
  void OnTaskMail(u32 mail);

  void QueuePush(u32 word);
  u32 QueuePop();

  // Runs queued commands until the queue is empty or a command takes over the mail flow.
  void ExecuteQueued();
  // Returns false when execution must stop (rendering started, or the ucode halted).
  bool Execute(u32 header);

  void SendSync(u16 sync);
  void SendFrameEnd();

  bool Rendering() const { return m_frame != m_frames_requested; }
  void ContinueRendering();

  u32 m_flags = 0;
  MailState m_mail_state = MailState::Idle;
  u32 m_expected_mails = 0;

  // Voices below m_voice_limit may be rendered; the rest wait for the next sync mail. Each bit
  // of m_voice_mask (MSB first, 16 voices per entry) enables one voice.
  u32 m_voice_limit = 0;
  std::array<u16, 256> m_voice_mask{};
  bool m_sync_second_half = false;

  // Command queue: a ring of 64 words.
  std::array<u32, 64> m_queue{};
  u32 m_queue_read = 0;
  u32 m_queue_write = 0;
  u32 m_queued_commands = 0;
  // Cleared after a rendering pass until the CPU answers DSP_FRAME_END.
  bool m_commands_enabled = true;

  u32 m_frames_requested = 0;
  u16 m_voices_per_frame = 0;
  u32 m_frame = 0;
  u32 m_voice = 0;

  ZeldaMixer m_mixer;
};
}  // namespace DSP::HLE
