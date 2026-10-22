// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DSPHLE/MailHandler.h"

class PointerWrap;

namespace Core
{
class System;
}

namespace DSP::HLE
{
class UCodeInterface;

// High-level DSP emulation: instead of running the DSP program, the ucode the game loads is
// recognised by its CRC and replaced by a C++ implementation of what it does.
class DSPHLE : public DSPEmulator
{
public:
  explicit DSPHLE(Core::System& system);
  DSPHLE(const DSPHLE&) = delete;
  DSPHLE(DSPHLE&&) = delete;
  DSPHLE& operator=(const DSPHLE&) = delete;
  DSPHLE& operator=(DSPHLE&&) = delete;
  ~DSPHLE() override;

  bool Initialize(bool wii, bool dsp_thread) override;
  void Shutdown() override;
  bool IsLLE() const override { return false; }
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

  CMailHandler& AccessMailHandler() { return m_mail_handler; }

  // Replaces the running ucode (reset, init, or a ROM boot).
  void SetUCode(u32 crc);

  // Task switch requested by the running ucode. The first ucode switched away from is kept
  // aside, and switching back to its CRC restores it with its state (games alternate between
  // their audio ucode and short tasks such as the CARD or GBA ucodes).
  void SwapUCode(u32 crc);

  Core::System& GetSystem() const { return m_system; }

private:
  // The CPU-to-DSP mailbox, plus a DSP-side word kept for savestate compatibility.
  struct Mailboxes
  {
    u32 to_dsp = 0;
    u32 unused_from_dsp = 0;
  };
  static_assert(sizeof(Mailboxes) == 8);

  Mailboxes m_mailboxes;

  std::unique_ptr<UCodeInterface> m_ucode;
  std::unique_ptr<UCodeInterface> m_last_ucode;

  DSP::UDSPControl m_dsp_control;
  // Time base value at which DSPInitCode drops back to 0 after an init.
  u64 m_control_reg_init_code_clear_time = 0;
  CMailHandler m_mail_handler;

  Core::System& m_system;
};
}  // namespace DSP::HLE
