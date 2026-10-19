// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "GX2_Resource.h"

#define GX2_MAX_STREAMOUT_BUFFERS 4

namespace GX2
{
	// guest layout of GX2StreamOutBuffer
	struct GX2StreamOutBuffer
	{
		/* +0x00 */ uint32be size; // byte size, used together with dataPtr
		/* +0x04 */ MEMPTR<void> dataPtr; // null: the buffer is rBuffer
		/* +0x08 */ uint32be vertexStride;
		/* +0x0C */ GX2RBuffer rBuffer;
		/* +0x1C */ MEMPTR<void> ctxPtr; // GX2StreamOutContext (saved write offset)
	};

	static_assert(offsetof(GX2StreamOutBuffer, rBuffer) == 0x0C);
	static_assert(sizeof(GX2StreamOutBuffer) == 0x20, "GX2StreamOutBuffer has invalid size");

	void GX2SetStreamOutBuffer(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer);
	void GX2StreamoutInit();
}
