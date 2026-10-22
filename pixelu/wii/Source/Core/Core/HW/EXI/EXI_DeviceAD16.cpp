// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceAD16.h"

#include <array>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace ExpansionInterface
{
namespace
{
constexpr u32 CMD_READ_ID = 0x00;
constexpr u32 CMD_WRITE = 0xa0;
constexpr u32 CMD_READ = 0xa2;

// The ID command loads the board's ID into the register and then shifts it out after one dummy
// byte. The register keeps the bytes in the order of a little-endian host word, so reading the
// four bytes gives 0x04120000.
constexpr std::array<u8, 4> AD16_ID = {0x04, 0x12, 0x00, 0x00};
}  // namespace

CEXIAD16::CEXIAD16(Core::System& system) : IEXIDevice(system)
{
}

void CEXIAD16::SetCS(int cs)
{
  if (cs != 0)
    m_position = 0;
}

bool CEXIAD16::IsPresent() const
{
  return true;
}

void CEXIAD16::TransferByte(u8& byte)
{
  const u32 position = m_position++;
  if (position == 0)
  {
    m_command = byte;
    return;
  }

  switch (m_command)
  {
  case CMD_READ_ID:
    m_register = AD16_ID;
    if (position == 1)
      DEBUG_ASSERT(byte == 0x00);
    else if (position <= 5)
      byte = m_register[position - 2];
    break;

  case CMD_WRITE:
    if (position <= 4)
      m_register[position - 1] = byte;
    if (position == 4)
    {
      INFO_LOG_FMT(EXPANSIONINTERFACE, "AD16 register now {:02x} {:02x} {:02x} {:02x}",
                   m_register[0], m_register[1], m_register[2], m_register[3]);
    }
    break;

  case CMD_READ:
    if (position <= 4)
      byte = m_register[position - 1];
    break;

  default:
    break;
  }
}

void CEXIAD16::DoState(PointerWrap& p)
{
  p.Do(m_position);
  p.Do(m_command);
  p.Do(m_register);
}
}  // namespace ExpansionInterface
