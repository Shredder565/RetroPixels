// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Audio paths through the AI:
//  - DMA audio: the DSP code moves samples from RAM to the mixer at the AID
//    rate (handled in DSP.cpp; only the rate lives here).
//  - Streaming audio: the drive interface feeds disc audio at the AIS rate;
//    its stereo sample counter is emulated here from elapsed CPU time.

#include "Core/HW/AudioInterface.h"

#include <algorithm>

#include "AudioCommon/AudioCommon.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/SystemTimers.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace AudioInterface
{
namespace
{
// Register offsets.
constexpr u32 AI_CONTROL_REGISTER = 0x6C00;
constexpr u32 AI_VOLUME_REGISTER = 0x6C04;
constexpr u32 AI_SAMPLE_COUNTER = 0x6C08;
constexpr u32 AI_INTERRUPT_TIMING = 0x6C0C;

// Control register bits.
constexpr u32 CR_PSTAT = 1u << 0;     // streaming on
constexpr u32 CR_AISFR = 1u << 1;     // streaming rate: 1 = 48 kHz, 0 = 32 kHz
constexpr u32 CR_AIINTMSK = 1u << 2;  // interrupt enable
constexpr u32 CR_AIINT = 1u << 3;     // interrupt pending (write 1 to clear)
constexpr u32 CR_AIINTVLD = 1u << 4;  // interrupt timing valid
constexpr u32 CR_SCRESET = 1u << 5;   // write 1 to reset the sample counter
constexpr u32 CR_AIDFR = 1u << 6;     // DMA rate: 1 = 32 kHz, 0 = 48 kHz

u32 LeftVolume(u32 volume)
{
  return volume & 0xFF;
}
u32 RightVolume(u32 volume)
{
  return (volume >> 8) & 0xFF;
}
}  // namespace

AudioInterfaceManager::AudioInterfaceManager(Core::System& system)
    : m_ais_sample_rate_divisor(Mixer::FIXED_SAMPLE_RATE_DIVIDEND / 48000),
      m_aid_sample_rate_divisor(Mixer::FIXED_SAMPLE_RATE_DIVIDEND / 32000), m_system(system)
{
}

AudioInterfaceManager::~AudioInterfaceManager() = default;

void AudioInterfaceManager::DoState(PointerWrap& p)
{
  p.Do(m_control);
  p.Do(m_volume);
  p.Do(m_sample_counter);
  p.Do(m_interrupt_timing);
  p.Do(m_last_cpu_time);
  p.Do(m_ais_sample_rate_divisor);
  p.Do(m_aid_sample_rate_divisor);
  p.Do(m_cpu_cycles_per_sample);

  m_system.GetSoundStream()->GetMixer()->DoState(p);
}

void AudioInterfaceManager::Init()
{
  m_control = 0;
  SetAISSampleRate(SampleRate::AI48KHz);
  SetAIDSampleRate(SampleRate::AI32KHz);
  m_volume = 0;
  m_sample_counter = 0;
  m_interrupt_timing = 0;
  m_last_cpu_time = 0;

  m_event_type_ai = m_system.GetCoreTiming().RegisterEvent("AICallback", GlobalUpdate);
}

void AudioInterfaceManager::Shutdown()
{
}

// --- Interrupt and sample counter ---------------------------------------------

void AudioInterfaceManager::UpdateInterrupts()
{
  const bool active = ControlBit(CR_AIINT) && ControlBit(CR_AIINTMSK);
  m_system.GetProcessorInterface().SetInterrupt(ProcessorInterface::INT_CAUSE_AI, active);
}

void AudioInterfaceManager::GenerateAudioInterrupt()
{
  SetControlBit(CR_AIINT, true);
  UpdateInterrupts();
}

void AudioInterfaceManager::GenerateAISInterrupt()
{
  GenerateAudioInterrupt();
}

// Advances the counter and fires the interrupt if the programmed value was
// passed, i.e. lies in (old counter, new counter] modulo 2^32.
void AudioInterfaceManager::IncreaseSampleCount(const u32 amount)
{
  if (!IsPlaying())
    return;

  const u32 first_new = m_sample_counter + 1;
  m_sample_counter += amount;

  if (m_interrupt_timing - first_new <= m_sample_counter - first_new)
  {
    DEBUG_LOG_FMT(AUDIO_INTERFACE, "AI interrupt: counter {:08x}, timing {:08x}, PC {:08x}",
                  m_sample_counter, m_interrupt_timing, m_system.GetPPCState().pc);
    GenerateAudioInterrupt();
  }
}

// Cycles until the next counter update: when the interrupt value is due, but
// at least once per second of streamed audio.
int AudioInterfaceManager::GetAIPeriod() const
{
  const u64 until_interrupt = m_cpu_cycles_per_sample * (m_interrupt_timing - m_sample_counter);
  const u64 one_second =
      m_cpu_cycles_per_sample * Mixer::FIXED_SAMPLE_RATE_DIVIDEND / m_ais_sample_rate_divisor;
  if (until_interrupt == 0)
    return static_cast<int>(one_second);
  return static_cast<int>(std::min(until_interrupt, one_second));
}

void AudioInterfaceManager::RestartCounterEvent()
{
  auto& timing = m_system.GetCoreTiming();
  timing.RemoveEvent(m_event_type_ai);
  timing.ScheduleEvent(GetAIPeriod(), m_event_type_ai);
}

void AudioInterfaceManager::GlobalUpdate(Core::System& system, u64 userdata, s64 cycles_late)
{
  system.GetAudioInterface().Update(userdata, cycles_late);
}

void AudioInterfaceManager::Update(u64, s64 cycles_late)
{
  // Stopping streaming ends the event chain.
  if (!IsPlaying())
    return;

  auto& timing = m_system.GetCoreTiming();
  const u64 elapsed = timing.GetTicks() - m_last_cpu_time;
  if (elapsed > m_cpu_cycles_per_sample)
  {
    const u32 samples = static_cast<u32>(elapsed / m_cpu_cycles_per_sample);
    m_last_cpu_time += samples * m_cpu_cycles_per_sample;
    IncreaseSampleCount(samples);
  }
  timing.ScheduleEvent(GetAIPeriod() - cycles_late, m_event_type_ai);
}

// --- Sample rates -----------------------------------------------------------------

void AudioInterfaceManager::SetAIDSampleRate(SampleRate sample_rate)
{
  const bool is_32khz = sample_rate == SampleRate::AI32KHz;
  SetControlBit(CR_AIDFR, is_32khz);
  m_aid_sample_rate_divisor = is_32khz ? Get32KHzSampleRateDivisor() : Get48KHzSampleRateDivisor();
  m_system.GetSoundStream()->GetMixer()->SetDMAInputSampleRateDivisor(m_aid_sample_rate_divisor);
}

void AudioInterfaceManager::SetAISSampleRate(SampleRate sample_rate)
{
  const bool is_48khz = sample_rate == SampleRate::AI48KHz;
  SetControlBit(CR_AISFR, is_48khz);
  m_ais_sample_rate_divisor = is_48khz ? Get48KHzSampleRateDivisor() : Get32KHzSampleRateDivisor();

  const u64 cpu_clock = m_system.GetSystemTimers().GetTicksPerSecond();
  m_cpu_cycles_per_sample =
      cpu_clock * m_ais_sample_rate_divisor / Mixer::FIXED_SAMPLE_RATE_DIVIDEND;
  m_system.GetSoundStream()->GetMixer()->SetStreamInputSampleRateDivisor(
      m_ais_sample_rate_divisor);
}

bool AudioInterfaceManager::IsPlaying() const
{
  return ControlBit(CR_PSTAT);
}

u32 AudioInterfaceManager::GetAIDSampleRateDivisor() const
{
  return m_aid_sample_rate_divisor;
}

u32 AudioInterfaceManager::GetAISSampleRateDivisor() const
{
  return m_ais_sample_rate_divisor;
}

SampleRate AudioInterfaceManager::GetAIDSampleRate() const
{
  return ControlBit(CR_AIDFR) ? SampleRate::AI32KHz : SampleRate::AI48KHz;
}

SampleRate AudioInterfaceManager::GetAISSampleRate() const
{
  return ControlBit(CR_AISFR) ? SampleRate::AI48KHz : SampleRate::AI32KHz;
}

// The "48 kHz" clock is 54 MHz / 1124 on the GameCube and 54 MHz / 1125 on
// the Wii (dividend 108 MHz, hence the factor 2); 32 kHz is two thirds of it.
u32 AudioInterfaceManager::Get32KHzSampleRateDivisor() const
{
  return Get48KHzSampleRateDivisor() * 3 / 2;
}

u32 AudioInterfaceManager::Get48KHzSampleRateDivisor() const
{
  return (m_system.IsWii() ? 1125 : 1124) * 2;
}

// --- Registers ----------------------------------------------------------------------

// Only the setting bits are stored; AIINT and SCRESET are actions.
void AudioInterfaceManager::WriteControl(u32 value)
{
  const auto changed = [&](u32 bit) { return ((value ^ m_control) & bit) != 0; };

  if (changed(CR_AIINTMSK))
    SetControlBit(CR_AIINTMSK, value & CR_AIINTMSK);
  if (changed(CR_AIINTVLD))
    SetControlBit(CR_AIINTVLD, value & CR_AIINTVLD);

  if (changed(CR_AISFR))
    SetAISSampleRate((value & CR_AISFR) ? SampleRate::AI48KHz : SampleRate::AI32KHz);
  if (changed(CR_AIDFR))
    SetAIDSampleRate((value & CR_AIDFR) ? SampleRate::AI32KHz : SampleRate::AI48KHz);

  auto& timing = m_system.GetCoreTiming();
  if (changed(CR_PSTAT))
  {
    DEBUG_LOG_FMT(AUDIO_INTERFACE, "Streaming {}", (value & CR_PSTAT) ? "started" : "stopped");
    SetControlBit(CR_PSTAT, value & CR_PSTAT);
    m_last_cpu_time = timing.GetTicks();
    RestartCounterEvent();
  }

  if (value & CR_AIINT)
    SetControlBit(CR_AIINT, false);

  if (value & CR_SCRESET)
  {
    m_sample_counter = 0;
    m_last_cpu_time = timing.GetTicks();
  }

  UpdateInterrupts();
}

void AudioInterfaceManager::RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
  mmio->Register(base | AI_CONTROL_REGISTER, MMIO::DirectRead<u32>(&m_control),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   system.GetAudioInterface().WriteControl(val);
                 }));

  mmio->Register(base | AI_VOLUME_REGISTER, MMIO::DirectRead<u32>(&m_volume),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ai = system.GetAudioInterface();
                   ai.m_volume = val;
                   system.GetSoundStream()->GetMixer()->SetStreamingVolume(LeftVolume(val),
                                                                          RightVolume(val));
                 }));

  // The counter is derived from elapsed time while streaming. When stopped,
  // the stored timestamp itself is used in place of the elapsed time
  // (long-standing behaviour, kept).
  mmio->Register(base | AI_SAMPLE_COUNTER, MMIO::ComplexRead<u32>([](Core::System& system, u32) {
                   auto& ai = system.GetAudioInterface();
                   const u64 cycles = ai.IsPlaying() ?
                                          system.GetCoreTiming().GetTicks() - ai.m_last_cpu_time :
                                          ai.m_last_cpu_time;
                   return ai.m_sample_counter +
                          static_cast<u32>(cycles / ai.m_cpu_cycles_per_sample);
                 }),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ai = system.GetAudioInterface();
                   ai.m_sample_counter = val;
                   ai.m_last_cpu_time = system.GetCoreTiming().GetTicks();
                   ai.RestartCounterEvent();
                 }));

  mmio->Register(base | AI_INTERRUPT_TIMING, MMIO::DirectRead<u32>(&m_interrupt_timing),
                 MMIO::ComplexWrite<u32>([](Core::System& system, u32, u32 val) {
                   auto& ai = system.GetAudioInterface();
                   DEBUG_LOG_FMT(AUDIO_INTERFACE, "AI_INTERRUPT_TIMING <- {:08x}", val);
                   ai.m_interrupt_timing = val;
                   ai.RestartCounterEvent();
                 }));
}
}  // namespace AudioInterface
