// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/MagCard/MagneticCardReader.h"

namespace MagCard
{

// The F-Zero AX card reader (CRP-1231BR-10). It has a shutter in front of the
// card slot that the game opens and closes.
class C1231BR final : public MagneticCardReader
{
public:
  using MagneticCardReader::MagneticCardReader;

protected:
  bool IsReadyForCard() override;

  u8 GetPositionValue() override;

  void Command_10_Initialize() override;
  void Command_D0_ControlShutter() override;

  void DoState(PointerWrap& p) override;

private:
  bool m_shutter_open = true;
};

}  // namespace MagCard
