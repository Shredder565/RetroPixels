// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

// Drops matching draws and EFB copies.
class SkipAction final : public GraphicsModAction
{
public:
  static constexpr std::string_view factory_name = "skip";
  void OnDrawStarted(GraphicsModActionData::DrawStarted*) override;
  void BeforeEFB(GraphicsModActionData::PreEFB*) override;
};
