// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"

// Stream out (transform feedback). The backend captures shader output into a host ring
// buffer; after the draw the captured bytes are moved to the guest buffer range, either
// straight into guest memory or into the buffer cache.

namespace
{
	constexpr sint32 kRingBufferSize = 8 * 1024 * 1024;

	struct StreamoutTarget
	{
		bool isActive;
		sint32 ringBufferOffset;
		uint32 rangeAddr;
		uint32 rangeSize; // bytes written, limited to the space left in the guest buffer
	};

	struct
	{
		sint32 ringWriteOffset;
		bool drawActive;
		uint32 writeMask;
		uint32 vertexCount;
		uint32 instanceCount;
		StreamoutTarget targets[LATTE_NUM_STREAMOUT_BUFFER];
	} s_streamout{};

	// buffer register offsets, each buffer has its own group of four registers
	uint32& StreamoutReg(uint32 baseReg, uint32 bufferIndex)
	{
		return LatteGPUState.contextRegister[baseReg + bufferIndex * 4];
	}

	// space in the ring buffer for one draw; wraps to the start when the end is reached
	sint32 AllocateRingSpace(sint32 size)
	{
		size = (size + 255) & ~255;
		if (s_streamout.ringWriteOffset + size > kRingBufferSize)
			s_streamout.ringWriteOffset = 0;
		const sint32 offset = s_streamout.ringWriteOffset;
		s_streamout.ringWriteOffset += size;
		return offset;
	}

	// bytes the draw emits into a buffer, capped at the buffer size
	uint32 EmittedBytes(uint32 bufferIndex)
	{
		const uint32 stride = StreamoutReg(mmVGT_STRMOUT_VTX_STRIDE_0, bufferIndex) << 2;
		const uint32 bufferSize = StreamoutReg(mmVGT_STRMOUT_BUFFER_SIZE_0, bufferIndex) << 2;
		// every vertex is assumed to be emitted once (point lists)
		const uint32 vertices = s_streamout.vertexCount * s_streamout.instanceCount;
		return std::min(vertices * stride, bufferSize);
	}

	uint32 WriteMaskOf(LatteDecompilerShader* shader)
	{
		uint32 mask = 0;
		for (sint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++)
		{
			if (shader->streamoutBufferWriteMask[i])
				mask |= 1u << i;
		}
		return mask;
	}
}

sint32 LatteStreamout_GetRingBufferSize()
{
	return kRingBufferSize;
}

void LatteStreamout_InitCache()
{
	s_streamout.ringWriteOffset = 0;
}

void LatteStreamout_PrepareDrawcall(uint32 count, uint32 instanceCount)
{
	if (LatteGPUState.contextRegister[mmVGT_STRMOUT_EN] == 0)
	{
		s_streamout.drawActive = false;
		return;
	}
	LatteDecompilerShader* vertexShader = LatteSHRC_GetActiveVertexShader();
	LatteDecompilerShader* geometryShader = LatteSHRC_GetActiveGeometryShader();
	// with a geometry shader each input vertex can become up to ring item size / vertex size output vertices
	sint32 verticesPerInput = 1;
	if (geometryShader)
	{
		const uint32 vertexBytes = LatteGPUState.contextRegister[mmSQ_GS_VERT_ITEMSIZE] * 4;
		verticesPerInput = ((LatteGPUState.contextRegister[mmSQ_GSVS_RING_ITEMSIZE] & 0x7FFF) * 4) / vertexBytes;
		cemu_assert_debug(verticesPerInput > 0);
	}
	s_streamout.vertexCount = count * verticesPerInput;
	s_streamout.instanceCount = instanceCount;
	// the last stage before rasterization writes the stream out data
	s_streamout.writeMask = WriteMaskOf(geometryShader ? geometryShader : vertexShader);
	for (uint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++)
	{
		StreamoutTarget& target = s_streamout.targets[i];
		target.isActive = (s_streamout.writeMask & (1u << i)) != 0;
		if (!target.isActive)
			continue;
		const uint32 bufferAddr = StreamoutReg(mmVGT_STRMOUT_BUFFER_BASE_0, i) << 8;
		const uint32 bufferSize = StreamoutReg(mmVGT_STRMOUT_BUFFER_SIZE_0, i) << 2;
		const uint32 writeOffset = StreamoutReg(mmVGT_STRMOUT_BUFFER_OFFSET_0, i);
		const uint32 emitted = EmittedBytes(i);
		// the ring holds the full output, the guest range only what fits behind the write offset
		target.ringBufferOffset = AllocateRingSpace(emitted);
		target.rangeAddr = bufferAddr + writeOffset;
		target.rangeSize = std::min(emitted, writeOffset > bufferSize ? 0 : bufferSize - writeOffset);
		g_renderer->streamout_setupXfbBuffer(i, target.ringBufferOffset, target.rangeAddr, target.rangeSize);
	}
	g_renderer->streamout_begin();
	s_streamout.drawActive = true;
}

void LatteStreamout_FinishDrawcall(bool useDirectMemoryMode)
{
	if (!s_streamout.drawActive)
		return;
	s_streamout.drawActive = false;
	for (uint32 i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++)
	{
		if ((s_streamout.writeMask & (1u << i)) == 0)
			continue;
		const StreamoutTarget& target = s_streamout.targets[i];
		if (target.rangeSize > 0)
		{
			if (useDirectMemoryMode)
				g_renderer->bufferCache_copyStreamoutToMainBuffer(target.ringBufferOffset, target.rangeAddr, target.rangeSize);
			else
				LatteBufferCache_copyStreamoutDataToCache(target.rangeAddr, target.rangeSize, target.ringBufferOffset);
		}
		// the guest sees the buffer filled up to the new offset
		StreamoutReg(mmVGT_STRMOUT_BUFFER_OFFSET_0, i) += target.rangeSize;
	}
	g_renderer->streamout_rendererFinishDrawcall();
}
