// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string_view>

#include <picojson.h>

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

// Scales matching projections and EFB copy sizes. Action data: {"X": .., "Y": .., "Z": ..}.
class ScaleAction final : public GraphicsModAction
{
public:
  static constexpr std::string_view factory_name = "scale";
  static std::unique_ptr<ScaleAction> Create(const picojson::value& json_data);
  explicit ScaleAction(Common::Vec3 scale);
  void BeforeEFB(GraphicsModActionData::PreEFB*) override;
  void OnProjection(GraphicsModActionData::Projection*) override;
  void OnProjectionAndTexture(GraphicsModActionData::Projection*) override;

private:
  Common::Vec3 m_factors;
};
