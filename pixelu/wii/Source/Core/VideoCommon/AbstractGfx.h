// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/HookableEvent.h"
#include "Common/MathUtil.h"

#include "VideoCommon/RenderState.h"

#include <array>
#include <memory>
#include <vector>

class AbstractFramebuffer;
class AbstractPipeline;
class AbstractShader;
class AbstractTexture;
class AbstractStagingTexture;
class NativeVertexFormat;
struct ComputePipelineConfig;
struct AbstractPipelineConfig;
struct PortableVertexDeclaration;
struct TextureConfig;
enum class AbstractTextureFormat : u32;
enum class ShaderStage;
enum class StagingTextureType;

// Size, DPI scale and format of the window surface.
struct SurfaceInfo
{
  u32 width = 0;
  u32 height = 0;
  float scale = 0.0f;
  AbstractTextureFormat format = {};
};

namespace VideoCommon
{
class AsyncShaderCompiler;
class ShaderIncluder;
}  // namespace VideoCommon

using ClearColor = std::array<float, 4>;

// The host graphics device as seen by VideoCommon. Each video backend implements one. It only
// deals in host objects (textures, framebuffers, shaders, pipelines) and draw calls; everything
// about the emulated GPU lives elsewhere.
class AbstractGfx
{
public:
  AbstractGfx();
  virtual ~AbstractGfx() = default;

  virtual bool IsHeadless() const = 0;

  // False when the backend cannot draw the UI or run post-processing passes.
  virtual bool SupportsUtilityDrawing() const { return true; }

  // Pipeline and resource binding.
  virtual void SetPipeline(const AbstractPipeline* /*pipeline*/) {}
  virtual void SetScissorRect(const MathUtil::Rectangle<int>& /*rc*/) {}
  virtual void SetTexture(u32 /*index*/, const AbstractTexture* /*texture*/) {}
  virtual void SetSamplerState(u32 /*index*/, const SamplerState& /*state*/) {}
  virtual void SetComputeImageTexture(u32 /*index*/, AbstractTexture* /*texture*/, bool /*read*/,
                                      bool /*write*/)
  {
  }
  virtual void UnbindTexture(const AbstractTexture* /*texture*/) {}
  virtual void SetViewport(float /*x*/, float /*y*/, float /*width*/, float /*height*/,
                           float /*near_depth*/, float /*far_depth*/)
  {
  }
  virtual void SetFullscreen(bool /*enable_fullscreen*/) {}
  virtual bool IsFullscreen() const { return false; }

  // Bracket draws that are not part of the emulated GPU's command stream (UI, copies,
  // post-processing). The end call puts the EFB framebuffer, viewport and scissor back.
  virtual void BeginUtilityDrawing();
  virtual void EndUtilityDrawing();

  // Object creation.
  virtual std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                         std::string_view name = "") = 0;
  virtual std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) = 0;
  virtual std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments = {}) = 0;

  // Render target binding. The base versions only record the current framebuffer.
  virtual void SetFramebuffer(AbstractFramebuffer* framebuffer);
  virtual void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer);
  virtual void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                      const ClearColor& color_value = {}, float depth_value = 0.0f);

  // Clears part of the current target. The base version draws a full-screen triangle with one
  // of the framebuffer manager's clear pipelines.
  virtual void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool colorEnable,
                           bool alphaEnable, bool zEnable, u32 color, u32 z);

  // Draw with whatever is bound.
  virtual void Draw(u32 /*base_vertex*/, u32 /*num_vertices*/) {}
  virtual void DrawIndexed(u32 /*base_index*/, u32 /*num_indices*/, u32 /*base_vertex*/) {}

  // Run a compute shader with whatever is bound.
  virtual void DispatchComputeShader(const AbstractShader* /*shader*/, u32 /*groupsize_x*/,
                                     u32 /*groupsize_y*/, u32 /*groupsize_z*/, u32 /*groups_x*/,
                                     u32 /*groups_y*/, u32 /*groups_z*/)
  {
  }

  // Binds the window's back buffer and clears it. Window size changes are picked up here. When
  // this fails nothing should be drawn, but PresentBackbuffer must still be called so per-frame
  // state is reset.
  virtual bool BindBackbuffer(const ClearColor& /*clear_color*/ = {}) { return true; }

  // Shows the back buffer (swap).
  virtual void PresentBackbuffer() {}

  // Shader and pipeline objects.
  virtual std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer = nullptr,
                         std::string_view name = "") = 0;
  virtual std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage,
                                                                 const void* data, size_t length,
                                                                 std::string_view name = "") = 0;
  virtual std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) = 0;
  virtual std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                           const void* cache_data = nullptr,
                                                           size_t cache_data_length = 0) = 0;

  AbstractFramebuffer* GetCurrentFramebuffer() const { return m_current_framebuffer; }

  struct ViewportAndScissor
  {
    MathUtil::Rectangle<int> scissor_rect;
    float viewport_x = 0;
    float viewport_y = 0;
    float viewport_width = 0;
    float viewport_height = 0;
    float viewport_near_depth = 0;
    float viewport_far_depth = 0;
  };

  // Remembers the emulated viewport and scissor; EndUtilityDrawing restores them.
  void StoreViewportAndScissor(const ViewportAndScissor& viewport_and_scissor);

  // Sets both viewport and scissor to rect (framebuffer coordinates, so lower-left origin on
  // backends that use it).
  void SetViewportAndScissor(const MathUtil::Rectangle<int>& rect, float min_depth = 0.0f,
                             float max_depth = 1.0f);

  // Copies src_rect of src_texture into dst_rect of an RGBA8 framebuffer with linear filtering.
  virtual void ScaleTexture(AbstractFramebuffer* dst_framebuffer,
                            const MathUtil::Rectangle<int>& dst_rect,
                            const AbstractTexture* src_texture,
                            const MathUtil::Rectangle<int>& src_rect);

  // Flips an upper-left-origin rectangle vertically for lower-left-origin backends.
  MathUtil::Rectangle<int> ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                                                       u32 fb_width, u32 fb_height) const;
  MathUtil::Rectangle<int>
  ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                              const AbstractFramebuffer* framebuffer) const;

  virtual void Flush() {}
  virtual void WaitForGPUIdle() {}

  // Eye selection for quad-buffered stereo on OpenGL.
  virtual void SelectLeftBuffer() {}
  virtual void SelectRightBuffer() {}
  virtual void SelectMainBuffer() {}

  // Minimal presentation path used by the software renderer.
  virtual void ShowImage(const AbstractTexture* /*source_texture*/,
                         const MathUtil::Rectangle<int>& /*source_rc*/)
  {
  }

  virtual std::unique_ptr<VideoCommon::AsyncShaderCompiler> CreateAsyncShaderCompiler();

  // Called from the config-changed event with CONFIG_CHANGE_BIT_* flags.
  virtual void OnConfigChanged(u32 changed_bits);

  // True when UI drawn to the output should go through a layer-expanding geometry shader
  // (quad-buffered stereo on backends with a two-layer back buffer).
  bool UseGeometryShaderForUI() const;

  // The window surface (back buffer).
  virtual SurfaceInfo GetSurfaceInfo() const = 0;

protected:
  AbstractFramebuffer* m_current_framebuffer = nullptr;
  const AbstractPipeline* m_current_pipeline = nullptr;

private:
  Common::EventHook m_on_config_changed;
  ViewportAndScissor m_saved_viewport;
};

extern std::unique_ptr<AbstractGfx> g_gfx;
