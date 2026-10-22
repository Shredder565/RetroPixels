// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Core/DSP/DSPCore.h"
#include "Core/DSPEmulator.h"

class PointerWrap;

namespace DSP::LLE
{
// Low-level DSP emulation: runs the real microcode on the interpreter or the JIT,
// either inline from DSP_Update or on a thread of its own.
class DSPLLE : public DSPEmulator
{
public:
  DSPLLE();
  ~DSPLLE() override;

  bool Initialize(bool wii, bool dsp_thread) override;
  void Shutdown() override;
  bool IsLLE() const override { return true; }
  void DoState(PointerWrap& p) override;
  void PauseAndLock() override;
  void UnpauseAndUnlock() override;

  void DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value) override;
  void DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value) override;
  u16 DSP_ReadMailBoxHigh(bool cpu_mailbox) override;
  u16 DSP_ReadMailBoxLow(bool cpu_mailbox) override;
  u16 DSP_ReadControlRegister() override;
  u16 DSP_WriteControlRegister(u16 value) override;
  void DSP_Update(int cycles) override;
  void DSP_StopSoundStream() override;
  u32 DSP_UpdateRate() override;

private:
  void ThreadLoop();
  void StopThread();

  DSPCore m_dsp_core;

  // Threaded mode: the CPU side adds to m_pending_cycles and wakes the thread
  // (m_wake_dsp); the thread signals m_dsp_idle once it has run them.
  std::thread m_thread;
  std::mutex m_core_lock;
  bool m_threaded = false;
  bool m_stop_threading_requested = false;
  Common::Flag m_thread_running;
  std::atomic<u32> m_pending_cycles{};
  Common::Event m_wake_dsp;
  Common::Event m_dsp_idle;
};
}  // namespace DSP::LLE
