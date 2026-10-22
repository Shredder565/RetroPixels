// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// HLE of the libasnd mixer ucode. The behaviour follows the ucode's published source
// (libogc's libasnd/dsp_mixer), including its quirks.

#include "Core/HW/DSPHLE/UCodes/ASnd.h"

#include <algorithm>
#include <type_traits>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace DSP::HLE
{
namespace
{
// Commands; the ucode only looks at the low half of the mail.
enum : u32
{
  CMD_MIX_FRESH = 0x0111,        // load the voice, clear the output, mix
  CMD_MIX_ON_TOP_OF_RAM = 0x0112,  // load the voice, load the output from RAM, mix (unused)
  CMD_SET_VOICE_ADDR = 0x0123,     // the next mail is the voice data address
  CMD_MIX_MORE = 0x0222,           // load the voice, mix into the current output
  CMD_SEND_OUTPUT = 0x0666,        // copy the output buffer to RAM
  CMD_DUMP_ROM_WORD = 0x0777,      // debugging aid (unused)
  CMD_SELF_TEST = 0x0888,          // debugging aid (unused)
  CMD_TERMINATE = 0x0999,          // not in the 2008 build
};

// Flag bits of the 2008/2009 builds, and of the 2011 and later builds.
constexpr u32 OLD_PAUSE_BIT = 1 << 5;
constexpr u32 OLD_LOOP_BIT = 1 << 2;
constexpr u32 OLD_FORMAT_MASK = 3;
constexpr u32 NEW_PAUSE_BIT = 1 << 9;
constexpr u32 NEW_LOOP_BIT = 1 << 8;
constexpr u32 NEW_FORMAT_MASK = 7;

constexpr u32 OUTPUT_RATE = 48000;

constexpr s16 Saturate16(s32 value)
{
  return static_cast<s16>(std::clamp(value, -32768, 32767));
}

// 8-bit samples: one byte in the top half of a 16-bit sample.
constexpr s16 ByteToSample(u32 byte)
{
  return static_cast<s16>((byte & 0xFF) << 8);
}
}  // namespace

ASndUCode::ASndUCode(DSPHLE* dsphle, u32 crc) : UCodeInterface(dsphle, crc)
{
}

bool ASndUCode::UsesNewFlagLayout() const
{
  switch (m_crc)
  {
  case HASH_2008:
  case HASH_2009:
    return false;
  default:
    return true;
  }
}

bool ASndUCode::HasRightBeforeLeftInput() const
{
  // Every build before the Desert Bus and 2024 ones reads the second channel of a stereo frame as
  // the left one.
  switch (m_crc)
  {
  case HASH_2008:
  case HASH_2009:
  case HASH_2011:
  case HASH_2020:
  case HASH_2020_PAD:
    return true;
  default:
    return false;
  }
}

void ASndUCode::Initialize()
{
  m_mail_handler.PushMail(DSP_INIT);
}

void ASndUCode::Update()
{
  // Mails already raise their interrupt; this just keeps reminding the CPU.
  if (m_mail_handler.HasPending())
    m_dsphle->GetSystem().GetDSP().GenerateDSPInterruptFromDSPEmu(INT_DSP);
}

void ASndUCode::HandleMail(u32 mail)
{
  if (m_upload_setup_in_progress)
  {
    PrepareBootUCode(mail);
    return;
  }

  if (m_next_mail_is_voice_addr)
  {
    m_next_mail_is_voice_addr = false;
    m_voice_addr = mail;
    INFO_LOG_FMT(DSPHLE, "ASnd: voice data at {:08x}", mail);
    return;
  }

  if ((mail & TASK_MAIL_MASK) == TASK_MAIL_TO_DSP)
  {
    // Task mails get no reply.
    if (mail == MAIL_NEW_UCODE)
      m_upload_setup_in_progress = true;
    else if (mail == MAIL_RESET)
      m_dsphle->SetUCode(UCODE_ROM);
    else
      WARN_LOG_FMT(DSPHLE, "ASnd: unknown task mail {:08x}", mail);
    return;
  }

  auto& memory = m_dsphle->GetSystem().GetMemory();
  switch (mail & 0xFFFF)
  {
  case CMD_MIX_FRESH:
    LoadVoice();
    m_output.fill(0);
    MixAndReply();
    break;

  case CMD_MIX_ON_TOP_OF_RAM:
    WARN_LOG_FMT(DSPHLE, "ASnd: rarely used command {:08x}", mail);
    LoadVoice();
    memory.CopyFromEmuSwapped(m_output.data(), m_voice.out_buf, sizeof(m_output));
    MixAndReply();
    break;

  case CMD_SET_VOICE_ADDR:
    m_next_mail_is_voice_addr = true;
    break;

  case CMD_MIX_MORE:
    LoadVoice();
    MixAndReply();
    break;

  case CMD_SEND_OUTPUT:
    memory.CopyToEmuSwapped(m_voice.out_buf, m_output.data(), sizeof(m_output));
    m_mail_handler.PushMail(DSP_SYNC, true);
    break;

  case CMD_DUMP_ROM_WORD:
    // The real ucode returns a word of the IROM; HLE has none to give. No interrupt.
    WARN_LOG_FMT(DSPHLE, "ASnd: ROM dump command {:08x} not supported", mail);
    m_mail_handler.PushMail(0x00000000);
    break;

  case CMD_SELF_TEST:
    // The ucode replies with its status register and a test value; this is a typical reply.
    WARN_LOG_FMT(DSPHLE, "ASnd: test command {:08x}", mail);
    m_mail_handler.PushMail(0x22640000);
    break;

  case CMD_TERMINATE:
    if (m_crc == HASH_2008)
    {
      // Not a command yet in this build: handled like an unknown one.
      WARN_LOG_FMT(DSPHLE, "ASnd: terminate command {:08x} not in this build", mail);
      m_mail_handler.PushMail(DSP_SYNC, true);
    }
    else
    {
      // Only reports DSP_DONE; the ucode keeps running.
      INFO_LOG_FMT(DSPHLE, "ASnd: terminate {:08x}", mail);
      m_mail_handler.PushMail(DSP_DONE, true);
    }
    break;

  default:
    WARN_LOG_FMT(DSPHLE, "ASnd: unknown command {:08x}", mail);
    m_mail_handler.PushMail(DSP_SYNC, true);
    break;
  }
}

// Visits the voice fields in their RAM order, with their byte offsets.
template <typename Voice, typename Visitor>
static void VisitVoiceFields(Voice& v, Visitor&& visit)
{
  visit(0x00, v.out_buf);
  visit(0x04, v.delay_samples);
  visit(0x08, v.flags);
  visit(0x0C, v.start_addr);
  visit(0x10, v.end_addr);
  visit(0x14, v.freq);
  visit(0x18, v.left);
  visit(0x1A, v.right);
  visit(0x1C, v.counter);
  visit(0x20, v.volume_l);
  visit(0x22, v.volume_r);
  visit(0x24, v.start_addr2);
  visit(0x28, v.end_addr2);
  visit(0x2C, v.volume2_l);
  visit(0x2E, v.volume2_r);
  visit(0x30, v.backup_addr);
  visit(0x34, v.tick_counter);
  visit(0x38, v.cb);
  visit(0x3C, v._pad);
}

void ASndUCode::LoadVoice()
{
  const auto& memory = m_dsphle->GetSystem().GetMemory();
  VisitVoiceFields(m_voice, [&](u32 offset, auto& field) {
    using Field = std::remove_reference_t<decltype(field)>;
    if constexpr (sizeof(Field) == 4)
      field = memory.Read_U32(m_voice_addr + offset);
    else
      field = static_cast<Field>(memory.Read_U16(m_voice_addr + offset));
  });
}

void ASndUCode::SaveVoice()
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  VisitVoiceFields(m_voice, [&](u32 offset, auto& field) {
    if constexpr (sizeof(field) == 4)
      memory.Write_U32(field, m_voice_addr + offset);
    else
      memory.Write_U16(static_cast<u16>(field), m_voice_addr + offset);
  });
}

void ASndUCode::FetchInputBlock(u32 address)
{
  const auto& memory = m_dsphle->GetSystem().GetMemory();
  memory.CopyFromEmuSwapped(m_input_block.data(), address, INPUT_BLOCK_BYTES);
}

void ASndUCode::SwitchToNextBuffer()
{
  m_voice.volume_l = m_voice.volume2_l;
  m_voice.volume_r = m_voice.volume2_r;
  m_voice.end_addr = m_voice.end_addr2;
  m_voice.start_addr = m_voice.start_addr2;
  m_voice.backup_addr = m_voice.start_addr2;

  // Without looping the second buffer is used up now.
  const u32 loop_bit = UsesNewFlagLayout() ? NEW_LOOP_BIT : OLD_LOOP_BIT;
  if ((m_voice.flags & loop_bit) == 0)
  {
    m_voice.start_addr2 = 0;
    m_voice.end_addr2 = 0;
  }
}

std::pair<s16, s16> ASndUCode::ReadInputFrame(u32 format) const
{
  const u32 addr = m_voice.start_addr;
  const u32 word_index = (addr >> 1) & (INPUT_BLOCK_WORDS - 1);
  const u16 word = m_input_block[word_index];
  // Stereo 16-bit reads take the following word too, without wrapping inside the block.
  const u16 next_word = m_input_block[word_index + 1];
  // Even addresses hold the byte in the top half of the word.
  const u32 selected_byte = (addr & 1) == 0 ? u32{word} >> 8 : u32{word};

  switch (format)
  {
  case 0:  // mono, signed 8 bit
  {
    const s16 s = ByteToSample(selected_byte);
    return {s, s};
  }
  case 1:  // mono, 16 bit big-endian
    return {static_cast<s16>(word), static_cast<s16>(word)};
  case 2:  // stereo, signed 8 bit
    return {static_cast<s16>(word & 0xFF00), ByteToSample(word)};
  case 3:  // stereo, 16 bit big-endian
    return {static_cast<s16>(word), static_cast<s16>(next_word)};
  case 4:  // mono, unsigned 8 bit
  {
    const s16 s = static_cast<s16>(ByteToSample(selected_byte) ^ 0x8000);
    return {s, s};
  }
  case 5:  // mono, 16 bit little-endian
  {
    const s16 s = static_cast<s16>(Common::swap16(word));
    return {s, s};
  }
  case 6:  // stereo, unsigned 8 bit
  {
    const u16 flipped = word ^ 0x8080;
    return {static_cast<s16>(flipped & 0xFF00), ByteToSample(flipped)};
  }
  default:  // 7: stereo, 16 bit little-endian
    return {static_cast<s16>(Common::swap16(word)), static_cast<s16>(Common::swap16(next_word))};
  }
}

void ASndUCode::MixVoice()
{
  const bool new_layout = UsesNewFlagLayout();
  const u32 format = m_voice.flags & (new_layout ? NEW_FORMAT_MASK : OLD_FORMAT_MASK);
  const u32 step = m_voice.flags >> 16;

  u32 frames = NUM_OUTPUT_SAMPLES;
  if (m_voice.delay_samples != 0)
  {
    // The delay loop counts the delay down by at most 1023 per call, and whichever way it ends
    // it leaves nothing to mix in this call (the ucode reloads the wrong register on exit).
    m_voice.left = 0;
    m_voice.right = 0;
    m_voice.delay_samples = m_voice.delay_samples > NUM_OUTPUT_SAMPLES - 1 ?
                                m_voice.delay_samples - (NUM_OUTPUT_SAMPLES - 1) :
                                0;
    frames = 0;
  }

  FetchInputBlock(m_voice.start_addr & ~(INPUT_BLOCK_BYTES - 1));

  for (u32 frame = 0; frame < frames; ++frame)
  {
    // Output the current sample (right first), saturating.
    u16& out_right = m_output[2 * frame];
    u16& out_left = m_output[2 * frame + 1];
    out_right = static_cast<u16>(Saturate16(static_cast<s16>(out_right) + m_voice.right));
    out_left = static_cast<u16>(Saturate16(static_cast<s16>(out_left) + m_voice.left));

    m_voice.counter += m_voice.freq;
    if (m_voice.counter < OUTPUT_RATE)
      continue;

    // Time for the next input frame.
    if (m_voice.freq <= OUTPUT_RATE)
    {
      m_voice.counter -= OUTPUT_RATE;
      m_voice.start_addr += step;
    }
    else
    {
      // Faster than the output: skip input frames, refetching each time a block is crossed.
      do
      {
        m_voice.counter -= OUTPUT_RATE;
        m_voice.start_addr += step;
        if ((m_voice.start_addr & (INPUT_BLOCK_BYTES - 1)) == 0)
          FetchInputBlock(m_voice.start_addr & ~(INPUT_BLOCK_BYTES - 1));
      } while (m_voice.counter >= OUTPUT_RATE);
    }

    if (m_voice.start_addr >= m_voice.end_addr)
    {
      SwitchToNextBuffer();
      if (m_voice.start_addr == 0)
      {
        // Nothing left to play: output silence.
        m_voice.left = 0;
        m_voice.right = 0;
        continue;
      }
      FetchInputBlock(m_voice.start_addr & ~(INPUT_BLOCK_BYTES - 1));
    }

    // At the start of a block the ucode fetches again, from the unmasked address.
    if (((m_voice.start_addr >> 1) & (INPUT_BLOCK_WORDS - 1)) == 0)
      FetchInputBlock(m_voice.start_addr);

    auto [first, second] = ReadInputFrame(format);
    if (HasRightBeforeLeftInput())
      std::swap(first, second);

    // first goes left, second goes right; volumes are 8.8 fixed point.
    m_voice.left = static_cast<s16>((s32{first} * m_voice.volume_l) >> 8);
    m_voice.right = static_cast<s16>((s32{second} * m_voice.volume_r) >> 8);
  }

  if (m_voice.start_addr == 0)
    SwitchToNextBuffer();
}

void ASndUCode::MixAndReply()
{
  const u32 pause_bit = UsesNewFlagLayout() ? NEW_PAUSE_BIT : OLD_PAUSE_BIT;
  if ((m_voice.flags & pause_bit) == 0)
  {
    // A voice without a current buffer starts on its second one, if it has one.
    if (m_voice.start_addr == 0)
      SwitchToNextBuffer();
    if (m_voice.start_addr != 0)
      MixVoice();
  }

  SaveVoice();
  m_mail_handler.PushMail(DSP_SYNC, true);
}

void ASndUCode::DoState(PointerWrap& p)
{
  DoStateShared(p);
  p.Do(m_next_mail_is_voice_addr);
  p.Do(m_voice_addr);
  p.Do(m_voice);
  p.Do(m_input_block);
  p.Do(m_output);
}
}  // namespace DSP::HLE
