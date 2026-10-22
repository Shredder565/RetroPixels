// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}

// The CPU clock and the periodic events that drive the rest of the hardware
// (decrementer, VI, DSP, audio DMA, IOS, GPU sleep, frame patches).
namespace SystemTimers
{
// CPU cycles per time base / decrementer tick. The time base runs at a
// quarter of the bus clock, and the bus clock is a third of the CPU clock.
enum
{
  TIMER_RATIO = 12
};

// A count of time base ticks, converted to CPU ticks.
struct TimeBaseTick
{
  constexpr TimeBaseTick() = default;
  constexpr explicit TimeBaseTick(u64 tb_ticks) : cpu_ticks(tb_ticks * TIMER_RATIO) {}
  constexpr operator u64() const { return cpu_ticks; }

  u64 cpu_ticks = 0;
};

enum class Mode
{
  GC,
  Wii,
};

class SystemTimersManager
{
public:
  explicit SystemTimersManager(Core::System& system);
  SystemTimersManager(const SystemTimersManager& other) = delete;
  SystemTimersManager(SystemTimersManager&& other) = delete;
  SystemTimersManager& operator=(const SystemTimersManager& other) = delete;
  SystemTimersManager& operator=(SystemTimersManager&& other) = delete;
  ~SystemTimersManager();

  // CPU clock: 486 MHz on the GameCube, 729 MHz on the Wii.
  u32 GetTicksPerSecond() const;
  // Sets the clock. Kept apart from Init() because VideoInterface::Init
  // needs the clock and SystemTimers::Init needs VideoInterface.
  void PreInit();
  void Init();
  void Shutdown();
  void ChangePPCClock(Mode mode);

  // Called after the guest writes the decrementer.
  void DecrementerSet();
  u32 GetFakeDecrementer() const;

  // Called after the guest writes the time base.
  void TimeBaseSet();
  u64 GetFakeTimeBase() const;

  // Offset of the configured custom RTC from the host clock, in seconds.
  s64 GetLocalTimeRTCOffset() const;

  // Emulation speed relative to the console over the last emulated second,
  // throttling not included (1.0 = full speed).
  double GetEstimatedEmulationPerformance() const;

private:
  static void DSPCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void AudioDMACallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void IPC_HLE_UpdateCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void GPUSleepCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void VICallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void DecrementerCallback(Core::System& system, u64 userdata, s64 cycles_late);
  static void PatchEngineCallback(Core::System& system, u64 userdata, s64 cycles_late);

  Core::System& m_system;

  u32 m_cpu_core_clock = 486000000u;

  // Cycles between IOS device updates (Wii only).
  int m_ipc_hle_period = 0;

  s64 m_localtime_rtc_offset = 0;

  CoreTiming::EventType* m_event_type_decrementer = nullptr;
  CoreTiming::EventType* m_event_type_vi = nullptr;
  CoreTiming::EventType* m_event_type_audio_dma = nullptr;
  CoreTiming::EventType* m_event_type_dsp = nullptr;
  CoreTiming::EventType* m_event_type_ipc_hle = nullptr;
  CoreTiming::EventType* m_event_type_gpu_sleeper = nullptr;
  CoreTiming::EventType* m_event_type_patch_engine = nullptr;
};
}  // namespace SystemTimers

inline namespace SystemTimersLiterals
{
// Time base ticks, as CPU ticks.
constexpr SystemTimers::TimeBaseTick operator""_tbticks(unsigned long long value)
{
  return SystemTimers::TimeBaseTick(value);
}
}  // namespace SystemTimersLiterals
