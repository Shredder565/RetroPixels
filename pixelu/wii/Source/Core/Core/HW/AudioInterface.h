// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

class PointerWrap;
namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}
namespace MMIO
{
class Mapping;
}

// The audio interface (AI, 0x0C006C00). It owns the sample rate settings of
// the two audio paths: DMA audio from RAM (AID, played by the DSP code) and
// disc streaming audio (AIS), whose stereo sample counter can raise an
// interrupt when it reaches a programmed value.
namespace AudioInterface
{
enum class SampleRate
{
  AI32KHz,
  AI48KHz,
};

class AudioInterfaceManager
{
public:
  AudioInterfaceManager(Core::System& system);
  AudioInterfaceManager(const AudioInterfaceManager&) = delete;
  AudioInterfaceManager(AudioInterfaceManager&&) = delete;
  AudioInterfaceManager& operator=(const AudioInterfaceManager&) = delete;
  AudioInterfaceManager& operator=(AudioInterfaceManager&&) = delete;
  ~AudioInterfaceManager();

  void Init();
  void Shutdown();
  void DoState(PointerWrap& p);
  // Whether disc streaming is running (and the sample counter counting).
  bool IsPlaying() const;

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  // Sample rate divisors: the rate is Mixer::FIXED_SAMPLE_RATE_DIVIDEND
  // divided by these.
  u32 GetAIDSampleRateDivisor() const;
  u32 GetAISSampleRateDivisor() const;

  // The rates selected in the control register. On the GameCube the real
  // rates are a little above their names.
  SampleRate GetAIDSampleRate() const;
  SampleRate GetAISSampleRate() const;

  u32 Get32KHzSampleRateDivisor() const;
  u32 Get48KHzSampleRateDivisor() const;

  void GenerateAISInterrupt();

private:
  void UpdateInterrupts();
  void GenerateAudioInterrupt();
  void IncreaseSampleCount(const u32 amount);
  int GetAIPeriod() const;
  void SetAIDSampleRate(SampleRate sample_rate);
  void SetAISSampleRate(SampleRate sample_rate);
  void RestartCounterEvent();
  void WriteControl(u32 value);

  void Update(u64 userdata, s64 cycles_late);
  static void GlobalUpdate(Core::System& system, u64 userdata, s64 cycles_late);

  bool ControlBit(u32 bit) const { return (m_control & bit) != 0; }
  void SetControlBit(u32 bit, bool on) { m_control = on ? (m_control | bit) : (m_control & ~bit); }

  // Registers.
  u32 m_control = 0;  // AICR
  u32 m_volume = 0;   // AIVR: left volume in bits 0-7, right in bits 8-15
  u32 m_sample_counter = 0;
  u32 m_interrupt_timing = 0;

  // CPU time up to which streamed samples have been counted.
  u64 m_last_cpu_time = 0;
  u64 m_cpu_cycles_per_sample = 0;

  u32 m_ais_sample_rate_divisor = 0;
  u32 m_aid_sample_rate_divisor = 0;

  CoreTiming::EventType* m_event_type_ai = nullptr;

  Core::System& m_system;
};
}  // namespace AudioInterface
