// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/HSP/HSP_DeviceGBPlayer.h"

#include <algorithm>
#include <climits>
#include <cstring>

#if defined(HAS_LIBMGBA)
#include "Core/HW/GBACore.h"

#include <mgba/internal/gba/gba.h>
#endif

#include "Common/ChunkFile.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"

#include "Core/Config/MainSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace
{
// The register an access targets is picked by address bits 20-27.
namespace Reg
{
constexpr u8 TEST = 0x10;
constexpr u8 VIDEO = 0x11;
constexpr u8 CONTROL = 0x14;
constexpr u8 SIO_CONTROL = 0x15;
constexpr u8 AUDIO = 0x18;
constexpr u8 SIO_DATA = 0x19;
constexpr u8 KEYPAD = 0x1c;
constexpr u8 IRQ = 0x1d;
}  // namespace Reg

u8 RegisterOf(u32 address)
{
  return static_cast<u8>(address >> 20);
}

// Control register bits. Bits 5-7 (sleep, link cable, link enable) are
// stored but have no effect here.
constexpr u8 CTRL_GB_CART = 0x01;
constexpr u8 CTRL_CART_PRESENT = 0x02;
constexpr u8 CTRL_POWER = 0x04 | 0x08;  // 3V and 5V supply
constexpr u8 CTRL_IRQ_MASKED = 0x10;
constexpr u8 CTRL_STORED_BITS = 0xfc;

// Set alongside any pending interrupt bit.
constexpr u16 IRQ_PENDING = 0x8000;

// Selects the 32-byte block within a video or audio region.
constexpr u32 AV_BLOCK_BITS = 0xfe0;

// Fills `data` with copies of a 4-byte pattern.
void RepeatWord(std::span<u8, HSP::TRANSFER_SIZE> data, const std::array<u8, 4>& word)
{
  for (std::size_t offset = 0; offset < data.size(); offset += word.size())
    std::copy(word.begin(), word.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
}

// Stand-in used when the build has no mGBA: no cartridge, nothing to run.
class NoHandheld final : public HSP::IGBPlayer
{
public:
  using IGBPlayer::IGBPlayer;

  void Reset() override {}
  void Stop() override {}
  bool IsLoaded() const override { return false; }
  bool IsGBA() const override { return false; }
  void SetKeys(u16) override {}
  u8 ReadSIOControl() override { return 0; }
  void WriteSIOControl(u8) override {}
  u32 ReadSIOData() override { return 0; }
  void WriteSIOData(u32) override {}
  void DoState(PointerWrap&) override {}
};

}  // namespace

namespace HSP
{

// Interrupt sources, numbered 0-5; source n owns bit 2 * n of the IRQ register.
enum class CHSPDevice_GBPlayer::IRQ : int { Link, GamePak, Sleep, Serial, Video, Audio };

#if defined(HAS_LIBMGBA)

namespace
{
constexpr std::size_t STEREO = 2;

// The disc wants a block of sound and a few scanlines this often (4096 Hz).
constexpr u32 BLOCK_RATE =
    static_cast<u32>(GBA_ARM7TDMI_FREQUENCY * STEREO / CHAR_BIT / IGBPlayer::AV_REGION_SIZE);
static_assert(BLOCK_RATE == 4096);

constexpr u32 LINES_PER_BLOCK = 4;

// Turns the top bits of an unsigned sample into a 32-bit PWM word whose set
// bits are all leading, which is the only shape the player disc accepts. The
// bits below the top five carry over in `carry` to dither the next word.
constexpr u32 PwmWord(u16 sample, u16& carry)
{
  const u32 total = u32{sample} + carry;
  const u32 ones = total >> 11;
  carry = static_cast<u16>(total & 0x7ff);
  return static_cast<u32>(0xffff'ffff'0000'0000ull >> ones);
}
}  // namespace

// Runs the cartridge in mGBA and turns its output into what the disc reads.
class MgbaHandheld final : public IGBPlayer
{
public:
  MgbaHandheld(Core::System& system, CHSPDevice_GBPlayer* device);
  MgbaHandheld(const MgbaHandheld&) = delete;
  MgbaHandheld& operator=(const MgbaHandheld&) = delete;

  void Reset() override;
  void Stop() override;
  bool IsLoaded() const override { return true; }
  bool IsGBA() const override;
  void SetKeys(u16 keys) override { m_keys = keys; }
  u8 ReadSIOControl() override;
  void WriteSIOControl(u8 value) override;
  u32 ReadSIOData() override;
  void WriteSIOData(u32 value) override;
  void DoState(PointerWrap& p) override;

private:
  // mGBA's stream hooks get a pointer to this; it finds its way back here.
  struct StreamHooks : mAVStream
  {
    MgbaHandheld* owner;
  };

  void ClearRunState();
  void OnTick(s64 cycles_late);
  void PushAudioBlock();
  void PushVideoAndReschedule(s64 cycles_late);
  void CopyNextScanlines();
  void CopyNextAudioBlock();
  void DrainSamples();

  HW::GBA::Core m_core;
  StreamHooks m_hooks{};
  CoreTiming::EventType* m_tick_event = nullptr;

  // PWM bytes waiting for the disc, as a ring.
  std::array<u8, AV_REGION_SIZE * 4> m_ring{};
  u16 m_ring_read = 0;
  u16 m_ring_write = 0;

  // Which of the BLOCK_RATE slots in the current second comes next.
  u16 m_block_slot = 0;
  // First scanline of the next video block; mGBA sets it to 0 when a new
  // frame is complete.
  u16 m_next_line = GBA_VIDEO_VERTICAL_PIXELS;
  s64 m_frame_start = 0;
  u16 m_keys = 0;
  u16 m_left_carry = 0;
  u16 m_right_carry = 0;
  // log2 of PWM bits per sample (at least 5); follows mGBA's sample rate.
  u8 m_pwm_shift = 9;
};

MgbaHandheld::MgbaHandheld(Core::System& system, CHSPDevice_GBPlayer* device)
    : IGBPlayer(system, device), m_core(m_system, Config::GBPLAYER_GBA_INDEX)
{
  m_tick_event = m_system.GetCoreTiming().RegisterEvent(
      "GBPmGBAUpdate", [this](Core::System&, u64, s64 cycles_late) { OnTick(cycles_late); });

  m_hooks.owner = this;
  m_hooks.audioRateChanged = [](mAVStream* stream, unsigned rate) {
    MgbaHandheld& self = *static_cast<StreamHooks*>(stream)->owner;
    self.DrainSamples();
    INFO_LOG_FMT(HSP, "GBPlayer: mGBA sample rate is now {}", rate);
    self.m_pwm_shift = static_cast<u8>(24 - MathUtil::IntLog2(rate));
  };
  m_hooks.postAudioBuffer = [](mAVStream* stream, mAudioBuffer*) {
    static_cast<StreamHooks*>(stream)->owner->DrainSamples();
  };
}

void MgbaHandheld::Reset()
{
  Stop();

  m_core.Start(m_system.GetCoreTiming().GetTicks());
  if (!m_core.IsStarted())
    return;  // missing BIOS or ROM

  mCore* const core = m_core.GetCore();
  core->setAVStream(core, &m_hooks);

  mCoreCallbacks hooks{};
  hooks.context = this;
  hooks.videoFrameEnded = [](void* context) {
    static_cast<MgbaHandheld*>(context)->m_next_line = 0;
  };
  core->addCoreCallbacks(core, &hooks);

  PushVideoAndReschedule(0);
}

void MgbaHandheld::ClearRunState()
{
  m_ring_read = 0;
  m_ring_write = 0;
  m_block_slot = 0;
  m_next_line = GBA_VIDEO_VERTICAL_PIXELS;
  m_frame_start = 0;
  m_keys = 0;
  m_left_carry = 0;
  m_right_carry = 0;
  m_pwm_shift = 9;
}

void MgbaHandheld::Stop()
{
  if (!m_core.IsStarted())
    return;

  m_core.Stop();
  m_system.GetCoreTiming().RemoveEvent(m_tick_event);
  ClearRunState();
}

bool MgbaHandheld::IsGBA() const
{
  if (!m_core.IsStarted())
    return false;
  return m_core.GetPlatform() == mPLATFORM_GBA;
}

void MgbaHandheld::CopyNextScanlines()
{
  const std::span<const u32> frame = m_core.GetVideoBuffer();
  const std::span<u16> out = m_device->GetScanlineData();

  const std::size_t first = std::size_t{m_next_line} * GBA_VIDEO_HORIZONTAL_PIXELS;
  for (std::size_t px = 0; px < GBA_VIDEO_HORIZONTAL_PIXELS * LINES_PER_BLOCK; ++px)
    out[px] = static_cast<u16>(M_RGB8_TO_RGB5(frame[first + px]));

  // The top bit of the first pixel marks the start of a frame.
  if (m_next_line == 0)
    out[0] |= 0x8000;

  m_next_line += LINES_PER_BLOCK;
}

void MgbaHandheld::CopyNextAudioBlock()
{
  const std::span<u8> out = m_device->GetAudioData();
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = m_ring[(m_ring_read + i) % m_ring.size()];
  m_ring_read = static_cast<u16>((m_ring_read + out.size()) % m_ring.size());
}

void MgbaHandheld::DrainSamples()
{
  mAudioBuffer* const source = m_core.GetAudioBuffer();
  const u32 words_per_sample = 1u << (m_pwm_shift - 5u);

  std::array<s16, 256 * STEREO> chunk;
  for (;;)
  {
    const std::size_t frames = mAudioBufferRead(source, chunk.data(), chunk.size() / STEREO);
    if (frames == 0)
      return;

    for (std::size_t f = 0; f < frames; ++f)
    {
      // Signed PCM to offset binary.
      const u16 left = static_cast<u16>(chunk[f * STEREO] ^ 0x8000);
      const u16 right = static_cast<u16>(chunk[f * STEREO + 1] ^ 0x8000);
      for (u32 w = 0; w < words_per_sample; ++w)
      {
        const u32 left_word = PwmWord(left, m_left_carry);
        const u32 right_word = PwmWord(right, m_right_carry);
        // Bytes interleave left/right, most significant first.
        for (int shift = 24; shift >= 0; shift -= 8)
        {
          m_ring[m_ring_write] = static_cast<u8>(left_word >> shift);
          m_ring[m_ring_write + 1u] = static_cast<u8>(right_word >> shift);
          m_ring_write = static_cast<u16>((m_ring_write + 2u) % m_ring.size());
        }
      }
    }
  }
}

void MgbaHandheld::OnTick(s64 cycles_late)
{
  m_core.Flush();
  PushAudioBlock();
  PushVideoAndReschedule(cycles_late);
}

void MgbaHandheld::PushAudioBlock()
{
  DrainSamples();

  const std::size_t queued = (m_ring_write + m_ring.size() - m_ring_read) % m_ring.size();
  if (queued < AV_REGION_SIZE)
  {
    // mGBA fell behind; skip this block rather than send stale sound.
    WARN_LOG_FMT(HSP, "GBPlayer: only {} of {} audio bytes ready", queued, AV_REGION_SIZE);
    return;
  }

  CopyNextAudioBlock();
  m_device->AssertIRQ(CHSPDevice_GBPlayer::IRQ::Audio);
}

void MgbaHandheld::PushVideoAndReschedule(s64 cycles_late)
{
  auto& timing = m_system.GetCoreTiming();
  const s64 now = static_cast<s64>(timing.GetTicks());
  const s64 gc_rate = m_system.GetSystemTimers().GetTicksPerSecond();

  if (m_next_line == 0)
    m_frame_start = now;

  // Send the next lines once the GBA would have drawn them. Video rides on
  // the audio block timing; the disc misbehaves if the two are paced apart.
  if (m_next_line < GBA_VIDEO_VERTICAL_PIXELS)
  {
    const s64 line_start_gba = s64{VIDEO_HORIZONTAL_LENGTH} * m_next_line;
    if ((now - m_frame_start) * GBA_ARM7TDMI_FREQUENCY >= line_start_gba * gc_rate)
    {
      CopyNextScanlines();
      m_device->AssertIRQ(CHSPDevice_GBPlayer::IRQ::Video);
    }
  }

  // Slot boundaries are computed from the slot number so that rounding never
  // accumulates.
  const s64 slot_begin = gc_rate * m_block_slot / BLOCK_RATE;
  const s64 slot_end = gc_rate * (m_block_slot + 1) / BLOCK_RATE;
  m_block_slot = static_cast<u16>((m_block_slot + 1) % BLOCK_RATE);
  const s64 slot_length = slot_end - slot_begin;

  m_core.SyncJoybus(static_cast<u64>(now + slot_length), m_keys);
  timing.ScheduleEvent(slot_length - cycles_late, m_tick_event);
}

u8 MgbaHandheld::ReadSIOControl()
{
  DEBUG_LOG_FMT(HSP, "GBPlayer: serial control read (not emulated)");
  return 0;
}

void MgbaHandheld::WriteSIOControl(u8 value)
{
  DEBUG_LOG_FMT(HSP, "GBPlayer: serial control write 0x{:02x} (not emulated)", value);
}

u32 MgbaHandheld::ReadSIOData()
{
  DEBUG_LOG_FMT(HSP, "GBPlayer: serial data read (not emulated)");
  return 0;
}

void MgbaHandheld::WriteSIOData(u32 value)
{
  DEBUG_LOG_FMT(HSP, "GBPlayer: serial data write 0x{:08x} (not emulated)", value);
}

void MgbaHandheld::DoState(PointerWrap& p)
{
  m_core.DoState(p);
  p.Do(m_block_slot);
  p.Do(m_next_line);
  p.Do(m_frame_start);
  p.Do(m_keys);
  p.Do(m_left_carry);
  p.Do(m_right_carry);
  p.Do(m_ring);
  p.Do(m_ring_read);
  p.Do(m_ring_write);
  p.Do(m_pwm_shift);
}

#endif  // HAS_LIBMGBA

CHSPDevice_GBPlayer::CHSPDevice_GBPlayer(Core::System& system) : m_system(system)
{
#if defined(HAS_LIBMGBA)
  m_handheld = std::make_unique<MgbaHandheld>(m_system, this);
#else
  m_handheld = std::make_unique<NoHandheld>(m_system, this);
#endif
}

void CHSPDevice_GBPlayer::Read(u32 address, std::span<u8, TRANSFER_SIZE> data)
{
  const u8 reg = RegisterOf(address);
  const std::size_t block_entry = (address & AV_BLOCK_BITS) / 4;

  if (reg == Reg::TEST)
  {
    std::copy(m_test_block.begin(), m_test_block.end(), data.begin());
  }
  else if (reg == Reg::CONTROL)
  {
    // The cartridge bits reflect the handheld side on every read.
    if (!m_handheld->IsLoaded())
      m_control &= static_cast<u8>(~(CTRL_GB_CART | CTRL_CART_PRESENT));
    else if (m_handheld->IsGBA())
      m_control = static_cast<u8>((m_control | CTRL_CART_PRESENT) & ~CTRL_GB_CART);
    else
      m_control |= CTRL_CART_PRESENT | CTRL_GB_CART;
    std::fill(data.begin(), data.end(), m_control);
  }
  else if (reg == Reg::IRQ)
  {
    // Big-endian word: the high byte of the flags is mirrored into bits
    // 16-23 and 24-31, and the pending bit is hidden in its own position.
    const u8 hi = static_cast<u8>(m_irq_flags >> 8);
    RepeatWord(data, {hi, hi, static_cast<u8>(hi & 0x7f), static_cast<u8>(m_irq_flags)});
  }
  else if (reg == Reg::VIDEO)
  {
    // One pixel per word, each byte doubled.
    for (std::size_t i = 0; i < TRANSFER_SIZE / 4; ++i)
    {
      const u16 pixel = m_video_lines[block_entry + i];
      const u8 hi = static_cast<u8>(pixel >> 8);
      const u8 lo = static_cast<u8>(pixel);
      const std::array<u8, 4> word{hi, hi, lo, lo};
      std::copy(word.begin(), word.end(), data.subspan(4 * i, 4).begin());
    }
  }
  else if (reg == Reg::AUDIO)
  {
    // One PWM byte per word, repeated.
    for (std::size_t i = 0; i < TRANSFER_SIZE / 4; ++i)
      {
      const std::span<u8> word = data.subspan(4 * i, 4);
      std::fill(word.begin(), word.end(), m_pwm_audio[block_entry + i]);
    }
  }
  else if (reg == Reg::SIO_CONTROL)
  {
    std::fill(data.begin(), data.end(), m_handheld->ReadSIOControl());
  }
  else if (reg == Reg::SIO_DATA)
  {
    // Host byte order, as the value is handed over.
    const u32 value = m_handheld->ReadSIOData();
    std::array<u8, 4> word;
    std::memcpy(word.data(), &value, word.size());
    RepeatWord(data, word);
  }
  else
  {
    WARN_LOG_FMT(HSP, "GBPlayer: read of unhandled address 0x{:08x}", address);
  }
}

void CHSPDevice_GBPlayer::Write(u32 address, std::span<const u8, TRANSFER_SIZE> data)
{
  // Byte registers live in the last byte of the block, halfwords and words
  // end there.
  const u8 last = data[TRANSFER_SIZE - 1];

  switch (RegisterOf(address))
  {
  case Reg::TEST:
    // Read back inverted.
    std::transform(data.begin(), data.end(), m_test_block.begin(),
                   [](u8 byte) { return static_cast<u8>(byte ^ 0xff); });
    break;

  case Reg::CONTROL:
  {
    const bool was_powered = (m_control & CTRL_POWER) != 0;
    const bool powered = (last & CTRL_POWER) != 0;
    if (powered && !was_powered)
    {
      INFO_LOG_FMT(HSP, "GBPlayer: power on, resetting the handheld");
      m_handheld->Reset();
    }
    if (was_powered && !powered)
    {
      INFO_LOG_FMT(HSP, "GBPlayer: power off, stopping the handheld");
      m_handheld->Stop();
    }
    m_control = last & CTRL_STORED_BITS;
    RefreshInterruptLine();
    break;
  }

  case Reg::IRQ:
  {
    // Writing 1 acknowledges a bit.
    const u16 acknowledged = static_cast<u16>((data[TRANSFER_SIZE - 2] << 8) | last);
    m_irq_flags &= static_cast<u16>(~acknowledged);
    RefreshInterruptLine();
    break;
  }

  case Reg::KEYPAD:
  {
    // Low byte holds most buttons; L and R arrive swapped in the high byte.
    const u8 shoulders = data[TRANSFER_SIZE - 2];
    const u16 l_button = static_cast<u16>((shoulders & 0x02) << 7);
    const u16 r_button = static_cast<u16>((shoulders & 0x01) << 9);
    m_handheld->SetKeys(static_cast<u16>(r_button | l_button | last));
    break;
  }

  case Reg::SIO_CONTROL:
    m_handheld->WriteSIOControl(last);
    break;

  case Reg::SIO_DATA:
    m_handheld->WriteSIOData(Common::swap32(&data[TRANSFER_SIZE - 4]));
    break;

  default:
    WARN_LOG_FMT(HSP, "GBPlayer: write to unhandled address 0x{:08x}", address);
    break;
  }
}

void CHSPDevice_GBPlayer::DoState(PointerWrap& p)
{
  // Builds with and without mGBA save different data; refuse to mix them.
#if defined(HAS_LIBMGBA)
  constexpr u32 HAS_MGBA = 1;
#else
  constexpr u32 HAS_MGBA = 0;
#endif
  p.DoMarker("Have mGBA", HAS_MGBA);

  p.Do(m_test_block);
  p.Do(m_control);
  p.Do(m_irq_flags);
  p.Do(m_video_lines);
  p.Do(m_pwm_audio);

  m_handheld->DoState(p);
}

void CHSPDevice_GBPlayer::AssertIRQ(IRQ irq)
{
  const auto source = static_cast<u32>(irq);
  const u16 bit = static_cast<u16>(1u << (2 * source));

  if (m_irq_flags & bit)
    WARN_LOG_FMT(HSP, "GBPlayer: IRQ {} raised again before it was acknowledged", source);

  m_irq_flags |= bit | IRQ_PENDING;
  RefreshInterruptLine();
}

void CHSPDevice_GBPlayer::RefreshInterruptLine()
{
  const bool masked = (m_control & CTRL_IRQ_MASKED) != 0;
  const bool pending = (m_irq_flags & IRQ_PENDING) != 0;
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_HSP,
                                                pending && !masked);
}

IGBPlayer::IGBPlayer(Core::System& system, CHSPDevice_GBPlayer* device)
    : m_system(system), m_device(device)
{
}

IGBPlayer::~IGBPlayer() = default;

}  // namespace HSP
