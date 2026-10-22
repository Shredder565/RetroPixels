// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/Triforce/SerialDevice.h"

#include <algorithm>
#include <iterator>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"

namespace Triforce
{

void SerialDevice::WriteRxBytes(std::span<const u8> bytes)
{
  std::ranges::copy(bytes, std::back_inserter(m_inbound));
}

void SerialDevice::TakeTxBytes(std::span<u8> bytes)
{
  DEBUG_ASSERT(bytes.size() <= m_outbound.size());

  const auto end = m_outbound.begin() + static_cast<std::ptrdiff_t>(bytes.size());
  std::copy(m_outbound.begin(), end, bytes.begin());
  m_outbound.erase(m_outbound.begin(), end);
}

void SerialDevice::ConsumeRxBytes(std::size_t count)
{
  DEBUG_ASSERT(count <= m_inbound.size());

  m_inbound.erase(m_inbound.begin(), m_inbound.begin() + static_cast<std::ptrdiff_t>(count));
}

void SerialDevice::WriteTxBytes(std::span<const u8> bytes)
{
  std::ranges::copy(bytes, std::back_inserter(m_outbound));
}

void SerialDevice::PassThroughTxBytes(SerialDevice& other)
{
  std::deque<u8>& theirs = other.m_outbound;
  if (theirs.size() > m_outbound.size())
  {
    // Cheaper to put our (short) queue in front of theirs and take theirs.
    theirs.insert(theirs.begin(), m_outbound.begin(), m_outbound.end());
    m_outbound.swap(theirs);
  }
  else
  {
    m_outbound.insert(m_outbound.end(), theirs.begin(), theirs.end());
  }
  theirs.clear();
}

void SerialDevice::DoState(PointerWrap& p)
{
  p.Do(m_inbound);
  p.Do(m_outbound);
}

}  // namespace Triforce
