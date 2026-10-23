// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Outputs.h"

#include "Supermodel.h"

namespace
{
  // Protocol names, in EOutputs order.
  const char *const kNames[NUM_OUTPUTS] =
  {
    "pause",
    "LampStart", "LampView1", "LampView2", "LampView3", "LampView4", "LampLeader",
    "RawDrive", "RawLamps",
    "BillDigit1", "BillDigit2", "BillDigit3", "BillDigit4", "BillDigit5"
  };

  inline bool Valid(EOutputs output)
  {
    return int(output) >= 0 && int(output) < NUM_OUTPUTS;
  }
}

const char *COutputs::GetOutputName(EOutputs output)
{
  return Valid(output) ? kNames[int(output)] : NULL;
}

EOutputs COutputs::GetOutputByName(const char *name)
{
  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    if (stricmp(name, kNames[i]) == 0)
      return EOutputs(i);
  }
  return OutputUnknown;
}

COutputs::COutputs()
{
  for (int i = 0; i < NUM_OUTPUTS; i++)
  {
    m_set[i] = false;
    m_values[i] = 0;
  }
}

COutputs::~COutputs()
{
}

const Game &COutputs::GetGame() const
{
  return m_game;
}

void COutputs::SetGame(const Game &game)
{
  m_game = game;
}

UINT8 COutputs::GetValue(EOutputs output) const
{
  return Valid(output) ? m_values[int(output)] : 0;
}

void COutputs::SetValue(EOutputs output, UINT8 value)
{
  if (!Valid(output))
    return;
  int i = int(output);
  UINT8 previous = m_values[i];
  bool first = !m_set[i];
  m_set[i] = true;
  m_values[i] = value;
  if (first || previous != value)
    SendOutput(output, previous, value);
}

bool COutputs::HasValue(EOutputs output)
{
  return Valid(output) && m_set[int(output)];
}
