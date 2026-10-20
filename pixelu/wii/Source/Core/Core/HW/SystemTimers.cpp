// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SystemTimers.h"

#include "AudioCommon/Mixer.h"
#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/DSP.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/VideoInterface.h"
#include "Core/IOS/IOS.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/PerformanceMetrics.h"

namespace SystemTimers
{
namespace
{
constexpr u32 GAMECUBE_CLOCK = 486000000u;
constexpr u32 WII_CLOCK = 729000000u;

// IOS devices (Wii remotes and so on) are updated this many times per
// emulated second. Not a hardware rate.
constexpr int IOS_UPDATES_PER_SECOND = 1500;

// The GPU thread is given a chance to sleep this many times per second.
constexpr u32 GPU_SLEEP_CHECKS_PER_SECOND = 1000;

// Retry delay when frame patches could not be applied.
constexpr s64 PATCH_RETRY_CYCLES = 1000;

// One audio DMA block is 32 bytes of 16-bit stereo, i.e. 8 samples.
// The divisor sets the sample rate as FIXED_SAMPLE_RATE_DIVIDEND / divisor.
int AudioDMAPeriod(u32 cpu_clock, u32 sample_rate_divisor)
{
  constexpr u64 blocks_per_dividend = Mixer::FIXED_SAMPLE_RATE_DIVIDEND * 4 / 32;
  return static_cast<int>(u64{cpu_clock} * sample_rate_divisor / blocks_per_dividend);
}
}  // namespace

SystemTimersManager::SystemTimersManager(Core::System& system) : m_system(system)
{
}

SystemTimersManager::~SystemTimersManager() = default;

// --- Periodic events ---------------------------------------------------------

void SystemTimersManager::DSPCallback(Core::System& system, u64, s64 cycles_late)
{
  auto& dsp = system.GetDSP();
  // The whole period goes to the DSP emulator (LLE splits it further).
  const s64 slice = s64{dsp.GetDSPEmulator()->DSP_UpdateRate()} - cycles_late;
  dsp.UpdateDSPSlice(static_cast<int>(slice));
  const s64 next = s64{dsp.GetDSPEmulator()->DSP_UpdateRate()} - cycles_late;
  system.GetCoreTiming().ScheduleEvent(next, system.GetSystemTimers().m_event_type_dsp);
}

void SystemTimersManager::AudioDMACallback(Core::System& system, u64, s64 cycles_late)
{
  system.GetDSP().UpdateAudioDMA();
  auto& timers = system.GetSystemTimers();
  const int period =
      AudioDMAPeriod(timers.m_cpu_core_clock, system.GetAudioInterface().GetAIDSampleRateDivisor());
  system.GetCoreTiming().ScheduleEvent(period - cycles_late, timers.m_event_type_audio_dma);
}

void SystemTimersManager::IPC_HLE_UpdateCallback(Core::System& system, u64, s64 cycles_late)
{
  if (!system.IsWii())
    return;
  system.GetIOS()->UpdateDevices();
  auto& timers = system.GetSystemTimers();
  system.GetCoreTiming().ScheduleEvent(timers.m_ipc_hle_period - cycles_late,
                                       timers.m_event_type_ipc_hle);
}

void SystemTimersManager::GPUSleepCallback(Core::System& system, u64, s64 cycles_late)
{
  auto& timing = system.GetCoreTiming();
  system.GetFifo().GpuMaySleep();
  auto& timers = system.GetSystemTimers();
  const s64 period = timers.GetTicksPerSecond() / GPU_SLEEP_CHECKS_PER_SECOND;
  timing.ScheduleEvent(period - cycles_late, timers.m_event_type_gpu_sleeper);
}

void SystemTimersManager::VICallback(Core::System& system, u64, s64 cycles_late)
{
  auto& timing = system.GetCoreTiming();
  auto& vi = system.GetVideoInterface();
  vi.Update(timing.GetTicks() - cycles_late);
  timing.ScheduleEvent(s64{vi.GetTicksPerHalfLine()} - cycles_late,
                       system.GetSystemTimers().m_event_type_vi);
}

void SystemTimersManager::DecrementerCallback(Core::System& system, u64, s64)
{
  auto& ppc_state = system.GetPPCState();
  ppc_state.spr[SPR_DEC] = 0xFFFFFFFF;
  ppc_state.Exceptions |= EXCEPTION_DECREMENTER;
}

// Applies frame patches once per field. The userdata carries how far the
// schedule has drifted from the field grid (retries add to it), so that a
// successful run lands back on the grid.
void SystemTimersManager::PatchEngineCallback(Core::System& system, u64 userdata, s64 cycles_late)
{
  const u32 field = system.GetVideoInterface().GetTicksPerField();
  s64 drift = static_cast<s64>((userdata + static_cast<u64>(cycles_late)) % field);

  s64 delay;
  if (PatchEngine::ApplyFramePatches(system))
  {
    delay = s64{field} - drift;
    drift = 0;
  }
  else
  {
    // Usually the CPU was in an interrupt handler; try again shortly.
    delay = PATCH_RETRY_CYCLES;
    drift += PATCH_RETRY_CYCLES;
  }

  system.GetCoreTiming().ScheduleEvent(delay, system.GetSystemTimers().m_event_type_patch_engine,
                                       static_cast<u64>(drift));
}

// --- Clock, time base and decrementer -----------------------------------------

u32 SystemTimersManager::GetTicksPerSecond() const
{
  return m_cpu_core_clock;
}

void SystemTimersManager::DecrementerSet()
{
  auto& timing = m_system.GetCoreTiming();
  const u32 value = m_system.GetPPCState().spr[SPR_DEC];

  timing.RemoveEvent(m_event_type_decrementer);
  // A negative decrementer has already passed zero; nothing to schedule.
  if (value & 0x80000000)
    return;

  timing.SetFakeDecStartTicks(timing.GetTicks());
  timing.SetFakeDecStartValue(value);
  // 32-bit product, as the decrementer is 32 bits wide.
  const u32 cycles = value * TIMER_RATIO;
  timing.ScheduleEvent(cycles, m_event_type_decrementer);
}

u32 SystemTimersManager::GetFakeDecrementer() const
{
  const auto& timing = m_system.GetCoreTiming();
  const u64 elapsed = timing.GetTicks() - timing.GetFakeDecStartTicks();
  return timing.GetFakeDecStartValue() - static_cast<u32>(elapsed / TIMER_RATIO);
}

void SystemTimersManager::TimeBaseSet()
{
  auto& timing = m_system.GetCoreTiming();
  timing.SetFakeTBStartTicks(timing.GetTicks());
  timing.SetFakeTBStartValue(m_system.GetPowerPC().ReadFullTimeBaseValue());
}

u64 SystemTimersManager::GetFakeTimeBase() const
{
  const auto& timing = m_system.GetCoreTiming();
  const u64 elapsed = timing.GetTicks() - timing.GetFakeTBStartTicks();
  return timing.GetFakeTBStartValue() + elapsed / TIMER_RATIO;
}

s64 SystemTimersManager::GetLocalTimeRTCOffset() const
{
  return m_localtime_rtc_offset;
}

double SystemTimersManager::GetEstimatedEmulationPerformance() const
{
  return m_system.GetPerfMetrics().GetMaxSpeed();
}

void SystemTimersManager::PreInit()
{
  ChangePPCClock(m_system.IsWii() ? Mode::Wii : Mode::GC);
}

void SystemTimersManager::ChangePPCClock(Mode mode)
{
  const u32 old_clock = m_cpu_core_clock;
  m_cpu_core_clock = mode == Mode::Wii ? WII_CLOCK : GAMECUBE_CLOCK;
  m_system.GetCoreTiming().AdjustEventQueueTimes(m_cpu_core_clock, old_clock);
}

// --- Lifetime -----------------------------------------------------------------

void SystemTimersManager::Init()
{
  if (m_system.IsWii())
    m_ipc_hle_period = GetTicksPerSecond() / IOS_UPDATES_PER_SECOND;

  Common::Timer::IncreaseResolution();
  if (Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE))
  {
    m_localtime_rtc_offset =
        Common::Timer::GetLocalTimeSinceJan1970() - Config::Get(Config::MAIN_CUSTOM_RTC_VALUE);
  }

  auto& timing = m_system.GetCoreTiming();
  auto& vi = m_system.GetVideoInterface();

  // The time base starts at the emulated wall clock time (seconds since 2000).
  const u64 tb_per_second = m_cpu_core_clock / TIMER_RATIO;
  const u64 seconds = ExpansionInterface::CEXIIPL::GetEmulatedTime(
      m_system, ExpansionInterface::CEXIIPL::GC_EPOCH);
  timing.SetFakeTBStartValue(tb_per_second * seconds);
  timing.SetFakeTBStartTicks(timing.GetTicks());

  timing.SetFakeDecStartValue(0xFFFFFFFF);
  timing.SetFakeDecStartTicks(timing.GetTicks());

  // The names are stored in savestates.
  m_event_type_decrementer = timing.RegisterEvent("DecCallback", DecrementerCallback);
  m_event_type_vi = timing.RegisterEvent("VICallback", VICallback);
  m_event_type_dsp = timing.RegisterEvent("DSPCallback", DSPCallback);
  m_event_type_audio_dma = timing.RegisterEvent("AudioDMACallback", AudioDMACallback);
  m_event_type_ipc_hle = timing.RegisterEvent("IPC_HLE_UpdateCallback", IPC_HLE_UpdateCallback);
  m_event_type_gpu_sleeper = timing.RegisterEvent("GPUSleeper", GPUSleepCallback);
  m_event_type_patch_engine = timing.RegisterEvent("PatchEngine", PatchEngineCallback);

  // Same-time events run in scheduling order, so this order matters.
  timing.ScheduleEvent(0, m_event_type_gpu_sleeper);
  timing.ScheduleEvent(vi.GetTicksPerHalfLine(), m_event_type_vi);
  timing.ScheduleEvent(0, m_event_type_dsp);
  timing.ScheduleEvent(
      AudioDMAPeriod(m_cpu_core_clock, m_system.GetAudioInterface().GetAIDSampleRateDivisor()),
      m_event_type_audio_dma);
  timing.ScheduleEvent(vi.GetTicksPerField(), m_event_type_patch_engine);
  if (m_system.IsWii())
    timing.ScheduleEvent(m_ipc_hle_period, m_event_type_ipc_hle);
}

void SystemTimersManager::Shutdown()
{
  Common::Timer::RestoreResolution();
  m_localtime_rtc_offset = 0;
}

}  // namespace SystemTimers
