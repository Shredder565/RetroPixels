// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionFactory.h"

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/CustomPipelineAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/MoveAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/PrintAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/ScaleAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/Actions/SkipAction.h"

namespace GraphicsModActionFactory
{
std::unique_ptr<GraphicsModAction> Create(std::string_view name, const picojson::value& json_data,
                                          std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  // Actions without settings.
  if (name == PrintAction::factory_name)
    return std::make_unique<PrintAction>();
  if (name == SkipAction::factory_name)
    return std::make_unique<SkipAction>();

  // Actions configured from the feature's action data.
  if (name == MoveAction::factory_name)
    return MoveAction::Create(json_data);
  if (name == ScaleAction::factory_name)
    return ScaleAction::Create(json_data);
  if (name == CustomPipelineAction::factory_name)
    return CustomPipelineAction::Create(json_data, std::move(library));

  return nullptr;
}
}  // namespace GraphicsModActionFactory
