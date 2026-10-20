// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// resource usage flags shared by GX2 surfaces and GX2R buffers
#define GX2_RESFLAG_USAGE_TEXTURE					(1<<0)
#define GX2_RESFLAG_USAGE_COLOR_BUFFER				(1<<1)
#define GX2_RESFLAG_USAGE_DEPTH_BUFFER				(1<<2)
#define GX2_RESFLAG_USAGE_SCAN_BUFFER				(1<<3)

// GX2R buffer kinds
#define GX2R_RESFLAG_USAGE_VERTEX_BUFFER			(1<<4)
#define GX2R_RESFLAG_USAGE_INDEX_BUFFER				(1<<5)
#define GX2R_RESFLAG_USAGE_UNIFORM_BLOCK			(1<<6)
#define GX2R_RESFLAG_USAGE_SHADER_PROGRAM			(1<<7)
#define GX2R_RESFLAG_USAGE_STREAM_OUTPUT			(1<<8)
#define GX2R_RESFLAG_USAGE_DISPLAY_LIST				(1<<9)
#define GX2R_RESFLAG_USAGE_GS_RINGBUFFER			(1<<10)

// GX2R access flags
#define GX2R_RESFLAG_USAGE_CPU_READ					(1<<11)
#define GX2R_RESFLAG_USAGE_CPU_WRITE				(1<<12)
#define GX2R_RESFLAG_USAGE_GPU_READ					(1<<13)
#define GX2R_RESFLAG_USAGE_GPU_WRITE				(1<<14)

#define GX2R_RESFLAG_USE_MEM1						(1<<17)

// passed through to the free callback by GX2RDestroySurfaceEx
#define GX2R_RESFLAG_UKN_BIT_19						(1<<19)
#define GX2R_RESFLAG_UKN_BIT_20						(1<<20)
#define GX2R_RESFLAG_UKN_BIT_21						(1<<21)
#define GX2R_RESFLAG_UKN_BIT_22						(1<<22)
#define GX2R_RESFLAG_UKN_BIT_23						(1<<23)

// state bits maintained by GX2R
#define GX2R_RESFLAG_ALLOCATED_BY_GX2R				(1<<29)
#define GX2R_RESFLAG_LOCKED							(1<<30)

// guest layout of GX2RBuffer
struct GX2RBuffer
{
	/* +0x00 */ uint32be		resFlags;
	/* +0x04 */ uint32be		elementSize;
	/* +0x08 */ uint32be		elementCount;
	/* +0x0C */ MEMPTR<void>	ptr;

	void* GetPtr() const { return ptr.GetPtr(); }
	MPTR GetVirtualAddr() const { return ptr.GetMPTR(); }
	uint32 GetSize() const { return (uint32)elementCount * (uint32)elementSize; } // in bytes
};

static_assert(sizeof(GX2RBuffer) == 0x10);

namespace GX2
{
	void GX2ResourceInit();

	void GX2RSetAllocator(MPTR funcAllocMPTR, MPTR funcFreeMPR);
};
