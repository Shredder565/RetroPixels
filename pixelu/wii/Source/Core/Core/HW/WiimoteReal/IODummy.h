// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Core/HW/WiimoteReal/WiimoteReal.h"

namespace WiimoteReal
{
// Stands in for a scanner backend the platform does not have: finds nothing.
class WiimoteScannerDummy final : public WiimoteScannerBackend
{
public:
  WiimoteScannerDummy() = default;
  ~WiimoteScannerDummy() override = default;

  void Update() override {}
  void RequestStopSearching() override {}
};
}  // namespace WiimoteReal
