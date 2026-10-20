// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <deque>
#include <utility>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace DSP
{
class DSPManager;
}

namespace DSP::HLE
{
// The DSP-to-CPU mailbox as seen by the HLE ucodes.
//
// The hardware only has one mailbox register pair. An HLE ucode answers a CPU request all at
// once, though, often with several mails, so the replies wait in a queue here and the CPU gets
// them one by one as it reads the mailbox (high half first, then low half).
class CMailHandler
{
public:
  explicit CMailHandler(DSP::DSPManager& dsp);
  CMailHandler(const CMailHandler&) = delete;
  CMailHandler(CMailHandler&&) = delete;
  CMailHandler& operator=(const CMailHandler&) = delete;
  CMailHandler& operator=(CMailHandler&&) = delete;
  ~CMailHandler();

  // Queues a mail for the CPU. With `interrupt`, the CPU also gets a DSP interrupt: after
  // `cycles_into_future` cycles when the queue was empty, otherwise when the mail currently at the
  // head of the queue has been read.
  void PushMail(u32 mail, bool interrupt = false, int cycles_into_future = 0);

  // A halted DSP delivers nothing new; the CPU keeps reading the last mail.
  void SetHalted(bool halt);

  void DoState(PointerWrap& p);
  bool HasPending() const;

  // Drops every queued mail (used when the ucode changes). The last delivered mail stays readable.
  void ClearPending();

  u16 ReadDSPMailboxHigh();
  u16 ReadDSPMailboxLow();

private:
  bool CanDeliver() const { return !m_halted && !m_queue.empty(); }

  // Queued mails, oldest first, each with "raise an interrupt once this one has been read".
  std::deque<std::pair<u32, bool>> m_queue;

  // What the mailbox registers currently hold. Once the low half has been read, bit 31 (the
  // "mail waiting" flag) reads back as clear until another mail is delivered.
  u32 m_mailbox = 0;

  bool m_halted = false;

  DSP::DSPManager& m_dsp;
};
}  // namespace DSP::HLE
