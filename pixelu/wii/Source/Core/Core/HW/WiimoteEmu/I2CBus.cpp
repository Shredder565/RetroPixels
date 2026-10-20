// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/WiimoteEmu/I2CBus.h"

#include <algorithm>

namespace WiimoteEmu
{
void I2CBus::AddSlave(I2CSlave* slave)
{
  m_slaves.push_back(slave);
}

void I2CBus::RemoveSlave(I2CSlave* slave)
{
  m_slaves.erase(std::remove(m_slaves.begin(), m_slaves.end(), slave), m_slaves.end());
}

void I2CBus::Reset()
{
  m_slaves.clear();
}

int I2CBus::BusRead(u8 slave_addr, u8 addr, int count, u8* data_out)
{
  int transferred = 0;
  for (auto it = m_slaves.begin(); it != m_slaves.end() && transferred == 0; ++it)
    transferred = (*it)->BusRead(slave_addr, addr, count, data_out);
  return transferred;
}

int I2CBus::BusWrite(u8 slave_addr, u8 addr, int count, const u8* data_in)
{
  int transferred = 0;
  for (auto it = m_slaves.begin(); it != m_slaves.end() && transferred == 0; ++it)
    transferred = (*it)->BusWrite(slave_addr, addr, count, data_in);
  return transferred;
}

}  // namespace WiimoteEmu
