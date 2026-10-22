// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "VideoCommon/AbstractGfx.h"

class SWOGLWindow;

namespace SW
{
// The graphics device of the software renderer. Textures live in system memory, shaders and
// pipelines are empty placeholders, and finished frames go to an OpenGL window for display.
class SWGfx final : public AbstractGfx
{
public:
  explicit SWGfx(std::unique_ptr<SWOGLWindow> window);
  ~SWGfx() override;

  bool IsHeadless() const override;
  bool SupportsUtilityDrawing() const override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  bool BindBackbuffer(const ClearColor& clear_color = {}) override;

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

  void ShowImage(const AbstractTexture* source_texture,
                 const MathUtil::Rectangle<int>& source_rc) override;

  void ScaleTexture(AbstractFramebuffer* dst_framebuffer, const MathUtil::Rectangle<int>& dst_rect,
                    const AbstractTexture* src_texture,
                    const MathUtil::Rectangle<int>& src_rect) override;

  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable, bool alphaEnable,
                   bool zEnable, u32 color, u32 z) override;

  SurfaceInfo GetSurfaceInfo() const override;

private:
  std::unique_ptr<SWOGLWindow> m_window;
};

}  // namespace SW
