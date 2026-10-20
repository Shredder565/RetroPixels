// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceMic.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>

#include <cubeb/cubeb.h>

#include "AudioCommon/CubebUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace ExpansionInterface
{
namespace
{
// Commands.
constexpr int MIC_GET_ID = 0x00;
constexpr int MIC_GET_BUFFER = 0x20;
constexpr int MIC_GET_STATUS = 0x40;
constexpr int MIC_SET_STATUS = 0x80;

// The EXI ID bytes (0x0a000000 as a word).
constexpr std::array<u8, 5> MIC_ID = {0x00, 0x0a, 0x00, 0x00, 0x00};

// Status word bits.
constexpr u16 STATUS_BUTTON = 1u << 8;
constexpr u16 STATUS_OVERFLOW = 1u << 9;
constexpr u16 STATUS_ACTIVE = 1u << 15;
constexpr u32 SampleRateField(u16 status)
{
  return (status >> 11) & 3;
}
constexpr u32 RingSizeField(u16 status)
{
  return (status >> 13) & 3;
}

constexpr int BASE_SAMPLE_RATE = 11025;
constexpr u32 BASE_RING_BYTES = 32;
constexpr u32 BYTES_PER_SAMPLE = sizeof(s16);

// The host-side buffer holds this many console blocks, enough to ride out scheduling hiccups
// without adding much delay.
constexpr std::size_t CAPTURE_BLOCKS = 500;

void IgnoreStateChange(cubeb_stream*, void*, cubeb_state)
{
}
}  // namespace

CEXIMic::CEXIMic(Core::System& system, int index)
    : IEXIDevice(system), m_slot(index)
#ifdef _WIN32
      ,
      m_work_queue("Mic Worker")
#endif
{
  m_sample_rate = BASE_SAMPLE_RATE;
  m_ring_bytes = BASE_RING_BYTES;
  m_ring_samples = m_ring_bytes / BYTES_PER_SAMPLE;

#ifdef _WIN32
  // cubeb's WASAPI backend wants a multithreaded COM apartment on the thread that calls it.
  m_work_queue.PushBlocking([this] {
    const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    m_coinit_success = result == S_OK;
    m_should_couninit = result == S_OK || result == S_FALSE;
  });
#endif

  OpenContext();
}

CEXIMic::~CEXIMic()
{
  CloseContext();

#ifdef _WIN32
  if (m_should_couninit)
  {
    m_work_queue.PushBlocking([this] {
      m_should_couninit = false;
      CoUninitialize();
    });
  }
  m_coinit_success = false;
#endif
}

bool CEXIMic::RunAudioTask(const std::function<void()>& task, bool needs_com)
{
#ifdef _WIN32
  if (needs_com && !m_coinit_success)
    return false;
  m_work_queue.PushBlocking(task);
#else
  (void)needs_com;
  task();
#endif
  return true;
}

void CEXIMic::OpenContext()
{
  m_capture.clear();
  m_capture_available = m_capture_write = m_capture_read = 0;
  RunAudioTask([this] { m_cubeb_ctx = CubebUtils::GetContext(); }, true);
}

void CEXIMic::CloseContext()
{
  StopCapture();
  if (m_cubeb_ctx)
    RunAudioTask([this] { m_cubeb_ctx.reset(); }, true);
}

long CEXIMic::OnCapturedFrames(cubeb_stream*, void* user_data, const void* input_buffer, void*,
                               long nframes)
{
  CEXIMic* const mic = static_cast<CEXIMic*>(user_data);
  const s16* const samples = static_cast<const s16*>(input_buffer);

  std::lock_guard guard(mic->m_capture_lock);
  const std::size_t capacity = mic->m_capture.size();
  for (long frame = 0; frame < nframes; ++frame)
  {
    mic->m_capture[mic->m_capture_write] = samples[frame];
    mic->m_capture_write = (mic->m_capture_write + 1) % capacity;
  }

  // If the console fell behind by more than the whole buffer, start over and tell it.
  mic->m_capture_available += static_cast<std::size_t>(nframes);
  if (mic->m_capture_available > capacity)
  {
    mic->m_capture_available = 0;
    mic->m_status |= STATUS_OVERFLOW;
  }

  return nframes;
}

void CEXIMic::StartCapture()
{
  if (!m_cubeb_ctx)
    return;

  RunAudioTask(
      [this] {
        m_capture.assign(m_ring_samples * CAPTURE_BLOCKS, 0);

        cubeb_stream_params params{};
        params.format = CUBEB_SAMPLE_S16LE;
        params.rate = static_cast<u32>(m_sample_rate);
        params.channels = 1;
        params.layout = CUBEB_LAYOUT_MONO;

        u32 min_latency = 0;
        if (cubeb_get_min_latency(m_cubeb_ctx.get(), &params, &min_latency) != CUBEB_OK)
          WARN_LOG_FMT(EXPANSIONINTERFACE, "Error getting minimum latency");

        const u32 latency = std::max<u32>(m_ring_samples, min_latency);
        if (cubeb_stream_init(m_cubeb_ctx.get(), &m_cubeb_stream,
                              "Dolphin Emulated GameCube Microphone", nullptr, &params, nullptr,
                              nullptr, latency, OnCapturedFrames, IgnoreStateChange,
                              this) != CUBEB_OK)
        {
          ERROR_LOG_FMT(EXPANSIONINTERFACE, "Error initializing cubeb stream");
          return;
        }

        if (cubeb_stream_start(m_cubeb_stream) != CUBEB_OK)
        {
          ERROR_LOG_FMT(EXPANSIONINTERFACE, "Error starting cubeb stream");
          return;
        }

        INFO_LOG_FMT(EXPANSIONINTERFACE, "started cubeb stream");
      },
      true);
}

void CEXIMic::StopCapture()
{
  if (m_cubeb_stream)
  {
    RunAudioTask(
        [this] {
          if (cubeb_stream_stop(m_cubeb_stream) != CUBEB_OK)
            ERROR_LOG_FMT(EXPANSIONINTERFACE, "Error stopping cubeb stream");
          cubeb_stream_destroy(m_cubeb_stream);
          m_cubeb_stream = nullptr;
        },
        false);
  }

  m_capture_available = m_capture_write = m_capture_read = 0;
  m_capture.clear();
}

void CEXIMic::CopyCapturedBlock()
{
  // Hand over one block when a whole one is waiting; otherwise the old block is read again.
  std::lock_guard guard(m_capture_lock);
  if (m_capture_available < m_ring_samples)
    return;

  std::memcpy(m_ring.data(), &m_capture[m_capture_read], m_ring_bytes);
  m_capture_available -= m_ring_samples;
  m_capture_read = (m_capture_read + m_ring_samples) % m_capture.size();
}

bool CEXIMic::IsPresent() const
{
  return true;
}

void CEXIMic::SetCS(int cs)
{
  // Selecting the mic starts a new command; deselecting it has no effect we model.
  if (cs != 0)
    m_position = 0;
}

void CEXIMic::ScheduleNextInterrupt()
{
  // One interrupt per filled ring buffer.
  const int delay = static_cast<int>(m_system.GetSystemTimers().GetTicksPerSecond() /
                                     static_cast<u32>(m_sample_rate) * m_ring_samples);
  m_next_irq_tick = m_system.GetCoreTiming().GetTicks() + static_cast<u64>(delay);
  m_system.GetExpansionInterface().ScheduleUpdateInterrupts(CoreTiming::FromThread::CPU, delay);
}

bool CEXIMic::IsInterruptSet()
{
  if (m_next_irq_tick == 0 || m_system.GetCoreTiming().GetTicks() < m_next_irq_tick)
    return false;

  // Due: report it and arm the next one while sampling continues.
  if ((m_status & STATUS_ACTIVE) != 0)
    ScheduleNextInterrupt();
  else
    m_next_irq_tick = 0;
  return true;
}

void CEXIMic::ReadStatusByte(u32 index, u8& byte)
{
  if (index == 0)
  {
    // Sample the button as the high byte goes out.
    if (Pad::GetMicButton(m_slot))
      m_status |= STATUS_BUTTON;
    else
      m_status &= static_cast<u16>(~STATUS_BUTTON);
  }

  if (index < 2)
    byte = static_cast<u8>(index == 0 ? m_status >> 8 : m_status);
  else
    byte = 0;

  // Reading the low byte acknowledges an overflow.
  if (index == 1)
    m_status &= static_cast<u16>(~STATUS_OVERFLOW);
}

void CEXIMic::WriteStatusByte(u32 index, u8 byte)
{
  if (index >= 2)
    return;

  const bool was_active = (m_status & STATUS_ACTIVE) != 0;
  if (index == 0)
    m_status = static_cast<u16>((m_status & 0x00ff) | (byte << 8));
  else
    m_status = static_cast<u16>((m_status & 0xff00) | byte);
  const bool is_active = (m_status & STATUS_ACTIVE) != 0;

  // The rate and ring size bits share the high byte with the active bit, so they are complete
  // by the time sampling can start.
  if (!was_active && is_active)
  {
    m_sample_rate = BASE_SAMPLE_RATE << SampleRateField(m_status);
    m_ring_bytes = BASE_RING_BYTES << RingSizeField(m_status);
    m_ring_samples = m_ring_bytes / BYTES_PER_SAMPLE;
    ScheduleNextInterrupt();
    StartCapture();
  }
  else if (was_active && !is_active)
  {
    StopCapture();
  }
}

u8 CEXIMic::NextBufferByte()
{
  // A new block is fetched when reading starts over at the front. Samples are little-endian
  // s16 but go out high byte first.
  if (m_ring_pos == 0)
    CopyCapturedBlock();
  const u8 value = m_ring[m_ring_pos ^ 1];
  m_ring_pos = (m_ring_pos + 1) % m_ring_bytes;
  return value;
}

void CEXIMic::TransferByte(u8& byte)
{
  if (m_position == 0)
  {
    m_command = byte;
    byte = 0xFF;
    m_position = 1;
    return;
  }

  const u32 index = m_position - 1;
  switch (m_command)
  {
  case MIC_GET_ID:
    byte = index < MIC_ID.size() ? MIC_ID[index] : 0;
    break;
  case MIC_GET_STATUS:
    ReadStatusByte(index, byte);
    break;
  case MIC_SET_STATUS:
    WriteStatusByte(index, byte);
    break;
  case MIC_GET_BUFFER:
    byte = NextBufferByte();
    break;
  default:
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "EXI MIC: unknown command byte {:02x}", m_command);
    break;
  }

  ++m_position;
}
}  // namespace ExpansionInterface
