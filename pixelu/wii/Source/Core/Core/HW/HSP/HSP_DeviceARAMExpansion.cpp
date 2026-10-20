// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/HSP/HSP_DeviceARAMExpansion.h"

#include <algorithm>

#include "Common/ChunkFile.h"
#include "Common/MathUtil.h"
#include "Common/MemoryUtil.h"

#include "Core/Config/MainSettings.h"

namespace HSP
{
CHSPDevice_ARAMExpansion::CHSPDevice_ARAMExpansion()
    : m_bytes(MathUtil::NextPowerOf2(Config::Get(Config::MAIN_ARAM_EXPANSION_SIZE))),
      m_wrap(m_bytes - 1), m_storage(static_cast<u8*>(Common::AllocateMemoryPages(m_bytes)))
{
}

CHSPDevice_ARAMExpansion::~CHSPDevice_ARAMExpansion()
{
  Common::FreeMemoryPages(m_storage, m_bytes);
}

u8* CHSPDevice_ARAMExpansion::Block(u32 address) const
{
  return m_storage + (address & m_wrap);
}

void CHSPDevice_ARAMExpansion::Read(u32 address, std::span<u8, TRANSFER_SIZE> data)
{
  const u8* const block = Block(address);
  std::copy(block, block + TRANSFER_SIZE, data.begin());
}

void CHSPDevice_ARAMExpansion::Write(u32 address, std::span<const u8, TRANSFER_SIZE> data)
{
  std::copy(data.begin(), data.end(), Block(address));
}

void CHSPDevice_ARAMExpansion::DoState(PointerWrap& p)
{
  // The whole expansion, raw.
  p.DoArray(m_storage, m_bytes);
}

}  // namespace HSP
