// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/EFBInterface.h"

namespace Null
{
// A graphics device that creates placeholder objects and has no surface to present to.
class NullGfx final : public AbstractGfx
{
public:
  NullGfx();
  ~NullGfx() override;

  bool IsHeadless() const override;
  bool SupportsUtilityDrawing() const override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer,
                         std::string_view name) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;
  SurfaceInfo GetSurfaceInfo() const override { return {}; }
};

// An EFB that holds nothing: peeks read zero and pokes and reinterprets are ignored.
class NullEFBInterface final : public EFBInterfaceBase
{
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;
};

}  // namespace Null
