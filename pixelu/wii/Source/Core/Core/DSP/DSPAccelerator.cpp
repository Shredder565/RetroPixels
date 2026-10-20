// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPAccelerator.h"

#include <algorithm>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace DSP
{
namespace
{
// FORMAT register fields.
enum class AccessSize : u16
{
  Nibble = 0,
  Byte = 1,
  Word = 2,
  Invalid = 3,
};

enum class Decoder : u16
{
  Adpcm = 0,        // ADPCM from ARAM, address advances
  InputNoStep = 1,  // PCM from ACIN, address stays
  Pcm = 2,          // PCM from ARAM, address advances
  InputStep = 3,    // PCM from ACIN, address advances
};

constexpr AccessSize SizeField(u16 format)
{
  return static_cast<AccessSize>(format & 3);
}

constexpr Decoder DecoderField(u16 format)
{
  return static_cast<Decoder>((format >> 2) & 3);
}

constexpr u16 GainScaleField(u16 format)
{
  return static_cast<u16>((format >> 4) & 3);
}

constexpr bool HasUnknownBits(u16 format)
{
  return (format >> 6) != 0;
}

constexpr u32 START_END_MASK = 0x3fffffff;
// Bit 31 of the current address is kept: raw writes need it.
constexpr u32 CURRENT_MASK = 0xbfffffff;

// 32-bit wrapping addition, as the hardware's adder behaves.
constexpr s32 WrapAdd(s32 a, s32 b)
{
  return static_cast<s32>(static_cast<u32>(a) + static_cast<u32>(b));
}

s16 ClampToS16(s32 value)
{
  return static_cast<s16>(std::clamp<s32>(value, -0x8000, 0x7fff));
}
}  // namespace

u16 Accelerator::GetCurrentSample()
{
  const u32 addr = m_current_address;

  switch (SizeField(m_format))
  {
  case AccessSize::Nibble:
  {
    // Two samples per byte, high nibble first.
    const u8 byte = ReadMemory(addr >> 1);
    return static_cast<u16>((addr & 1) != 0 ? (byte & 0xf) : (byte >> 4));
  }
  case AccessSize::Byte:
    return ReadMemory(addr);
  case AccessSize::Word:
  {
    const u8 high = ReadMemory(addr * 2);
    const u8 low = ReadMemory(addr * 2 + 1);
    return static_cast<u16>((high << 8) | low);
  }
  case AccessSize::Invalid:
  default:
    // Nothing useful comes back, but the address still moves on.
    ERROR_LOG_FMT(DSPLLE, "Accelerator sample fetch with invalid size in format {:#x}", m_format);
    return 0;
  }
}

u16 Accelerator::ReadRaw()
{
  const u16 value = GetCurrentSample();

  u32& addr = m_current_address;
  if (SizeField(m_format) == AccessSize::Invalid)
    addr = (addr & ~3u) | ((addr + 1) & 3u);  // only the low two bits count
  else
    addr = addr + 1;

  // Only reading exactly the end address wraps back to the start. (The real chip
  // has more cases around the end address in the 4- and 8-bit modes, where the
  // last u16 can be read in pieces; those are not emulated.)
  if (addr - 1 == m_end_address)
  {
    addr = m_start_address;
    OnRawReadEndException();
  }

  SetCurrentAddress(m_current_address);
  return value;
}

void Accelerator::WriteRaw(u16 value)
{
  // Raw writes go through as big-endian words, whatever the format, and only
  // while bit 31 of the current address is set. Every write raises exception 4.
  if ((m_current_address & 0x80000000) == 0)
  {
    ERROR_LOG_FMT(DSPLLE, "Accelerator raw write ignored: address {:#x} lacks bit 31",
                  m_current_address);
    return;
  }

  const u32 byte_addr = m_current_address * 2;
  WriteMemory(byte_addr, static_cast<u8>(value >> 8));
  WriteMemory(byte_addr + 1, static_cast<u8>(value & 0xff));
  m_current_address += 1;
  OnRawWriteEndException();
}

u16 Accelerator::ReadSample(const s16* coefs)
{
  if (m_reads_stopped)
    return 0;

  if (HasUnknownBits(m_format))
    WARN_LOG_FMT(DSPLLE, "Accelerator format {:#x} has unknown upper bits set", m_format);

  const Decoder decoder = DecoderField(m_format);
  const bool from_input = decoder == Decoder::InputNoStep || decoder == Decoder::InputStep;
  s16 input = static_cast<s16>(from_input ? m_input : GetCurrentSample());

  const int predictor = (m_pred_scale >> 4) & 7;
  const s32 c1 = coefs[predictor * 2];
  const s32 c2 = coefs[predictor * 2 + 1];

  u16 output = 0;
  u32 consumed = 0;  // words this read covers, for the end-of-buffer test below

  if (decoder == Decoder::Adpcm)
  {
    // Only the low nibble is used, as a signed 4-bit value.
    s32 nibble = input & 0xf;
    if (nibble >= 8)
      nibble -= 16;

    const s32 history = WrapAdd(WrapAdd(0x400, c1 * m_yn1), c2 * m_yn2) >> 11;
    const s32 decoded = (nibble << (m_pred_scale & 0xf)) + history;
    output = static_cast<u16>(ClampToS16(decoded));
    consumed = 2;

    m_yn2 = m_yn1;
    m_yn1 = static_cast<s16>(output);
    m_current_address += 1;

    // Ends of buffers at 16-byte frame boundaries wrap without an exception and
    // without reading a new header: to start + 1 for an end at offset 0 of a frame,
    // to start for an end at offset 1.
    const u32 end_in_frame = m_end_address & 0xf;
    if (end_in_frame == 0 && m_current_address == m_end_address)
    {
      m_current_address = m_start_address + 1;
    }
    else if (end_in_frame == 1 && m_current_address == m_end_address - 1)
    {
      m_current_address = m_start_address;
    }
    else if ((m_current_address & 0xf) == 0)
    {
      // Start of the next 8-byte frame: its first byte is the new predictor/scale.
      m_pred_scale = ReadMemory((m_current_address & ~0xfu) >> 1);
      m_current_address += 2;
      consumed += 2;
    }
  }
  else
  {
    // PCM: the gain and both history terms are scaled down separately.
    int shift = 0;
    switch (GainScaleField(m_format))
    {
    case 0:
      shift = 11;
      break;
    case 1:
      shift = 0;
      break;
    case 2:
      shift = 16;
      break;
    default:
      ERROR_LOG_FMT(DSPLLE, "Accelerator format {:#x} has an invalid gain scale", m_format);
      break;
    }

    const s32 scaled_input = (s32{m_gain} * input) >> shift;
    const s32 term1 = (c1 * m_yn1) >> shift;
    const s32 term2 = (c2 * m_yn2) >> shift;
    output = static_cast<u16>(WrapAdd(scaled_input, WrapAdd(term1, term2)));
    consumed = 2;

    m_yn2 = m_yn1;
    m_yn1 = static_cast<s16>(output);
    if (decoder != Decoder::InputNoStep)
      m_current_address += 1;
  }

  // Leaving the buffer: back to the start, and reads stop until YN2 is rewritten
  // (the exception handler normally reloads the history for the loop).
  if (m_current_address == m_end_address + consumed - 1)
  {
    m_current_address = m_start_address;
    m_reads_stopped = true;
    OnSampleReadEndException();
  }

  SetCurrentAddress(m_current_address);
  return output;
}

void Accelerator::DoState(PointerWrap& p)
{
  // Gain and ACIN are not part of the savestate.
  p.Do(m_start_address);
  p.Do(m_end_address);
  p.Do(m_current_address);
  p.Do(m_format);
  p.Do(m_yn1);
  p.Do(m_yn2);
  p.Do(m_pred_scale);
  p.Do(m_reads_stopped);
}

void Accelerator::SetStartAddress(u32 address)
{
  m_start_address = address & START_END_MASK;
}

void Accelerator::SetEndAddress(u32 address)
{
  m_end_address = address & START_END_MASK;
}

void Accelerator::SetCurrentAddress(u32 address)
{
  m_current_address = address & CURRENT_MASK;
}

void Accelerator::SetSampleFormat(u16 format)
{
  m_format = format;
}

void Accelerator::SetGain(s16 gain)
{
  m_gain = gain;
}

void Accelerator::SetYn1(s16 yn1)
{
  m_yn1 = yn1;
}

void Accelerator::SetYn2(s16 yn2)
{
  m_yn2 = yn2;
  m_reads_stopped = false;
}

void Accelerator::SetPredScale(u16 pred_scale)
{
  m_pred_scale = pred_scale & 0x7f;
}

void Accelerator::SetInput(u16 input)
{
  m_input = input;
}
}  // namespace DSP
