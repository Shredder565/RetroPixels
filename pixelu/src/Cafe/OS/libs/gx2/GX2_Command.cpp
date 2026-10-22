// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/TCL/TCL.h"
#include "Cafe/HW/Latte/Core/LattePM4.h"
#include "GX2.h"
#include "GX2_Command.h"
#include "GX2_Shader.h"
#include "GX2_Misc.h"

namespace GX2
{
	GX2PerCoreCBState s_perCoreCBState[Espresso::CORE_COUNT];
}

// ---------------------------------------------------------------------------------
// raw word writers
// ---------------------------------------------------------------------------------

static GX2::GX2PerCoreCBState& _CurrentTarget()
{
	return GX2::s_perCoreCBState[PPCInterpreter_getCoreIndex(PPCInterpreter_getCurrentInstance())];
}

void gx2WriteGather_submitU32AsBE(uint32 v)
{
	auto& target = _CurrentTarget();
	if (!target.currentWritePtr)
		return;
	*target.currentWritePtr++ = v;
	cemu_assert_debug(target.currentWritePtr <= target.bufferPtr + target.bufferSizeInU32s);
}

// stores v unchanged (v already holds the big endian byte order)
void gx2WriteGather_submitU32AsLE(uint32 v)
{
	auto& target = _CurrentTarget();
	if (!target.currentWritePtr)
		return;
	memcpy(target.currentWritePtr, &v, sizeof(v));
	target.currentWritePtr++;
	cemu_assert_debug(target.currentWritePtr <= target.bufferPtr + target.bufferSizeInU32s);
}

void gx2WriteGather_submitU32AsLEArray(uint32* v, uint32 numValues)
{
	auto& target = _CurrentTarget();
	if (!target.currentWritePtr)
		return;
	memcpy(target.currentWritePtr, v, numValues * sizeof(uint32));
	target.currentWritePtr += numValues;
	cemu_assert_debug(target.currentWritePtr <= target.bufferPtr + target.bufferSizeInU32s);
}

namespace GX2
{
	// ---------------------------------------------------------------------------------
	// Command pool
	//
	// Command buffers are carved out of one big pool that is used like a ring. After the
	// GPU has executed a buffer it stores the address just past it into gpuReadPtr (an
	// IT_MEM_WRITE appended by us), which tells us how much of the pool is free again.
	// ---------------------------------------------------------------------------------

	struct CommandPoolShared // lives in guest memory because the GPU writes gpuReadPtr
	{
		MEMPTR<uint32be> poolBase;
		uint32 poolWords;
		MEMPTR<uint32be> gpuReadPtr;
		uint64be lastSubmittedStamp; // retire stamp of the newest submission (from TCL)
	};

	SysAllocator<CommandPoolShared> g_pool;
	bool g_poolOwned = false; // pool was allocated by us and must be freed on shutdown
	GX2PerCoreCBState g_savedMainCoreTarget; // command buffer of the main core while a display list is recorded

	constexpr uint32 kDefaultPoolBytes = 0x400000; // 4MB
	constexpr uint32 kMinBufferWords = 0x100;
	constexpr uint32 kMaxBufferWords = 0x20000;

	static GX2PerCoreCBState& _CoreTarget()
	{
		return s_perCoreCBState[coreinit::OSGetCoreId()];
	}

	static void _SetTarget(uint32be* buffer, uint32 words, bool isDisplayList)
	{
		auto& target = _CoreTarget();
		target.bufferPtr = buffer;
		target.bufferSizeInU32s = words;
		target.currentWritePtr = buffer;
		target.isDisplayList = isDisplayList;
	}

	static uint32 _GpuReadIndex()
	{
		stdx::atomic_ref<MEMPTR<uint32be>> readPtr(g_pool->gpuReadPtr);
		MEMPTR<uint32be> p = readPtr.load();
		cemu_assert_debug(p);
		return (uint32)(p.GetPtr() - g_pool->poolBase.GetPtr());
	}

	uint64 GX2GetLastSubmittedTimeStamp()
	{
		stdx::atomic_ref<uint64be> stamp(g_pool->lastSubmittedStamp);
		return stamp.load();
	}

	uint64 GX2GetRetiredTimeStamp()
	{
		uint64be stamp = 0;
		TCL::TCLTimestamp(TCL::TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED, &stamp);
		return stamp;
	}

	bool GX2WaitTimeStamp(uint64 tsWait)
	{
		TCL::TCLWaitTimestamp(TCL::TCLTimestampId::TIMESTAMP_LAST_BUFFER_RETIRED, tsWait, Espresso::TIMER_CLOCK * 60);
		return true;
	}

	// blocks until the GPU has retired at least one more submission (if there is one)
	static void _WaitForOneRetire()
	{
		uint64 target = std::min<uint64>(GX2GetRetiredTimeStamp() + 1, GX2GetLastSubmittedTimeStamp());
		GX2WaitTimeStamp(target);
	}

	// makes a fresh command buffer of at least minWords the write target of the calling core
	static void _BeginPoolBuffer(uint32 minWords)
	{
		// buffers are padded to 8 words when submitted, keep their size a multiple of 8 so the padding stays inside
		uint32 words = std::max<uint32>((minWords + 7) & ~7u, kMinBufferWords);
		auto& target = _CoreTarget();
		uint32be* const poolStart = g_pool->poolBase.GetPtr();
		const uint32 poolWords = g_pool->poolWords;
		uint32be* place = nullptr;
		while (!place)
		{
			const uint32 rd = _GpuReadIndex();
			const uint32 wr = target.bufferPtr ? (uint32)(target.bufferPtr + target.bufferSizeInU32s - poolStart) : 0;
			cemu_assert_debug(rd < poolWords && wr < poolWords);
			// one word always stays unused, otherwise a full pool would look like an empty one
			auto fits = [&](uint32 freeWords) { return freeWords > 0 && freeWords - 1 >= words; };
			if (wr < rd)
			{
				if (fits(rd - wr))
					place = poolStart + wr;
			}
			else if (fits(poolWords - wr))
				place = poolStart + wr;
			else if (fits(rd))
				place = poolStart; // wrap around
			if (!place)
				_WaitForOneRetire();
		}
		_SetTarget(place, std::min<uint32>(words, kMaxBufferWords), false);
	}

	static void _SubmitIndirectBuffer(uint32be* buffer, uint32 words, MEMPTR<uint32be>* readPtrToUpdate, bool raiseInterrupt)
	{
		uint32be packet[9];
		uint32 n = 0;
		packet[n++] = pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3);
		packet[n++] = memory_virtualToPhysical(MEMPTR<void>(buffer).GetMPTR());
		packet[n++] = 0; // upper address bits
		packet[n++] = words;
		if (readPtrToUpdate)
		{
			// once the buffer is done, let the GPU store the address just past it
			packet[n++] = pm4HeaderType3(IT_MEM_WRITE, 4);
			packet[n++] = memory_virtualToPhysical(MEMPTR<void>(readPtrToUpdate).GetMPTR()) | 2;
			packet[n++] = 0x40000; // 32-bit write
			packet[n++] = MEMPTR<void>(buffer + words).GetMPTR();
			packet[n++] = 0;
		}
		betype<TCL::TCLSubmissionFlag> flags = TCL::TCLSubmissionFlag::USE_RETIRED_MARKER;
		if (!raiseInterrupt)
			flags |= TCL::TCLSubmissionFlag::NO_MARKER_INTERRUPT;
		TCL::TCLSubmitToRing(packet, n, &flags, &g_pool->lastSubmittedStamp);
	}

	// fills the write target with type-2 filler packets up to the next multiple of 8 words
	static void _PadTarget()
	{
		auto& target = _CoreTarget();
		if (!target.currentWritePtr)
			return;
		while (((target.currentWritePtr - target.bufferPtr) & 7) != 0)
			*target.currentWritePtr++ = pm4HeaderType2Filler();
	}

	void GX2Command_Flush(uint32 numU32sForNextBuffer, bool triggerMarkerInterrupt)
	{
		auto& target = _CoreTarget();
		if (target.isDisplayList)
		{
			cemuLog_logDebugOnce(LogType::Force, "GX2: flush requested while recording a display list");
			return;
		}
		if (target.currentWritePtr == target.bufferPtr)
		{
			// nothing written yet, keep the buffer unless it can never hold the request
			if (numU32sForNextBuffer > g_pool->poolWords)
				_BeginPoolBuffer(numU32sForNextBuffer);
			return;
		}
		_PadTarget();
		const uint32 used = (uint32)(target.currentWritePtr - target.bufferPtr);
		cemu_assert_debug(used <= target.bufferSizeInU32s);
		_SubmitIndirectBuffer(target.bufferPtr, used, &g_pool->gpuReadPtr, triggerMarkerInterrupt);
		_BeginPoolBuffer(numU32sForNextBuffer);
	}

	void GX2Flush()
	{
		GX2Command_Flush(0x100, true);
	}

	void GX2ReserveCmdSpace(uint32 reservedFreeSpaceInU32)
	{
		auto& target = _CoreTarget();
		if (!target.currentWritePtr)
			return;
		const uint32 used = (uint32)(target.currentWritePtr - target.bufferPtr);
		if (used + reservedFreeSpaceInU32 > target.bufferSizeInU32s)
			GX2Command_Flush(reservedFreeSpaceInU32, true);
	}

	void GX2Init_commandBufferPool(void* bufferBase, uint32 bufferSize)
	{
		cemu_assert_debug(!g_pool->poolBase);
		const uint32 poolBytes = bufferSize ? bufferSize : kDefaultPoolBytes;
		g_poolOwned = (bufferBase == nullptr);
		if (g_poolOwned)
			bufferBase = coreinit::_weak_MEMAllocFromDefaultHeapEx(poolBytes, 0x100);
		g_pool->poolBase = (uint32be*)bufferBase;
		if (!bufferBase)
			cemuLog_log(LogType::Force, "GX2: could not allocate the command pool");
		g_pool->poolWords = poolBytes / sizeof(uint32be);
		g_pool->gpuReadPtr = g_pool->poolBase;
		for (auto& target : s_perCoreCBState)
			target = {};
		_BeginPoolBuffer(kMinBufferWords);
	}

	void GX2Shutdown_commandBufferPool()
	{
		if (!g_pool->poolBase)
			return;
		if (g_poolOwned)
			coreinit::_weak_MEMFreeToDefaultHeap(g_pool->poolBase.GetPtr());
		GX2CommandResetToDefaultState();
	}

	void GX2CommandResetToDefaultState()
	{
		g_pool->poolBase = nullptr;
		g_pool->poolWords = 0;
		g_pool->gpuReadPtr = nullptr;
		g_poolOwned = false;
	}

	// ---------------------------------------------------------------------------------
	// Display lists
	// ---------------------------------------------------------------------------------

	static void _BeginDisplayList(MPTR buffer, uint32 sizeInBytes)
	{
		if ((uint32)coreinit::OSGetCoreId() == sGX2MainCoreIndex)
		{
			// the main core's command buffer is parked and continues after GX2EndDisplayList
			_PadTarget();
			cemu_assert_debug(!_CoreTarget().isDisplayList);
			g_savedMainCoreTarget = _CoreTarget();
		}
		_SetTarget(MEMPTR<uint32be>(buffer).GetPtr(), sizeInBytes / 4, true);
	}

	void GX2BeginDisplayList(MEMPTR<void> displayListAddr, uint32 size)
	{
		_BeginDisplayList(displayListAddr.GetMPTR(), size);
	}

	void GX2BeginDisplayListEx(MEMPTR<void> displayListAddr, uint32 size, bool profiling)
	{
		_BeginDisplayList(displayListAddr.GetMPTR(), size);
	}

	uint32 GX2EndDisplayList(MEMPTR<void> displayListAddr)
	{
		cemu_assert_debug(displayListAddr != nullptr);
		auto& target = _CoreTarget();
		_PadTarget();
		const uint32 words = (uint32)(target.currentWritePtr - target.bufferPtr);
		cemu_assert_debug(words <= target.bufferSizeInU32s);
		if ((uint32)coreinit::OSGetCoreId() == sGX2MainCoreIndex)
			target = g_savedMainCoreTarget;
		else
			target = {};
		return words * 4;
	}

	bool GX2GetCurrentDisplayList(MEMPTR<uint32be>* displayListAddr, uint32be* displayListSize)
	{
		auto& target = _CoreTarget();
		if (!target.isDisplayList)
			return false;
		if (displayListAddr)
			*displayListAddr = target.bufferPtr;
		if (displayListSize)
			*displayListSize = target.bufferSizeInU32s * 4;
		return true;
	}

	bool GX2GetDisplayListWriteStatus()
	{
		return _CoreTarget().isDisplayList;
	}

	void GX2CallDisplayList(MPTR addr, uint32 size)
	{
		cemu_assert_debug((size & 3) == 0);
		GX2ReserveCmdSpace(4);
		gx2WriteGather_submit(pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3), memory_virtualToPhysical(addr), 0, size / 4);
	}

	// submits the display list straight to the ring, bypassing the command buffer
	void GX2DirectCallDisplayList(void* addr, uint32 size)
	{
		if ((uint32)coreinit::OSGetCoreId() != sGX2MainCoreIndex)
			cemuLog_logDebugOnce(LogType::Force, "GX2DirectCallDisplayList() used on a core other than the GX2 main core");
		if (!_CoreTarget().isDisplayList)
			GX2Command_Flush(0x100, false); // commands queued before it must run first
		_SubmitIndirectBuffer((uint32be*)addr, size / 4, nullptr, false);
	}

	// copies the display list words into the current command buffer
	void GX2CopyDisplayList(MEMPTR<uint32be*> addr, uint32 size)
	{
		const uint32 words = size / 4;
		if (words == 0)
			return;
		GX2ReserveCmdSpace(words);
		gx2WriteGather_submitU32AsLEArray((uint32*)addr.GetPtr(), words);
	}

	enum class GX2_PATCH_TYPE : uint32
	{
		FETCH_SHADER = 1,
		VERTEX_SHADER = 2,
		GEOMETRY_COPY_SHADER = 3,
		GEOMETRY_SHADER = 4,
		PIXEL_SHADER = 5,
		COMPUTE_SHADER = 6
	};

	// display lists that set a shader store its program address in the third word of the
	// SQ_PGM_START packet; this updates it after the program has been moved
	void GX2PatchDisplayList(uint32be* displayData, GX2_PATCH_TYPE patchType, uint32 patchOffset, void* obj)
	{
		cemu_assert_debug((patchOffset & 3) == 0);
		MPTR program;
		switch (patchType)
		{
		case GX2_PATCH_TYPE::FETCH_SHADER:
			program = ((GX2FetchShader*)obj)->GetProgramAddr();
			break;
		case GX2_PATCH_TYPE::VERTEX_SHADER:
			program = ((GX2VertexShader*)obj)->GetProgramAddr();
			break;
		case GX2_PATCH_TYPE::GEOMETRY_COPY_SHADER:
			program = ((GX2GeometryShader_t*)obj)->GetCopyProgramAddr();
			break;
		case GX2_PATCH_TYPE::GEOMETRY_SHADER:
			program = ((GX2GeometryShader_t*)obj)->GetGeometryProgramAddr();
			break;
		case GX2_PATCH_TYPE::PIXEL_SHADER:
			program = ((GX2PixelShader_t*)obj)->GetProgramAddr();
			break;
		default:
			cemuLog_log(LogType::Force, "GX2PatchDisplayList(): patch type {} is not supported", (uint32)patchType);
			return;
		}
		displayData[patchOffset / 4 + 2] = memory_virtualToPhysical(program) >> 8;
	}

	void GX2CommandInit()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("gx2", fn, LogType::GX2)
		EXPORT_FN(GX2Flush);

		EXPORT_FN(GX2GetLastSubmittedTimeStamp); EXPORT_FN(GX2GetRetiredTimeStamp); EXPORT_FN(GX2WaitTimeStamp);

		EXPORT_FN(GX2BeginDisplayList); EXPORT_FN(GX2BeginDisplayListEx); EXPORT_FN(GX2EndDisplayList);
		EXPORT_FN(GX2GetCurrentDisplayList); EXPORT_FN(GX2GetDisplayListWriteStatus);

		EXPORT_FN(GX2CallDisplayList); EXPORT_FN(GX2DirectCallDisplayList); EXPORT_FN(GX2CopyDisplayList);
		EXPORT_FN(GX2PatchDisplayList);
#undef EXPORT_FN
	}
}
