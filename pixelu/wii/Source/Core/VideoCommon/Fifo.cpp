// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/Fifo.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "Common/Assert.h"
#include "Common/BlockingLoop.h"
#include "Common/ChunkFile.h"
#include "Common/Event.h"
#include "Common/FPURoundMode.h"
#include "Common/MemoryUtil.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/System.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/DataReader.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoBackendBase.h"

namespace Fifo
{
namespace
{
// Granularity, in emulated CPU cycles, of GPU time slices and sync checks.
constexpr int SLICE_CYCLES = 1000;
constexpr u32 BURST = GPFifo::GATHER_PIPE_SIZE;
constexpr auto RELAXED = std::memory_order_relaxed;

// Next burst address after `ptr` in the circular FIFO window [base, end].
u32 NextBurst(const CommandProcessor::SCPFifoStruct& fifo, u32 ptr)
{
  return ptr == fifo.CPEnd.load(RELAXED) ? fifo.CPBase.load(RELAXED) : ptr + BURST;
}

// Whether the GPU may fetch another burst from the FIFO.
bool FifoReadable(Core::System& system, const CommandProcessor::SCPFifoStruct& fifo)
{
  return fifo.bFF_GPReadEnable.load(RELAXED) && fifo.CPReadWriteDistance.load(RELAXED) &&
         !AtBreakpoint(system);
}
}  // namespace

FifoManager::FifoManager(Core::System& system) : m_system{system}
{
}

FifoManager::~FifoManager() = default;

void FifoManager::RefreshConfig()
{
  m_sync_gpu = Config::Get(Config::MAIN_SYNC_GPU);
  m_sync_max_distance = Config::Get(Config::MAIN_SYNC_GPU_MAX_DISTANCE);
  m_sync_min_distance = Config::Get(Config::MAIN_SYNC_GPU_MIN_DISTANCE);
  m_sync_overclock = Config::Get(Config::MAIN_SYNC_GPU_OVERCLOCK);
}

void FifoManager::DoState(PointerWrap& p)
{
  p.DoArray(m_staging, FIFO_SIZE);

  // Cursors are stored as offsets into the staging buffer; stop at the first bad one.
  u8* write = m_staging_write;
  if (!p.DoPointer(write, m_staging, FIFO_SIZE))
    return;
  m_staging_write = write;
  if (!p.DoPointer(m_staging_read, m_staging, FIFO_SIZE))
    return;

  if (p.IsReadMode())
  {
    if (m_staging_read > write)
    {
      // Inconsistent state: abandon the load.
      p.SetMeasureMode();
      return;
    }
    // Loading happens while paused, so the deterministic-mode cursors can simply restart at
    // the read position.
    if (m_deterministic)
      m_staging_seen = m_staging_pp = m_staging_read;
  }

  p.Do(m_sync_ticks);
  p.Do(m_sync_event_parked);
}

void FifoManager::PauseAndLock()
{
  SyncGPU(SyncGPUReason::Other);
  EmulatorState(false);

  const bool free_running_gpu = m_system.IsDualCoreMode() && !m_deterministic;
  if (free_running_gpu)
    m_gpu_loop.WaitYield(std::chrono::milliseconds(100), Host_YieldToUI);
}

void FifoManager::RestoreState(const bool was_running)
{
  if (was_running)
    EmulatorState(true);
}

void FifoManager::Init()
{
  if (!m_config_hook)
    m_config_hook = Config::AddConfigChangedCallback([this] { RefreshConfig(); });
  RefreshConfig();

  // Four spare bytes let the vertex loaders' wide loads run past the end safely.
  m_staging = static_cast<u8*>(Common::AllocateMemoryPages(FIFO_SIZE + 4));
  ResetVideoBuffer();
  if (m_system.IsDualCoreMode())
    m_gpu_loop.Prepare();
  m_sync_ticks.store(0);
}

void FifoManager::Shutdown()
{
  if (m_gpu_loop.IsRunning())
    PanicAlertFmt("FIFO shutting down while active");

  Common::FreeMemoryPages(m_staging, FIFO_SIZE + 4);
  m_staging = nullptr;
  m_staging_write = nullptr;
  m_staging_pp = nullptr;
  m_staging_read = nullptr;
  m_staging_seen = nullptr;
  m_aux_write = nullptr;
  m_aux_read = nullptr;

  if (m_config_hook)
  {
    Config::RemoveConfigChangedCallback(*m_config_hook);
    m_config_hook = std::nullopt;
  }
}

// Safe from any thread, including the GPU thread itself.
void FifoManager::ExitGpuLoop()
{
  // Disabling reads releases a CPU thread waiting for the GPU.
  m_system.GetCommandProcessor().GetFifo().bFF_GPReadEnable.store(0, RELAXED);
  FlushGpu();

  m_emulation_running.Set();
  m_gpu_loop.Stop(Common::BlockingLoop::StopMode::NonBlock);
}

void FifoManager::EmulatorState(bool running)
{
  m_emulation_running.Set(running);
  if (!running)
  {
    m_gpu_loop.AllowSleep();
    return;
  }
  m_gpu_loop.Wakeup();
}

void FifoManager::SyncGPU([[maybe_unused]] SyncGPUReason reason, bool may_move_read_ptr)
{
  if (!m_deterministic)
    return;

  m_gpu_loop.Wait();
  if (!m_gpu_loop.IsRunning())
    return;

  // With the GPU idle, both buffers can be compacted back to their starts so they do not run
  // into their ends.
  if (may_move_read_ptr && m_aux_write != m_aux_read)
  {
    PanicAlertFmt("Aux FIFO not synced ({}, {})", fmt::ptr(m_aux_write), fmt::ptr(m_aux_read));
  }
  const ptrdiff_t aux_pending = m_aux_write - m_aux_read;
  std::memmove(m_aux, m_aux_read, static_cast<size_t>(aux_pending));
  m_aux_write = m_aux + aux_pending;
  m_aux_read = m_aux;

  if (!may_move_read_ptr)
    return;

  u8* const write = m_staging_write;
  const size_t leftover = static_cast<size_t>(write - m_staging_pp);
  std::memmove(m_staging, m_staging_pp, leftover);
  // Every cursor only moves backwards here. `seen` is published after `write` (and the GPU loop
  // reads it before `write`), so the loop can never observe write > seen spuriously.
  m_staging_write = m_staging + leftover;
  m_staging_pp = m_staging;
  m_staging_read = m_staging;
  m_staging_seen = m_staging + leftover;
}

void FifoManager::PushFifoAuxBuffer(const void* ptr, size_t size)
{
  const auto room = [this] { return static_cast<size_t>(m_aux + FIFO_SIZE - m_aux_write); };
  if (size > room())
  {
    SyncGPU(SyncGPUReason::AuxSpace, /* may_move_read_ptr */ false);
    if (!m_gpu_loop.IsRunning())
      return;  // shutting down
    if (size > room())
    {
      // Only possible for a display list of roughly the buffer's size.
      PanicAlertFmt("Absurdly large aux buffer");
      return;
    }
  }
  std::memcpy(m_aux_write, ptr, size);
  m_aux_write += size;
}

void* FifoManager::PopFifoAuxBuffer(size_t size)
{
  u8* const data = m_aux_read;
  m_aux_read = data + size;
  return data;
}

// Appends one burst from emulated RAM to the staging buffer, compacting it first if the burst
// would not fit at the end.
void FifoManager::CopyBurstToStaging(u32 read_ptr)
{
  const size_t room_at_end = static_cast<size_t>(m_staging + FIFO_SIZE - m_staging_write);
  if (BURST > room_at_end)
  {
    const size_t pending = static_cast<size_t>(m_staging_write - m_staging_read);
    if (BURST > static_cast<size_t>(FIFO_SIZE - pending))
    {
      PanicAlertFmt("FIFO out of bounds (existing {} + new {} > {})", pending, BURST, FIFO_SIZE);
      return;
    }
    std::memmove(m_staging, m_staging_read, pending);
    m_staging_write = m_staging + pending;
    m_staging_read = m_staging;
  }
  m_system.GetMemory().CopyFromEmu(m_staging_write, read_ptr, BURST);
  m_staging_write += BURST;
}

// Deterministic mode, CPU thread: append a burst and pre-parse it, then publish it.
void FifoManager::CopyAndPreprocessBurst(u32 read_ptr)
{
  u8* write = m_staging_write;
  if (BURST > static_cast<size_t>(m_staging + FIFO_SIZE - write))
  {
    // The GPU thread may be reading the buffer, so wait for it before compacting (rare thanks
    // to the compaction in SyncGPU).
    SyncGPU(SyncGPUReason::Wraparound);
    if (!m_gpu_loop.IsRunning())
      return;  // shutting down; the checks below could misfire

    if (m_staging_pp != m_staging_read)
    {
      PanicAlertFmt("Desynced read pointers");
      return;
    }
    write = m_staging_write;
    const size_t pending = static_cast<size_t>(write - m_staging_pp);
    if (BURST > static_cast<size_t>(FIFO_SIZE - pending))
    {
      PanicAlertFmt("FIFO out of bounds (existing {} + new {} > {})", pending, BURST, FIFO_SIZE);
      return;
    }
  }
  m_system.GetMemory().CopyFromEmu(m_staging_write, read_ptr, BURST);
  m_staging_pp =
      OpcodeDecoder::RunFifo<true>(DataReader(m_staging_pp, write + BURST), nullptr);
  // The GPU thread polls this, so a plain atomic store is enough.
  m_staging_write = write + BURST;
}

void FifoManager::ResetVideoBuffer()
{
  m_staging_read = m_staging;
  m_staging_write = m_staging;
  m_staging_seen = m_staging;
  m_staging_pp = m_staging;
  m_aux_write = m_aux;
  m_aux_read = m_aux;
}

// One iteration of the free-running GPU thread: drain the FIFO, keeping the CP registers and
// the sync-GPU credit up to date.
void FifoManager::DecodeOnGpuThread()
{
  CommandProcessor::CommandProcessorManager& cp = m_system.GetCommandProcessor();
  CommandProcessor::SCPFifoStruct& fifo = cp.GetFifo();
  cp.SetCPStatusFromGPU();

  while (!cp.IsInterruptWaiting() && FifoReadable(m_system, fifo))
  {
    // Sync GPU: do not run ahead of the CPU by more than the allowed distance.
    if (m_sync_gpu && m_sync_ticks.load() < m_sync_min_distance)
      break;

    const u32 burst_addr = fifo.CPReadPointer.load(RELAXED);
    CopyBurstToStaging(burst_addr);
    const u32 next_read = NextBurst(fifo, burst_addr);

    const s32 remaining = static_cast<s32>(fifo.CPReadWriteDistance.load(RELAXED)) -
                          static_cast<s32>(BURST);
    ASSERT_MSG(COMMANDPROCESSOR, remaining >= 0,
               "Negative fifo.CPReadWriteDistance = {} in FIFO Loop !\nThat can produce "
               "instability in the game. Please report it.",
               remaining);

    u32 cycles = 0;
    u8* const write = m_staging_write;
    m_staging_read = OpcodeDecoder::RunFifo(DataReader(m_staging_read, write), &cycles);

    fifo.CPReadPointer.store(next_read, RELAXED);
    fifo.CPReadWriteDistance.fetch_sub(BURST, std::memory_order_seq_cst);
    // Publish the read pointer to the CPU only once every staged byte has been consumed.
    if (write == m_staging_read)
      fifo.SafeCPReadPointer.store(fifo.CPReadPointer.load(RELAXED), RELAXED);

    cp.SetCPStatusFromGPU();

    if (m_sync_gpu)
    {
      const int spent = static_cast<int>(static_cast<float>(cycles) / m_sync_overclock);
      const int before = m_sync_ticks.fetch_sub(spent);
      // Release a CPU blocked on the maximum distance once we drop below it.
      if (before >= m_sync_max_distance && before - spent < m_sync_max_distance)
        m_gpu_caught_up.Set();
    }

    // Service CPU requests (swaps, EFB access) between bursts so the CPU is not left waiting.
    AsyncRequests::GetInstance()->PullEvents();
  }

  // Out of work: give up any remaining credit.
  if (m_sync_ticks.load() > 0)
  {
    const int before = m_sync_ticks.exchange(0);
    if (before >= m_sync_max_distance)
      m_gpu_caught_up.Set();
  }

  // Idle for now: submit buffered geometry and refresh the EFB peek cache.
  g_vertex_manager->Flush();
  g_framebuffer_manager->RefreshPeekCache();
}

void FifoManager::RunGpuLoop()
{
  m_gpu_loop.Run(
      [this] {
        AsyncRequests::GetInstance()->PullEvents();

        if (!m_emulation_running.IsSet())
          return;

        if (!m_deterministic)
        {
          DecodeOnGpuThread();
          return;
        }

        // Deterministic mode: the CPU already handled the FIFO; just decode what it published.
        // `seen` must be read before `write` (see SyncGPU).
        u8* const seen = m_staging_seen;
        u8* const write = m_staging_write;
        if (write > seen)
        {
          m_staging_read = OpcodeDecoder::RunFifo(DataReader(m_staging_read, write), nullptr);
          m_staging_seen = write;
        }
      },
      100);
}

void FifoManager::FlushGpu()
{
  if (m_system.IsDualCoreMode() && !m_deterministic)
    m_gpu_loop.Wait();
}

void FifoManager::GpuMaySleep()
{
  m_gpu_loop.AllowSleep();
}

bool AtBreakpoint(Core::System& system)
{
  const CommandProcessor::SCPFifoStruct& fifo = system.GetCommandProcessor().GetFifo();
  if (!fifo.bFF_BPEnable.load(RELAXED))
    return false;
  return fifo.CPReadPointer.load(RELAXED) == fifo.CPBreakpoint.load(RELAXED);
}

void FifoManager::RunGpu()
{
  const bool dual_core = m_system.IsDualCoreMode();
  const bool gpu_thread_decodes = dual_core && !m_deterministic;

  if (gpu_thread_decodes)
    m_gpu_loop.Wakeup();

  // The CPU-side sync event is needed whenever the CPU drives or paces the GPU.
  const bool cpu_paces_gpu = !gpu_thread_decodes || m_sync_gpu;
  if (cpu_paces_gpu && m_sync_event_parked)
  {
    m_sync_event_parked = false;
    m_system.GetCoreTiming().ScheduleEvent(SLICE_CYCLES, m_sync_event, SLICE_CYCLES);
  }
}

// Single core and deterministic mode: process FIFO bursts on the CPU thread for up to `ticks`
// emulated cycles (plus any carried credit). Returns the cycles until the next slice, or -1
// when the FIFO ran dry.
int FifoManager::RunGpuOnCpu(int ticks)
{
  CommandProcessor::CommandProcessorManager& cp = m_system.GetCommandProcessor();
  CommandProcessor::SCPFifoStruct& fifo = cp.GetFifo();

  bool simd_state_saved = false;
  int budget = static_cast<int>(static_cast<float>(ticks) * m_sync_overclock) + m_sync_ticks.load();
  while (FifoReadable(m_system, fifo) && budget >= 0)
  {
    const u32 burst_addr = fifo.CPReadPointer.load(RELAXED);
    if (m_deterministic)
    {
      CopyAndPreprocessBurst(burst_addr);
      m_gpu_loop.Wakeup();
    }
    else
    {
      // The decoder expects the host's default floating point environment.
      if (!simd_state_saved)
      {
        Common::FPU::SaveSIMDState();
        Common::FPU::LoadDefaultSIMDState();
        simd_state_saved = true;
      }
      CopyBurstToStaging(burst_addr);
      u32 cycles = 0;
      m_staging_read =
          OpcodeDecoder::RunFifo(DataReader(m_staging_read, m_staging_write), &cycles);
      budget -= static_cast<int>(cycles);
    }

    fifo.CPReadPointer.store(NextBurst(fifo, fifo.CPReadPointer.load(RELAXED)), RELAXED);
    fifo.CPReadWriteDistance.fetch_sub(BURST, RELAXED);
  }

  cp.SetCPStatusFromGPU();

  if (simd_state_saved)
    Common::FPU::LoadSIMDState();

  // Leftover positive credit is dropped; a deficit is carried into the next slice.
  m_sync_ticks.store(std::min(budget, 0));

  if (budget >= 0)
    return -1;
  // Overran the budget: pay it back, but never schedule sooner than one slice.
  return SLICE_CYCLES - budget;
}

void FifoManager::UpdateWantDeterminism(bool want)
{
  // Called while paused (or before starting), so switching modes here is safe.
  bool deterministic = false;
  switch (Config::GetGPUDeterminismMode())
  {
  case Config::GPUDeterminismMode::Auto:
    deterministic = want;
    break;
  case Config::GPUDeterminismMode::Disabled:
    deterministic = false;
    break;
  case Config::GPUDeterminismMode::FakeCompletion:
    deterministic = true;
    break;
  }
  // Only meaningful with a GPU thread.
  deterministic = deterministic && m_system.IsDualCoreMode();

  if (deterministic == m_deterministic)
    return;
  m_deterministic = deterministic;
  if (!deterministic)
    return;

  // The CPU-side cursors and CP copy were not maintained until now.
  m_staging_seen = m_staging_pp = m_staging_read;
  CopyPreprocessCPStateFromMain();
  VertexLoaderManager::MarkAllDirty();
}

// Dual core with sync GPU, CPU thread: account `ticks` of CPU progress, waking the GPU when it
// may run and blocking when the CPU is too far ahead. Returns when to check again, or -1 once
// the GPU is idle.
int FifoManager::WaitForGpuThread(int ticks)
{
  const int before = m_sync_ticks.fetch_add(ticks);
  const int after = before + ticks;

  if (before >= 0 && m_gpu_loop.IsDone())
    return -1;

  if (before < m_sync_min_distance && after >= m_sync_min_distance)
    RunGpu();

  if (after < m_sync_min_distance)
    return SLICE_CYCLES + m_sync_min_distance - after;

  if (after >= m_sync_max_distance)
    m_gpu_caught_up.Wait();

  return SLICE_CYCLES;
}

void FifoManager::SyncGPUCallback(Core::System& system, u64 ticks, s64 cyclesLate)
{
  FifoManager& self = system.GetFifo();
  const int elapsed = static_cast<int>(ticks + static_cast<u64>(cyclesLate));

  int next = -1;
  if (!system.IsDualCoreMode() || self.m_deterministic)
    next = self.RunGpuOnCpu(elapsed);
  else if (self.m_sync_gpu)
    next = self.WaitForGpuThread(elapsed);

  // Park the event when there is nothing to do; RunGpu() re-arms it.
  self.m_sync_event_parked = next < 0;
  if (!self.m_sync_event_parked)
    system.GetCoreTiming().ScheduleEvent(next, self.m_sync_event, static_cast<u64>(next));
}

void FifoManager::SyncGPUForRegisterAccess()
{
  SyncGPU(SyncGPUReason::Other);

  if (!m_system.IsDualCoreMode() || m_deterministic)
    RunGpuOnCpu(SLICE_CYCLES);
  else if (m_sync_gpu)
    WaitForGpuThread(SLICE_CYCLES);
}

// Arms the CPU-side sync event in its parked state, giving the GPU a deterministic start.
void FifoManager::Prepare()
{
  m_sync_event = m_system.GetCoreTiming().RegisterEvent("SyncGPUCallback", SyncGPUCallback);
  m_sync_event_parked = true;
}
}  // namespace Fifo
