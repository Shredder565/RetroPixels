// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/WiimoteSettings.h"

#include "Core/HW/Wiimote.h"

namespace Config
{
namespace
{
Location SourceKey(std::string_view section)
{
  return Location{System::WiiPad, std::string(section), "Source"};
}
}  // namespace

// Only the first Wii Remote is connected (emulated) by default.
const Info<WiimoteSource> WIIMOTE_1_SOURCE{SourceKey("Wiimote1"), WiimoteSource::Emulated};
const Info<WiimoteSource> WIIMOTE_2_SOURCE{SourceKey("Wiimote2"), WiimoteSource::None};
const Info<WiimoteSource> WIIMOTE_3_SOURCE{SourceKey("Wiimote3"), WiimoteSource::None};
const Info<WiimoteSource> WIIMOTE_4_SOURCE{SourceKey("Wiimote4"), WiimoteSource::None};
const Info<WiimoteSource> WIIMOTE_BB_SOURCE{SourceKey("BalanceBoard"), WiimoteSource::None};

const Info<WiimoteSource>& GetInfoForWiimoteSource(int index)
{
  // Indices 0-3 are the four Wii Remote slots, index 4 is the balance board.
  switch (index)
  {
  case 0:
    return WIIMOTE_1_SOURCE;
  case 1:
    return WIIMOTE_2_SOURCE;
  case 2:
    return WIIMOTE_3_SOURCE;
  case 3:
    return WIIMOTE_4_SOURCE;
  default:
    return WIIMOTE_BB_SOURCE;
  }
}

}  // namespace Config
