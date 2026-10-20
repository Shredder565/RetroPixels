// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/UCodes/Zelda.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <utility>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPHLE/MailHandler.h"
#include "Core/HW/DSPHLE/UCodes/GBA.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

namespace DSP::HLE
{
namespace
{
// What sets the ucode versions apart. The values are saved in savestates.
enum ZeldaFlag : u32
{
  // Wii: there is no ARAM; sample data comes from main memory, at a base given by command 0E.
  FLAG_NO_ARAM = 1u << 0,
  // Dolby panning gains are scaled by 2^-15 instead of 2^-16 (twice as loud).
  FLAG_LOUD_DOLBY = 1u << 1,
  // The early protocol: other mails and acknowledgements, a single sync mail, no Dolby.
  FLAG_LIGHT = 1u << 2,
  // Voices have 4 outputs instead of 6.
  FLAG_FOUR_OUTPUTS = 1u << 3,
  // 0x80-word voice blocks, without the Dolby area.
  FLAG_SMALL_VPB = 1u << 4,
  // A voice output's second word is the volume step itself, not the volume to ramp to.
  FLAG_EXPLICIT_STEP = 1u << 5,
  // Two sync mails per frame, carrying the enable bits of voices 0-63.
  FLAG_FRAME_SYNC = 1u << 6,
  // Command 0D does nothing.
  FLAG_IGNORE_0D = 1u << 7,
  // Command 0C runs the GBA key exchange (from before the separate GBA ucode).
  FLAG_GBA_0C = 1u << 8,
  // Command 0C takes two parameters whose meaning is unknown.
  FLAG_TWO_ARG_0C = 1u << 9,
  // Command 02 is followed by command 0D's two parameters.
  FLAG_RENDER_HAS_0D = 1u << 10,
};

struct VersionInfo
{
  u32 crc;
  u32 flags;
};

constexpr VersionInfo VERSIONS[] = {
    // GameCube IPL, NTSC.
    {0x24B22038, FLAG_LIGHT | FLAG_FOUR_OUTPUTS | FLAG_SMALL_VPB | FLAG_EXPLICIT_STEP |
                     FLAG_IGNORE_0D | FLAG_TWO_ARG_0C},
    // GameCube IPL, PAL.
    {0x6BA3B3EA, FLAG_LIGHT | FLAG_FOUR_OUTPUTS | FLAG_IGNORE_0D},
    // Pikmin (NTSC demo).
    {0xDF059F68, FLAG_LIGHT | FLAG_IGNORE_0D | FLAG_GBA_0C},
    // Pikmin (NTSC), Animal Crossing.
    {0x4BE6A5CB, FLAG_LIGHT | FLAG_IGNORE_0D | FLAG_GBA_0C},
    // Luigi's Mansion.
    {0x42F64AC4, FLAG_LIGHT | FLAG_IGNORE_0D | FLAG_TWO_ARG_0C},
    // Pikmin (PAL).
    {0x267FD05A, FLAG_FRAME_SYNC | FLAG_IGNORE_0D},
    // Super Mario Sunshine.
    {0x56D36052, FLAG_FRAME_SYNC | FLAG_IGNORE_0D},
    // The Wind Waker.
    {0x86840740, 0},
    // Zelda Collector's Edition (other games), Four Swords Adventures, Mario Kart: Double Dash!!,
    // Pikmin 2.
    {0x2FCDF1EC, FLAG_LOUD_DOLBY},
    // Twilight Princess (GameCube), Donkey Kong Jungle Beat. These also filter the final mix,
    // which is not emulated.
    {0x6CA33A6D, FLAG_LOUD_DOLBY | FLAG_RENDER_HAS_0D},
    // Twilight Princess (Wii), Link's Crossbow Training.
    {0x6C3F6F94, FLAG_NO_ARAM | FLAG_LOUD_DOLBY | FLAG_RENDER_HAS_0D},
    // Super Mario Galaxy 1 and 2, Donkey Kong Jungle Beat (Wii).
    {0xD643001F, FLAG_NO_ARAM | FLAG_LOUD_DOLBY | FLAG_RENDER_HAS_0D},
    // Pikmin (New Play Control).
    {0xB7EB9A9C, FLAG_NO_ARAM | FLAG_LOUD_DOLBY | FLAG_RENDER_HAS_0D},
    // Pikmin 2 (New Play Control).
    {0xEAEB38CC, FLAG_NO_ARAM | FLAG_LOUD_DOLBY | FLAG_RENDER_HAS_0D},
};

// Word offsets in a voice parameter block, in the full 0xC0-word layout. The first 0x80 words
// are written back after each frame; the rest is set up by the game only.
namespace vpb
{
constexpr size_t ENABLED = 0x00;
constexpr size_t DONE = 0x01;
// 4.12 ratio of raw samples to output samples.
constexpr size_t RATIO = 0x02;
// Restart the sample source and the filters.
constexpr size_t RESET = 0x04;
constexpr size_t END_REACHED = 0x05;
// Output CONSTANT_SAMPLE instead of reading the source (used to silence a voice).
constexpr size_t USE_CONSTANT = 0x06;
// Six outputs of {buffer DSP address, target volume (or step), current volume, unused}.
constexpr size_t OUTPUTS = 0x08;
constexpr size_t OUTPUT_WORDS = 4;
// Dolby panning: 0XXXXXXX0YYYYYYY (0x7F is full left / full front), reverb send, volumes.
constexpr size_t DOLBY_POSITION = 0x28;
constexpr size_t DOLBY_REVERB = 0x29;
constexpr size_t DOLBY_CURRENT = 0x2A;
constexpr size_t DOLBY_TARGET = 0x2B;
constexpr size_t USE_DOLBY = 0x2C;
// 0.12 fraction of the source position.
constexpr size_t POSITION_FRAC = 0x30;
// Decoded AFC samples still waiting in AFC_SAMPLES.
constexpr size_t AFC_CACHED = 0x32;
constexpr size_t CONSTANT_SAMPLE = 0x33;
constexpr size_t POSITION = 0x34;  // 32 bits
constexpr size_t SAMPLES_BEFORE_LOOP = 0x36;
constexpr size_t ARAM_ADDRESS = 0x38;  // 32 bits
constexpr size_t REMAINING = 0x3A;     // 32 bits
// The last 4 raw samples of the previous frame, for interpolation.
constexpr size_t HISTORY = 0x3C;
constexpr size_t BIQUAD_X1 = 0x54;
constexpr size_t BIQUAD_X2 = 0x55;
constexpr size_t BIQUAD_Y1 = 0x56;
constexpr size_t BIQUAD_Y2 = 0x57;
// The last decoded AFC block; its last two samples are the predictor history.
constexpr size_t AFC_SAMPLES = 0x58;
constexpr size_t AFC_Y2 = AFC_SAMPLES + 0xE;
constexpr size_t AFC_Y1 = AFC_SAMPLES + 0xF;
constexpr size_t LOWPASS_Y1 = 0x68;
constexpr size_t LOWPASS_X1 = 0x69;
constexpr size_t SOURCE = 0x80;
constexpr size_t LOOPING = 0x81;
constexpr size_t LOOP_Y1 = 0x82;
constexpr size_t LOOP_Y2 = 0x83;
// Bit 5 enables the biquad.
constexpr size_t FILTER_FLAGS = 0x84;
constexpr size_t END_REQUESTED = 0x85;
constexpr size_t LOOP_ADDRESS = 0x88;  // 32 bits
constexpr size_t LOOP_START = 0x8A;    // 32 bits
constexpr size_t BASE_ADDRESS = 0x8C;  // 32 bits
constexpr size_t BIQUAD_B1 = 0xA4;
constexpr size_t BIQUAD_B2 = 0xA5;
constexpr size_t BIQUAD_A1 = 0xA6;
constexpr size_t BIQUAD_A2 = 0xA7;
constexpr size_t LOWPASS_COEF = 0xA8;

constexpr size_t WORDS = 0xC0;
constexpr size_t WRITTEN_WORDS = 0x80;

// The small layout drops the Dolby area and the unused words, so its read-only part starts at
// word 0x40. These are its pieces and where they go in the full layout.
struct Segment
{
  size_t small;
  size_t full;
  size_t words;
};
constexpr Segment SMALL_LAYOUT[] = {
    {0x00, 0x00, 0x18},
    {0x18, 0x30, 0x18},
    {0x30, 0x58, 0x10},
    {0x40, 0x80, 0x40},
};
constexpr size_t SMALL_WORDS = 0x80;
constexpr size_t SMALL_WRITTEN_WORDS = 0x40;
}  // namespace vpb

// Sample sources (VPB SOURCE).
enum : u16
{
  SOURCE_SQUARE = 0,
  SOURCE_SAW = 1,
  SOURCE_SQUARE_QUARTER = 3,
  SOURCE_PATTERN_1 = 4,
  // For the AFC sources, the value is also the size in bytes of a 16-sample block.
  SOURCE_AFC_LQ = 5,
  SOURCE_PATTERN_0 = 7,
  SOURCE_PCM8 = 8,
  SOURCE_AFC_HQ = 9,
  SOURCE_PATTERN_0_MODULATED = 10,
  SOURCE_PATTERN_2 = 11,
  SOURCE_PATTERN_3 = 12,
  SOURCE_PCM16 = 16,
  SOURCE_PCM16_MRAM = 33,
};

constexpr size_t FRAME = 0x50;
constexpr u32 FRAME_BYTES = FRAME * sizeof(s16);
using FrameBuffer = std::array<s16, FRAME>;

constexpr size_t PATTERN_LENGTH = 0x40;

template <typename T>
s16 Saturate16(T value)
{
  return static_cast<s16>(std::clamp<T>(value, -0x8000, 0x7FFF));
}

// Scales a buffer by a fixed-point gain with `shift` fractional bits, saturating.
void ScaleInPlace(FrameBuffer& samples, u16 gain, int shift)
{
  for (s16& sample : samples)
    sample = Saturate16((s32{sample} * gain) >> shift);
}

// dst += src * gain (1.15, saturated per sample; the sum itself wraps).
void MixScaled(s16* dst, const s16* src, size_t count, u16 gain)
{
  for (size_t i = 0; i < count; ++i)
  {
    const s16 scaled = Saturate16((s32{src[i]} * gain) >> 15);
    dst[i] = static_cast<s16>(dst[i] + scaled);
  }
}

// dst += src * gain, with a 16.16 gain moving by `step` each sample. Returns the final gain.
s32 MixRamped(FrameBuffer& dst, const FrameBuffer& src, s32 gain, s32 step)
{
  if (gain == 0 && step == 0)
    return 0;

  u32 current = static_cast<u32>(gain);
  for (size_t i = 0; i < FRAME; ++i)
  {
    const s32 whole = static_cast<s32>(current) >> 16;
    dst[i] = static_cast<s16>(dst[i] + ((whole * src[i]) >> 16));
    current += static_cast<u32>(step);
  }
  return static_cast<s32>(current);
}

// A 16-bit volume in 16.16, and its per-sample ramp over a frame.
s32 ToGain(s32 volume)
{
  return volume * 0x10000;
}
s32 ToStep(s32 volume_change)
{
  return volume_change * 0x10000 / static_cast<s32>(FRAME);
}

// Patterns 2 and 3 evolve from frame to frame.
void AdvancePatterns(std::array<s16, 0x100>& patterns)
{
  s16* const noise = patterns.data() + 2 * PATTERN_LENGTH;
  s32 older = noise[PATTERN_LENGTH - 2];
  s32 newer = noise[PATTERN_LENGTH - 1];
  for (size_t i = 0; i < PATTERN_LENGTH; ++i)
  {
    const s32 input = noise[i];
    const u32 product = static_cast<u32>(older * newer);
    const u32 raised = static_cast<u32>(input * 0x10000);
    // Even entries subtract the input; odd entries add it and double the result.
    const u32 mixed = (i % 2 == 0) ? product - raised : 2u * (product + raised);
    older = newer;
    newer = input;
    noise[i] = static_cast<s16>(static_cast<s32>(mixed) >> 16);
  }

  s16* const ramp = patterns.data() + 3 * PATTERN_LENGTH;
  const s32 last = ramp[PATTERN_LENGTH - 1];
  const s32 before_last = ramp[PATTERN_LENGTH - 2];
  const u32 seed = static_cast<u32>(last * before_last) +
                   static_cast<u32>(before_last * 0x10000) + static_cast<u32>(last);
  const s16 raw_step = static_cast<s16>(ramp[0] + (static_cast<s32>(seed) >> 16));
  const s32 increment = (raw_step & 0x1FF) | 0x2000;
  const s16 origin = static_cast<s16>(last);
  for (size_t i = 0; i < PATTERN_LENGTH; ++i)
    ramp[i] = static_cast<s16>(origin + static_cast<s32>(i + 1) * increment);
}

// Two's complement value of the low `bits` bits of `code`.
s32 SignExtend(u32 code, u32 bits)
{
  const u32 sign = 1u << (bits - 1);
  return static_cast<s32>(code & (sign - 1)) - static_cast<s32>(code & sign);
}

// The reverb blocks, 16 words each.
namespace rpb
{
constexpr size_t MODE = 0;  // 0 = off; bit 0 filters before the sends, bit 1 after
constexpr size_t RING_SLOTS = 1;
constexpr size_t RING_ADDRESS = 2;  // 32 bits
constexpr size_t SENDS = 4;         // 2 x {buffer DSP address, 1.15 volume}
constexpr size_t TAPS = 8;          // 8 filter coefficients
constexpr size_t WORDS = 16;
}  // namespace rpb
}  // namespace

// ---- Voice blocks -----------------------------------------------------------------------------

class ZeldaMixer::Voice
{
public:
  u16 Get(size_t word) const { return m_words[word]; }
  s16 GetSigned(size_t word) const { return static_cast<s16>(m_words[word]); }
  bool Test(size_t word) const { return m_words[word] != 0; }
  u32 Get32(size_t word) const { return (u32{m_words[word]} << 16) | m_words[word + 1]; }

  void Set(size_t word, u32 value) { m_words[word] = static_cast<u16>(value); }
  void SetSigned(size_t word, s32 value) { m_words[word] = static_cast<u16>(value); }
  void Set32(size_t word, u32 value)
  {
    m_words[word] = static_cast<u16>(value >> 16);
    m_words[word + 1] = static_cast<u16>(value);
  }

  std::array<u16, vpb::WORDS>& Words() { return m_words; }

private:
  std::array<u16, vpb::WORDS> m_words{};
};

ZeldaMixer::ZeldaMixer(Core::System& system) : m_system(system)
{
}

void ZeldaMixer::ReadVoice(u16 voice_id, Voice& voice) const
{
  auto& memory = m_system.GetMemory();
  if ((m_flags & FLAG_SMALL_VPB) == 0)
  {
    const u32 address = m_vpb_base + u32{voice_id} * u32{vpb::WORDS * sizeof(u16)};
    memory.CopyFromEmuSwapped(voice.Words().data(), address, vpb::WORDS * sizeof(u16));
    return;
  }

  std::array<u16, vpb::SMALL_WORDS> small{};
  const u32 address = m_vpb_base + u32{voice_id} * u32{vpb::SMALL_WORDS * sizeof(u16)};
  memory.CopyFromEmuSwapped(small.data(), address, small.size() * sizeof(u16));
  for (const vpb::Segment& segment : vpb::SMALL_LAYOUT)
  {
    std::copy_n(small.begin() + static_cast<std::ptrdiff_t>(segment.small), segment.words,
                voice.Words().begin() + static_cast<std::ptrdiff_t>(segment.full));
  }
}

void ZeldaMixer::WriteVoice(u16 voice_id, Voice& voice)
{
  auto& memory = m_system.GetMemory();
  if ((m_flags & FLAG_SMALL_VPB) == 0)
  {
    const u32 address = m_vpb_base + u32{voice_id} * u32{vpb::WORDS * sizeof(u16)};
    memory.CopyToEmuSwapped(address, voice.Words().data(), vpb::WRITTEN_WORDS * sizeof(u16));
    return;
  }

  std::array<u16, vpb::SMALL_WRITTEN_WORDS> small{};
  for (const vpb::Segment& segment : vpb::SMALL_LAYOUT)
  {
    if (segment.small >= vpb::SMALL_WRITTEN_WORDS)
      continue;
    std::copy_n(voice.Words().begin() + static_cast<std::ptrdiff_t>(segment.full), segment.words,
                small.begin() + static_cast<std::ptrdiff_t>(segment.small));
  }
  const u32 address = m_vpb_base + u32{voice_id} * u32{vpb::SMALL_WORDS * sizeof(u16)};
  memory.CopyToEmuSwapped(address, small.data(), small.size() * sizeof(u16));
}

// ---- Tables and buffers -----------------------------------------------------------------------

void ZeldaMixer::LoadTables(u32 tables_addr, u32 afc_addr, bool with_sine_table)
{
  auto& memory = m_system.GetMemory();
  m_resample_coefs.fill(0);
  memory.CopyFromEmuSwapped(m_resample_coefs.data(), tables_addr, sizeof(m_resample_coefs));
  m_patterns.fill(0);
  memory.CopyFromEmuSwapped(m_patterns.data(), tables_addr + 0x200, sizeof(m_patterns));
  // Only Dolby panning uses the sine table, and the light protocol has no Dolby.
  if (with_sine_table)
  {
    m_sine.fill(0);
    memory.CopyFromEmuSwapped(m_sine.data(), tables_addr + 0x400, sizeof(m_sine));
  }
  m_afc_coefs.fill(0);
  memory.CopyFromEmuSwapped(m_afc_coefs.data(), afc_addr, sizeof(m_afc_coefs));
}

ZeldaMixer::MixBuffer* ZeldaMixer::FindBuffer(u16 dsp_address)
{
  static constexpr std::array<std::pair<u16, Buffer>, NUM_BUFFERS> BY_ADDRESS = {{
      {0x0D00, FRONT_LEFT},
      {0x0D60, FRONT_RIGHT},
      {0x0F40, BACK_LEFT},
      {0x0CA0, BACK_RIGHT},
      {0x0E80, FRONT_LEFT_REVERB},
      {0x0EE0, FRONT_RIGHT_REVERB},
      {0x0C00, BACK_LEFT_REVERB},
      {0x0C50, BACK_RIGHT_REVERB},
      {0x0DC0, UNK0_REVERB},
      {0x0E20, UNK1_REVERB},
      {0x09A0, UNK0},  // a reverb send of the GameCube IPL
      {0x0FA0, UNK1},  // a voice output of the GameCube IPL
      {0x0B00, UNK2},  // a voice output of Pikmin
  }};
  for (const auto& [address, buffer] : BY_ADDRESS)
  {
    if (address == dsp_address)
      return &m_buffers[buffer];
  }
  return nullptr;
}

// ---- Frames -----------------------------------------------------------------------------------

void ZeldaMixer::BeginFrame()
{
  if (m_frame_prepared)
    return;

  Buf(FRONT_LEFT).fill(0);
  Buf(FRONT_RIGHT).fill(0);

  // The back channels decay rather than being cleared. (The ucode also filters them into the
  // front channels; no known game needs that.)
  ScaleInPlace(Buf(BACK_LEFT), 0x6784, 15);
  ScaleInPlace(Buf(BACK_RIGHT), 0x6784, 15);

  ReverbInput();

  // Fold the back reverb buffers into the front ones: back left fully, back right only for
  // its second half.
  constexpr size_t HALF = FRAME / 2;
  MixScaled(Buf(FRONT_LEFT_REVERB).data(), Buf(BACK_LEFT_REVERB).data(), FRAME, 0x7FFF);
  MixScaled(Buf(FRONT_RIGHT_REVERB).data(), Buf(BACK_LEFT_REVERB).data(), FRAME, 0xB820);
  MixScaled(Buf(FRONT_LEFT_REVERB).data(), Buf(BACK_RIGHT_REVERB).data() + HALF, HALF, 0xB820);
  MixScaled(Buf(FRONT_RIGHT_REVERB).data(), Buf(BACK_RIGHT_REVERB).data() + HALF, HALF, 0x7FFF);
  Buf(BACK_LEFT_REVERB).fill(0);
  Buf(BACK_RIGHT_REVERB).fill(0);

  AdvancePatterns(m_patterns);

  m_frame_prepared = true;
}

void ZeldaMixer::EndFrame()
{
  // The output volume is 4.12.
  ScaleInPlace(Buf(FRONT_LEFT), m_out_volume, 12);
  ScaleInPlace(Buf(FRONT_RIGHT), m_out_volume, 12);

  auto& memory = m_system.GetMemory();
  memory.CopyToEmuSwapped(m_out_left, Buf(FRONT_LEFT).data(), FRAME_BYTES);
  memory.CopyToEmuSwapped(m_out_right, Buf(FRONT_RIGHT).data(), FRAME_BYTES);
  m_out_left += FRAME_BYTES;
  m_out_right += FRAME_BYTES;

  ReverbOutput();

  m_frame_prepared = false;
}

// ---- Reverb -----------------------------------------------------------------------------------

namespace
{
constexpr size_t REVERB_BLOCKS = 4;

std::array<u16, rpb::WORDS> ReadReverbBlock(const Memory::MemoryManager& memory, u32 base,
                                            size_t index)
{
  std::array<u16, rpb::WORDS> words{};
  const u32 address = base + static_cast<u32>(index * rpb::WORDS * sizeof(u16));
  memory.CopyFromEmuSwapped(words.data(), address, sizeof(words));
  return words;
}

u32 ReverbRingAddress(const std::array<u16, rpb::WORDS>& block, u16 slot)
{
  const u32 ring = (u32{block[rpb::RING_ADDRESS]} << 16) | block[rpb::RING_ADDRESS + 1];
  return ring + u32{slot} * FRAME_BYTES;
}

// 8-tap FIR over `samples` (8 history samples, then the frame), in place: output i replaces
// input i once it is no longer needed.
void FilterReverb(std::array<s16, 8 + FRAME>& samples, const std::array<u16, rpb::WORDS>& block)
{
  for (size_t i = 0; i < FRAME; ++i)
  {
    u32 sum = 0;
    for (size_t tap = 0; tap < 8; ++tap)
    {
      const s32 coef = static_cast<s16>(block[rpb::TAPS + tap]);
      sum += static_cast<u32>(samples[i + tap] * coef);
    }
    samples[i] = Saturate16(static_cast<s32>(sum) >> 15);
  }
}
}  // namespace

void ZeldaMixer::ReverbInput()
{
  if (m_reverb_base == 0)
    return;

  static constexpr Buffer RETURNS[REVERB_BLOCKS] = {UNK0_REVERB, UNK1_REVERB, FRONT_LEFT_REVERB,
                                                    FRONT_RIGHT_REVERB};
  auto& memory = m_system.GetMemory();
  for (size_t index = 0; index < REVERB_BLOCKS; ++index)
  {
    const std::array<u16, rpb::WORDS> block = ReadReverbBlock(memory, m_reverb_base, index);
    const u16 mode = block[rpb::MODE];
    if (mode == 0)
      continue;

    std::array<s16, 8 + FRAME> samples{};
    std::array<s16, 8>& tail = m_reverb_tail[index];
    std::copy(tail.begin(), tail.end(), samples.begin());
    memory.CopyFromEmuSwapped(samples.data() + 8,
                              ReverbRingAddress(block, m_reverb_slot[index]), FRAME_BYTES);
    std::copy_n(samples.begin() + FRAME, tail.size(), tail.begin());

    if ((mode & 1) != 0)
      FilterReverb(samples, block);

    for (size_t send = 0; send < 2; ++send)
    {
      const u16 target_address = block[rpb::SENDS + 2 * send];
      if (target_address == 0)
        continue;
      MixBuffer* target = FindBuffer(target_address);
      if (target == nullptr)
        continue;
      MixScaled(target->data(), samples.data(), FRAME, block[rpb::SENDS + 2 * send + 1]);
    }

    if ((mode & 2) != 0)
      FilterReverb(samples, block);

    std::copy_n(samples.begin(), FRAME, Buf(RETURNS[index]).begin());
  }
}

void ZeldaMixer::ReverbOutput()
{
  if (m_reverb_base == 0)
    return;

  static constexpr Buffer SOURCES[REVERB_BLOCKS] = {UNK0_REVERB, UNK1_REVERB, FRONT_LEFT_REVERB,
                                                    FRONT_RIGHT_REVERB};
  auto& memory = m_system.GetMemory();
  for (size_t index = 0; index < REVERB_BLOCKS; ++index)
  {
    const std::array<u16, rpb::WORDS> block = ReadReverbBlock(memory, m_reverb_base, index);
    if (block[rpb::MODE] == 0)
      continue;

    memory.CopyToEmuSwapped(ReverbRingAddress(block, m_reverb_slot[index]),
                            Buf(SOURCES[index]).data(), FRAME_BYTES);
    const u16 slots = block[rpb::RING_SLOTS];
    // An empty ring (a division by zero for the ucode) stays on slot 0.
    m_reverb_slot[index] = slots != 0 ? static_cast<u16>((m_reverb_slot[index] + 1) % slots) : 0;
  }
}

// ---- Voices -----------------------------------------------------------------------------------

void ZeldaMixer::RenderVoice(u16 voice_id)
{
  Voice voice;
  ReadVoice(voice_id, voice);
  if (!voice.Test(vpb::ENABLED) || voice.Test(vpb::DONE))
    return;

  MixBuffer input{};
  GenerateInput(voice, input);

  const u16 lowpass_coef = voice.Get(vpb::LOWPASS_COEF);
  if (lowpass_coef != 0)
  {
    const bool reset = voice.Test(vpb::RESET);
    s32 y1 = reset ? 0 : voice.GetSigned(vpb::LOWPASS_Y1);
    s32 x1 = reset ? 0 : voice.GetSigned(vpb::LOWPASS_X1);
    for (s16& sample : input)
    {
      const s32 x0 = sample;
      sample = Saturate16<s64>(((s64{x0 - x1} * lowpass_coef) >> 7) + y1);
      y1 = sample;
      x1 = x0;
    }
    voice.SetSigned(vpb::LOWPASS_Y1, y1);
    voice.SetSigned(vpb::LOWPASS_X1, x1);
  }

  // The biquad has no direct term: y[n] = b1 x[n-1] + b2 x[n-2] + a1 y[n-1] + a2 y[n-2]. The
  // ucode skips it for the pass-through setting.
  const s32 b1 = voice.GetSigned(vpb::BIQUAD_B1);
  const s32 b2 = voice.GetSigned(vpb::BIQUAD_B2);
  const s32 a1 = voice.GetSigned(vpb::BIQUAD_A1);
  const s32 a2 = voice.GetSigned(vpb::BIQUAD_A2);
  const bool biquad_on = (voice.Get(vpb::FILTER_FLAGS) & 0x20) != 0;
  if (biquad_on && (a2 != 0 || a1 != 0 || b2 != 0 || b1 != 0x7FFF))
  {
    s32 x1 = voice.GetSigned(vpb::BIQUAD_X1);
    s32 x2 = voice.GetSigned(vpb::BIQUAD_X2);
    s32 y1 = voice.GetSigned(vpb::BIQUAD_Y1);
    s32 y2 = voice.GetSigned(vpb::BIQUAD_Y2);
    for (s16& sample : input)
    {
      const s32 x0 = sample;
      const s64 sum = s64{b1 * x1} + s64{b2 * x2} + s64{a1 * y1} + s64{a2 * y2};
      sample = Saturate16<s64>(sum >> 15);
      x2 = x1;
      x1 = x0;
      y2 = y1;
      y1 = sample;
    }
    voice.SetSigned(vpb::BIQUAD_X1, x1);
    voice.SetSigned(vpb::BIQUAD_X2, x2);
    voice.SetSigned(vpb::BIQUAD_Y1, y1);
    voice.SetSigned(vpb::BIQUAD_Y2, y2);
  }

  if (voice.Test(vpb::USE_DOLBY))
    MixDolby(voice, input);
  else
    MixChannels(voice, input);

  // A voice held on its constant sample stays in reset.
  if (!voice.Test(vpb::USE_CONSTANT))
    voice.Set(vpb::RESET, 0);

  WriteVoice(voice_id, voice);
}

void ZeldaMixer::MixDolby(Voice& voice, const MixBuffer& input)
{
  if (voice.Test(vpb::END_REQUESTED))
  {
    const s16 halved = static_cast<s16>(voice.GetSigned(vpb::DOLBY_CURRENT) / 2);
    voice.SetSigned(vpb::DOLBY_TARGET, halved);
    if (halved == 0)
      voice.Set(vpb::DONE, 1);
  }

  // Quadrant gains from the sine table: X pans right to left, Y back to front.
  const u16 position = voice.Get(vpb::DOLBY_POSITION);
  const size_t x = (position >> 8) & 0x7F;
  const size_t y = position & 0x7F;
  const s32 right = m_sine[x];
  const s32 left = m_sine[x ^ 0x7F];
  const s32 back = m_sine[y];
  const s32 front = m_sine[y ^ 0x7F];
  const int shift = (m_flags & FLAG_LOUD_DOLBY) != 0 ? 15 : 16;

  std::array<s16, 4> gain = {
      static_cast<s16>((left * front) >> shift),
      static_cast<s16>((left * back) >> shift),
      static_cast<s16>((right * front) >> shift),
      static_cast<s16>((right * back) >> shift),
  };

  const s32 current = voice.GetSigned(vpb::DOLBY_CURRENT);
  const s32 change = static_cast<s16>(voice.GetSigned(vpb::DOLBY_TARGET) - current);
  std::array<s16, 4> step{};
  for (size_t q = 0; q < 4; ++q)
  {
    step[q] = static_cast<s16>((s32{static_cast<u16>(gain[q])} * change) >> shift);
    gain[q] = static_cast<s16>((gain[q] * current) >> shift);
  }

  static constexpr Buffer DRY[4] = {FRONT_LEFT, BACK_LEFT, FRONT_RIGHT, BACK_RIGHT};
  static constexpr Buffer WET[4] = {FRONT_LEFT_REVERB, BACK_LEFT_REVERB, FRONT_RIGHT_REVERB,
                                    BACK_RIGHT_REVERB};
  for (size_t q = 0; q < 4; ++q)
    MixRamped(Buf(DRY[q]), input, ToGain(gain[q]), ToStep(step[q]));

  const s32 send = voice.GetSigned(vpb::DOLBY_REVERB);
  for (size_t q = 0; q < 4; ++q)
  {
    const s16 wet_gain = static_cast<s16>((gain[q] * send) >> shift);
    const s16 wet_step = static_cast<s16>((step[q] * send) >> shift);
    MixRamped(Buf(WET[q]), input, ToGain(wet_gain), ToStep(wet_step));
  }

  voice.Set(vpb::DOLBY_CURRENT, voice.Get(vpb::DOLBY_TARGET));
}

void ZeldaMixer::MixChannels(Voice& voice, const MixBuffer& input)
{
  const size_t outputs = (m_flags & FLAG_FOUR_OUTPUTS) != 0 ? 4 : 6;
  auto word = [](size_t output, size_t field) {
    return vpb::OUTPUTS + output * vpb::OUTPUT_WORDS + field;
  };
  constexpr size_t ADDRESS = 0, TARGET = 1, CURRENT = 2;

  // Ending: each output fades by half per frame; the voice is done once all are silent.
  if (voice.Test(vpb::END_REQUESTED))
  {
    bool silent = true;
    for (size_t out = 0; out < outputs; ++out)
    {
      const s16 halved = static_cast<s16>(voice.GetSigned(word(out, CURRENT)) / 2);
      voice.SetSigned(word(out, TARGET), halved);
      silent = silent && halved == 0;
    }
    if (silent)
      voice.Set(vpb::DONE, 1);
  }

  for (size_t out = 0; out < outputs; ++out)
  {
    const u16 address = voice.Get(word(out, ADDRESS));
    if (address == 0)
      continue;

    const s32 target = voice.GetSigned(word(out, TARGET));
    const s32 current = voice.GetSigned(word(out, CURRENT));
    const s32 change =
        (m_flags & FLAG_EXPLICIT_STEP) != 0 ? target : static_cast<s16>(target - current);
    const s32 step = ToStep(change);
    if (current == 0 && step == 0)
      continue;

    MixBuffer* destination = FindBuffer(address);
    if (destination == nullptr)
      continue;

    const s32 final_gain = MixRamped(*destination, input, ToGain(current), step);
    voice.SetSigned(word(out, CURRENT), final_gain >> 16);
  }
}

// ---- Sample sources ---------------------------------------------------------------------------

void ZeldaMixer::GenerateInput(Voice& voice, MixBuffer& out)
{
  if (voice.Test(vpb::USE_CONSTANT))
  {
    out.fill(voice.GetSigned(vpb::CONSTANT_SAMPLE));
    return;
  }

  const u16 source = voice.Get(vpb::SOURCE);
  const u32 ratio = voice.Get(vpb::RATIO);
  const u32 frac = voice.Get(vpb::POSITION_FRAC);

  switch (source)
  {
  case SOURCE_SQUARE:
  case SOURCE_SQUARE_QUARTER:
  {
    // A +-0x4000 square wave; the quarter variant is high for one period in four.
    const u32 shift = source == SOURCE_SQUARE ? 1 : 2;
    const u32 low_mask = (1u << shift) - 1;
    const u32 step = ratio << (shift - 1);
    u32 phase = frac << shift;
    for (s16& sample : out)
    {
      sample = ((phase >> 16) & low_mask) != 0 ? s16{-0x4000} : s16{0x4000};
      phase += step;
    }
    voice.Set(vpb::POSITION_FRAC, (phase >> shift) & 0xFFFF);
    return;
  }

  case SOURCE_SAW:
  {
    u32 phase = frac;
    for (s16& sample : out)
    {
      sample = static_cast<s16>(static_cast<u16>(phase));
      phase += ratio >> 1;
    }
    voice.Set(vpb::POSITION_FRAC, phase & 0xFFFF);
    return;
  }

  case SOURCE_PATTERN_0:
  case SOURCE_PATTERN_0_MODULATED:
  case SOURCE_PATTERN_1:
  case SOURCE_PATTERN_2:
  case SOURCE_PATTERN_3:
  {
    size_t pattern = 0;
    if (source == SOURCE_PATTERN_1)
      pattern = 1;
    else if (source == SOURCE_PATTERN_2)
      pattern = 2;
    else if (source == SOURCE_PATTERN_3)
      pattern = 3;
    const s16* const table = m_patterns.data() + pattern * PATTERN_LENGTH;

    // 6.16 position in the pattern. The modulated variant adds the back right buffer, scaled
    // by the ratio, to the phase.
    constexpr u32 WRAP = PATTERN_LENGTH << 16;
    const u32 step = ratio << 5;
    const s32 modulation_scale = static_cast<s32>(ratio);
    u32 phase = frac << 6;
    for (size_t i = 0; i < FRAME; ++i)
    {
      out[i] = table[phase >> 16];
      phase = (phase + step) % WRAP;
      if (source == SOURCE_PATTERN_0_MODULATED)
      {
        const u32 offset = static_cast<u32>(Buf(BACK_RIGHT)[i] * modulation_scale);
        phase = ((phase << 10) + offset) >> 10;
      }
    }
    voice.Set(vpb::POSITION_FRAC, (phase >> 6) & 0xFFFF);
    return;
  }

  case SOURCE_PCM8:
  case SOURCE_AFC_LQ:
  case SOURCE_AFC_HQ:
  case SOURCE_PCM16:
  case SOURCE_PCM16_MRAM:
  {
    // The 4 raw samples kept from the previous frame, then this frame's: at most 0x50F (a
    // ratio of 0xFFFF with a stray integer part in the fraction), rounded up to a whole AFC
    // block.
    std::array<s16, 4 + 0x500 + 0x10> raw{};
    for (size_t i = 0; i < 4; ++i)
      raw[i] = voice.GetSigned(vpb::HISTORY + i);
    s16* const fresh = raw.data() + 4;
    const u16 needed = static_cast<u16>((frac + FRAME * ratio) >> 12);

    if (source == SOURCE_PCM8)
      FetchPCM<1>(voice, fresh, needed);
    else if (source == SOURCE_PCM16)
      FetchPCM<2>(voice, fresh, needed);
    else if (source == SOURCE_PCM16_MRAM)
      FetchFromMRAM(voice, fresh, needed);
    else
      FetchAFC(voice, fresh, needed);

    Interpolate(voice, raw.data(), out);
    return;
  }

  default:
    PanicAlertFmt("Using an unknown/unimplemented sample source: {:04x}", source);
    out.fill(0);
    return;
  }
}

void ZeldaMixer::Interpolate(Voice& voice, const s16* raw, MixBuffer& out) const
{
  // 20.12 position in `raw`.
  const u32 ratio = voice.Get(vpb::RATIO);
  u32 position = voice.Get(vpb::POSITION_FRAC);

  if ((ratio >> 12) >= 4)
  {
    // Decimating by 4 or more: take the nearest following sample.
    for (s16& sample : out)
    {
      position += ratio;
      sample = raw[position >> 12];
    }
  }
  else
  {
    // 4-tap polyphase filter, 64 phases selected by the top 6 bits of the fraction.
    for (s16& sample : out)
    {
      const s16* const taps = &m_resample_coefs[((position & 0xFFF) >> 6) * 4];
      const s16* const window = raw + (position >> 12);
      s64 sum = 0;
      for (size_t k = 0; k < 4; ++k)
        sum += s64{2} * taps[k] * window[k];
      sample = Saturate16<s64>(sum >> 16);
      position += ratio;
    }
  }

  for (size_t k = 0; k < 4; ++k)
    voice.SetSigned(vpb::HISTORY + k, raw[(position >> 12) + k]);
  voice.SetSigned(vpb::CONSTANT_SAMPLE, out.back());
  voice.Set(vpb::POSITION_FRAC, position & 0xFFF);
}

const u8* ZeldaMixer::ARAMRange(u32 offset, size_t size) const
{
  // The Wii versions read "ARAM" from main memory.
  if (m_system.IsWii())
    return m_system.GetMemory().GetPointerForRange(m_aram_base + offset, size);

  const auto& dsp = m_system.GetDSP();
  const u32 aram_size = dsp.GetARAMSize();
  if (offset >= aram_size || size > aram_size - offset)
    return nullptr;
  return dsp.GetARAMPtr() + offset;
}

template <size_t BYTES>
void ZeldaMixer::FetchPCM(Voice& voice, s16* dst, u16 count)
{
  if (voice.Test(vpb::DONE))
  {
    std::fill_n(dst, count, s16{0});
    return;
  }

  // (Re)start at POSITION, playing up to LOOP_START; after that, from LOOP_ADDRESS.
  auto seek = [&voice] {
    const u32 position = voice.Get32(vpb::POSITION);
    voice.Set32(vpb::REMAINING, voice.Get32(vpb::LOOP_START) - position);
    voice.Set32(vpb::ARAM_ADDRESS, voice.Get32(vpb::BASE_ADDRESS) + position * u32{BYTES});
  };
  if (voice.Test(vpb::RESET))
    seek();

  voice.Set(vpb::END_REACHED, 0);
  while (count != 0)
  {
    if (voice.Test(vpb::END_REACHED))
    {
      voice.Set(vpb::END_REACHED, 0);
      if (!voice.Test(vpb::LOOPING))
      {
        std::fill_n(dst, count, s16{0});
        voice.Set(vpb::DONE, 1);
        return;
      }
      voice.Set32(vpb::POSITION, voice.Get32(vpb::LOOP_ADDRESS));
      seek();
    }

    const u32 remaining = voice.Get32(vpb::REMAINING);
    const u16 chunk = static_cast<u16>(std::min<u32>(remaining, count));
    const u32 address = voice.Get32(vpb::ARAM_ADDRESS);
    const u8* src = ARAMRange(address, size_t{chunk} * BYTES);
    for (size_t i = 0; i < chunk; ++i)
    {
      if (src == nullptr)
        dst[i] = 0;
      else if constexpr (BYTES == 1)
        dst[i] = static_cast<s16>(static_cast<s8>(src[i]) * 0x100);
      else
        dst[i] = static_cast<s16>((src[2 * i] << 8) | src[2 * i + 1]);
    }
    dst += chunk;

    voice.Set32(vpb::REMAINING, remaining - chunk);
    voice.Set32(vpb::ARAM_ADDRESS, address + u32{chunk} * u32{BYTES});
    count = static_cast<u16>(count - chunk);
    if (remaining == chunk)
      voice.Set(vpb::END_REACHED, 1);
  }
}

void ZeldaMixer::DecodeAFC(Voice& voice, s16* dst, size_t blocks)
{
  const u16 block_bytes = voice.Get(vpb::SOURCE);
  const bool four_bit = block_bytes == SOURCE_AFC_HQ;
  const u32 address = voice.Get32(vpb::ARAM_ADDRESS);
  const u8* src = ARAMRange(address, blocks * block_bytes);
  voice.Set32(vpb::ARAM_ADDRESS, address + static_cast<u32>(blocks) * block_bytes);

  if (src == nullptr)
  {
    std::fill_n(dst, blocks * 16, s16{0});
    return;
  }

  s32 y1 = voice.GetSigned(vpb::AFC_Y1);
  s32 y2 = voice.GetSigned(vpb::AFC_Y2);
  for (size_t block = 0; block < blocks; ++block)
  {
    // Header: scale exponent, predictor index. 1 << 15 wraps to -32768 like on the DSP.
    const u8 header = src[0];
    const s32 scale = static_cast<s16>(1 << (header >> 4));
    const size_t predictor = (header & 0xFu) * 2;
    const s32 c1 = m_afc_coefs[predictor];
    const s32 c2 = m_afc_coefs[predictor + 1];
    const u8* const codes = src + 1;

    for (size_t i = 0; i < 16; ++i)
    {
      // Codes are scaled to 1.15 halves: 4-bit codes by 2^11, 2-bit codes by 2^13.
      s32 code;
      if (four_bit)
        code = SignExtend(u32{codes[i / 2]} >> (4 - 4 * (i % 2)), 4) * 0x800;
      else
        code = SignExtend(u32{codes[i / 4]} >> (6 - 2 * (i % 4)), 2) * 0x2000;

      const u32 sum = static_cast<u32>(scale * code) + static_cast<u32>(y1 * c1) +
                      static_cast<u32>(y2 * c2);
      const s32 sample = std::clamp(static_cast<s32>(sum) >> 11, -0x8000, 0x7FFF);
      *dst++ = static_cast<s16>(sample);
      y2 = y1;
      y1 = sample;
    }
    src += block_bytes;

    voice.SetSigned(vpb::AFC_Y2, y2);
    voice.SetSigned(vpb::AFC_Y1, y1);
  }
}

void ZeldaMixer::FetchAFC(Voice& voice, s16* dst, u16 count)
{
  if (voice.Test(vpb::RESET))
  {
    voice.Set(vpb::AFC_Y1, 0);
    voice.Set(vpb::AFC_Y2, 0);
    voice.Set(vpb::AFC_CACHED, 0);
    voice.Set32(vpb::REMAINING, voice.Get32(vpb::LOOP_START));
    voice.Set32(vpb::ARAM_ADDRESS, voice.Get32(vpb::BASE_ADDRESS));
  }

  if (voice.Test(vpb::DONE))
  {
    std::fill_n(dst, count, s16{0});
    return;
  }

  if (voice.Get(vpb::AFC_CACHED) > 16)
  {
    ERROR_LOG_FMT(DSPHLE, "Zelda: AFC voice claims more than 16 cached samples");
    voice.Set(vpb::AFC_CACHED, 16);
  }

  for (;;)
  {
    // The cached samples are the last `cached` ones of the stored block.
    const u16 cached = voice.Get(vpb::AFC_CACHED);
    const u16 from_cache = std::min(cached, count);
    const size_t cache_start = vpb::AFC_SAMPLES + 16 - cached;
    for (size_t i = 0; i < from_cache; ++i)
      *dst++ = voice.GetSigned(cache_start + i);
    voice.Set(vpb::AFC_CACHED, u32{cached} - from_cache);
    count = static_cast<u16>(count - from_cache);
    if (count == 0)
      return;

    const u32 remaining = voice.Get32(vpb::REMAINING);
    if (count <= remaining)
    {
      // Decode whole blocks; what is decoded beyond `count` stays cached.
      const u16 blocks = static_cast<u16>((count + 0xF) >> 4);
      const u16 decoded = static_cast<u16>(blocks << 4);
      if (decoded < remaining)
      {
        voice.Set(vpb::AFC_CACHED, u32{decoded} - count);
        voice.Set32(vpb::REMAINING, remaining - decoded);
      }
      else
      {
        voice.Set(vpb::AFC_CACHED, remaining - count);
        voice.Set32(vpb::REMAINING, 0);
      }
      DecodeAFC(voice, dst, blocks);

      const u16 left_over = voice.Get(vpb::AFC_CACHED);
      if (left_over != 0)
      {
        for (size_t i = 0; i < 16; ++i)
          voice.SetSigned(vpb::AFC_SAMPLES + i, dst[decoded - 16 + i]);

        // At the end of a looping sound, shift the cached samples to line up with the loop
        // (the ucode copies downwards in place, so the order of the copies matters).
        const u32 loop_start = voice.Get32(vpb::LOOP_START);
        if (voice.Get32(vpb::REMAINING) == 0 && loop_start != 0)
        {
          const size_t from = vpb::AFC_SAMPLES + ((loop_start + 0xF) & 0xF);
          for (size_t i = 0; i < left_over; ++i)
            voice.Set(vpb::AFC_SAMPLES + 15 - i, voice.Get(from - i));
        }
      }
      return;
    }

    // Not enough left: play out the rest, then stop or loop.
    if (remaining != 0)
    {
      count = static_cast<u16>(count - remaining);
      DecodeAFC(voice, dst, (remaining + 0xF) >> 4);
      dst += remaining;
    }

    if (!voice.Test(vpb::LOOPING))
    {
      voice.Set(vpb::DONE, 1);
      std::fill_n(dst, count, s16{0});
      return;
    }

    // Jump to the block holding the loop point, with the predictor history the game gave for
    // it, and cache that block from the loop point on.
    const u32 loop_address = voice.Get32(vpb::LOOP_ADDRESS);
    voice.Set32(vpb::ARAM_ADDRESS,
                voice.Get32(vpb::BASE_ADDRESS) + (loop_address >> 4) * voice.Get(vpb::SOURCE));
    voice.Set(vpb::AFC_Y2, voice.Get(vpb::LOOP_Y2));
    voice.Set(vpb::AFC_Y1, voice.Get(vpb::LOOP_Y1));

    std::array<s16, 16> block{};
    DecodeAFC(voice, block.data(), 1);
    for (size_t i = 0; i < block.size(); ++i)
      voice.SetSigned(vpb::AFC_SAMPLES + i, block[i]);

    const u32 now_cached = 16 - (loop_address & 0xF);
    voice.Set(vpb::AFC_CACHED, now_cached);
    voice.Set32(vpb::REMAINING, voice.Get32(vpb::LOOP_START) - now_cached - loop_address);
  }
}

void ZeldaMixer::FetchFromMRAM(Voice& voice, s16* dst, u16 count)
{
  // Only the high word of POSITION is used here, as a sample offset from BASE_ADDRESS.
  auto& memory = m_system.GetMemory();
  const u16 position = voice.Get(vpb::POSITION);
  const u32 address = voice.Get32(vpb::BASE_ADDRESS) + u32{position} * 2;
  const u32 remaining = voice.Get32(vpb::REMAINING);

  if (count > remaining)
  {
    // The end: copy what is left, then repeat the last sample. The ucode writes the repeats
    // from the start of the buffer, over the samples it just copied.
    s16 last = 0;
    if (remaining != 0)
    {
      memory.CopyFromEmuSwapped(dst, address, remaining * sizeof(s16));
      last = dst[remaining - 1];
    }
    std::fill_n(dst, count - remaining, last);
    voice.Set(vpb::POSITION, position + remaining);
    voice.Set32(vpb::REMAINING, 0);
    voice.Set(vpb::DONE, 1);
    return;
  }

  voice.Set32(vpb::REMAINING, remaining - count);
  const u16 before_loop = static_cast<u16>(voice.Get(vpb::LOOP_START) - position);
  voice.Set(vpb::SAMPLES_BEFORE_LOOP, before_loop);
  if (count <= before_loop)
  {
    memory.CopyFromEmuSwapped(dst, address, size_t{count} * sizeof(s16));
    voice.Set(vpb::POSITION, u32{position} + count);
    return;
  }

  // Crossing the loop point: the rest comes from LOOP_ADDRESS, which becomes the new base.
  memory.CopyFromEmuSwapped(dst, address, size_t{before_loop} * sizeof(s16));
  const u32 loop_address = voice.Get32(vpb::LOOP_ADDRESS);
  voice.Set32(vpb::BASE_ADDRESS, loop_address);
  const u16 after_loop = static_cast<u16>(count - before_loop);
  voice.Set(vpb::POSITION, after_loop);
  memory.CopyFromEmuSwapped(dst + before_loop, loop_address, size_t{after_loop} * sizeof(s16));
}

void ZeldaMixer::DoState(PointerWrap& p)
{
  p.Do(m_flags);
  p.Do(m_frame_prepared);
  p.Do(m_out_left);
  p.Do(m_out_right);
  p.Do(m_out_volume);
  p.Do(m_buffers);
  p.Do(m_resample_coefs);
  p.Do(m_patterns);
  p.Do(m_sine);
  p.Do(m_afc_coefs);
  p.Do(m_aram_base);
  p.Do(m_vpb_base);
  p.Do(m_reverb_base);
  p.Do(m_reverb_slot);
  p.Do(m_reverb_tail);
}

// ---- The ucode --------------------------------------------------------------------------------

ZeldaUCode::ZeldaUCode(DSPHLE* dsphle, u32 crc)
    : UCodeInterface(dsphle, crc), m_mixer(dsphle->GetSystem())
{
  const auto* version = std::find_if(std::begin(VERSIONS), std::end(VERSIONS),
                                     [crc](const VersionInfo& v) { return v.crc == crc; });
  if (version == std::end(VERSIONS))
    PanicAlertFmt("No flags definition found for Zelda CRC {:08x}", crc);
  else
    m_flags = version->flags;
  m_mixer.SetFlags(m_flags);

  INFO_LOG_FMT(DSPHLE, "Zelda ucode {:08x} started (flags {:08x})", crc, m_flags);
}

void ZeldaUCode::Initialize()
{
  if ((m_flags & FLAG_LIGHT) != 0)
  {
    m_mail_handler.PushMail(0x88881111);
    return;
  }
  m_mail_handler.PushMail(DSP_INIT, true);
  m_mail_handler.PushMail(0xF3551111);
}

void ZeldaUCode::Update()
{
  if (NeedsResumeMail())
    m_mail_handler.PushMail(DSP_RESUME, true);
}

void ZeldaUCode::DoState(PointerWrap& p)
{
  p.Do(m_flags);
  p.Do(m_mail_state);
  p.Do(m_expected_mails);
  p.Do(m_voice_limit);
  p.Do(m_voice_mask);
  p.Do(m_sync_second_half);
  p.Do(m_queue);
  p.Do(m_queue_read);
  p.Do(m_queue_write);
  p.Do(m_queued_commands);
  p.Do(m_commands_enabled);
  p.Do(m_frames_requested);
  p.Do(m_voices_per_frame);
  p.Do(m_frame);
  p.Do(m_voice);
  m_mixer.DoState(p);
  DoStateShared(p);
}

void ZeldaUCode::HandleMail(u32 mail)
{
  if (m_upload_setup_in_progress)
  {
    PrepareBootUCode(mail);
    return;
  }

  if ((m_flags & FLAG_LIGHT) != 0)
    OnLightMail(mail);
  else
    OnMail(mail);
}

// Standard protocol: in the idle state, a mail with the MSB set is a task mail, a mail with a
// zero low half starts the sync mails of a rendering pass, and anything else announces a
// command of that many mails.
void ZeldaUCode::OnMail(u32 mail)
{
  switch (m_mail_state)
  {
  case MailState::Idle:
    if ((mail & 0x80000000) != 0)
    {
      OnTaskMail(mail);
    }
    else if ((mail & 0xFFFF) == 0)
    {
      if (Rendering())
      {
        m_mail_state = MailState::Rendering;
      }
      else
      {
        NOTICE_LOG_FMT(DSPHLE, "Zelda: sync mail {:08x} while not rendering; halting", mail);
        m_mail_state = MailState::Halted;
      }
    }
    else
    {
      m_mail_state = MailState::ReceivingCommand;
      m_expected_mails = mail & 0xFFFF;
    }
    return;

  case MailState::Rendering:
    if ((m_flags & FLAG_FRAME_SYNC) != 0)
    {
      // Two mails, each with the enable bits of 32 voices; the second releases the frame.
      const bool second = m_sync_second_half;
      const size_t first_group = second ? 2 : 0;
      m_voice_mask[first_group] = static_cast<u16>(mail >> 16);
      m_voice_mask[first_group + 1] = static_cast<u16>(mail);
      if (second)
        m_voice_limit = 0xFFFF;
      ContinueRendering();
      if (second)
        m_mail_state = MailState::Idle;
      m_sync_second_half = !second;
    }
    else
    {
      // One mail per 16 voices: the group in the high half, its enable bits in the low half.
      const u32 group = (mail >> 16) & 0xFF;
      m_voice_limit = (((mail >> 16) & 0xF) + 1) * 16;
      m_voice_mask[group] = static_cast<u16>(mail);
      ContinueRendering();
      m_mail_state = MailState::Idle;
    }
    return;

  case MailState::ReceivingCommand:
    QueuePush(mail);
    if (--m_expected_mails == 0)
    {
      ++m_queued_commands;
      m_mail_state = MailState::Idle;
      ExecuteQueued();
    }
    return;

  case MailState::Halted:
    WARN_LOG_FMT(DSPHLE, "Zelda: ignoring mail {:08x} while halted", mail);
    return;
  }
}

void ZeldaUCode::OnTaskMail(u32 mail)
{
  // The ucode only looks at the low half.
  if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP)
  {
    WARN_LOG_FMT(DSPHLE, "Zelda: task mail {:08x} lacks the CDD1 prefix", mail);
    mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);
  }

  switch (mail)
  {
  case MAIL_NEW_UCODE:
    m_commands_enabled = true;
    ExecuteQueued();
    NOTICE_LOG_FMT(DSPHLE, "Zelda: switching to another ucode");
    m_upload_setup_in_progress = true;
    m_mail_state = MailState::Idle;
    return;

  case MAIL_RESET:
    NOTICE_LOG_FMT(DSPHLE, "Zelda: rebooting into the ROM");
    m_mail_state = MailState::Halted;
    // Destroys this object.
    m_dsphle->SetUCode(UCODE_ROM);
    return;

  case MAIL_CONTINUE:
    m_commands_enabled = true;
    ExecuteQueued();
    return;

  case MAIL_RESUME:
    NOTICE_LOG_FMT(DSPHLE, "Zelda: halting on request");
    m_mail_state = MailState::Halted;
    return;

  default:
    NOTICE_LOG_FMT(DSPHLE, "Zelda: unknown task mail {:08x}; halting", mail);
    m_mail_state = MailState::Halted;
    return;
  }
}

// Light protocol: every command starts with a mail that has the MSB set; the number of
// parameter mails is implied by the command.
void ZeldaUCode::OnLightMail(u32 mail)
{
  switch (m_mail_state)
  {
  case MailState::Idle:
  {
    if ((mail & 0x80000000) == 0)
      PanicAlertFmt("Mail received in waiting state has MSB=0: {:08x}", mail);

    QueuePush(mail);
    bool counts_as_command = true;
    switch ((mail >> 24) & 0x7F)
    {
    case 0x00:
      m_expected_mails = 0;
      break;
    case 0x01:
      m_expected_mails = 4;
      break;
    case 0x02:
      m_expected_mails = 2;
      break;
    case 0x03:
      // Goes straight back to the dispatcher (seen on 4BE6A5CB and 42F64AC4).
      counts_as_command = false;
      break;
    case 0x0C:
      if ((m_flags & FLAG_GBA_0C) != 0)
        m_expected_mails = 1;
      else if ((m_flags & FLAG_TWO_ARG_0C) != 0)
        m_expected_mails = 2;
      else
        m_expected_mails = 0;
      break;
    default:
      PanicAlertFmt("Received unknown command in light protocol: {:08x}", mail);
      break;
    }

    if (m_expected_mails != 0)
    {
      m_mail_state = MailState::ReceivingCommand;
    }
    else if (counts_as_command)
    {
      ++m_queued_commands;
      ExecuteQueued();
    }
    return;
  }

  case MailState::ReceivingCommand:
    QueuePush(mail);
    if (--m_expected_mails == 0)
    {
      ++m_queued_commands;
      m_mail_state = MailState::Idle;
      ExecuteQueued();
    }
    return;

  case MailState::Rendering:
    // A single sync mail per frame, enabling every voice.
    if (mail != 0)
      PanicAlertFmt("Sync mail is not zero: {:08x}", mail);
    m_voice_limit = 0xFFFFFFFF;
    m_voice_mask.fill(0xFFFF);
    ContinueRendering();
    m_dsphle->GetSystem().GetDSP().GenerateDSPInterruptFromDSPEmu(INT_DSP);
    return;

  case MailState::Halted:
    WARN_LOG_FMT(DSPHLE, "Zelda: ignoring mail {:08x} while halted", mail);
    return;
  }
}

void ZeldaUCode::QueuePush(u32 word)
{
  m_queue[m_queue_write] = word;
  m_queue_write = (m_queue_write + 1) % static_cast<u32>(m_queue.size());
}

u32 ZeldaUCode::QueuePop()
{
  if (m_queue_read == m_queue_write)
  {
    ERROR_LOG_FMT(DSPHLE, "Zelda: command needs more parameters than were sent");
    return 0;
  }
  const u32 word = m_queue[m_queue_read];
  m_queue_read = (m_queue_read + 1) % static_cast<u32>(m_queue.size());
  return word;
}

void ZeldaUCode::ExecuteQueued()
{
  // Commands wait while a rendering pass runs and until the CPU acknowledges its end.
  if (Rendering() || !m_commands_enabled)
    return;

  while (m_queued_commands != 0)
  {
    // Words without the MSB are not command starts and are skipped. (As in the ucode, this
    // spins forever if the queue holds no command start.)
    const u32 header = QueuePop();
    if ((header & 0x80000000) == 0)
      continue;
    --m_queued_commands;
    if (!Execute(header))
      return;
  }
}

bool ZeldaUCode::Execute(u32 header)
{
  const u32 command = (header >> 24) & 0x7F;
  const u16 sync = static_cast<u16>(header >> 16);
  const bool light = (m_flags & FLAG_LIGHT) != 0;

  switch (command)
  {
  case 0x00:
  case 0x0A:  // unused; does something on the IPL ucode
  case 0x0B:  // unused; does something on both IPL ucodes
  case 0x0F:
    NOTICE_LOG_FMT(DSPHLE, "Zelda: no-op command {:02x}", command);
    SendSync(sync);
    return true;

  case 0x01:
  {
    // Setup: voice count, VPB base, the table block, the AFC coefficients and the reverb
    // blocks.
    m_voices_per_frame = static_cast<u16>(header);
    const u32 vpb_base = QueuePop();
    const u32 tables = QueuePop();
    const u32 afc_coefs = QueuePop();
    const u32 reverb_base = QueuePop();
    m_mixer.SetVoiceBlocksAddress(vpb_base);
    m_mixer.LoadTables(tables, afc_coefs, !light);
    m_mixer.SetReverbBlocksAddress(reverb_base);
    SendSync(sync);
    return true;
  }

  case 0x02:
  {
    // Render: frame count and output volume, then the two output buffers. Rendering takes
    // over the mail flow until it is done.
    m_frames_requested = (header >> 16) & 0xFF;
    const u32 left = QueuePop();
    const u32 right = QueuePop();
    m_mixer.SetOutput(left, right, static_cast<u16>(header));
    if ((m_flags & FLAG_RENDER_HAS_0D) != 0)
    {
      // Command 0D's parameters (Pikmin on the Wii); not used.
      QueuePop();
      QueuePop();
    }
    m_frame = 0;
    m_voice = 0;
    if (light)
    {
      SendSync(static_cast<u16>(m_frames_requested));
      m_mail_state = MailState::Rendering;
    }
    else
    {
      ContinueRendering();
    }
    return false;
  }

  case 0x03:
    // The light protocol never queues this one.
    if (light)
    {
      PanicAlertFmt("Received a 03 command on light protocol.");
      return true;
    }
    SendSync(sync);
    return true;

  case 0x04:
  case 0x05:
  case 0x06:
  case 0x07:
  case 0x08:
  case 0x09:
    // These crash the standard ucode.
    NOTICE_LOG_FMT(DSPHLE, "Zelda: command {:02x} would crash the ucode; halting", command);
    m_mail_state = MailState::Halted;
    return false;

  case 0x0C:
    if ((m_flags & FLAG_GBA_0C) != 0)
    {
      const u32 address = QueuePop();
      ProcessGBACrypto(m_dsphle->GetSystem().GetMemory(), address);
    }
    else if ((m_flags & FLAG_TWO_ARG_0C) != 0)
    {
      const u32 first = QueuePop();
      const u32 second = QueuePop();
      NOTICE_LOG_FMT(DSPHLE, "Zelda: unemulated command 0C ({:08x}, {:08x})", first, second);
    }
    else
    {
      WARN_LOG_FMT(DSPHLE, "Zelda: command 0C does nothing here (flags {:08x})", m_flags);
    }
    SendSync(sync);
    return true;

  case 0x0D:
    if ((m_flags & FLAG_IGNORE_0D) != 0)
    {
      WARN_LOG_FMT(DSPHLE, "Zelda: command 0D does nothing on this ucode");
    }
    else
    {
      // Used by The Wind Waker; purpose unknown.
      const u32 parameter = QueuePop();
      WARN_LOG_FMT(DSPHLE, "Zelda: unemulated command 0D ({:08x})", parameter);
    }
    SendSync(sync);
    return true;

  case 0x0E:
    // The Wii's replacement for ARAM.
    if ((m_flags & FLAG_NO_ARAM) == 0)
      PanicAlertFmt("Setting base ARAM addr on non Wii DAC.");
    m_mixer.SetARAMBaseAddress(QueuePop());
    SendSync(sync);
    return true;

  default:
    NOTICE_LOG_FMT(DSPHLE, "Zelda: unknown command {:02x}; halting", command);
    m_mail_state = MailState::Halted;
    return false;
  }
}

void ZeldaUCode::SendSync(u16 sync)
{
  if ((m_flags & FLAG_LIGHT) != 0)
  {
    // The light protocol answers with the DSP address of the command's handler.
    const u32 handler = 2 * ((u32{sync} >> 8) & 0x7F) + 0x62;
    m_mail_handler.PushMail(0x80000000 | handler);
    return;
  }
  m_mail_handler.PushMail(DSP_SYNC, true);
  m_mail_handler.PushMail(0xF3550000 | sync);
}

void ZeldaUCode::SendFrameEnd()
{
  m_mail_handler.PushMail(DSP_FRAME_END, true);
}

void ZeldaUCode::ContinueRendering()
{
  if (!Rendering())
  {
    WARN_LOG_FMT(DSPHLE, "Zelda: asked to render with no rendering pass in progress");
    return;
  }

  const u32 voices = std::min<u32>(m_voices_per_frame, static_cast<u32>(m_voice_mask.size() * 16));
  while (m_frame < m_frames_requested)
  {
    if (m_voice == 0)
      m_mixer.BeginFrame();

    for (; m_voice < voices; ++m_voice)
    {
      // Voices past the synced limit wait for the next sync mail.
      if (m_voice >= m_voice_limit)
        return;
      const u16 enable_bits = m_voice_mask[m_voice / 16];
      if (((enable_bits >> (15 - m_voice % 16)) & 1) != 0)
        m_mixer.RenderVoice(static_cast<u16>(m_voice));
    }

    if ((m_flags & FLAG_LIGHT) == 0)
      SendSync(static_cast<u16>(0xFF00 | m_frame));
    m_mixer.EndFrame();

    m_voice = 0;
    m_voice_limit = 0;
    ++m_frame;
  }

  if ((m_flags & FLAG_LIGHT) == 0)
  {
    SendFrameEnd();
    m_commands_enabled = false;
  }
  else
  {
    m_mail_state = MailState::Idle;
  }
}
}  // namespace DSP::HLE
