// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/MagCard/MagneticCardReader.h"

namespace MagCard
{

// The Mario Kart Arcade GP / GP2 card reader (CRP-1231LR-10NAB).
class C1231LR final : public MagneticCardReader
{
public:
  using MagneticCardReader::MagneticCardReader;

protected:
  u8 GetPositionValue() override;
};

}  // namespace MagCard
