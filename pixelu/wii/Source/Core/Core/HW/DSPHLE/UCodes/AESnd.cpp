// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// HLE of the libaesnd mixer ucode. The behaviour follows the ucode's published source
// (libogc's libaesnd/dspcode/dspmixer.s), including its quirks.

#include "Core/HW/DSPHLE/UCodes/AESnd.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/DSP/DSPAccelerator.h"
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
// Commands, prefix included.
constexpr u32 CMD_FIRST_VOICE = 0xface0010;  // clear the output, then mix the voice
constexpr u32 CMD_NEXT_VOICE = 0xface0020;   // mix the voice into the output
constexpr u32 CMD_SET_PB_ADDR = 0xface0080;  // the next mail is the parameter block address
constexpr u32 CMD_SEND_OUTPUT = 0xface0100;  // copy the output to RAM
constexpr u32 CMD_TERMINATE = 0xfacedead;

// Voice formats: bit 0 = stereo, bit 1 = 16-bit, bit 2 = unsigned (2020 and later layouts).
constexpr u32 FORMAT_STEREO_BIT = 1;
constexpr u32 FORMAT_16BIT_BIT = 2;
constexpr u32 FORMAT_UNSIGNED_BIT = 4;
constexpr u32 FORMAT_STEREO8 = 1;
constexpr u32 FORMAT_STEREO16 = 3;
constexpr u32 FORMAT_STEREO8_UNSIGNED = 5;

// Flag bits the DSP looks at. Older layouts have a 2-bit format and the pause bit at 2; newer
// ones a 3-bit format and the pause bit at 3.
constexpr u32 OLD_FORMAT_MASK = 3;
constexpr u32 OLD_PAUSE_BIT = 0x00000004;
constexpr u32 NEW_FORMAT_MASK = 7;
constexpr u32 NEW_PAUSE_BIT = 0x00000008;
constexpr u32 FLAG_FINISHED = 0x00100000;
constexpr u32 FLAG_RUNNING = 0x40000000;

// Accelerator setups: PCM from ARAM with 8-bit reads and a gain of 0x100 (scale 1), or 16-bit
// reads and a gain of 0x800 (scale 1/2048), so both give full-range 16-bit samples.
constexpr u16 ACCEL_FORMAT_PCM8 = 0x0019;
constexpr u16 ACCEL_GAIN_PCM8 = 0x0100;
constexpr u16 ACCEL_FORMAT_PCM16 = 0x000a;
constexpr u16 ACCEL_GAIN_PCM16 = 0x0800;

// The ucode zeroes all ADPCM coefficients.
constexpr std::array<s16, 16> NO_COEFFICIENTS{};

constexpr s16 Saturate16(s32 value)
{
  return static_cast<s16>(std::clamp(value, -32768, 32767));
}
}  // namespace

AESndAccelerator::AESndAccelerator(DSPManager& dsp) : m_dsp(dsp)
{
}

AESndAccelerator::~AESndAccelerator() = default;

void AESndAccelerator::OnSampleReadEndException()
{
  // The ucode's handler writes the history and predictor registers back, which lets the
  // accelerator carry on reading.
  SetYn1(GetYn1());
  SetYn2(GetYn2());
  SetPredScale(GetPredScale());
}

u8 AESndAccelerator::ReadMemory(u32 address)
{
  return m_dsp.ReadARAM(address);
}

void AESndAccelerator::WriteMemory(u32 address, u8 value)
{
  m_dsp.WriteARAM(value, address);
}

AESndUCode::AESndUCode(DSPHLE* dsphle, u32 crc)
    : UCodeInterface(dsphle, crc), m_accelerator(dsphle->GetSystem().GetDSP())
{
}

bool AESndUCode::UsesNewFlagLayout() const
{
  return m_crc != HASH_2010 && m_crc != HASH_2012;
}

bool AESndUCode::HasLeftFirstInput() const
{
  return m_crc != HASH_2010;
}

void AESndUCode::Initialize()
{
  m_mail_handler.PushMail(DSP_INIT);
}

void AESndUCode::Update()
{
  // Mails already raise their interrupt; this just keeps reminding the CPU.
  if (m_mail_handler.HasPending())
    m_dsphle->GetSystem().GetDSP().GenerateDSPInterruptFromDSPEmu(INT_DSP);
}

void AESndUCode::HandleMail(u32 mail)
{
  if (m_upload_setup_in_progress)
  {
    PrepareBootUCode(mail);
    return;
  }

  if (m_next_mail_is_parameter_block_addr)
  {
    m_next_mail_is_parameter_block_addr = false;
    m_parameter_block_addr = mail;
    INFO_LOG_FMT(DSPHLE, "AESnd: parameter block at {:08x}", mail);
    return;
  }

  if ((mail & TASK_MAIL_MASK) == TASK_MAIL_TO_DSP)
  {
    // No reply. (The real ucode halts on unknown task mails.)
    if (mail == MAIL_NEW_UCODE)
      m_upload_setup_in_progress = true;
    else if (mail == MAIL_RESET)
      m_dsphle->SetUCode(UCODE_ROM);
    else
      WARN_LOG_FMT(DSPHLE, "AESnd: unknown task mail {:08x}", mail);
    return;
  }

  switch (mail)
  {
  case CMD_FIRST_VOICE:
    LoadParameterBlock();
    m_output.fill(0);
    MixAndReply();
    break;

  case CMD_NEXT_VOICE:
    LoadParameterBlock();
    MixAndReply();
    break;

  case CMD_SET_PB_ADDR:
    m_next_mail_is_parameter_block_addr = true;
    break;

  case CMD_SEND_OUTPUT:
    m_dsphle->GetSystem().GetMemory().CopyToEmuSwapped(m_pb.out_buf, m_output.data(),
                                                       sizeof(m_output));
    m_mail_handler.PushMail(DSP_SYNC, true);
    break;

  case CMD_TERMINATE:
    INFO_LOG_FMT(DSPHLE, "AESnd: terminate {:08x}", mail);
    if (m_crc != HASH_2022_PAD && m_crc != HASH_2023)
    {
      // In these builds the comparison against 0xdead sign-extends the constant but not the
      // received mail, so it never matches and the CPU waits forever for DSP_DONE. The reply is
      // sent anyway so that homebrew calling AESND_Reset does not hang.
      WARN_LOG_FMT(DSPHLE, "AESnd: this ucode build cannot terminate; real hardware and LLE "
                           "hang here");
    }
    m_mail_handler.PushMail(DSP_DONE, true);
    break;

  default:
    WARN_LOG_FMT(DSPHLE, "AESnd: unknown command {:08x}", mail);
    break;
  }
}

// The fields the DSP transfers, as (byte offset, size). The packed struct mirrors the RAM
// layout, so the offsets are the same in both.
namespace
{
struct PBField
{
  u8 offset;
  u8 size;
};
constexpr PBField PB_FIELDS[] = {
    {0, 4},  {4, 4},  {8, 4},  {12, 4}, {16, 2}, {18, 2}, {20, 2}, {22, 4},
    {26, 2}, {28, 2}, {30, 2}, {32, 2}, {34, 2}, {36, 4}, {40, 4},
};
}  // namespace

void AESndUCode::LoadParameterBlock()
{
  const auto& memory = m_dsphle->GetSystem().GetMemory();
  u8* const bytes = reinterpret_cast<u8*>(&m_pb);
  for (const PBField& field : PB_FIELDS)
  {
    const u32 address = m_parameter_block_addr + field.offset;
    if (field.size == 4)
    {
      const u32 value = memory.Read_U32(address);
      std::memcpy(bytes + field.offset, &value, sizeof(value));
    }
    else
    {
      const u16 value = memory.Read_U16(address);
      std::memcpy(bytes + field.offset, &value, sizeof(value));
    }
  }
}

void AESndUCode::SaveParameterBlock()
{
  auto& memory = m_dsphle->GetSystem().GetMemory();
  const u8* const bytes = reinterpret_cast<const u8*>(&m_pb);
  for (const PBField& field : PB_FIELDS)
  {
    const u32 address = m_parameter_block_addr + field.offset;
    if (field.size == 4)
    {
      u32 value;
      std::memcpy(&value, bytes + field.offset, sizeof(value));
      memory.Write_U32(value, address);
    }
    else
    {
      u16 value;
      std::memcpy(&value, bytes + field.offset, sizeof(value));
      memory.Write_U16(value, address);
    }
  }
}

void AESndUCode::ArmAccelerator(u16 format, u16 gain)
{
  m_accelerator.SetSampleFormat(format);
  m_accelerator.SetGain(static_cast<s16>(gain));
  m_accelerator.SetStartAddress(m_pb.buf_start);
  m_accelerator.SetEndAddress(m_pb.buf_end);
  m_accelerator.SetCurrentAddress(m_pb.buf_curr);
  m_accelerator.SetYn1(static_cast<s16>(m_pb.yn1));
  m_accelerator.SetYn2(static_cast<s16>(m_pb.yn2));
  m_accelerator.SetPredScale(m_pb.pds);
}

void AESndUCode::MixVoice()
{
  const bool new_layout = UsesNewFlagLayout();
  const u32 format = m_pb.flags & (new_layout ? NEW_FORMAT_MASK : OLD_FORMAT_MASK);
  const bool stereo = (format & FORMAT_STEREO_BIT) != 0;
  const bool is_unsigned = (format & FORMAT_UNSIGNED_BIT) != 0;
  const bool is_16bit = (format & FORMAT_16BIT_BIT) != 0;

  if (m_crc == HASH_EDUKE32 && format != FORMAT_STEREO8 && format != FORMAT_STEREO16 &&
      format != FORMAT_STEREO8_UNSIGNED && !m_warned_about_format)
  {
    // EDuke32's build mixes the mono formats wrongly and lacks unsigned 16-bit stereo.
    m_warned_about_format = true;
    PanicAlertFmt("EDuke32 Wii aesndlib uCode does not correctly handle this sample format: "
                  "{} (flags: {:08x})",
                  format, m_pb.flags);
  }

  if (is_16bit)
    ArmAccelerator(ACCEL_FORMAT_PCM16, ACCEL_GAIN_PCM16);
  else
    ArmAccelerator(ACCEL_FORMAT_PCM8, ACCEL_GAIN_PCM8);

  // The delay counts down one per output sample. When it runs out inside this buffer, mixing
  // starts at the sample where it hit zero (that slot is written by both).
  u32 first = 0;
  u32 count = NUM_OUTPUT_SAMPLES;
  if (m_pb.delay != 0)
  {
    if (m_pb.delay <= NUM_OUTPUT_SAMPLES)
    {
      first = m_pb.delay - 1;
      count = NUM_OUTPUT_SAMPLES - m_pb.delay;
      m_pb.delay = 0;
    }
    else
    {
      count = 0;
      m_pb.delay -= NUM_OUTPUT_SAMPLES;
    }
  }

  for (u32 i = first; i < first + count; ++i)
  {
    // Output the current sample (right first), saturating.
    s16& out_right = m_output[2 * i];
    s16& out_left = m_output[2 * i + 1];
    out_right = Saturate16(out_right + m_pb.right);
    out_left = Saturate16(out_left + m_pb.left);

    // 16.16 rate conversion: the integer part says how many input frames to step over.
    const u32 position = u32{m_pb.counter} + m_pb.freq;
    m_pb.counter = static_cast<u16>(position);
    u32 frames_to_read = (position >> 16) & 0xFFFF;
    if (frames_to_read == 0)
      continue;

    s16 a = 0;
    s16 b = 0;
    while (frames_to_read-- != 0)
    {
      a = static_cast<s16>(m_accelerator.ReadSample(NO_COEFFICIENTS.data()));
      b = stereo ? static_cast<s16>(m_accelerator.ReadSample(NO_COEFFICIENTS.data())) : a;
    }
    if (is_unsigned)
    {
      a = static_cast<s16>(a ^ 0x8000);
      b = static_cast<s16>(b ^ 0x8000);
    }

    // The original layout reads right then left; later builds read left then right.
    s16 new_left = b;
    s16 new_right = a;
    if (HasLeftFirstInput())
      std::swap(new_left, new_right);

    // Volumes are 8.8 fixed point.
    m_pb.left = Saturate16((s32{new_left} * m_pb.volume_l) >> 8);
    m_pb.right = Saturate16((s32{new_right} * m_pb.volume_r) >> 8);
  }

  m_pb.pds = m_accelerator.GetPredScale();
  m_pb.yn2 = static_cast<u16>(m_accelerator.GetYn2());
  m_pb.yn1 = static_cast<u16>(m_accelerator.GetYn1());
  m_pb.buf_curr = m_accelerator.GetCurrentAddress();
}

void AESndUCode::MixAndReply()
{
  const u32 pause_bit = UsesNewFlagLayout() ? NEW_PAUSE_BIT : OLD_PAUSE_BIT;
  const bool playing = (m_pb.flags & pause_bit) == 0 && (m_pb.flags & FLAG_RUNNING) != 0 &&
                       m_pb.buf_start != 0;
  if (playing)
    MixVoice();

  m_pb.flags |= FLAG_FINISHED;
  SaveParameterBlock();
  m_mail_handler.PushMail(DSP_SYNC, true);
}

void AESndUCode::DoState(PointerWrap& p)
{
  DoStateShared(p);
  p.Do(m_next_mail_is_parameter_block_addr);
  p.Do(m_parameter_block_addr);
  p.Do(m_pb);
  p.Do(m_output);
  m_accelerator.DoState(p);
}
}  // namespace DSP::HLE
