// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "IRQ.h"

#include "Supermodel.h"
#include "CPU/PowerPC/ppc.h"

void CIRQ::SaveState(CBlockFile *SaveState)
{
  SaveState->NewBlock("IRQ", __FILE__);
  uint32_t words[2] = { uint32_t(m_enabled), uint32_t(m_pending) };
  SaveState->Write(words, sizeof(words));
}

void CIRQ::LoadState(CBlockFile *SaveState)
{
  if (SaveState->FindBlock("IRQ") != Result::OKAY)
  {
    ErrorLog("Unable to load IRQ controller state. Save state file is corrupt.");
    return;
  }
  uint32_t words[2] = { uint32_t(m_enabled), uint32_t(m_pending) };
  SaveState->Read(words, sizeof(words));
  m_enabled = words[0];
  m_pending = words[1];
}

void CIRQ::Assert(unsigned irqBits)
{
  m_pending |= irqBits;
  // The line is driven once for a pending enabled source and once more for
  // a pending unmaskable one (the CPU core counts the calls).
  if ((m_pending & m_enabled) != 0)
    ppc_set_irq_line(1);
  if ((m_pending & kUnmaskable) != 0)
    ppc_set_irq_line(1);
}

void CIRQ::Deassert(unsigned irqBits)
{
  m_pending &= ~irqBits;
  if ((m_pending & m_enabled) == 0 && (m_pending & kUnmaskable) == 0)
    ppc_set_irq_line(0);
}

void CIRQ::WriteIRQEnable(UINT8 data)
{
  m_enabled = data;
}

UINT8 CIRQ::ReadIRQEnable(void) const
{
  return UINT8(m_enabled);
}

UINT8 CIRQ::ReadIRQState(void) const
{
  return UINT8(m_pending);
}

void CIRQ::Reset(void)
{
  m_enabled = 0;
  m_pending = 0;
}

void CIRQ::Init(void)
{
}

CIRQ::CIRQ(void)
{
}

CIRQ::~CIRQ(void)
{
}
