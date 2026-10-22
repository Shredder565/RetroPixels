// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/CustomPipelineAction.h"

#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Core/System.h"

#include "VideoCommon/Resources/CustomResourceManager.h"

std::unique_ptr<CustomPipelineAction>
CustomPipelineAction::Create(std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  return std::make_unique<CustomPipelineAction>(std::move(library));
}

std::unique_ptr<CustomPipelineAction>
CustomPipelineAction::Create(const picojson::value& json_data,
                             std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
{
  std::optional<std::string> material =
      ReadStringFromJson(json_data.get<picojson::object>(), "material_asset");
  if (!material)
  {
    ERROR_LOG_FMT(VIDEO,
                  "Failed to load custom pipeline action, 'material_asset' does not have a value");
    return nullptr;
  }
  return std::make_unique<CustomPipelineAction>(std::move(library), std::move(*material));
}

CustomPipelineAction::CustomPipelineAction(std::shared_ptr<VideoCommon::CustomAssetLibrary> library)
    : m_assets(std::move(library))
{
}

CustomPipelineAction::CustomPipelineAction(std::shared_ptr<VideoCommon::CustomAssetLibrary> library,
                                           std::string material_asset)
    : m_assets(std::move(library)), m_material_id(std::move(material_asset))
{
}

void CustomPipelineAction::OnDrawStarted(GraphicsModActionData::DrawStarted*)
{
  // Per-draw custom shaders are not implemented yet.
}

void CustomPipelineAction::AfterEFB(GraphicsModActionData::PostEFB* post_efb)
{
  if (post_efb == nullptr || m_material_id.empty()) [[unlikely]]
    return;

  // The resource manager owns the material; it may still be loading, in which case this
  // returns null and the copy is left unprocessed.
  post_efb->material =
      Core::System::GetInstance().GetCustomResourceManager().GetPostProcessingMaterialFromAsset(
          m_material_id, m_assets);
}
