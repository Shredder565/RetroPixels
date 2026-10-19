// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/Core/LatteTextureReadbackInfo.h"

// GPU to CPU copy of one mip/slice of a texture into a host visible buffer, completed when the
// command buffer that recorded it has finished.
class LatteTextureReadbackInfoVk : public LatteTextureReadbackInfo
{
public:
	LatteTextureReadbackInfoVk(VkDevice device, LatteTextureView* textureView);
	~LatteTextureReadbackInfoVk();

	// bytes needed for the readback, 0 when the texture cannot be read back
	static uint32 GetImageSize(LatteTextureView* textureView);

	void StartTransfer() override;

	bool IsFinished() override;
	void ForceFinish() override;

	uint8* GetData() override
	{
		return m_mapped + m_offset;
	}

	uint32 GetImageSize() const
	{
		return m_image_size;
	}

	// where the data goes: a buffer, its mapping and the offset within it
	void SetBuffer(VkBuffer buffer, uint8* buffer_ptr, const uint32 buffer_offset)
	{
		m_target = buffer;
		m_mapped = buffer_ptr;
		m_offset = buffer_offset;
	}

private:
	VkDevice m_device = nullptr;

	VkBuffer m_target = nullptr;
	uint8* m_mapped = nullptr;
	uint32 m_offset = 0;
	uint64 m_commandBufferId = 0;
};
