// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <cstddef>
#include <optional>

#include "Common/BlockingLoop.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/Flag.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}

// Moves GX command data from the emulated GP FIFO in RAM into a host-side staging buffer and
// feeds it to the opcode decoder. Three schedules are supported:
//  - single core: the CPU thread decodes in time slices driven by a CoreTiming event;
//  - dual core: a GPU thread decodes on its own, optionally kept within a cycle distance of
//    the CPU ("sync GPU");
//  - deterministic dual core: the CPU thread copies and pre-parses the data (resolving display
//    lists into an auxiliary buffer) and the GPU thread only decodes what it is handed.
namespace Fifo
{
// Why a caller needed the GPU to catch up (diagnostics only).
enum class SyncGPUReason
{
  Other,
  Wraparound,
  EFBPoke,
  PerfQuery,
  BBox,
  Swap,
  AuxSpace,
};

class FifoManager final
{
public:
  explicit FifoManager(Core::System& system);
  FifoManager(const FifoManager& other) = delete;
  FifoManager(FifoManager&& other) = delete;
  FifoManager& operator=(const FifoManager& other) = delete;
  FifoManager& operator=(FifoManager&& other) = delete;
  ~FifoManager();

  void Init();
  void Shutdown();
  void Prepare();  // CPU thread only.
  void DoState(PointerWrap& f);
  void PauseAndLock();
  void RestoreState(bool was_running);
  void UpdateWantDeterminism(bool want);
  bool UseDeterministicGPUThread() const { return m_deterministic; }
  bool UseSyncGPU() const { return m_sync_gpu; }

  // Deterministic mode: wait until the GPU thread has consumed everything handed to it.
  void SyncGPU(SyncGPUReason reason, bool may_move_read_ptr = true);

  // Before the CPU reads or writes a CP register: run a slice of GPU work in single core, or
  // honour the sync distance in dual core.
  void SyncGPUForRegisterAccess();

  // Deterministic mode: display-list bytes resolved on the CPU, consumed in order by the GPU.
  void PushFifoAuxBuffer(const void* ptr, size_t size);
  void* PopFifoAuxBuffer(size_t size);

  void FlushGpu();
  void RunGpu();
  void GpuMaySleep();
  void RunGpuLoop();
  void ExitGpuLoop();
  void EmulatorState(bool running);
  void ResetVideoBuffer();

private:
  void RefreshConfig();
  void CopyBurstToStaging(u32 read_ptr);
  void CopyAndPreprocessBurst(u32 read_ptr);
  void DecodeOnGpuThread();
  int RunGpuOnCpu(int ticks);
  int WaitForGpuThread(int ticks);
  static void SyncGPUCallback(Core::System& system, u64 ticks, s64 cyclesLate);

  static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

  Common::BlockingLoop m_gpu_loop;
  Common::Flag m_emulation_running;

  // Large and mostly untouched; only deterministic mode uses it.
  u8 m_aux[FIFO_SIZE]{};
  u8* m_aux_write = nullptr;
  u8* m_aux_read = nullptr;

  // Derived from several settings and the core mode; changed only while paused.
  bool m_deterministic = false;

  CoreTiming::EventType* m_sync_event = nullptr;

  // Staging buffer for command data, plus cursors into it (saved in savestates):
  //  read  - next byte the decoder will consume (GPU side);
  //  write - end of copied data;
  //  seen  - deterministic mode: how far the GPU thread has got, which can trail `read` when a
  //          command was cut off at the end of the data;
  //  pp    - deterministic mode: the CPU-side preprocessing cursor.
  // `write` is atomic because the CPU publishes it to the polling GPU thread in deterministic
  // mode; in the other modes the GPU thread owns all cursors.
  u8* m_staging = nullptr;
  u8* m_staging_read = nullptr;
  std::atomic<u8*> m_staging_write = nullptr;
  std::atomic<u8*> m_staging_seen = nullptr;
  u8* m_staging_pp = nullptr;

  // Sync-GPU credit: emulated CPU cycles the GPU may still spend (negative when it is ahead).
  std::atomic<int> m_sync_ticks = 0;
  bool m_sync_event_parked = false;
  Common::Event m_gpu_caught_up;

  std::optional<Config::ConfigChangedCallbackID> m_config_hook = std::nullopt;
  bool m_sync_gpu = false;
  int m_sync_max_distance = 0;
  int m_sync_min_distance = 0;
  float m_sync_overclock = 0.0f;

  Core::System& m_system;
};

// True when the CP breakpoint is enabled and the read pointer sits on it.
bool AtBreakpoint(Core::System& system);
}  // namespace Fifo
