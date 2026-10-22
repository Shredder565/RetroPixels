// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Z80 CTC channels (see Z80CTC.h). Control word bits:
//   7 interrupt enable   6 counter mode      5 prescaler 256 (else 16)
//   4 rising edge        3 wait for trigger  2 time constant follows
//   1 software reset     0 1 = control word (0 = vector or time constant)

#include "Z80CTC.h"

void Z80CTC::Write(UINT32 channel, UINT8 value)
{
  Channel &c = m_channel[channel];
  if (value & 0x01)
  {
    c.control = value;
    c.interrupt = (value & 0x80) != 0;
    c.counterMode = (value & 0x40) != 0;
    c.prescale256 = (value & 0x20) != 0;
    c.risingEdge = (value & 0x10) != 0;
    c.waitForTrigger = (value & 0x08) != 0;
    c.expectConstant = (value & 0x04) != 0;
    if (value & 0x02)
    {
      c.counter = 0;
      c.running = false;
    }
  }
  else if (c.expectConstant)
  {
    // 0 stands for 256, which does not fit the 8-bit constant: it stays 0
    // (and the timer's rate then divides by zero, as it always has).
    c.timeConstant = (UINT8)(value != 0 ? value : 256);
    c.counter = c.timeConstant;
    c.expectConstant = false;
    c.running = !c.waitForTrigger;
  }
  // (an interrupt vector otherwise: not used)
}

UINT32 Z80CTC::CalcFrequency(UINT32 channel, UINT32 inputFrequency)
{
  const Channel &c = m_channel[channel];
  if (!c.counterMode)
    return inputFrequency / (c.timeConstant * (c.prescale256 ? 256u : 16u));
  if (c.risingEdge)
  {
    // counting the previous channel's timer
    return CalcFrequency(channel - 1, inputFrequency) / c.counter;
  }
  return 0;
}

bool Z80CTC::TimerRunning(UINT32 channel) const
{
  return m_channel[channel].running;
}

void Z80CTC::Reset()
{
  for (Channel &c : m_channel)
    c = Channel{};
}

void Z80CTC::SaveState(CBlockFile *saveState) const
{
  saveState->Write(m_channel, sizeof(m_channel));
}

void Z80CTC::LoadState(CBlockFile *saveState)
{
  saveState->Read(m_channel, sizeof(m_channel));
}
