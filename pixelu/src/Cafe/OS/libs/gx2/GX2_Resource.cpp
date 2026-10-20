// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"
#include "GX2.h"
#include "GX2_Command.h"
#include "GX2_Resource.h"
#include "GX2_Streamout.h"
#include "GX2_Draw.h"

// GX2R: buffers and surfaces whose memory is managed through a pair of guest callbacks
// alloc(resFlags, size, alignment) and free(resFlags, ptr).

namespace GX2
{
	namespace
	{
		MPTR s_gx2rAlloc = MPTR_NULL;
		MPTR s_gx2rFree = MPTR_NULL;

		void MarkAllocatedByGX2R(uint32be& flags)
		{
			flags = ((uint32)flags & ~(uint32)GX2R_RESFLAG_LOCKED) | GX2R_RESFLAG_ALLOCATED_BY_GX2R;
		}

		uint8* OffsetPtr(void* p, uint32 offset)
		{
			return (uint8*)p + offset;
		}
	}

	void GX2RSetAllocator(MPTR funcAllocMPTR, MPTR funcFreeMPR)
	{
		s_gx2rAlloc = funcAllocMPTR;
		s_gx2rFree = funcFreeMPR;
	}

	/* buffers */

	uint32 GX2RGetBufferAllocationSize(GX2RBuffer* buffer)
	{
		return (buffer->GetSize() + 63) & ~63u;
	}

	uint32 GX2RGetBufferAlignment(uint32 resFlags)
	{
		// GPU written / program-like buffers need 256 byte alignment, plain input buffers 64
		constexpr uint32 kNeeds256 = GX2R_RESFLAG_USAGE_STREAM_OUTPUT | GX2R_RESFLAG_USAGE_UNIFORM_BLOCK | GX2R_RESFLAG_USAGE_SHADER_PROGRAM | GX2R_RESFLAG_USAGE_GS_RINGBUFFER;
		constexpr uint32 kNeeds64 = GX2R_RESFLAG_USAGE_VERTEX_BUFFER | GX2R_RESFLAG_USAGE_INDEX_BUFFER | GX2R_RESFLAG_USAGE_DISPLAY_LIST;
		if (resFlags & kNeeds256)
			return 0x100;
		if (resFlags & kNeeds64)
			return 0x40;
		return 0x100;
	}

	bool GX2RCreateBuffer(GX2RBuffer* buffer)
	{
		const uint32 alignment = GX2RGetBufferAlignment(buffer->resFlags);
		const uint32 size = GX2RGetBufferAllocationSize(buffer);
		const MPTR mem = PPCCoreCallback(s_gx2rAlloc, (uint32)buffer->resFlags, size, alignment);
		buffer->ptr = mem;
		MarkAllocatedByGX2R(buffer->resFlags);
		return mem != MPTR_NULL;
	}

	bool GX2RCreateBufferUserMemory(GX2RBuffer* buffer, void* ptr, uint32 unusedSizeParameter)
	{
		buffer->ptr = ptr;
		buffer->resFlags = (uint32)buffer->resFlags & ~(uint32)(GX2R_RESFLAG_LOCKED | GX2R_RESFLAG_ALLOCATED_BY_GX2R);
		return true;
	}

	void GX2RDestroyBufferEx(GX2RBuffer* buffer, uint32 resFlags)
	{
		// memory supplied by the game is not ours to free
		if (((uint32)buffer->resFlags & GX2R_RESFLAG_ALLOCATED_BY_GX2R) != 0)
			PPCCoreCallback(s_gx2rFree, (uint32)buffer->resFlags, buffer->GetPtr());
		buffer->ptr = nullptr;
	}

	bool GX2RBufferExists(GX2RBuffer* buffer)
	{
		return buffer && buffer->GetPtr();
	}

	void GX2RSetBufferName(GX2RBuffer* buffer, const char* name)
	{
		// debug names are not kept
	}

	void* GX2RLockBufferEx(GX2RBuffer* buffer, uint32 resFlags)
	{
		return buffer->GetPtr();
	}

	// the CPU may have changed the contents: let the GPU buffer cache know (regardless of flags)
	void GX2RUnlockBufferEx(GX2RBuffer* buffer, uint32 resFlags)
	{
		LatteBufferCache_notifyDCFlush(buffer->GetVirtualAddr(), buffer->GetSize());
	}

	void GX2RInvalidateBuffer(GX2RBuffer* buffer, uint32 resFlags)
	{
		LatteBufferCache_notifyDCFlush(buffer->GetVirtualAddr(), buffer->GetSize());
	}

	void GX2RSetAttributeBuffer(GX2RBuffer* buffer, uint32 bufferIndex, uint32 stride, uint32 offset)
	{
		const uint32 size = buffer->GetSize();
		if (offset > size)
			cemuLog_log(LogType::Force, "GX2RSetAttributeBuffer(): offset {} is beyond the buffer size {}", offset, size);
		GX2SetAttribBuffer(bufferIndex, size - offset, stride, OffsetPtr(buffer->GetPtr(), offset));
	}

	void GX2RSetStreamOutBuffer(uint32 bufferIndex, GX2StreamOutBuffer* soBuffer)
	{
		GX2SetStreamOutBuffer(bufferIndex, soBuffer);
	}

	/* surfaces */

	bool GX2RCreateSurface(GX2Surface* surface, uint32 resFlags)
	{
		surface->resFlag = resFlags;
		GX2CalcSurfaceSizeAndAlignment(surface);
		MarkAllocatedByGX2R(surface->resFlag);
		// image and mip chain share one allocation, the mips directly follow the image
		const uint32 imageSize = surface->imageSize;
		const uint32 mipSize = surface->mipSize;
		const MPTR mem = PPCCoreCallback(s_gx2rAlloc, (uint32)surface->resFlag, imageSize + mipSize, (uint32)surface->alignment);
		surface->imagePtr = mem;
		surface->mipPtr = (mem != MPTR_NULL && mipSize > 0) ? mem + imageSize : MPTR_NULL;
		return mem != MPTR_NULL;
	}

	bool GX2RCreateSurfaceUserMemory(GX2Surface* surface, void* imagePtr, void* mipPtr, uint32 resFlags)
	{
		surface->resFlag = resFlags & ~(uint32)(GX2R_RESFLAG_LOCKED | GX2R_RESFLAG_ALLOCATED_BY_GX2R);
		GX2CalcSurfaceSizeAndAlignment(surface);
		surface->imagePtr = memory_getVirtualOffsetFromPointer(imagePtr);
		surface->mipPtr = memory_getVirtualOffsetFromPointer(mipPtr);
		return true;
	}

	void GX2RDestroySurfaceEx(GX2Surface* surface, uint32 resFlags)
	{
		if (((uint32)surface->resFlag & GX2R_RESFLAG_ALLOCATED_BY_GX2R) != 0)
		{
			// bits 19-23 of the destroy flags are forwarded to the free callback
			const uint32 passThrough = resFlags & (GX2R_RESFLAG_UKN_BIT_19 | GX2R_RESFLAG_UKN_BIT_20 | GX2R_RESFLAG_UKN_BIT_21 | GX2R_RESFLAG_UKN_BIT_22 | GX2R_RESFLAG_UKN_BIT_23);
			PPCCoreCallback(s_gx2rFree, (uint32)surface->resFlag | passThrough, (uint32)surface->imagePtr);
		}
		surface->imagePtr = MPTR_NULL;
	}

	bool GX2RSurfaceExists(GX2Surface* surface)
	{
		constexpr uint32 kAnyAccess = GX2R_RESFLAG_USAGE_CPU_READ | GX2R_RESFLAG_USAGE_CPU_WRITE | GX2R_RESFLAG_USAGE_GPU_READ | GX2R_RESFLAG_USAGE_GPU_WRITE;
		return surface && surface->imagePtr != MPTR_NULL && ((uint32)surface->resFlag & kAnyAccess) != 0;
	}

	void* GX2RLockSurfaceEx(GX2Surface* surface, uint32 mipLevel, uint32 resFlags)
	{
		surface->resFlag = (uint32)surface->resFlag | GX2R_RESFLAG_LOCKED;
		return memory_getPointerFromVirtualOffset(surface->imagePtr);
	}

	void GX2RUnlockSurfaceEx(GX2Surface* surface, uint32 mipLevel, uint32 resFlags)
	{
		surface->resFlag = (uint32)surface->resFlag & ~(uint32)GX2R_RESFLAG_LOCKED;
	}

	/* display lists in GX2R buffers */

	void GX2RBeginDisplayListEx(GX2RBuffer* buffer, bool ukn, uint32 resFlags)
	{
		GX2BeginDisplayList(buffer->GetPtr(), buffer->GetSize());
	}

	uint32 GX2REndDisplayList(GX2RBuffer* buffer)
	{
		return GX2EndDisplayList(buffer->GetPtr());
	}

	void GX2RCallDisplayList(GX2RBuffer* buffer, uint32 size)
	{
		GX2CallDisplayList(buffer->GetVirtualAddr(), size);
	}

	void GX2RDirectCallDisplayList(GX2RBuffer* buffer, uint32 size)
	{
		GX2DirectCallDisplayList(buffer->GetPtr(), size);
	}

	void GX2RDrawIndexed(GX2PrimitiveMode2 primitiveMode, GX2RBuffer* indexBuffer, Latte::LATTE_VGT_DMA_INDEX_TYPE::E_INDEX_TYPE indexType, uint32 count, uint32 baseIndex, uint32 baseVertex, uint32 numInstances)
	{
		void* firstIndex = OffsetPtr(indexBuffer->GetPtr(), baseIndex * (uint32)indexBuffer->elementSize);
		GX2DrawIndexedEx(primitiveMode, count, indexType, firstIndex, baseVertex, numInstances);
	}

	void GX2ResourceInit()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("gx2", fn, LogType::GX2)
		EXPORT_FN(GX2RSetAllocator); EXPORT_FN(GX2RGetBufferAllocationSize); EXPORT_FN(GX2RGetBufferAlignment);

		EXPORT_FN(GX2RCreateBuffer); EXPORT_FN(GX2RCreateBufferUserMemory); EXPORT_FN(GX2RDestroyBufferEx);
		EXPORT_FN(GX2RBufferExists); EXPORT_FN(GX2RSetBufferName); EXPORT_FN(GX2RLockBufferEx);
		EXPORT_FN(GX2RUnlockBufferEx); EXPORT_FN(GX2RInvalidateBuffer);

		EXPORT_FN(GX2RSetAttributeBuffer); EXPORT_FN(GX2RSetStreamOutBuffer);

		EXPORT_FN(GX2RCreateSurface); EXPORT_FN(GX2RCreateSurfaceUserMemory); EXPORT_FN(GX2RDestroySurfaceEx);
		EXPORT_FN(GX2RSurfaceExists); EXPORT_FN(GX2RLockSurfaceEx); EXPORT_FN(GX2RUnlockSurfaceEx);

		EXPORT_FN(GX2RBeginDisplayListEx); EXPORT_FN(GX2REndDisplayList); EXPORT_FN(GX2RCallDisplayList);
		EXPORT_FN(GX2RDirectCallDisplayList);

		EXPORT_FN(GX2RDrawIndexed);
#undef EXPORT_FN

		s_gx2rAlloc = MPTR_NULL;
		s_gx2rFree = MPTR_NULL;
	}
};
