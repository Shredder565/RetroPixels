// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/SkipAction.h"

namespace
{
void SetIfPresent(bool* flag)
{
  if (flag != nullptr) [[likely]]
    *flag = true;
}
}  // namespace

void SkipAction::OnDrawStarted(GraphicsModActionData::DrawStarted* draw_started)
{
  if (draw_started != nullptr) [[likely]]
    SetIfPresent(draw_started->skip);
}

void SkipAction::BeforeEFB(GraphicsModActionData::PreEFB* efb)
{
  if (efb != nullptr) [[likely]]
    SetIfPresent(efb->skip);
}
