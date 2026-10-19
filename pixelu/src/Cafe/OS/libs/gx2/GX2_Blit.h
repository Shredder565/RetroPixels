// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

namespace GX2
{
	// flags of GX2ClearBuffersEx / GX2ClearDepthStencilEx
	enum class GX2ClearFlags : uint32
	{
		CLEAR_DEPTH = 0x01, // clear the depth plane
		CLEAR_STENCIL = 0x02, // clear the stencil plane
		SET_DEPTH_REG = 0x04, // also make the value the new DB_DEPTH_CLEAR
		SET_STENCIL_REG = 0x08, // also make the value the new DB_STENCIL_CLEAR
	};

	void GX2BlitInit();
}
ENABLE_BITMASK_OPERATORS(GX2::GX2ClearFlags);
