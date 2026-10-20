// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "VideoCommon/PerfQueryBase.h"

namespace Null
{
// Nothing is drawn, so every counter stays at zero and results are always ready.
class PerfQuery final : public PerfQueryBase
{
public:
  void EnableQuery(PerfQueryGroup) override {}
  void DisableQuery(PerfQueryGroup) override {}
  void ResetQuery() override {}
  u32 GetQueryResult(PerfQueryType) override { return 0; }
  void FlushResults() override {}
  bool IsFlushed() const override { return true; }
};

}  // namespace Null
