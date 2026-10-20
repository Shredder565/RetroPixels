// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Null/NullGfx.h"

#include <utility>

#include "VideoBackends/Null/NullTexture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/VideoConfig.h"

namespace Null
{
namespace
{
class PlaceholderShader final : public AbstractShader
{
public:
  explicit PlaceholderShader(ShaderStage stage) : AbstractShader(stage) {}
};

class PlaceholderPipeline final : public AbstractPipeline
{
};
}  // namespace

// The active config is refreshed when the device comes and goes, as the other backends do.
NullGfx::NullGfx()
{
  UpdateActiveConfig();
}

NullGfx::~NullGfx()
{
  UpdateActiveConfig();
}

bool NullGfx::IsHeadless() const
{
  return true;
}

bool NullGfx::SupportsUtilityDrawing() const
{
  return false;
}

std::unique_ptr<AbstractTexture> NullGfx::CreateTexture(const TextureConfig& config,
                                                        std::string_view)
{
  return std::make_unique<NullTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> NullGfx::CreateStagingTexture(StagingTextureType type,
                                                                      const TextureConfig& config)
{
  return std::make_unique<NullStagingTexture>(type, config);
}

std::unique_ptr<AbstractFramebuffer>
NullGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                           std::vector<AbstractTexture*> additional_color_attachments)
{
  return NullFramebuffer::Create(static_cast<NullTexture*>(color_attachment),
                                 static_cast<NullTexture*>(depth_attachment),
                                 std::move(additional_color_attachments));
}

std::unique_ptr<AbstractShader> NullGfx::CreateShaderFromSource(ShaderStage stage, std::string_view,
                                                                VideoCommon::ShaderIncluder*,
                                                                std::string_view)
{
  return std::make_unique<PlaceholderShader>(stage);
}

std::unique_ptr<AbstractShader> NullGfx::CreateShaderFromBinary(ShaderStage stage, const void*,
                                                                size_t, std::string_view)
{
  return std::make_unique<PlaceholderShader>(stage);
}

std::unique_ptr<NativeVertexFormat>
NullGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<NativeVertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> NullGfx::CreatePipeline(const AbstractPipelineConfig&,
                                                          const void*, size_t)
{
  return std::make_unique<PlaceholderPipeline>();
}

void NullEFBInterface::ReinterpretPixelData(EFBReinterpretType)
{
}

void NullEFBInterface::PokeColor(u16, u16, u32)
{
}

void NullEFBInterface::PokeDepth(u16, u16, u32)
{
}

u32 NullEFBInterface::PeekColorInternal(u16, u16)
{
  return 0;
}

u32 NullEFBInterface::PeekDepthInternal(u16, u16)
{
  return 0;
}

}  // namespace Null
