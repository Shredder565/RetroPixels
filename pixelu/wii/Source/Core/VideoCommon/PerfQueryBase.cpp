// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/PerfQueryBase.h"

#include <memory>

#include "VideoCommon/VideoConfig.h"

std::unique_ptr<PerfQueryBase> g_perf_query;

bool PerfQueryBase::ShouldEmulate()
{
  // Set per game ("PerfQueriesEnable" in the game INI's video settings).
  return g_ActiveConfig.bPerfQueriesEnable;
}
