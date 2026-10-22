// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/GraphicsModSystem/Runtime/Actions/PrintAction.h"

#include "Common/Logging/Log.h"

void PrintAction::OnDrawStarted(GraphicsModActionData::DrawStarted*)
{
  INFO_LOG_FMT(VIDEO, "OnDrawStarted Called");
}

void PrintAction::BeforeEFB(GraphicsModActionData::PreEFB* efb)
{
  if (efb == nullptr || efb->scaled_width == nullptr || efb->scaled_height == nullptr) [[unlikely]]
    return;

  INFO_LOG_FMT(VIDEO, "OnEFB Called. Original [{}, {}], Scaled [{}, {}]", efb->texture_width,
               efb->texture_height, *efb->scaled_width, *efb->scaled_height);
}

void PrintAction::OnProjection(GraphicsModActionData::Projection*)
{
  INFO_LOG_FMT(VIDEO, "OnProjection Called");
}

void PrintAction::OnProjectionAndTexture(GraphicsModActionData::Projection*)
{
  INFO_LOG_FMT(VIDEO, "OnProjectionAndTexture Called");
}

void PrintAction::OnTextureLoad(GraphicsModActionData::TextureLoad* texture_load)
{
  if (texture_load == nullptr) [[unlikely]]
    return;

  // Two spaces before "Texture:" match the existing log format.
  INFO_LOG_FMT(VIDEO, "OnTextureLoad Called.  Texture: {}", texture_load->texture_name);
}
