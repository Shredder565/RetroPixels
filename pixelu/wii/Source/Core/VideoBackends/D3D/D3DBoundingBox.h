// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "VideoBackends/D3D/D3DBase.h"

#include "VideoCommon/BoundingBox.h"

namespace DX11
{
// The four bounding-box values live in a raw buffer that pixel shaders update
// through a UAV bound next to the render targets; reads go through a staging
// copy.
class D3DBoundingBox final : public BoundingBox
{
public:
  ~D3DBoundingBox() override;

  bool Initialize() override;

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, std::span<const BBoxType> values) override;

private:
  ComPtr<ID3D11Buffer> m_buffer;
  ComPtr<ID3D11Buffer> m_staging_buffer;
  ComPtr<ID3D11UnorderedAccessView> m_uav;
};

}  // namespace DX11
