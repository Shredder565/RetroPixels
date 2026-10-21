// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoBackends/D3D/D3DBoundingBox.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"

#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"

namespace DX11
{
namespace
{
constexpr UINT VALUE_SIZE = sizeof(BBoxType);
constexpr UINT BUFFER_BYTES = NUM_BBOX_VALUES * VALUE_SIZE;
}  // namespace

D3DBoundingBox::~D3DBoundingBox()
{
  // View first, then the staging copy, then the buffer itself.
  m_uav.Reset();
  m_staging_buffer.Reset();
  m_buffer.Reset();
}

bool D3DBoundingBox::Initialize()
{
  // GPU-side buffer, written by pixel shaders, starting out as zeros.
  D3D11_BUFFER_DESC desc;
  desc.ByteWidth = BUFFER_BYTES;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  desc.CPUAccessFlags = 0;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  desc.StructureByteStride = VALUE_SIZE;

  const std::array<BBoxType, NUM_BBOX_VALUES> zeros{};
  D3D11_SUBRESOURCE_DATA initial;
  initial.pSysMem = zeros.data();
  initial.SysMemPitch = BUFFER_BYTES;
  initial.SysMemSlicePitch = 0;

  HRESULT result = D3D::device->CreateBuffer(&desc, &initial, &m_buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create BoundingBox Buffer: {}",
             DX11HRWrap(result));
  if (FAILED(result))
    return false;
  D3DCommon::SetDebugObjectName(m_buffer.Get(), "BoundingBox Buffer");

  // CPU-readable copy of the same size (the stride is left as it was).
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  result = D3D::device->CreateBuffer(&desc, nullptr, &m_staging_buffer);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create BoundingBox Staging Buffer: {}",
             DX11HRWrap(result));
  if (FAILED(result))
    return false;
  D3DCommon::SetDebugObjectName(m_staging_buffer.Get(), "BoundingBox Staging Buffer");

  // Raw (byte-addressed) view, so shaders can use atomic min/max on it.
  D3D11_UNORDERED_ACCESS_VIEW_DESC view = {};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  view.Buffer.FirstElement = 0;
  view.Buffer.NumElements = NUM_BBOX_VALUES;
  view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  result = D3D::device->CreateUnorderedAccessView(m_buffer.Get(), &view, &m_uav);
  ASSERT_MSG(VIDEO, SUCCEEDED(result), "Failed to create BoundingBox UAV: {}", DX11HRWrap(result));
  if (FAILED(result))
    return false;
  D3DCommon::SetDebugObjectName(m_uav.Get(), "BoundingBox UAV");

  D3D::stateman->SetOMUAV(m_uav.Get());
  return true;
}

std::vector<BBoxType> D3DBoundingBox::Read(u32 index, u32 length)
{
  // Values stay zero when the staging copy cannot be mapped.
  std::vector<BBoxType> values(length);
  D3D::context->CopyResource(m_staging_buffer.Get(), m_buffer.Get());

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (FAILED(D3D::context->Map(m_staging_buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
    return values;

  const u8* const first = static_cast<const u8*>(mapped.pData) + VALUE_SIZE * index;
  std::memcpy(values.data(), first, VALUE_SIZE * length);
  D3D::context->Unmap(m_staging_buffer.Get(), 0);
  return values;
}

void D3DBoundingBox::Write(u32 index, std::span<const BBoxType> values)
{
  // A byte range of the buffer: [index, index + count) values.
  D3D11_BOX range;
  range.left = index * VALUE_SIZE;
  range.top = 0;
  range.front = 0;
  range.right = static_cast<UINT>((index + values.size()) * VALUE_SIZE);
  range.bottom = 1;
  range.back = 1;
  D3D::context->UpdateSubresource(m_buffer.Get(), 0, &range, values.data(), 0, 0);
}

}  // namespace DX11
