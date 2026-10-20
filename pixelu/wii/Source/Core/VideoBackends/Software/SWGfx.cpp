// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/Software/SWGfx.h"

#include <algorithm>
#include <utility>

#include "Common/GL/GLContext.h"

#include "VideoBackends/Software/CopyRegion.h"
#include "VideoBackends/Software/EfbCopy.h"
#include "VideoBackends/Software/Rasterizer.h"
#include "VideoBackends/Software/SWOGLWindow.h"
#include "VideoBackends/Software/SWTexture.h"

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/Present.h"

namespace SW
{
namespace
{
// Shaders and pipelines exist only so that VideoCommon has objects to hold on to.
class NoShader final : public AbstractShader
{
public:
  explicit NoShader(ShaderStage stage) : AbstractShader(stage) {}
  BinaryData GetBinary() const override { return {}; }
};

class NoPipeline final : public AbstractPipeline
{
};

// One texel of the RGBA8 textures the software renderer presents.
struct Rgba8Texel
{
  u8 channel[4];
};
static_assert(sizeof(Rgba8Texel) == 4);
}  // namespace

SWGfx::SWGfx(std::unique_ptr<SWOGLWindow> window) : m_window(std::move(window))
{
}

SWGfx::~SWGfx() = default;

bool SWGfx::IsHeadless() const
{
  return m_window->IsHeadless();
}

bool SWGfx::SupportsUtilityDrawing() const
{
  return false;
}

std::unique_ptr<AbstractTexture> SWGfx::CreateTexture(const TextureConfig& config, std::string_view)
{
  return std::make_unique<SWTexture>(config);
}

std::unique_ptr<AbstractStagingTexture> SWGfx::CreateStagingTexture(StagingTextureType type,
                                                                    const TextureConfig& config)
{
  return std::make_unique<SWStagingTexture>(type, config);
}

std::unique_ptr<AbstractFramebuffer>
SWGfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                         std::vector<AbstractTexture*> additional_color_attachments)
{
  return SWFramebuffer::Create(static_cast<SWTexture*>(color_attachment),
                               static_cast<SWTexture*>(depth_attachment),
                               std::move(additional_color_attachments));
}

bool SWGfx::BindBackbuffer(const ClearColor&)
{
  // Only a resized window needs any work: pick up the new backbuffer size.
  if (g_presenter->SurfaceResizedTestAndClear())
  {
    GLContext* const context = m_window->GetContext();
    context->Update();
    g_presenter->SetBackbuffer(static_cast<int>(context->GetBackBufferWidth()),
                               static_cast<int>(context->GetBackBufferHeight()));
  }
  return true;
}

std::unique_ptr<AbstractShader> SWGfx::CreateShaderFromSource(ShaderStage stage, std::string_view,
                                                              VideoCommon::ShaderIncluder*,
                                                              std::string_view)
{
  return std::make_unique<NoShader>(stage);
}

std::unique_ptr<AbstractShader> SWGfx::CreateShaderFromBinary(ShaderStage stage, const void*,
                                                              size_t, std::string_view)
{
  return std::make_unique<NoShader>(stage);
}

std::unique_ptr<NativeVertexFormat>
SWGfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<NativeVertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> SWGfx::CreatePipeline(const AbstractPipelineConfig&, const void*,
                                                        size_t)
{
  return std::make_unique<NoPipeline>();
}

void SWGfx::ShowImage(const AbstractTexture* source_texture,
                      const MathUtil::Rectangle<int>& source_rc)
{
  // Runs on the GPU thread.
  if (IsHeadless())
    return;
  m_window->ShowImage(source_texture, source_rc);
}

void SWGfx::ScaleTexture(AbstractFramebuffer* dst_framebuffer,
                         const MathUtil::Rectangle<int>& dst_rect,
                         const AbstractTexture* src_texture,
                         const MathUtil::Rectangle<int>& src_rect)
{
  const auto* from = static_cast<const SWTexture*>(src_texture);
  auto* to = static_cast<SWTexture*>(dst_framebuffer->GetColorAttachment());
  CopyRegion(reinterpret_cast<const Rgba8Texel*>(from->GetData(0, 0)), src_rect,
             static_cast<int>(src_texture->GetWidth()), static_cast<int>(src_texture->GetHeight()),
             reinterpret_cast<Rgba8Texel*>(to->GetData(0, 0)), dst_rect,
             static_cast<int>(dst_framebuffer->GetWidth()),
             static_cast<int>(dst_framebuffer->GetHeight()));
}

void SWGfx::SetScissorRect(const MathUtil::Rectangle<int>&)
{
  // VideoCommon passes the single rectangle that best fits the scissor and viewport. The
  // rasterizer can honour several rectangles at once (Major Minor's Majestic March needs that),
  // so treat the call as a cue to rebuild its own list and ignore the argument.
  Rasterizer::ScissorChanged();
}

void SWGfx::ClearRegion(const MathUtil::Rectangle<int>&, bool, bool, bool, u32, u32)
{
  // The EFB clear reads everything it needs from BP memory.
  EfbCopy::ClearEfb();
}

SurfaceInfo SWGfx::GetSurfaceInfo() const
{
  const GLContext* const context = m_window->GetContext();
  SurfaceInfo info;
  info.width = std::max(context->GetBackBufferWidth(), 1u);
  info.height = std::max(context->GetBackBufferHeight(), 1u);
  info.scale = 1.0f;
  info.format = AbstractTextureFormat::RGBA8;
  return info;
}

}  // namespace SW
