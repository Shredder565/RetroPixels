// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DSPHLE/MailHandler.h"

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/DSP.h"

namespace DSP::HLE
{
CMailHandler::CMailHandler(DSP::DSPManager& dsp) : m_dsp(dsp)
{
}

CMailHandler::~CMailHandler() = default;

void CMailHandler::PushMail(u32 mail, bool interrupt, int cycles_into_future)
{
  if (interrupt)
  {
    // Nothing in front of this mail: interrupt now. Otherwise the interrupt is attached to the
    // mail the CPU will read next.
    if (m_queue.empty())
      m_dsp.GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP, cycles_into_future);
    else
      m_queue.front().second = true;
  }

  m_queue.emplace_back(mail, false);
  DEBUG_LOG_FMT(DSP_MAIL, "DSP mail out: {:08x}", mail);
}

u16 CMailHandler::ReadDSPMailboxHigh()
{
  if (CanDeliver())
    m_mailbox = m_queue.front().first;

  return static_cast<u16>(m_mailbox >> 16);
}

u16 CMailHandler::ReadDSPMailboxLow()
{
  if (CanDeliver())
  {
    const auto [mail, raise_interrupt] = m_queue.front();
    m_queue.pop_front();
    m_mailbox = mail;
    if (raise_interrupt)
      m_dsp.GenerateDSPInterruptFromDSPEmu(DSP::INT_DSP);
  }

  // Reading the low half consumes the mail: the next high-half read shows bit 31 cleared.
  m_mailbox &= 0x7FFF'FFFFu;
  return static_cast<u16>(m_mailbox);
}

void CMailHandler::ClearPending()
{
  m_queue.clear();
}

bool CMailHandler::HasPending() const
{
  return !m_queue.empty();
}

void CMailHandler::SetHalted(bool halt)
{
  m_halted = halt;
}

void CMailHandler::DoState(PointerWrap& p)
{
  p.Do(m_queue);
  p.Do(m_mailbox);
  p.Do(m_halted);
}
}  // namespace DSP::HLE
