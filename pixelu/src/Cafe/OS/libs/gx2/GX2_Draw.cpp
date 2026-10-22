// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"

#include "Cafe/OS/common/OSCommon.h"

#include "GX2_Command.h"
#include "GX2_Draw.h"

// Draw calls. Every draw first programs the per draw state (base vertex, primitive type,
// index type, instance count) and then issues the draw packet. The draw initiator word has
// bit 0 set in the packet header (shader type "vertex").

namespace GX2
{
	using namespace Latte;

	static constexpr uint32 kDrawInitiatorBit = 0x00000001;
	static constexpr uint32 kVertexBufferSlot0 = 0x8C0; // first vertex fetch resource, 7 words per slot

	static uint32 _HostToPhysical(void* p)
	{
		return memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(p));
	}

	// SET_CTL_CONST: index 0 = base vertex, index 1 = base instance (3 words)
	static void _EmitCtlConst(uint32 index, uint32 value)
	{
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CTL_CONST, 2), index, value);
	}

	// primitive type, index type and instance count (7 words)
	static void _EmitDrawSetup(GX2PrimitiveMode2 primitiveMode, uint32 indexType, uint32 numInstances)
	{
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONFIG_REG, 2), REGADDR::VGT_PRIMITIVE_TYPE - 0x2000, (uint32)primitiveMode,
			pm4HeaderType3(IT_INDEX_TYPE, 1), indexType,
			pm4HeaderType3(IT_NUM_INSTANCES, 1), numInstances);
	}

	// DRAW_INDEX_2: max index (unbounded), index buffer address (low/high), count, initiator (6 words)
	static void _EmitDrawIndex2(void* indexData, uint32 count)
	{
		gx2WriteGather_submit(pm4HeaderType3(IT_DRAW_INDEX_2, 5) | kDrawInitiatorBit,
			0xFFFFFFFFu, _HostToPhysical(indexData), 0, count, 0);
	}

	void GX2SetAttribBuffer(uint32 bufferIndex, uint32 sizeInBytes, uint32 stride, void* data)
	{
		GX2ReserveCmdSpace(9);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8),
			kVertexBufferSlot0 + bufferIndex * 7,
			_HostToPhysical(data),
			sizeInBytes - 1,
			(stride & 0xFFFF) << 11, // stride field of SQ_VTX_CONSTANT_WORD2
			0, 0, 0,
			0xC0000000); // resource type: vertex buffer
	}

	void GX2DrawIndexedEx(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances)
	{
		GX2ReserveCmdSpace(16);
		_EmitCtlConst(0, baseVertex);
		_EmitDrawSetup(primitiveMode, (uint32)indexType, numInstances);
		_EmitDrawIndex2(indexData, count);
	}

	void GX2DrawIndexedEx2(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances, uint32 baseInstance)
	{
		GX2ReserveCmdSpace(22);
		_EmitCtlConst(0, baseVertex);
		_EmitCtlConst(1, baseInstance);
		_EmitDrawSetup(primitiveMode, (uint32)indexType, numInstances);
		_EmitDrawIndex2(indexData, count);
		_EmitCtlConst(1, 0); // later draws expect base instance 0
	}

	void GX2DrawEx(GX2PrimitiveMode2 primitiveMode, uint32 count, uint32 baseVertex, uint32 numInstances)
	{
		GX2ReserveCmdSpace(16);
		_EmitCtlConst(0, baseVertex);
		_EmitDrawSetup(primitiveMode, (uint32)GX2IndexType::U32_BE, numInstances);
		gx2WriteGather_submit(pm4HeaderType3(IT_DRAW_INDEX_AUTO, 2) | kDrawInitiatorBit, count, 0);
	}

	void GX2DrawIndexedImmediateEx(GX2PrimitiveMode2 primitiveMode, uint32 count, GX2IndexType indexType, void* indexData, uint32 baseVertex, uint32 numInstances)
	{
		bool wideIndices;
		if (indexType == GX2IndexType::U32_BE || indexType == GX2IndexType::U32_LE)
			wideIndices = true;
		else if (indexType == GX2IndexType::U16_BE || indexType == GX2IndexType::U16_LE)
			wideIndices = false;
		else
		{
			// the index data size would be undefined; nothing sensible can be drawn
			cemuLog_log(LogType::APIErrors, "GX2DrawIndexedImmediateEx(): unsupported index type {}", (uint32)indexType);
			return;
		}
		// indices are embedded into the packet, packed into dwords
		const uint32 dataWords = wideIndices ? count : (count + 1) / 2;
		GX2ReserveCmdSpace(22 + dataWords);
		if (dataWords > 0x4000 - 2)
		{
			cemuLog_log(LogType::Force, "GX2DrawIndexedImmediateEx(): index data too large for one PM4 packet (limit is 16KiB minus 8 bytes)");
			return;
		}

		_EmitCtlConst(0, baseVertex);
		_EmitDrawSetup(primitiveMode, (uint32)indexType, numInstances);
		gx2WriteGather_submit(pm4HeaderType3(IT_DRAW_INDEX_IMMD, 2 + dataWords) | kDrawInitiatorBit, count, 0);

		// the guest words are copied unchanged, except that the two 16-bit indices of a dword trade places
		const uint8* src = (const uint8*)indexData;
		for (uint32 i = 0; i < dataWords; i++)
		{
			uint32 raw;
			memcpy(&raw, src + i * 4, 4);
			if (!wideIndices)
				raw = (raw << 16) | (raw >> 16);
			gx2WriteGather_submitU32AsLE(raw);
		}
	}

	struct GX2DispatchComputeParam
	{
		/* +0x00 */ uint32be worksizeX;
		/* +0x04 */ uint32be worksizeY;
		/* +0x08 */ uint32be worksizeZ;
	};

	void GX2DispatchCompute(GX2DispatchComputeParam* dispatchParam)
	{
		GX2ReserveCmdSpace(19);
		// the dispatch dimensions are made available to the shader through a buffer resource
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_RESOURCE, 8),
			mmSQ_CS_DISPATCH_PARAMS - mmSQ_TEX_RESOURCE_WORD0,
			memory_virtualToPhysical(MEMPTR<GX2DispatchComputeParam>(dispatchParam).GetMPTR()),
			0xF, 0x862000, 1, 0xABCD1234, 0xABCD1234, 0xC0000000);
		// one point per work item, a single instance
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONFIG_REG, 2), REGADDR::VGT_PRIMITIVE_TYPE - 0x2000, 1,
			pm4HeaderType3(IT_NUM_INSTANCES, 1), 1);
		const uint32 items = (uint32)dispatchParam->worksizeX * (uint32)dispatchParam->worksizeY * (uint32)dispatchParam->worksizeZ;
		gx2WriteGather_submit(pm4HeaderType3(IT_DRAW_INDEX_AUTO, 2) | kDrawInitiatorBit, items, 0);
	}

	void GX2DrawInit()
	{
		cafeExportRegister("gx2", GX2SetAttribBuffer, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawIndexedEx, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawIndexedEx2, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawEx, LogType::GX2);
		cafeExportRegister("gx2", GX2DrawIndexedImmediateEx, LogType::GX2);
		cafeExportRegister("gx2", GX2DispatchCompute, LogType::GX2);
	}
}
