// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string_view>
#include <vector>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Constants.h"

class GLContext;

namespace OGL
{
class OGLFramebuffer;
class OGLTexture;

// The OpenGL implementation of the renderer interface. GL is a state machine, so this class
// remembers what it last set (pipeline state, texture units, image units) and skips redundant
// calls.
class OGLGfx final : public AbstractGfx
{
public:
  OGLGfx(std::unique_ptr<GLContext> main_gl_context, float backbuffer_scale);
  ~OGLGfx() override;

  bool IsHeadless() const override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
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
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable, bool alphaEnable,
                   bool zEnable, u32 color, u32 z) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void SetComputeImageTexture(u32 index, AbstractTexture* texture, bool read, bool write) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;
  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  void DispatchComputeShader(const AbstractShader* shader, u32 groupsize_x, u32 groupsize_y,
                             u32 groupsize_z, u32 groups_x, u32 groups_y, u32 groups_z) override;
  bool BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;

  void BeginUtilityDrawing() override;
  void EndUtilityDrawing() override;

  void Flush() override;
  void WaitForGPUIdle() override;
  void OnConfigChanged(u32 bits) override;

  void SelectLeftBuffer() override;
  void SelectRightBuffer() override;
  void SelectMainBuffer() override;

  std::unique_ptr<VideoCommon::AsyncShaderCompiler> CreateAsyncShaderCompiler() override;

  // GPU thread only.
  GLContext* GetMainGLContext() const { return m_context.get(); }
  bool IsGLES() const;

  // Forgets what is bound to a texture unit, for code that binds texel buffers there itself.
  void InvalidateTextureBinding(u32 index) { m_unit_textures[index] = nullptr; }

  // Two scratch framebuffer objects for copies done by attaching textures (blits, readbacks)
  // when no direct copy function is available.
  u32 GetSharedReadFramebuffer() const { return m_scratch_read_fbo; }
  u32 GetSharedDrawFramebuffer() const { return m_scratch_draw_fbo; }
  void BindSharedReadFramebuffer();
  void BindSharedDrawFramebuffer();

  // Binds the current framebuffer again after a scratch framebuffer was used.
  void RestoreFramebufferBinding();

  SurfaceInfo GetSurfaceInfo() const override;

private:
  void PickUpSurfaceChange();
  void PickUpSurfaceResize();
  void ResizeWindowFramebuffer();
  void SyncDebugOutputWithLogLevel();
  void SetClipDistances(bool enable);

  void SetRasterization(RasterizationState state);
  void SetDepth(DepthState state);
  void SetBlending(BlendingState state);

  std::unique_ptr<GLContext> m_context;
  std::unique_ptr<OGLFramebuffer> m_window_framebuffer;
  std::array<const OGLTexture*, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_unit_textures{};
  std::array<const AbstractTexture*, VideoCommon::MAX_COMPUTE_SHADER_SAMPLERS> m_image_units{};
  RasterizationState m_raster_state;
  DepthState m_depth_state;
  BlendingState m_blend_state;
  u32 m_scratch_read_fbo = 0;
  u32 m_scratch_draw_fbo = 0;
  float m_surface_scale;
};

inline OGLGfx* GetOGLGfx()
{
  return static_cast<OGLGfx*>(g_gfx.get());
}

}  // namespace OGL
