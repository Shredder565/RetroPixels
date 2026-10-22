// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Included before every implot source file. This implot version calls isnan()
// unqualified, which only compiles where <cmath> puts isnan in the global namespace;
// bring std::isnan in so it builds with every compiler.

#pragma once

#include <cmath>

using std::isnan;
