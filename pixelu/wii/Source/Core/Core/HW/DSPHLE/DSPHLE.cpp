// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/DSPHLE.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace DSP::HLE
{
DSPHLE::DSPHLE(Core::System& system) : m_mail_handler(system.GetDSP()), m_system(system)
{
}

DSPHLE::~DSPHLE() = default;

bool DSPHLE::Initialize(bool wii, bool /*dsp_thread*/)
{
  m_wii = wii;
  m_ucode.reset();
  m_last_ucode.reset();

  // Power on: the ROM is running, and the DSP is held halted with the init bit set.
  SetUCode(UCODE_ROM);

  m_dsp_control.Hex = 0;
  m_dsp_control.DSPHalt = 1;
  m_dsp_control.DSPInit = 1;
  m_mail_handler.SetHalted(true);

  m_mailboxes = {};
  return true;
}

void DSPHLE::Shutdown()
{
  m_ucode.reset();
}

void DSPHLE::DSP_StopSoundStream()
{
}

void DSPHLE::PauseAndLock()
{
}

void DSPHLE::UnpauseAndUnlock()
{
}

void DSPHLE::DSP_Update(int /*cycles*/)
{
  if (m_ucode)
    m_ucode->Update();
}

u32 DSPHLE::DSP_UpdateRate()
{
  // AX frames are 5 ms (GameCube) or 3 ms (Wii); updating every millisecond is fine for all.
  return m_system.GetSystemTimers().GetTicksPerSecond() / 1000;
}

void DSPHLE::SetUCode(u32 crc)
{
  m_mail_handler.ClearPending();
  m_ucode = UCodeFactory(crc, this, m_wii);
  m_ucode->Initialize();
}

void DSPHLE::SwapUCode(u32 crc)
{
  m_mail_handler.ClearPending();

  if (m_last_ucode != nullptr && UCodeInterface::GetCRC(m_last_ucode.get()) == crc)
  {
    // Back to the ucode we left: it continues where it was.
    m_ucode = std::move(m_last_ucode);
    return;
  }

  // Keep the ucode we are leaving only if nothing is parked yet.
  if (m_last_ucode == nullptr)
    m_last_ucode = std::move(m_ucode);

  m_ucode = UCodeFactory(crc, this, m_wii);
  m_ucode->Initialize();
}

void DSPHLE::DoState(PointerWrap& p)
{
  bool is_hle = true;
  p.Do(is_hle);
  if (p.IsReadMode() && !is_hle)
  {
    Core::DisplayMessage("State is incompatible with current DSP engine. Aborting load state.",
                         3000);
    p.SetVerifyMode();
    return;
  }

  p.Do(m_dsp_control);
  p.Do(m_control_reg_init_code_clear_time);
  p.Do(m_mailboxes);

  const u32 current_crc = UCodeInterface::GetCRC(m_ucode.get());
  const u32 parked_crc = UCodeInterface::GetCRC(m_last_ucode.get());
  u32 saved_current_crc = current_crc;
  u32 saved_parked_crc = parked_crc;
  p.Do(saved_current_crc);
  p.Do(saved_parked_crc);

  // A state made with other ucodes needs those ucodes built (without starting them) so they can
  // read their own state.
  std::unique_ptr<UCodeInterface> current = saved_current_crc == current_crc ?
                                                std::move(m_ucode) :
                                                UCodeFactory(saved_current_crc, this, m_wii);
  std::unique_ptr<UCodeInterface> parked = saved_parked_crc == parked_crc ?
                                               std::move(m_last_ucode) :
                                               UCodeFactory(saved_parked_crc, this, m_wii);
  if (current)
    current->DoState(p);
  if (parked)
    parked->DoState(p);
  m_ucode = std::move(current);
  m_last_ucode = std::move(parked);

  m_mail_handler.DoState(p);
}

u16 DSPHLE::DSP_ReadMailBoxHigh(bool cpu_mailbox)
{
  if (!cpu_mailbox)
    return m_mail_handler.ReadDSPMailboxHigh();
  return static_cast<u16>(m_mailboxes.to_dsp >> 16);
}

u16 DSPHLE::DSP_ReadMailBoxLow(bool cpu_mailbox)
{
  if (!cpu_mailbox)
    return m_mail_handler.ReadDSPMailboxLow();
  return static_cast<u16>(m_mailboxes.to_dsp);
}

void DSPHLE::DSP_WriteMailBoxHigh(bool cpu_mailbox, u16 value)
{
  if (!cpu_mailbox)
  {
    PanicAlertFmt("CPU can't write {:08x} to DSP mailbox", value);
    return;
  }
  m_mailboxes.to_dsp = (u32{value} << 16) | (m_mailboxes.to_dsp & 0xFFFF);
}

void DSPHLE::DSP_WriteMailBoxLow(bool cpu_mailbox, u16 value)
{
  if (!cpu_mailbox)
  {
    PanicAlertFmt("CPU can't write {:08x} to DSP mailbox", value);
    return;
  }

  // Writing the low half sends the mail. The ucode handles it right away, so the CPU sees the
  // "mail waiting" bit already cleared afterwards.
  m_mailboxes.to_dsp = (m_mailboxes.to_dsp & 0xFFFF0000) | value;
  if (m_ucode)
  {
    DEBUG_LOG_FMT(DSP_MAIL, "CPU mail in: {:08x}", m_mailboxes.to_dsp);
    m_ucode->HandleMail(m_mailboxes.to_dsp);
  }
  m_mailboxes.to_dsp &= 0x7FFFFFFF;
}

u16 DSPHLE::DSP_WriteControlRegister(u16 value)
{
  DSP::UDSPControl requested(value);

  if (requested.DSPHalt != m_dsp_control.DSPHalt)
  {
    INFO_LOG_FMT(DSPHLE, "DSP halt bit: {:04x} -> {:04x}", m_dsp_control.Hex, value);
    m_mail_handler.SetHalted(requested.DSPHalt != 0);
  }

  if (requested.DSPReset != 0)
  {
    // Reset reboots into the ROM; the bit reads back as done at once.
    SetUCode(UCODE_ROM);
    requested.DSPReset = 0;
  }

  if (m_dsp_control.DSPInit != 0 && requested.DSPInit == 0)
  {
    // Clearing DSPInit makes the DSP run the small program the SDK and libogc boot it with. That
    // program is not hashed: it is only ever the standard one. DSPInitCode then reads 1 for a
    // short while (about 130 time base ticks, measured on a Wii). Datel software clears the
    // wrong bit here and never takes this path.
    SetUCode(UCODE_INIT_AUDIO_SYSTEM);
    requested.DSPInitCode = 1;
    m_control_reg_init_code_clear_time = m_system.GetSystemTimers().GetFakeTimeBase() + 130;
  }

  m_dsp_control.Hex = requested.Hex;
  return m_dsp_control.Hex;
}

u16 DSPHLE::DSP_ReadControlRegister()
{
  if (m_dsp_control.DSPInitCode != 0)
  {
    if (m_system.GetSystemTimers().GetFakeTimeBase() >= m_control_reg_init_code_clear_time)
      m_dsp_control.DSPInitCode = 0;
    else
      m_system.GetCoreTiming().ForceExceptionCheck(50);  // come back soon to clear it
  }
  return m_dsp_control.Hex;
}
}  // namespace DSP::HLE
