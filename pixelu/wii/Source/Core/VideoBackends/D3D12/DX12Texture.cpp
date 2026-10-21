// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D12/DX12Texture.h"

#include <algorithm>
#include <cstring>

#include "Common/Align.h"
#include "Common/Assert.h"
#include "Common/StringUtil.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12Gfx.h"
#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"

namespace DX12
{
namespace
{
// Uploads at least this large get a buffer of their own rather than a slice
// of the shared upload ring; such textures (high-resolution packs) would
// otherwise force a stall every few uploads.
constexpr u32 LARGE_UPLOAD_BYTES = 4 * 1024 * 1024;

D3D12_HEAP_PROPERTIES HeapOfType(D3D12_HEAP_TYPE type)
{
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  return heap;
}

// A plain row-major buffer resource of the given size.
D3D12_RESOURCE_DESC LinearBufferDesc(u32 size)
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment = 0;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  return desc;
}

D3D12_BOX BoxFor(const MathUtil::Rectangle<int>& rect)
{
  D3D12_BOX box;
  box.left = static_cast<UINT>(rect.left);
  box.top = static_cast<UINT>(rect.top);
  box.front = 0;
  box.right = static_cast<UINT>(rect.right);
  box.bottom = static_cast<UINT>(rect.bottom);
  box.back = 1;
  return box;
}

D3D12_TEXTURE_COPY_LOCATION SubresourceAt(ID3D12Resource* resource, u32 index)
{
  D3D12_TEXTURE_COPY_LOCATION where = {};
  where.pResource = resource;
  where.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  where.SubresourceIndex = index;
  return where;
}

// An image of width x height texels laid out in a buffer at the given offset.
D3D12_TEXTURE_COPY_LOCATION FootprintAt(ID3D12Resource* buffer, u64 offset, DXGI_FORMAT format,
                                        u32 width, u32 height, u32 row_pitch)
{
  D3D12_TEXTURE_COPY_LOCATION where = {};
  where.pResource = buffer;
  where.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  where.PlacedFootprint.Offset = offset;
  where.PlacedFootprint.Footprint.Format = format;
  where.PlacedFootprint.Footprint.Width = width;
  where.PlacedFootprint.Footprint.Height = height;
  where.PlacedFootprint.Footprint.Depth = 1;
  where.PlacedFootprint.Footprint.RowPitch = row_pitch;
  return where;
}

ComPtr<ID3D12Resource> MakeUploadBuffer(u32 size)
{
  const D3D12_HEAP_PROPERTIES heap = HeapOfType(D3D12_HEAP_TYPE_UPLOAD);
  const D3D12_RESOURCE_DESC desc = LinearBufferDesc(size);

  ComPtr<ID3D12Resource> buffer;
  const HRESULT hr = g_dx_context->GetDevice()->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
      IID_PPV_ARGS(&buffer));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create texture upload buffer: {}", DX12HRWrap(hr));
  return buffer;
}

// Array views over every layer; only the array size is non-zero.
D3D12_RENDER_TARGET_VIEW_DESC ArrayRtvDesc(DXGI_FORMAT format, bool multisampled, u32 layers)
{
  D3D12_RENDER_TARGET_VIEW_DESC desc = {};
  desc.Format = format;
  if (multisampled)
  {
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
    desc.Texture2DMSArray.ArraySize = layers;
  }
  else
  {
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.ArraySize = layers;
  }
  return desc;
}

D3D12_DEPTH_STENCIL_VIEW_DESC ArrayDsvDesc(DXGI_FORMAT format, bool multisampled, u32 layers)
{
  D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
  desc.Format = format;
  desc.Flags = D3D12_DSV_FLAG_NONE;
  if (multisampled)
  {
    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
    desc.Texture2DMSArray.ArraySize = layers;
  }
  else
  {
    desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.ArraySize = layers;
  }
  return desc;
}

ID3D12Resource* ResourceOf(AbstractTexture* texture)
{
  return static_cast<DXTexture*>(texture)->GetResource();
}
}  // namespace

// ---------------------------------------------------------------------------
// DXTexture

DXTexture::DXTexture(const TextureConfig& config, ID3D12Resource* resource,
                     D3D12_RESOURCE_STATES state, std::string_view name)
    : AbstractTexture(config), m_resource(resource), m_name(UTF8ToWString(name)), m_state(state)
{
  if (!m_name.empty())
    resource->SetName(m_name.c_str());
}

DXTexture::~DXTexture()
{
  DescriptorHeapManager& views = g_dx_context->GetDescriptorHeapManager();
  if (m_uav_descriptor)
    g_dx_context->DeferDescriptorDestruction(views, m_uav_descriptor.index);
  if (m_srv_descriptor)
    g_dx_context->DeferDescriptorDestruction(views, m_srv_descriptor.index);
  if (m_resource)
    g_dx_context->DeferResourceDestruction(m_resource.Get());
}

std::unique_ptr<DXTexture> DXTexture::Create(const TextureConfig& config, std::string_view name)
{
  const bool render_target = config.IsRenderTarget();
  const bool depth = IsDepthFormat(config.format);

  // Render targets start out writable; everything else starts out sampled.
  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_FLAGS usage = D3D12_RESOURCE_FLAG_NONE;
  if (render_target && depth)
  {
    initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    usage = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  }
  else if (render_target)
  {
    initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    usage = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
  if (config.IsComputeImage())
    usage |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Alignment = 0;
  desc.Width = config.width;
  desc.Height = config.height;
  desc.DepthOrArraySize = static_cast<u16>(config.layers);
  desc.MipLevels = static_cast<u16>(config.levels);
  // Render targets are typeless so differently typed views can share them.
  desc.Format = D3DCommon::GetDXGIFormatForAbstractFormat(config.format, render_target);
  desc.SampleDesc.Count = config.samples;
  desc.SampleDesc.Quality = 0;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = usage;

  // The driver may optimise clears to the default (zero) value.
  D3D12_CLEAR_VALUE fast_clear = {};
  if (render_target)
  {
    fast_clear.Format = depth ? D3DCommon::GetDSVFormatForAbstractFormat(config.format) :
                                D3DCommon::GetRTVFormatForAbstractFormat(config.format, false);
  }

  const D3D12_HEAP_PROPERTIES heap = HeapOfType(D3D12_HEAP_TYPE_DEFAULT);
  ComPtr<ID3D12Resource> resource;
  const HRESULT hr = g_dx_context->GetDevice()->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, render_target ? &fast_clear : nullptr,
      IID_PPV_ARGS(&resource));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create D3D12 texture resource: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return nullptr;

  std::unique_ptr<DXTexture> texture(new DXTexture(config, resource.Get(), initial_state, name));
  if (!texture->CreateSRVDescriptor())
    return nullptr;
  if (config.IsComputeImage() && !texture->CreateUAVDescriptor())
    return nullptr;
  return texture;
}

std::unique_ptr<DXTexture> DXTexture::CreateAdopted(ID3D12Resource* resource)
{
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  const AbstractTextureFormat format = D3DCommon::GetAbstractFormatForDXGIFormat(desc.Format);
  const bool usable = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                      format != AbstractTextureFormat::Undefined;
  if (!usable)
  {
    PanicAlertFmt("Unknown format for adopted texture");
    return nullptr;
  }

  u32 flags = 0;
  if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 ||
      (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
  {
    flags |= AbstractTextureFlag_RenderTarget;
  }
  if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0)
    flags |= AbstractTextureFlag_ComputeImage;

  TextureConfig config(static_cast<u32>(desc.Width), desc.Height, desc.MipLevels,
                       desc.DepthOrArraySize, desc.SampleDesc.Count, format, 0,
                       AbstractTextureType::Texture_2DArray);
  config.flags |= flags;

  std::unique_ptr<DXTexture> texture(
      new DXTexture(config, resource, D3D12_RESOURCE_STATE_COMMON, ""));
  if (!texture->CreateSRVDescriptor())
    return nullptr;
  return texture;
}

bool DXTexture::CreateSRVDescriptor()
{
  if (!g_dx_context->GetDescriptorHeapManager().Allocate(&m_srv_descriptor))
  {
    PanicAlertFmt("Failed to allocate SRV descriptor");
    return false;
  }

  const bool msaa = m_config.IsMultisampled();
  D3D12_SRV_DIMENSION dimension;
  switch (m_config.type)
  {
  case AbstractTextureType::Texture_2DArray:
    dimension = msaa ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    break;
  case AbstractTextureType::Texture_2D:
    dimension = msaa ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    break;
  case AbstractTextureType::Texture_CubeMap:
    dimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    break;
  default:
    PanicAlertFmt("Failed to allocate SRV - unhandled type");
    return false;
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = D3DCommon::GetSRVFormatForAbstractFormat(m_config.format);
  desc.ViewDimension = dimension;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (dimension == D3D12_SRV_DIMENSION_TEXTURECUBE)
  {
    desc.TextureCube.MostDetailedMip = 0;
    desc.TextureCube.MipLevels = m_config.levels;
    desc.TextureCube.ResourceMinLODClamp = 0.0f;
  }
  else if (msaa)
  {
    // Written through the array member whatever the dimension, as the views
    // have always been described.
    desc.Texture2DMSArray.ArraySize = m_config.layers;
  }
  else
  {
    desc.Texture2DArray.MipLevels = m_config.levels;
    desc.Texture2DArray.ArraySize = m_config.layers;
  }

  g_dx_context->GetDevice()->CreateShaderResourceView(m_resource.Get(), &desc,
                                                      m_srv_descriptor.cpu_handle);
  return true;
}

bool DXTexture::CreateUAVDescriptor()
{
  if (!g_dx_context->GetDescriptorHeapManager().Allocate(&m_uav_descriptor))
  {
    PanicAlertFmt("Failed to allocate UAV descriptor");
    return false;
  }

  D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
  desc.Format = D3DCommon::GetSRVFormatForAbstractFormat(m_config.format);
  desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
  desc.Texture2DArray.ArraySize = m_config.layers;
  g_dx_context->GetDevice()->CreateUnorderedAccessView(m_resource.Get(), nullptr, &desc,
                                                       m_uav_descriptor.cpu_handle);
  return true;
}

void DXTexture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                     size_t buffer_size, u32 layer)
{
  // Rows are counted in blocks for compressed formats, and each row of the
  // upload copy must start on a 512-byte boundary.
  const u32 block = GetBlockSizeForFormat(GetFormat());
  const u32 rows = Common::AlignUp(height, block) / block;
  const u32 src_pitch = CalculateStrideForFormat(m_config.format, row_length);
  const u32 dst_pitch = Common::AlignUp(src_pitch, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  const u32 total = dst_pitch * rows;

  // Every mip level is copied in, so go to COPY_DEST once up front.
  TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12Resource> own_buffer;
  ID3D12Resource* source;
  u8* write_ptr;
  u32 source_offset;
  StreamBuffer& ring = g_dx_context->GetTextureUploadBuffer();
  if (total >= LARGE_UPLOAD_BYTES)
  {
    own_buffer = MakeUploadBuffer(total);
    if (!own_buffer)
    {
      PanicAlertFmt("Failed to allocate temporary texture upload buffer");
      return;
    }

    const D3D12_RANGE nothing_read = {0, 0};
    void* mapped;
    const HRESULT hr = own_buffer->Map(0, &nothing_read, &mapped);
    if (FAILED(hr))
    {
      PanicAlertFmt("Failed to map temporary texture upload buffer: {}", DX12HRWrap(hr));
      return;
    }

    // Kept alive until the list recording the copy has run.
    g_dx_context->DeferResourceDestruction(own_buffer.Get());
    source = own_buffer.Get();
    write_ptr = static_cast<u8*>(mapped);
    source_offset = 0;
  }
  else
  {
    if (!ring.ReserveMemory(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
    {
      WARN_LOG_FMT(VIDEO,
                   "Executing command list while waiting for space in texture upload buffer");
      Gfx::GetInstance()->ExecuteCommandList(false);
      if (!ring.ReserveMemory(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT))
      {
        PanicAlertFmt("Failed to allocate texture upload buffer");
        return;
      }
    }
    source = ring.GetBuffer();
    write_ptr = ring.GetCurrentHostPointer();
    source_offset = ring.GetCurrentOffset();
  }

  if (src_pitch == dst_pitch)
  {
    std::memcpy(write_ptr, buffer, std::min<size_t>(buffer_size, total));
  }
  else
  {
    const u32 row_bytes = std::min(src_pitch, dst_pitch);
    for (u32 row = 0; row < rows; ++row)
      std::memcpy(write_ptr + row * dst_pitch, buffer + row * src_pitch, row_bytes);
  }

  if (own_buffer)
  {
    const D3D12_RANGE written = {0, std::min<size_t>(buffer_size, total)};
    own_buffer->Unmap(0, &written);
  }
  else
  {
    ring.CommitMemory(total);
  }

  const u32 copy_width = Common::AlignUp(width, block);
  const u32 copy_height = Common::AlignUp(height, block);
  const D3D12_TEXTURE_COPY_LOCATION to =
      SubresourceAt(m_resource.Get(), CalcSubresource(level, layer));
  const D3D12_TEXTURE_COPY_LOCATION from = FootprintAt(
      source, source_offset, D3DCommon::GetDXGIFormatForAbstractFormat(m_config.format, false),
      copy_width, copy_height, dst_pitch);
  D3D12_BOX region;
  region.left = 0;
  region.top = 0;
  region.front = 0;
  region.right = copy_width;
  region.bottom = copy_height;
  region.back = 1;
  g_dx_context->GetCommandList()->CopyTextureRegion(&to, 0, 0, 0, &from, &region);

  // The last level is normally the final write, so make it sampleable now.
  if (level == m_config.levels - 1)
    TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void DXTexture::CopyRectangleFromTexture(const AbstractTexture* src,
                                         const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                         u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                         u32 dst_layer, u32 dst_level)
{
  const DXTexture* source = static_cast<const DXTexture*>(src);
  ASSERT(static_cast<u32>(src_rect.right) <= src->GetWidth() &&
         static_cast<u32>(src_rect.bottom) <= src->GetHeight() && src_layer <= src->GetLayers() &&
         src_level <= src->GetLevels() && static_cast<u32>(dst_rect.right) <= GetWidth() &&
         static_cast<u32>(dst_rect.bottom) <= GetHeight() && dst_layer <= GetLayers() &&
         dst_level <= GetLevels() && src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());

  const D3D12_TEXTURE_COPY_LOCATION to =
      SubresourceAt(m_resource.Get(), CalcSubresource(dst_level, dst_layer));
  const D3D12_TEXTURE_COPY_LOCATION from =
      SubresourceAt(source->m_resource.Get(), source->CalcSubresource(src_level, src_layer));
  const D3D12_BOX region = BoxFor(src_rect);

  // The source goes back to its state afterwards; this texture stays a copy
  // destination until FinishedRendering().
  const D3D12_RESOURCE_STATES source_was = source->m_state;
  source->TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);
  TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);
  g_dx_context->GetCommandList()->CopyTextureRegion(
      &to, static_cast<UINT>(dst_rect.left), static_cast<UINT>(dst_rect.top), 0, &from, &region);
  source->TransitionToState(source_was);
}

void DXTexture::ResolveFromTexture(const AbstractTexture* src,
                                   const MathUtil::Rectangle<int>& /*rect*/, u32 layer, u32 level)
{
  const DXTexture* source = static_cast<const DXTexture*>(src);
  const D3D12_RESOURCE_STATES source_was = source->m_state;
  source->TransitionToState(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
  TransitionToState(D3D12_RESOURCE_STATE_RESOLVE_DEST);

  // The whole subresource is resolved; rect is not needed by D3D12.
  g_dx_context->GetCommandList()->ResolveSubresource(
      m_resource.Get(), CalcSubresource(level, layer), source->m_resource.Get(),
      source->CalcSubresource(level, layer),
      D3DCommon::GetDXGIFormatForAbstractFormat(m_config.format, false));
  source->TransitionToState(source_was);
}

void DXTexture::FinishedRendering()
{
  TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void DXTexture::TransitionToState(D3D12_RESOURCE_STATES state) const
{
  if (state != m_state)
  {
    ResourceBarrier(g_dx_context->GetCommandList(), m_resource.Get(), m_state, state);
    m_state = state;
  }
}

void DXTexture::DestroyResource()
{
  DescriptorHeapManager& views = g_dx_context->GetDescriptorHeapManager();
  if (m_uav_descriptor)
    views.Free(m_uav_descriptor);
  if (m_srv_descriptor)
    views.Free(m_srv_descriptor);
  m_resource.Reset();

  // Nothing is left for the destructor to defer.
  m_uav_descriptor = {};
  m_srv_descriptor = {};
}

// ---------------------------------------------------------------------------
// DXFramebuffer

DXFramebuffer::DXFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                             std::vector<AbstractTexture*> additional_color_attachments,
                             AbstractTextureFormat color_format, AbstractTextureFormat depth_format,
                             u32 width, u32 height, u32 layers, u32 samples)
    : AbstractFramebuffer(color_attachment, depth_attachment,
                          std::move(additional_color_attachments), color_format, depth_format,
                          width, height, layers, samples)
{
}

DXFramebuffer::~DXFramebuffer()
{
  if (m_depth_attachment)
  {
    g_dx_context->DeferDescriptorDestruction(g_dx_context->GetDSVHeapManager(),
                                             m_dsv_descriptor.index);
  }

  DescriptorHeapManager& rtvs = g_dx_context->GetRTVHeapManager();
  if (m_color_attachment && m_int_rtv_descriptor)
    g_dx_context->DeferDescriptorDestruction(rtvs, m_int_rtv_descriptor.index);
  for (const DescriptorHandle& view : m_render_targets)
    g_dx_context->DeferDescriptorDestruction(rtvs, view.index);
}

const D3D12_CPU_DESCRIPTOR_HANDLE* DXFramebuffer::GetIntRTVDescriptorArray() const
{
  if (!m_color_attachment)
    return nullptr;

  // A separate integer view only exists when its format differs from the
  // normal view's; otherwise the normal views serve both purposes.
  if (m_int_rtv_descriptor.cpu_handle.ptr == 0)
    return GetRTVDescriptorArray();
  return &m_int_rtv_descriptor.cpu_handle;
}

void DXFramebuffer::Unbind()
{
  static const D3D12_DISCARD_REGION whole = {0, nullptr, 0, 1};
  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();
  if (HasColorBuffer())
    list->DiscardResource(ResourceOf(GetColorAttachment()), &whole);
  for (AbstractTexture* extra : m_additional_color_attachments)
    list->DiscardResource(ResourceOf(extra), &whole);
  if (HasDepthBuffer())
    list->DiscardResource(ResourceOf(GetDepthAttachment()), &whole);
}

void DXFramebuffer::ClearRenderTargets(const ClearColor& color_value,
                                       const D3D12_RECT* rectangle) const
{
  const UINT rect_count = rectangle != nullptr ? 1 : 0;
  ID3D12GraphicsCommandList* const list = g_dx_context->GetCommandList();
  for (const D3D12_CPU_DESCRIPTOR_HANDLE view : m_render_targets_raw)
    list->ClearRenderTargetView(view, color_value.data(), rect_count, rectangle);
}

void DXFramebuffer::ClearDepth(float depth_value, const D3D12_RECT* rectangle) const
{
  if (!HasDepthBuffer())
    return;

  g_dx_context->GetCommandList()->ClearDepthStencilView(GetDSVDescriptor().cpu_handle,
                                                        D3D12_CLEAR_FLAG_DEPTH, depth_value, 0,
                                                        rectangle != nullptr ? 1 : 0, rectangle);
}

void DXFramebuffer::TransitionRenderTargets() const
{
  if (HasColorBuffer())
  {
    static_cast<DXTexture*>(GetColorAttachment())
        ->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  for (AbstractTexture* extra : m_additional_color_attachments)
    static_cast<DXTexture*>(extra)->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
}

std::unique_ptr<DXFramebuffer>
DXFramebuffer::Create(DXTexture* color_attachment, DXTexture* depth_attachment,
                      std::vector<AbstractTexture*> additional_color_attachments)
{
  if (!ValidateConfig(color_attachment, depth_attachment, additional_color_attachments))
    return nullptr;

  // Size, layers and samples come from whichever attachment exists (colour
  // first); ValidateConfig made sure they agree.
  const DXTexture* const sized_by =
      color_attachment != nullptr ? color_attachment : depth_attachment;
  std::unique_ptr<DXFramebuffer> framebuffer(new DXFramebuffer(
      color_attachment, depth_attachment, std::move(additional_color_attachments),
      color_attachment != nullptr ? color_attachment->GetFormat() :
                                    AbstractTextureFormat::Undefined,
      depth_attachment != nullptr ? depth_attachment->GetFormat() :
                                    AbstractTextureFormat::Undefined,
      sized_by->GetWidth(), sized_by->GetHeight(), sized_by->GetLayers(), sized_by->GetSamples()));

  if (!framebuffer->CreateRTVDescriptors())
    return nullptr;
  if (color_attachment != nullptr && !framebuffer->CreateIRTVDescriptor())
    return nullptr;
  if (depth_attachment != nullptr && !framebuffer->CreateDSVDescriptor())
    return nullptr;
  return framebuffer;
}

bool DXFramebuffer::CreateRTVDescriptors()
{
  // The main attachment is viewed across all layers, extra ones as a single layer.
  if (m_color_attachment && !CreateRTVDescriptor(m_layers, m_color_attachment))
    return false;
  for (AbstractTexture* extra : m_additional_color_attachments)
  {
    if (!CreateRTVDescriptor(1, extra))
      return false;
  }
  return true;
}

bool DXFramebuffer::CreateRTVDescriptor(u32 layers, AbstractTexture* attachment)
{
  DescriptorHandle view;
  if (!g_dx_context->GetRTVHeapManager().Allocate(&view))
  {
    PanicAlertFmt("Failed to allocate RTV descriptor");
    return false;
  }
  m_render_targets.push_back(view);
  m_render_targets_raw.push_back(view.cpu_handle);

  // Every colour view uses the framebuffer's colour format, extra
  // attachments included.
  const D3D12_RENDER_TARGET_VIEW_DESC desc = ArrayRtvDesc(
      D3DCommon::GetRTVFormatForAbstractFormat(m_color_format, false), m_samples > 1, layers);
  g_dx_context->GetDevice()->CreateRenderTargetView(ResourceOf(attachment), &desc, view.cpu_handle);
  return true;
}

bool DXFramebuffer::CreateIRTVDescriptor()
{
  const DXGI_FORMAT plain = D3DCommon::GetRTVFormatForAbstractFormat(m_color_format, false);
  const DXGI_FORMAT integer = D3DCommon::GetRTVFormatForAbstractFormat(m_color_format, true);
  if (integer == plain)
    return true;  // GetIntRTVDescriptorArray() falls back to the normal view.

  if (!g_dx_context->GetRTVHeapManager().Allocate(&m_int_rtv_descriptor))
    return false;

  const D3D12_RENDER_TARGET_VIEW_DESC desc = ArrayRtvDesc(integer, m_samples > 1, m_layers);
  g_dx_context->GetDevice()->CreateRenderTargetView(ResourceOf(m_color_attachment), &desc,
                                                    m_int_rtv_descriptor.cpu_handle);
  return true;
}

bool DXFramebuffer::CreateDSVDescriptor()
{
  if (!g_dx_context->GetDSVHeapManager().Allocate(&m_dsv_descriptor))
  {
    // The message has always named the RTV heap.
    PanicAlertFmt("Failed to allocate RTV descriptor");
    return false;
  }

  const D3D12_DEPTH_STENCIL_VIEW_DESC desc = ArrayDsvDesc(
      D3DCommon::GetDSVFormatForAbstractFormat(m_depth_format), m_samples > 1, m_layers);
  g_dx_context->GetDevice()->CreateDepthStencilView(ResourceOf(m_depth_attachment), &desc,
                                                    m_dsv_descriptor.cpu_handle);
  return true;
}

// ---------------------------------------------------------------------------
// DXStagingTexture

DXStagingTexture::DXStagingTexture(StagingTextureType type, const TextureConfig& config,
                                   ID3D12Resource* resource, u32 stride, u32 buffer_size)
    : AbstractStagingTexture(type, config), m_resource(resource), m_buffer_size(buffer_size)
{
  m_map_stride = stride;
}

DXStagingTexture::~DXStagingTexture()
{
  // Any mapping simply goes away with the resource.
  g_dx_context->DeferResourceDestruction(m_resource.Get());
}

void DXStagingTexture::CopyFromTexture(const AbstractTexture* src,
                                       const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                       u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  const DXTexture* source = static_cast<const DXTexture*>(src);
  ASSERT(m_type == StagingTextureType::Readback || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());
  ASSERT(src_rect.left >= 0 && static_cast<u32>(src_rect.right) <= source->GetWidth() &&
         src_rect.top >= 0 && static_cast<u32>(src_rect.bottom) <= source->GetHeight());
  ASSERT(dst_rect.left >= 0 && static_cast<u32>(dst_rect.right) <= m_config.width &&
         dst_rect.top >= 0 && static_cast<u32>(dst_rect.bottom) <= m_config.height);

  const D3D12_RESOURCE_STATES source_was = source->GetState();
  source->TransitionToState(D3D12_RESOURCE_STATE_COPY_SOURCE);

  // The GPU must not write into a buffer the CPU has mapped.
  Unmap();

  const D3D12_TEXTURE_COPY_LOCATION to = FootprintAt(
      m_resource.Get(), 0, D3DCommon::GetDXGIFormatForAbstractFormat(m_config.format, false),
      m_config.width, m_config.height, static_cast<UINT>(m_map_stride));
  const D3D12_TEXTURE_COPY_LOCATION from =
      SubresourceAt(source->GetResource(), source->CalcSubresource(src_level, src_layer));
  const D3D12_BOX region = BoxFor(src_rect);
  g_dx_context->GetCommandList()->CopyTextureRegion(
      &to, static_cast<UINT>(dst_rect.left), static_cast<UINT>(dst_rect.top), 0, &from, &region);
  source->TransitionToState(source_was);

  // The data lands once the current command list has executed.
  m_needs_flush = true;
  m_completed_fence = g_dx_context->GetCurrentFenceValue();
}

void DXStagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                     u32 dst_level)
{
  const DXTexture* target = static_cast<const DXTexture*>(dst);
  ASSERT(m_type == StagingTextureType::Upload || m_type == StagingTextureType::Mutable);
  ASSERT(src_rect.GetWidth() == dst_rect.GetWidth() &&
         src_rect.GetHeight() == dst_rect.GetHeight());
  ASSERT(src_rect.left >= 0 && static_cast<u32>(src_rect.right) <= m_config.width &&
         src_rect.top >= 0 && static_cast<u32>(src_rect.bottom) <= m_config.height);
  ASSERT(dst_rect.left >= 0 && static_cast<u32>(dst_rect.right) <= target->GetWidth() &&
         dst_rect.top >= 0 && static_cast<u32>(dst_rect.bottom) <= target->GetHeight());

  const D3D12_RESOURCE_STATES target_was = target->GetState();
  target->TransitionToState(D3D12_RESOURCE_STATE_COPY_DEST);

  // The GPU must not read a buffer the CPU still has mapped.
  Unmap();

  const D3D12_TEXTURE_COPY_LOCATION to =
      SubresourceAt(target->GetResource(), target->CalcSubresource(dst_level, dst_layer));
  const D3D12_TEXTURE_COPY_LOCATION from = FootprintAt(
      m_resource.Get(), 0, D3DCommon::GetDXGIFormatForAbstractFormat(m_config.format, false),
      m_config.width, m_config.height, static_cast<UINT>(m_map_stride));
  const D3D12_BOX region = BoxFor(src_rect);
  g_dx_context->GetCommandList()->CopyTextureRegion(
      &to, static_cast<UINT>(dst_rect.left), static_cast<UINT>(dst_rect.top), 0, &from, &region);
  target->TransitionToState(target_was);

  m_needs_flush = true;
  m_completed_fence = g_dx_context->GetCurrentFenceValue();
}

bool DXStagingTexture::Map()
{
  if (m_map_pointer != nullptr)
    return true;

  // Upload buffers are never read back by the CPU.
  D3D12_RANGE may_read = {};
  may_read.End = m_type == StagingTextureType::Upload ? 0 : m_buffer_size;
  const HRESULT hr = m_resource->Map(0, &may_read, reinterpret_cast<void**>(&m_map_pointer));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Map resource failed: {}", DX12HRWrap(hr));
  return SUCCEEDED(hr);
}

void DXStagingTexture::Unmap()
{
  if (m_map_pointer == nullptr)
    return;

  // Only upload buffers carry CPU writes the GPU has to see.
  D3D12_RANGE written = {};
  written.End = m_type == StagingTextureType::Upload ? m_buffer_size : 0;
  m_resource->Unmap(0, &written);
  m_map_pointer = nullptr;
}

void DXStagingTexture::Flush()
{
  if (!m_needs_flush)
    return;
  m_needs_flush = false;

  // A copy still sitting in the open command list has to be submitted first;
  // one already submitted only needs its fence.
  if (m_completed_fence != g_dx_context->GetCurrentFenceValue())
    g_dx_context->WaitForFence(m_completed_fence);
  else
    Gfx::GetInstance()->ExecuteCommandList(true);
}

std::unique_ptr<DXStagingTexture> DXStagingTexture::Create(StagingTextureType type,
                                                           const TextureConfig& config)
{
  ASSERT(config.levels == 1 && config.layers == 1 && config.samples == 1);

  const u32 pitch =
      Common::AlignUp(config.width * AbstractTexture::GetTexelSizeForFormat(config.format),
                      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const u32 size = pitch * config.height;

  // Upload buffers sit in GENERIC_READ for good; readback (and mutable)
  // buffers sit in COPY_DEST for good.
  const bool upload = type == StagingTextureType::Upload;
  const D3D12_HEAP_PROPERTIES heap =
      HeapOfType(upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK);
  const D3D12_RESOURCE_DESC desc = LinearBufferDesc(size);
  ComPtr<ID3D12Resource> resource;
  const HRESULT hr = g_dx_context->GetDevice()->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &desc,
      upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
      IID_PPV_ARGS(&resource));
  ASSERT_MSG(VIDEO, SUCCEEDED(hr), "Failed to create staging texture resource: {}", DX12HRWrap(hr));
  if (FAILED(hr))
    return nullptr;

  return std::unique_ptr<DXStagingTexture>(
      new DXStagingTexture(type, config, resource.Get(), pitch, size));
}

}  // namespace DX12
