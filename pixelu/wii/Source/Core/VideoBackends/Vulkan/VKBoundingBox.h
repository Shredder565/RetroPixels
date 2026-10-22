// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string>

#include "Common/CommonTypes.h"
#include "VideoBackends/Vulkan/StagingBuffer.h"
#include "VideoBackends/Vulkan/VulkanLoader.h"

#include "VideoCommon/BoundingBox.h"

namespace Vulkan
{
class StagingBuffer;

// The bounding box lives in a device-local storage buffer the pixel shaders update atomically.
// Reads copy it into a host-visible buffer and wait; writes are recorded as buffer updates.
class VKBoundingBox final : public BoundingBox
{
public:
  ~VKBoundingBox() override;

  bool Initialize() override;

protected:
  std::vector<BBoxType> Read(u32 index, u32 length) override;
  void Write(u32 index, std::span<const BBoxType> values) override;

private:
  static constexpr size_t BUFFER_SIZE = sizeof(BBoxType) * NUM_BBOX_VALUES;

  // Barrier between pixel shader use of the buffer and a transfer (either direction).
  void ShaderToTransfer(VkAccessFlags transfer_access);
  void TransferToShader(VkAccessFlags transfer_access);

  VkBuffer m_buffer = VK_NULL_HANDLE;
  VmaAllocation m_allocation = VK_NULL_HANDLE;
  std::unique_ptr<StagingBuffer> m_readback;
};

}  // namespace Vulkan
