// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

// Timing model of the GameCube/Wii optical drive. All results are in seconds.
namespace DVDMath
{
// Distance in metres from the disc centre to the track holding the given byte.
double CalculatePhysicalDiscPosition(u64 offset);

// Head travel time between two byte offsets.
double CalculateSeekTime(u64 offset_from, u64 offset_to);

// Time until the given byte rotates under the head, starting at `time` seconds.
double CalculateRotationalLatency(u64 offset, double time, bool wii_disc);

// Streaming time for `length` bytes starting at `offset`, seeks not included.
double CalculateRawDiscReadTime(u64 offset, u64 length, bool wii_disc);

}  // namespace DVDMath
