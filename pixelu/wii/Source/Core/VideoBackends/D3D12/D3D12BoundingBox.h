// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12StreamBuffer.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"

#include "VideoCommon/BoundingBox.h"

namespace DX12
{
// Bounding-box values live in a GPU buffer the pixel shader updates through
// a UAV. Reads copy it to a readback buffer and wait; writes go through a
// small upload ring.
class D3D12BoundingBox final : public BoundingBox
{
public:
  ~D3D12BoundingBox() override;

  bool Initialize() override;

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, std::span<const BBoxType> values) override;

private:
  static constexpr u32 BUFFER_SIZE = sizeof(BBoxType) * NUM_BBOX_VALUES;
  static constexpr u32 MAX_UPDATES_PER_FRAME = 128;
  static constexpr u32 STREAM_BUFFER_SIZE = BUFFER_SIZE * MAX_UPDATES_PER_FRAME;

  bool CreateBuffers();

  ComPtr<ID3D12Resource> m_gpu_buffer;       // shader read/write
  ComPtr<ID3D12Resource> m_readback_buffer;  // CPU reads
  StreamBuffer m_upload_buffer;              // CPU writes
  DescriptorHandle m_gpu_descriptor{};
};

}  // namespace DX12
