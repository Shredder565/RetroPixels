// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Debugger/Dump.h"

#include <cstdint>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

// A trace file is a sequence of fixed-size records, one per executed step, each holding the
// register file as big-endian words.

CDump::CDump(const std::string& filename)
{
  File::IOFile file(filename, "rb");
  if (!file)
    return;

  m_size = static_cast<size_t>(file.GetSize());
  m_pData = new u8[m_size];
  file.ReadArray(m_pData, m_size);
}

CDump::~CDump()
{
  delete[] m_pData;
}

int CDump::GetNumberOfSteps()
{
  return static_cast<int>(m_size / STRUCTUR_SIZE);
}

u32 CDump::GetGPR(int _step, int _gpr)
{
  const u32 record = static_cast<u32>(_step) * STRUCTUR_SIZE;
  if (record >= m_size)
    return UINT32_MAX;
  return Read32(record + OFFSET_GPR + static_cast<u32>(_gpr) * 4);
}

u32 CDump::GetPC(int _step)
{
  const u32 record = static_cast<u32>(_step) * STRUCTUR_SIZE;
  if (record >= m_size)
    return UINT32_MAX;
  return Read32(record + OFFSET_PC);
}

u32 CDump::Read32(u32 _pos)
{
  // A truncated last record has no data to return.
  if (static_cast<size_t>(_pos) + 4 > m_size)
    return UINT32_MAX;

  u32 word = 0;
  for (u32 i = 0; i < 4; ++i)
    word = (word << 8) | m_pData[_pos + i];
  return word;
}
