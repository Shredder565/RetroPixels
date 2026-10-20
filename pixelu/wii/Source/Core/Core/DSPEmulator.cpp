// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSPEmulator.h"

#include <memory>

#include "Core/HW/DSPHLE/DSPHLE.h"
#include "Core/HW/DSPLLE/DSPLLE.h"

DSPEmulator::~DSPEmulator() = default;

std::unique_ptr<DSPEmulator> CreateDSPEmulator(Core::System& system, bool hle)
{
  std::unique_ptr<DSPEmulator> emulator;
  if (hle)
    emulator = std::make_unique<DSP::HLE::DSPHLE>(system);
  else
    emulator = std::make_unique<DSP::LLE::DSPLLE>();
  return emulator;
}
