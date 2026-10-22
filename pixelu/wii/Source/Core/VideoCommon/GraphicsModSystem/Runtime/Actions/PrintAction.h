// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

// Logs each event it receives; useful when writing a mod.
class PrintAction final : public GraphicsModAction
{
public:
  static constexpr std::string_view factory_name = "print";
  void OnDrawStarted(GraphicsModActionData::DrawStarted*) override;
  void BeforeEFB(GraphicsModActionData::PreEFB*) override;
  void OnProjection(GraphicsModActionData::Projection*) override;
  void OnProjectionAndTexture(GraphicsModActionData::Projection*) override;
  void OnTextureLoad(GraphicsModActionData::TextureLoad*) override;
};
