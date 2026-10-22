// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "GX2.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "Cafe/CafeSystem.h"
#include "GX2_Query.h"

// Occlusion queries. A query object holds eight 64-bit little endian counters (one begin/end
// pair per render backend, Latte has two backends). The GPU emulation fills them in when
// it processes the HLE begin/end packets.

namespace GX2
{
	static constexpr uint32 kRenderBackends = 2;
	static constexpr uint32 kCounterCount = 8;
	// CPU queries mark "result pending" with this tag in the dword pair after the per-backend counters
	static constexpr uint32 kTagIndex = kRenderBackends * 4;

	struct GX2Query
	{
		uint32 reg[kCounterCount * 2]; // raw counter memory
	};

	static_assert(sizeof(GX2Query) == 0x40);

	static uint64 _ReadCounter(const GX2Query* q, uint32 dwordIndex)
	{
		uint64 v;
		memcpy(&v, q->reg + dwordIndex, sizeof(v));
		return v;
	}

	// Xenoblade Chronicles X decides object visibility from GPU query results and flickers if a
	// result of 0 is still in place when it looks. For its title ids the end counter is preset
	// to a large value instead of resetting the counters on the GPU.
	static bool _IsPresetQueryTitle()
	{
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		return titleId == 0x00050000101c4c00ULL || titleId == 0x00050000101c4d00ULL || titleId == 0x0005000010116100ULL;
	}

	static void _PrepareOcclusionQuery(GX2Query* query, bool gpuQuery)
	{
		if (!gpuQuery)
		{
			// CPU queries: clear the first counters and tag the query as pending
			memset(query, 0, 0x10);
			query->reg[kTagIndex + 0] = 0;
			query->reg[kTagIndex + 1] = _swapEndianU32('OCPU');
			return;
		}
		if (_IsPresetQueryTitle())
		{
			const uint64 preset = 0x100000;
			memcpy(query->reg + 2, &preset, sizeof(preset));
			return;
		}
		// let the GPU reset all counters; counters of backends that don't exist get the "invalid" bit
		GX2ReserveCmdSpace(5 * kCounterCount);
		const MPTR queryPhys = memory_virtualToPhysical(memory_getVirtualOffsetFromPointer(query));
		for (uint32 i = 0; i < kCounterCount; i++)
		{
			const uint32 high = i >= kRenderBackends * 2 ? 0x80000000 : 0;
			gx2WriteGather_submit(pm4HeaderType3(IT_MEM_WRITE, 4), (queryPhys + i * 8) | 2, 0x20000, 0, high);
		}
	}

	static bool _IsOcclusionQueryType(uint32 queryType)
	{
		return queryType == GX2_QUERY_TYPE_OCCLUSION_CPU || queryType == GX2_QUERY_TYPE_OCCLUSION_GPU;
	}

	void GX2QueryBegin(uint32 queryType, GX2Query* query)
	{
		if (!_IsOcclusionQueryType(queryType))
		{
			debug_printf("GX2QueryBegin(): query type %d is not supported\n", queryType);
			return;
		}
		_PrepareOcclusionQuery(query, queryType == GX2_QUERY_TYPE_OCCLUSION_GPU);
		GX2ReserveCmdSpace(2);
		gx2WriteGather_submit(pm4HeaderType3(IT_HLE_BEGIN_OCCLUSION_QUERY, 1), MEMPTR<GX2Query>(query).GetMPTR());
	}

	void GX2QueryEnd(uint32 queryType, GX2Query* query)
	{
		GX2ReserveCmdSpace(2);
		if (!_IsOcclusionQueryType(queryType))
		{
			debug_printf("GX2QueryEnd(): query type %d is not supported\n", queryType);
			return;
		}
		gx2WriteGather_submit(pm4HeaderType3(IT_HLE_END_OCCLUSION_QUERY, 1), MEMPTR<GX2Query>(query).GetMPTR());
	}

	uint32 GX2QueryGetOcclusionResult(GX2Query* query, uint64be* resultOut)
	{
		// a CPU query that the GPU has not finished yet still carries the pending tag
		if (query->reg[kTagIndex + 1] == _swapEndianU32('OCPU') && query->reg[kTagIndex + 0] == 0)
			return GX2_FALSE;
		const uint64 begin = _ReadCounter(query, 0);
		const uint64 end = _ReadCounter(query, 2);
		constexpr uint64 kInvalidBit = 0x8000000000000000ULL;
		if ((begin | end) & kInvalidBit)
			return GX2_FALSE;
		*resultOut = end - begin;
		return GX2_TRUE;
	}

	void GX2QueryBeginConditionalRender(uint32 queryType, GX2Query* query, uint32 dontWaitBool, uint32 pixelsMustPassBool)
	{
		// SET_PREDICATION flags: bit 31 = draw if visible, bits 13-15 = predicate type, bit 19 = don't wait
		uint32 flags = (queryType == GX2_QUERY_TYPE_OCCLUSION_GPU ? 1u : 2u) << 13;
		if (pixelsMustPassBool)
			flags |= 1u << 31;
		if (dontWaitBool != 0)
			flags |= 1u << 19;
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_PREDICATION, 2), memory_virtualToPhysical(MEMPTR<GX2Query>(query).GetMPTR()), flags);
	}

	void GX2QueryEndConditionalRender()
	{
		GX2ReserveCmdSpace(3);
		gx2WriteGather_submit(pm4HeaderType3(IT_SET_PREDICATION, 2), MPTR_NULL, 0);
	}

	void GX2QueryInit()
	{
		cafeExportRegister("gx2", GX2QueryBegin, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryEnd, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryGetOcclusionResult, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryBeginConditionalRender, LogType::GX2);
		cafeExportRegister("gx2", GX2QueryEndConditionalRender, LogType::GX2);
	}
};
