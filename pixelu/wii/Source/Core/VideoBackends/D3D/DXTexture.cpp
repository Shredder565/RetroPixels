// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/DXTexture.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/VideoConfig.h"

namespace DX11
{
namespace
{
UINT Subresource(u32 level, u32 layer, u32 level_count)
{
  return level + layer * level_count;
}

D3D11_TEXTURE2D_DESC MakeTextureDesc(DXGI_FORMAT format, u32 width, u32 height, u32 layers,
                                     u32 levels, UINT bind, D3D11_USAGE usage, UINT cpu_access,
                                     u32 samples, UINT misc)
{
  D3D11_TEXTURE2D_DESC desc;
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = levels;
  desc.ArraySize = layers;
  desc.Format = format;
  desc.SampleDesc.Count = samples;
  desc.SampleDesc.Quality = 0;
  desc.Usage = usage;
  desc.BindFlags = bind;
  desc.CPUAccessFlags = cpu_access;
  desc.MiscFlags = misc;
  return desc;
}

D3D11_BOX BoxFor(const MathUtil::Rectangle<int>& rect)
{
  D3D11_BOX box;
  box.left = static_cast<UINT>(rect.left);
  box.top = static_cast<UINT>(rect.top);
  box.front = 0;
  box.right = static_cast<UINT>(rect.right);
  box.bottom = static_cast<UINT>(rect.bottom);
  box.back = 1;
  return box;
}

// Only the members of the chosen dimension are filled in.
D3D11_RENDER_TARGET_VIEW_DESC MakeRtvDesc(bool multisampled, DXGI_FORMAT format, u32 layers)
{
  D3D11_RENDER_TARGET_VIEW_DESC desc;
  desc.Format = format;
  if (multisampled)
  {
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
    desc.Texture2DMSArray.FirstArraySlice = 0;
    desc.Texture2DMSArray.ArraySize = layers;
  }
  else
  {
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = 0;
    desc.Texture2DArray.FirstArraySlice = 0;
    desc.Texture2DArray.ArraySize = layers;
  }
  return desc;
}

D3D11_DEPTH_STENCIL_VIEW_DESC MakeDsvDesc(bool multisampled, DXGI_FORMAT format, u32 layers)
{
  D3D11_DEPTH_STENCIL_VIEW_DESC desc;
  desc.Format = format;
  desc.Flags = 0;
  if (multisampled)
  {
    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
    desc.Texture2DMSArray.FirstArraySlice = 0;
    desc.Texture2DMSArray.ArraySize = layers;
  }
  else
  {
    desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = 0;
    desc.Texture2DArray.FirstArraySlice = 0;
    desc.Texture2DArray.ArraySize = layers;
  }
  return desc;
}

// When no view format is known, the view takes the texture's own format.
DXGI_FORMAT ResolveViewFormat(ID3D11Texture2D* texture, DXGI_FORMAT format)
{
  if (format != DXGI_FORMAT_UNKNOWN)
    return format;
  D3D11_TEXTURE2D_DESC tex_desc;
  texture->GetDesc(&tex_desc);
  return tex_desc.Format;
}

const DXTexture* AsDX(const AbstractTexture* texture)
{
  return static_cast<const DXTexture*>(texture);
}

// Clears srv from the pixel-stage slots; returns whether any slot held it.
bool DropFromTextureSlots(ID3D11ShaderResourceView* srv)
{
  return D3D::stateman->UnsetTexture(srv) != 0;
}
// A rectangle with a non-negative origin that ends inside width x height.
bool FitsIn(const MathUtil::Rectangle<int>& rect, u32 width, u32 height)
{
  return rect.left >= 0 && rect.top >= 0 && static_cast<u32>(rect.right) <= width &&
         static_cast<u32>(rect.bottom) <= height;
}

bool SameSize(const MathUtil::Rectangle<int>& a, const MathUtil::Rectangle<int>& b)
{
  return a.GetWidth() == b.GetWidth() && a.GetHeight() == b.GetHeight();
}
}  // namespace

DXTexture::DXTexture(const TextureConfig& config, ComPtr<ID3D11Texture2D> texture,
                     std::string_view name)
    : AbstractTexture(config), m_texture(std::move(texture)), m_name(name)
{
  if (m_name.empty())
    return;
  m_texture->SetPrivateData(WKPDID_D3DDebugObjectName, static_cast<UINT>(m_name.size()),
                            m_name.c_str());
}

DXTexture::~DXTexture()
{
  // Make sure the context no longer samples from this texture.
  if (m_srv && DropFromTextureSlots(m_srv.Get()))
    D3D::stateman->ApplyTextures();
}

std::unique_ptr<DXTexture> DXTexture::Create(const TextureConfig& config, std::string_view name)
{
  // Render targets are stored typeless so the EFB can also be viewed with an
  // integer format.
  const bool target = config.IsRenderTarget();
  const DXGI_FORMAT storage = D3DCommon::GetDXGIFormatForAbstractFormat(config.format, target);

  UINT bind = D3D11_BIND_SHADER_RESOURCE;
  if (target)
    bind |= IsDepthFormat(config.format) ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET;
  if (config.IsComputeImage())
    bind |= D3D11_BIND_UNORDERED_ACCESS;
  const UINT misc =
      config.type == AbstractTextureType::Texture_CubeMap ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

  const D3D11_TEXTURE2D_DESC desc =
      MakeTextureDesc(storage, config.width, config.height, config.layers, config.levels, bind,
                      D3D11_USAGE_DEFAULT, 0, config.samples, misc);
  ComPtr<ID3D11Texture2D> resource;
  const HRESULT result = D3D::device->CreateTexture2D(&desc, nullptr, &resource);
  if (FAILED(result))
  {
    PanicAlertFmt("Failed to create {}x{}x{} D3D backing texture: {}", config.width, config.height,
                  config.layers, DX11HRWrap(result));
    return nullptr;
  }

  std::unique_ptr<DXTexture> texture(new DXTexture(config, std::move(resource), name));
  if (!texture->CreateSRV())
    return nullptr;
  if (config.IsComputeImage() && !texture->CreateUAV())
    return nullptr;
  return texture;
}

std::unique_ptr<DXTexture> DXTexture::CreateAdopted(ComPtr<ID3D11Texture2D> texture)
{
  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

  TextureConfig config(desc.Width, desc.Height, desc.MipLevels, desc.ArraySize,
                       desc.SampleDesc.Count,
                       D3DCommon::GetAbstractFormatForDXGIFormat(desc.Format), 0,
                       AbstractTextureType::Texture_2DArray);
  const bool shader_visible = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
  const bool unordered = (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;
  if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
    config.flags |= AbstractTextureFlag_RenderTarget;
  if (unordered)
    config.flags |= AbstractTextureFlag_ComputeImage;

  std::unique_ptr<DXTexture> adopted(new DXTexture(config, std::move(texture), ""));
  if (shader_visible && !adopted->CreateSRV())
    return nullptr;
  if (unordered && !adopted->CreateUAV())
    return nullptr;
  return adopted;
}

bool DXTexture::CreateSRV()
{
  const bool msaa = m_config.IsMultisampled();
  D3D11_SHADER_RESOURCE_VIEW_DESC desc;
  switch (m_config.type)
  {
  case AbstractTextureType::Texture_2DArray:
    if (msaa)
    {
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
      desc.Texture2DMSArray.FirstArraySlice = 0;
      desc.Texture2DMSArray.ArraySize = m_config.layers;
    }
    else
    {
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      desc.Texture2DArray.MostDetailedMip = 0;
      desc.Texture2DArray.MipLevels = m_config.levels;
      desc.Texture2DArray.FirstArraySlice = 0;
      desc.Texture2DArray.ArraySize = m_config.layers;
    }
    break;
  case AbstractTextureType::Texture_2D:
    if (msaa)
    {
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    }
    else
    {
      desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      desc.Texture2D.MostDetailedMip = 0;
      desc.Texture2D.MipLevels = m_config.levels;
    }
    break;
  case AbstractTextureType::Texture_CubeMap:
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    desc.TextureCube.MostDetailedMip = 0;
    desc.TextureCube.MipLevels = m_config.levels;
    break;
  default:
    PanicAlertFmt("Failed to create D3D SRV - unhandled type");
    return false;
  }
  desc.Format =
      ResolveViewFormat(m_texture.Get(), D3DCommon::GetSRVFormatForAbstractFormat(m_config.format));

  DEBUG_ASSERT(!m_srv);
  const HRESULT result = D3D::device->CreateShaderResourceView(m_texture.Get(), &desc, &m_srv);
  if (SUCCEEDED(result))
    return true;

  PanicAlertFmt("Failed to create {}x{}x{} D3D SRV: {}", m_config.width, m_config.height,
                m_config.layers, DX11HRWrap(result));
  return false;
}

bool DXTexture::CreateUAV()
{
  D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
  desc.Format =
      ResolveViewFormat(m_texture.Get(), D3DCommon::GetSRVFormatForAbstractFormat(m_config.format));
  desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
  desc.Texture2DArray.MipSlice = 0;
  desc.Texture2DArray.FirstArraySlice = 0;
  desc.Texture2DArray.ArraySize = m_config.layers;

  DEBUG_ASSERT(!m_uav);
  const HRESULT result = D3D::device->CreateUnorderedAccessView(m_texture.Get(), &desc, &m_uav);
  if (SUCCEEDED(result))
    return true;

  PanicAlertFmt("Failed to create {}x{}x{} D3D UAV: {}", m_config.width, m_config.height,
                m_config.layers, DX11HRWrap(result));
  return false;
}

void DXTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                         u32 dst_layer, u32 dst_level)
{
  const DXTexture* source = AsDX(src);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());

  const D3D11_BOX box = BoxFor(src_rect);
  D3D::context->CopySubresourceRegion(
      m_texture.Get(), Subresource(dst_level, dst_layer, m_config.levels),
      static_cast<UINT>(dst_rect.left), static_cast<UINT>(dst_rect.top), 0, source->m_texture.Get(),
      Subresource(src_level, src_layer, source->m_config.levels), &box);
}

void DXTexture::ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                                   u32 layer, u32 level)
{
  const DXTexture* source = AsDX(src);
  DEBUG_ASSERT(m_config.samples == 1 && m_config.width == source->m_config.width &&
               m_config.height == source->m_config.height && source->m_config.samples > 1);
  DEBUG_ASSERT(rect.left + rect.GetWidth() <= static_cast<int>(source->m_config.width) &&
               rect.top + rect.GetHeight() <= static_cast<int>(source->m_config.height));

  // D3D11 resolves whole subresources; the rectangle is only checked.
  D3D::context->ResolveSubresource(
      m_texture.Get(), Subresource(level, layer, m_config.levels), source->m_texture.Get(),
      Subresource(level, layer, source->m_config.levels),
      D3DCommon::GetDXGIFormatForAbstractFormat(m_config.format, false));
}

void DXTexture::Load(u32 level, u32 /*width*/, u32 /*height*/, u32 row_length, const u8* buffer,
                     size_t /*buffer_size*/, u32 layer)
{
  const auto row_pitch = static_cast<UINT>(CalculateStrideForFormat(m_config.format, row_length));
  D3D::context->UpdateSubresource(m_texture.Get(), Subresource(level, layer, m_config.levels),
                                  nullptr, buffer, row_pitch, 0);
}

DXStagingTexture::DXStagingTexture(StagingTextureType type, const TextureConfig& config,
                                   ComPtr<ID3D11Texture2D> texture)
    : AbstractStagingTexture(type, config), m_tex(std::move(texture))
{
}

DXStagingTexture::~DXStagingTexture()
{
  // Called non-virtually: the object is already being torn down.
  if (IsMapped())
    DXStagingTexture::Unmap();
}

std::unique_ptr<DXStagingTexture> DXStagingTexture::Create(StagingTextureType type,
                                                           const TextureConfig& config)
{
  // Uploads go through a dynamic texture; anything the CPU reads needs staging.
  D3D11_USAGE usage = D3D11_USAGE_STAGING;
  UINT cpu_access = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  UINT bind = 0;
  switch (type)
  {
  case StagingTextureType::Readback:
    cpu_access = D3D11_CPU_ACCESS_READ;
    break;
  case StagingTextureType::Upload:
    usage = D3D11_USAGE_DYNAMIC;
    cpu_access = D3D11_CPU_ACCESS_WRITE;
    bind = D3D11_BIND_SHADER_RESOURCE;
    break;
  default:
    break;
  }

  const D3D11_TEXTURE2D_DESC desc =
      MakeTextureDesc(D3DCommon::GetDXGIFormatForAbstractFormat(config.format, false), config.width,
                      config.height, 1, 1, bind, usage, cpu_access, 1, 0);
  ComPtr<ID3D11Texture2D> resource;
  const HRESULT result = D3D::device->CreateTexture2D(&desc, nullptr, &resource);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create staging texture: {}", DX11HRWrap(result));
  if (FAILED(result))
    return nullptr;

  return std::unique_ptr<DXStagingTexture>(new DXStagingTexture(type, config, std::move(resource)));
}

void DXStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                       const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                       u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  ASSERT(m_type == StagingTextureType::Readback || m_type == StagingTextureType::Mutable);
  ASSERT(SameSize(src_rect, dst_rect));
  ASSERT(FitsIn(src_rect, src->GetWidth(), src->GetHeight()));
  ASSERT(FitsIn(dst_rect, m_config.width, m_config.height));

  // The copy may not target a mapped texture.
  if (IsMapped())
    DXStagingTexture::Unmap();

  ID3D11Texture2D* const source = AsDX(src)->GetD3DTexture();
  const UINT source_sub = Subresource(src_level, src_layer, src->GetLevels());
  const bool whole = static_cast<u32>(src_rect.GetWidth()) == GetWidth() &&
                     static_cast<u32>(src_rect.GetHeight()) == GetHeight();
  if (whole)
  {
    // A full-size copy goes without a box (depth formats require that); it
    // lands at the origin.
    D3D::context->CopySubresourceRegion(m_tex.Get(), 0, 0, 0, 0, source, source_sub, nullptr);
  }
  else
  {
    const D3D11_BOX box = BoxFor(src_rect);
    D3D::context->CopySubresourceRegion(m_tex.Get(), 0, static_cast<u32>(dst_rect.left),
                                        static_cast<u32>(dst_rect.top), 0, source, source_sub,
                                        &box);
  }

  m_needs_flush = true;
}

void DXStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                     u32 dst_level)
{
  ASSERT(m_type == StagingTextureType::Upload);
  ASSERT(SameSize(src_rect, dst_rect));
  ASSERT(FitsIn(src_rect, GetWidth(), GetHeight()));
  ASSERT(FitsIn(dst_rect, dst->GetWidth(), dst->GetHeight()));

  // The copy may not read a mapped texture.
  if (IsMapped())
    DXStagingTexture::Unmap();

  ID3D11Texture2D* const target = AsDX(dst)->GetD3DTexture();
  const UINT target_sub = Subresource(dst_level, dst_layer, dst->GetLevels());
  const bool whole = static_cast<u32>(src_rect.GetWidth()) == dst->GetWidth() &&
                     static_cast<u32>(src_rect.GetHeight()) == dst->GetHeight();
  if (whole)
  {
    D3D::context->CopySubresourceRegion(target, target_sub, 0, 0, 0, m_tex.Get(), 0, nullptr);
  }
  else
  {
    const D3D11_BOX box = BoxFor(src_rect);
    D3D::context->CopySubresourceRegion(target, target_sub, static_cast<u32>(dst_rect.left),
                                        static_cast<u32>(dst_rect.top), 0, m_tex.Get(), 0, &box);
  }
}

bool DXStagingTexture::Map()
{
  if (m_map_pointer)
    return true;

  D3D11_MAP mode = D3D11_MAP_READ_WRITE;
  if (m_type == StagingTextureType::Readback)
    mode = D3D11_MAP_READ;
  else if (m_type == StagingTextureType::Upload)
    mode = D3D11_MAP_WRITE_DISCARD;

  D3D11_MAPPED_SUBRESOURCE mapped;
  const HRESULT result = D3D::context->Map(m_tex.Get(), 0, mode, 0, &mapped);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to map readback texture: {}", DX11HRWrap(result));
  if (FAILED(result))
    return false;

  m_map_pointer = static_cast<char*>(mapped.pData);
  m_map_stride = mapped.RowPitch;
  return true;
}

void DXStagingTexture::Unmap()
{
  if (!m_map_pointer)
    return;

  D3D::context->Unmap(m_tex.Get(), 0);
  m_map_pointer = nullptr;
}

void DXStagingTexture::Flush()
{
  // D3D11 orders the copy before the map by itself.
  m_needs_flush = false;
}

DXFramebuffer::DXFramebuffer(AbstractTexture* color, AbstractTexture* depth,
                             std::vector<AbstractTexture*> extra_colors,
                             AbstractTextureFormat color_fmt, AbstractTextureFormat depth_fmt,
                             u32 w, u32 h, u32 layer_count, u32 sample_count,
                             ComPtr<ID3D11RenderTargetView> rtv,
                             ComPtr<ID3D11RenderTargetView> integer_rtv,
                             ComPtr<ID3D11DepthStencilView> dsv,
                             std::vector<ComPtr<ID3D11RenderTargetView>> additional_rtvs)
    : AbstractFramebuffer(color, depth, std::move(extra_colors), color_fmt, depth_fmt, w, h,
                          layer_count, sample_count),
      m_integer_rtv(std::move(integer_rtv)), m_dsv(std::move(dsv))
{
  m_render_targets.reserve(additional_rtvs.size() + 1);
  m_render_targets.push_back(std::move(rtv));
  std::move(additional_rtvs.begin(), additional_rtvs.end(), std::back_inserter(m_render_targets));

  m_render_targets_raw.reserve(m_render_targets.size());
  for (const ComPtr<ID3D11RenderTargetView>& view : m_render_targets)
    m_render_targets_raw.push_back(view.Get());

  m_integer_targets_raw = m_render_targets_raw;
  m_integer_targets_raw[0] = m_integer_rtv.Get();
}

DXFramebuffer::~DXFramebuffer() = default;

void DXFramebuffer::Unbind()
{
  // Every attachment is checked, even after one was found bound.
  bool found = false;
  if (AbstractTexture* color = GetColorAttachment())
    found |= DropFromTextureSlots(static_cast<DXTexture*>(color)->GetD3DSRV());
  if (AbstractTexture* depth = GetDepthAttachment())
    found |= DropFromTextureSlots(static_cast<DXTexture*>(depth)->GetD3DSRV());
  for (AbstractTexture* extra : m_additional_color_attachments)
    found |= DropFromTextureSlots(static_cast<DXTexture*>(extra)->GetD3DSRV());

  if (found)
    D3D::stateman->ApplyTextures();
}

void DXFramebuffer::Clear(const ClearColor& color_value, float depth_value)
{
  // Depth first, then every colour slot (a depth-only framebuffer clears its null view).
  if (GetDepthFormat() != AbstractTextureFormat::Undefined)
    D3D::context->ClearDepthStencilView(GetDSV(), D3D11_CLEAR_DEPTH, depth_value, 0);

  for (ID3D11RenderTargetView* view : m_render_targets_raw)
    D3D::context->ClearRenderTargetView(view, color_value.data());
}

std::unique_ptr<DXFramebuffer>
DXFramebuffer::Create(DXTexture* color_attachment, DXTexture* depth_attachment,
                      std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  const DXTexture* const sizing = color_attachment ? color_attachment : depth_attachment;
  const AbstractTextureFormat color_format =
      color_attachment ? color_attachment->GetFormat() : AbstractTextureFormat::Undefined;
  const AbstractTextureFormat depth_format =
      depth_attachment ? depth_attachment->GetFormat() : AbstractTextureFormat::Undefined;

  ComPtr<ID3D11RenderTargetView> color_view;
  ComPtr<ID3D11RenderTargetView> integer_view;
  if (color_attachment)
  {
    D3D11_RENDER_TARGET_VIEW_DESC desc =
        MakeRtvDesc(color_attachment->IsMultisampled(),
                    D3DCommon::GetRTVFormatForAbstractFormat(color_attachment->GetFormat(), false),
                    color_attachment->GetLayers());
    HRESULT result =
        D3D::device->CreateRenderTargetView(color_attachment->GetD3DTexture(), &desc, &color_view);
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create render target view for framebuffer: {}",
               DX11HRWrap(result));
    if (FAILED(result))
      return nullptr;

    // The unsigned-integer alias is only needed for logic ops (D3D 11.1), and
    // only when it differs from the normal view.
    const DXGI_FORMAT integer_format =
        D3DCommon::GetRTVFormatForAbstractFormat(color_attachment->GetFormat(), true);
    if (g_backend_info.bSupportsLogicOp && integer_format != desc.Format)
    {
      desc.Format = integer_format;
      result = D3D::device->CreateRenderTargetView(color_attachment->GetD3DTexture(), &desc,
                                                   &integer_view);
      ASSERT_MSG(VIDEO, SUCCEEDED(result),
                 "Failed to create integer render target view for framebuffer: {}",
                 DX11HRWrap(result));
    }
  }

  std::vector<ComPtr<ID3D11RenderTargetView>> extra_views;
  for (AbstractTexture* extra : additional_color_attachments)
  {
    // Extra attachments are bound as a single layer.
    const D3D11_RENDER_TARGET_VIEW_DESC desc =
        MakeRtvDesc(extra->IsMultisampled(),
                    D3DCommon::GetRTVFormatForAbstractFormat(extra->GetFormat(), false), 1);
    ComPtr<ID3D11RenderTargetView> view;
    const HRESULT result = D3D::device->CreateRenderTargetView(
        static_cast<DXTexture*>(extra)->GetD3DTexture(), &desc, &view);
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Create render target view for framebuffer: {}",
               DX11HRWrap(result));
    if (FAILED(result))
      return nullptr;
    extra_views.push_back(std::move(view));
  }

  ComPtr<ID3D11DepthStencilView> depth_view;
  if (depth_attachment)
  {
    const D3D11_DEPTH_STENCIL_VIEW_DESC desc =
        MakeDsvDesc(depth_attachment->GetConfig().IsMultisampled(),
                    D3DCommon::GetDSVFormatForAbstractFormat(depth_attachment->GetFormat()),
                    depth_attachment->GetLayers());
    const HRESULT result =
        D3D::device->CreateDepthStencilView(depth_attachment->GetD3DTexture(), &desc, &depth_view);
    ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create depth stencil view for framebuffer: {}",
               DX11HRWrap(result));
    if (FAILED(result))
      return nullptr;
  }

  return std::make_unique<DXFramebuffer>(
      color_attachment, depth_attachment, std::move(additional_color_attachments), color_format,
      depth_format, sizing->GetWidth(), sizing->GetHeight(), sizing->GetLayers(),
      sizing->GetSamples(), std::move(color_view), std::move(integer_view), std::move(depth_view),
      std::move(extra_views));
}

}  // namespace DX11
