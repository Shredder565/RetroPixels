// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/Triforce/SerialDevice.h"

namespace Triforce
{

// Touch panel on serial port B of The Key of Avalon cabinets. It only ever
// talks: each update sends one position packet built from the C stick.
class Touchscreen final : public SerialDevice
{
public:
  void Update() override;
};

}  // namespace Triforce
