// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/Constants.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/Constants.h"

namespace Vulkan
{
class SwapChain;
class VKFramebuffer;
class VKPipeline;
class VKTexture;

// The renderer interface on top of the state tracker, command buffer manager and swap chain.
class VKGfx final : public ::AbstractGfx
{
public:
  VKGfx(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale);
  ~VKGfx() override;

  static VKGfx* GetInstance() { return static_cast<VKGfx*>(g_gfx.get()); }

  bool IsHeadless() const override;

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

  SwapChain* GetSwapChain() const { return m_swap_chain.get(); }

  void Flush() override;
  void WaitForGPUIdle() override;
  void OnConfigChanged(u32 bits) override;

  void ClearRegion(const MathUtil::Rectangle<int>& target_rc, bool color_enable, bool alpha_enable,
                   bool z_enable, u32 color, u32 z) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
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
  void SetFullscreen(bool enable_fullscreen) override;
  bool IsFullscreen() const override;

  SurfaceInfo GetSurfaceInfo() const override;

  // Ends the render pass, submits the command buffer (optionally on the worker thread and/or
  // waiting for it) and forgets the recorded state. Used to free ring buffer space too.
  void ExecuteCommandBuffer(bool execute_off_thread, bool wait_for_completion = false);

private:
  // Handle window changes signalled by the presenter.
  void HandleSurfaceChange();
  void HandleSurfaceResize();
  // Recovers from a failed acquire or present; returns the new acquire result.
  VkResult RecoverSwapChain(VkResult failure);

  void ResetSamplerStates();
  void PublishBackbufferSize();
  void BindFramebuffer(VKFramebuffer* fb);

  std::unique_ptr<SwapChain> m_swap_chain;
  float m_backbuffer_scale;

  // The GX sampler state per slot, so unchanged states skip the sampler lookup.
  std::array<SamplerState, VideoCommon::MAX_PIXEL_SHADER_SAMPLERS> m_sampler_states = {};
};
}  // namespace Vulkan
