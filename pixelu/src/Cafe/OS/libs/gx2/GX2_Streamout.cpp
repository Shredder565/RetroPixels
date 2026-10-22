// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "GX2_Streamout.h"
#include "GX2_Command.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/OS/common/OSCommon.h"

// Stream out (transform feedback) buffers. The per buffer registers are four apart.

namespace GX2
{
	static bool _CheckStreamOutIndex(uint32 bufferIndex, const char* func)
	{
		if (bufferIndex < GX2_MAX_STREAMOUT_BUFFERS)
			return true;
		cemu_assert_suspicious();
		debug_printf("%s(): buffer index %u out of range\n", func, bufferIndex);
		return false;
	}

	void GX2SetStreamOutBuffer(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer)
	{
		if (!_CheckStreamOutIndex(bufferIndex, "GX2SetStreamOutBuffer"))
			return;
		const bool useRBuffer = streamOutBuffer->dataPtr.IsNull();
		const MPTR addr = useRBuffer ? streamOutBuffer->rBuffer.GetVirtualAddr() : streamOutBuffer->dataPtr.GetMPTR();
		const uint32 size = useRBuffer ? streamOutBuffer->rBuffer.GetSize() : (uint32)streamOutBuffer->size;
		GX2ReserveCmdSpace(6);
		// size in dwords, base address in 256 byte units
		gx2WriteGather_submit(
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2), (mmVGT_STRMOUT_BUFFER_SIZE_0 + bufferIndex * 4) - 0xA000, size >> 2,
			pm4HeaderType3(IT_SET_CONTEXT_REG, 2), (mmVGT_STRMOUT_BUFFER_BASE_0 + bufferIndex * 4) - 0xA000, memory_virtualToPhysical(addr) >> 8);
	}

	void GX2SetStreamOutEnable(uint32 enable)
	{
		cemu_assert_debug(enable <= 1);
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_CONTEXT_REG, 2), mmVGT_STRMOUT_EN - 0xA000, enable & 1);
	}

	// STRMOUT_BUFFER_UPDATE control word: bit 0 = store the offset to memory,
	// bits 1-2 = offset source (0: from context memory, 2: reset to zero, 3: keep), bits 8+ = buffer
	static void _EmitStreamOutUpdate(uint32 control, MPTR storeAddr, MPTR loadAddr)
	{
		gx2WriteGather_submit(pm4HeaderType3(IT_STRMOUT_BUFFER_UPDATE, 5), control, storeAddr, 0, loadAddr, 0);
	}

	void GX2SetStreamOutContext(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer, uint32 mode)
	{
		if (!_CheckStreamOutIndex(bufferIndex, "GX2SetStreamOutContext"))
			return;
		GX2ReserveCmdSpace(6);
		const MPTR ctxPhys = memory_virtualToPhysical(streamOutBuffer->ctxPtr.GetMPTR());
		if (mode == 0) // start from the beginning of the buffer
			_EmitStreamOutUpdate((2 << 1) | (bufferIndex << 8), MPTR_NULL, ctxPhys);
		else if (mode == 1) // continue where the saved context left off
			_EmitStreamOutUpdate((0 << 1) | (bufferIndex << 8), MPTR_NULL, ctxPhys);
		else
			cemu_assert_unimplemented();
	}

	void GX2SaveStreamOutContext(uint32 bufferIndex, GX2StreamOutBuffer* streamOutBuffer)
	{
		if (!_CheckStreamOutIndex(bufferIndex, "GX2SaveStreamOutContext"))
			return;
		GX2ReserveCmdSpace(6);
		_EmitStreamOutUpdate(1 | (3 << 1) | (bufferIndex << 8), memory_virtualToPhysical(streamOutBuffer->ctxPtr.GetMPTR()), MPTR_NULL);
	}

	void GX2StreamoutInit()
	{
		cafeExportRegister("gx2", GX2SetStreamOutBuffer, LogType::GX2);
		cafeExportRegister("gx2", GX2SetStreamOutEnable, LogType::GX2);
		cafeExportRegister("gx2", GX2SetStreamOutContext, LogType::GX2);
		cafeExportRegister("gx2", GX2SaveStreamOutContext, LogType::GX2);
	}
}
