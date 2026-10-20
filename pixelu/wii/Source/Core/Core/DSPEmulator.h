// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}
class PointerWrap;

// The audio DSP as the rest of the emulator sees it. There are two
// implementations: HLE, which replaces known microcodes with native code, and
// LLE, which emulates the DSP itself.
class DSPEmulator
{
public:
  virtual ~DSPEmulator();
  virtual bool IsLLE() const = 0;

  virtual bool Initialize(bool wii, bool dsp_thread) = 0;
  virtual void Shutdown() = 0;

  virtual void DoState(PointerWrap& p) = 0;
  virtual void PauseAndLock() = 0;
  virtual void UnpauseAndUnlock() = 0;

  // cpu_mailbox selects the CPU-to-DSP mailbox, otherwise the DSP-to-CPU one.
  virtual void DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value) = 0;
  virtual void DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value) = 0;
  virtual u16 DSP_ReadMailBoxHigh(bool cpu_mailbox) = 0;
  virtual u16 DSP_ReadMailBoxLow(bool cpu_mailbox) = 0;
  virtual u16 DSP_ReadControlRegister() = 0;
  virtual u16 DSP_WriteControlRegister(u16 value) = 0;

  // Runs the DSP for the given number of CPU cycles.
  virtual void DSP_Update(int cycles) = 0;
  virtual void DSP_StopSoundStream() = 0;
  // How many CPU cycles DSP_Update should be called with at a time.
  virtual u32 DSP_UpdateRate() = 0;

protected:
  bool m_wii = false;
};

std::unique_ptr<DSPEmulator> CreateDSPEmulator(Core::System& system, bool hle);
