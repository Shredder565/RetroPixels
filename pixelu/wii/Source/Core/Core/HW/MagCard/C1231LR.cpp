// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/MagCard/C1231LR.h"

namespace MagCard
{

u8 C1231LR::GetPositionValue()
{
  // ASCII '0' for no card, then one step per position along the card path.
  return static_cast<u8>(0x30 + static_cast<u8>(m_status.r));
}

}  // namespace MagCard
