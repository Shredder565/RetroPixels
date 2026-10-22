// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Config/Config.h"

enum class WiimoteSource;

namespace Config
{
extern const Info<WiimoteSource> WIIMOTE_1_SOURCE;
extern const Info<WiimoteSource> WIIMOTE_2_SOURCE;
extern const Info<WiimoteSource> WIIMOTE_3_SOURCE;
extern const Info<WiimoteSource> WIIMOTE_4_SOURCE;
extern const Info<WiimoteSource> WIIMOTE_BB_SOURCE;

const Info<WiimoteSource>& GetInfoForWiimoteSource(int index);

}  // namespace Config
