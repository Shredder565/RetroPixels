// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <picojson.h>

#include "VideoCommon/Assets/CustomAssetLibrary.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"

// Applies a material asset as a post-processing pass after an EFB copy.
// Action data: {"material_asset": "<asset id>"}.
class CustomPipelineAction final : public GraphicsModAction
{
public:
  static constexpr std::string_view factory_name = "custom_pipeline";
  static std::unique_ptr<CustomPipelineAction>
  Create(const picojson::value& json_data,
         std::shared_ptr<VideoCommon::CustomAssetLibrary> library);
  static std::unique_ptr<CustomPipelineAction>
  Create(std::shared_ptr<VideoCommon::CustomAssetLibrary> library);
  explicit CustomPipelineAction(std::shared_ptr<VideoCommon::CustomAssetLibrary> library);
  CustomPipelineAction(std::shared_ptr<VideoCommon::CustomAssetLibrary> library,
                       std::string material_asset);
  void OnDrawStarted(GraphicsModActionData::DrawStarted*) override;
  void AfterEFB(GraphicsModActionData::PostEFB*) override;

private:
  std::shared_ptr<VideoCommon::CustomAssetLibrary> m_assets;
  std::string m_material_id;
};
