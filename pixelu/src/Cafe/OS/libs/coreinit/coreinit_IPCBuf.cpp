// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "coreinit_IPCBuf.h"

// IPC buffer pools: a fixed number of equally sized blocks carved out of one area. Free blocks
// sit in a ring buffer of pointers, so blocks are handed out in the order they were returned.

namespace coreinit
{
	namespace
	{
		constexpr uint32 kPoolMagic = 0x0BADF00D;
		constexpr uint32 kBlockGranularity = 0x40;
		constexpr uint32 kRingEmpty = 0xFFFFFFFF;

		constexpr sint32 kErrRingFull = -8;
		constexpr sint32 kErrRingEmpty = -7;
		constexpr sint32 kErrBadPool = -4;

		void RingReset(IPCBufPoolFreeRing& ring, uint32 capacity, MPTR slotArray)
		{
			ring.capacity = capacity;
			ring.slots = MEMPTR<MEMPTR<uint8>>(slotArray);
			ring.putPos = 0;
			ring.takePos = kRingEmpty;
			ring.used = 0;
		}

		bool RingIsEmpty(const IPCBufPoolFreeRing& ring)
		{
			return (sint32)(uint32)ring.takePos < 0;
		}

		sint32 RingPut(IPCBufPoolFreeRing& ring, MPTR block)
		{
			uint32 put = ring.putPos;
			if (ring.takePos == put)
			{
				cemuLog_log(LogType::APIErrors, "IPCBufPool: free ring overflow");
				return kErrRingFull;
			}
			ring.slots.GetPtr()[put] = MEMPTR<uint8>(block);
			if (RingIsEmpty(ring))
				ring.takePos = put;
			ring.used = ring.used + 1;
			ring.putPos = (put + 1) % (uint32)ring.capacity;
			return 0;
		}

		sint32 RingTake(IPCBufPoolFreeRing& ring, MPTR& block)
		{
			block = MPTR_NULL;
			if (RingIsEmpty(ring))
				return kErrRingEmpty;
			uint32 take = ring.takePos;
			ring.used = ring.used - 1;
			block = ring.slots.GetPtr()[take].GetMPTR();
			if (ring.used == 0)
				ring.takePos = kRingEmpty;
			else
				ring.takePos = (take + 1) % (uint32)ring.capacity;
			return 0;
		}

		bool IsValidPool(const IPCBufPool_t* pool)
		{
			return pool->magic == kPoolMagic;
		}
	}

	IPCBufPool_t* IPCBufPoolCreate(uint8* bufferArea, uint32 bufferSize, uint32 entrySize, uint32be* entryCountOutput, uint32 uknR7)
	{
		memset(bufferArea, 0, bufferSize);
		const MPTR areaAddr = memory_getVirtualOffsetFromPointer(bufferArea);
		const MPTR areaEnd = areaAddr + bufferSize;
		const MPTR poolAddr = (areaAddr + 3) & ~3u;
		const MPTR slotsAddr = (poolAddr + (uint32)sizeof(IPCBufPool_t) + 3) & ~3u;
		IPCBufPool_t* pool = MEMPTR<IPCBufPool_t>(poolAddr).GetPtr();

		*entryCountOutput = 0;
		OSInitMutexEx(&pool->mutex, nullptr);
		pool->area = bufferArea;
		pool->areaSize = bufferSize;
		pool->createFlags = uknR7;
		pool->reserved10 = 0;
		pool->magic = kPoolMagic;

		const uint32 blockSize = (entrySize + (kBlockGranularity - 1)) & ~(kBlockGranularity - 1);
		pool->blockSize = blockSize;
		pool->blockStride = blockSize;

		// every block needs one ring slot; the slot array takes the place of one block
		uint32 fit = blockSize ? (uint32)(areaEnd - slotsAddr) / blockSize : 0;
		cemu_assert_debug(fit > 1);
		pool->slotBytes = fit * 4;
		if (fit * 4 >= blockSize)
		{
			// the slot array would not fit in the space of one block: the pool stays empty
			cemuLog_log(LogType::APIErrors, "IPCBufPoolCreate: unsupported geometry (size 0x{:x}, block 0x{:x})", bufferSize, blockSize);
		}
		else
		{
			const uint32 blockCount = fit - 1;
			pool->blockCount = blockCount;
			*entryCountOutput = blockCount;
			pool->firstBlock = MEMPTR<uint8>(slotsAddr + fit * 4);
			RingReset(pool->freeRing, blockCount, slotsAddr);
		}
		const MPTR firstBlock = pool->firstBlock.GetMPTR();
		for (sint32 i = 0; i < (sint32)(uint32)pool->blockCount; i++)
		{
			if (RingPut(pool->freeRing, firstBlock + (uint32)i * blockSize) != 0)
				return nullptr;
		}
		return pool;
	}

	uint8* IPCBufPoolAllocate(IPCBufPool_t* ipcBufPool, uint32 size)
	{
		MPTR block = MPTR_NULL;
		OSLockMutex(&ipcBufPool->mutex);
		if (IsValidPool(ipcBufPool) && size <= ipcBufPool->blockSize)
			RingTake(ipcBufPool->freeRing, block);
		else
			cemuLog_log(LogType::APIErrors, "IPCBufPoolAllocate: bad pool or size 0x{:x} too large", size);
		OSUnlockMutex(&ipcBufPool->mutex);
		return MEMPTR<uint8>(block).GetPtr();
	}

	sint32 IPCBufPoolFree(IPCBufPool_t* ipcBufPool, uint8* entry)
	{
		sint32 result = 0;
		OSLockMutex(&ipcBufPool->mutex);
		if (IsValidPool(ipcBufPool))
		{
			MPTR block = memory_getVirtualOffsetFromPointer(entry);
			uint32 offset = block - ipcBufPool->firstBlock.GetMPTR();
			uint32 stride = ipcBufPool->blockStride;
			if (stride == 0 || offset % stride != 0 || offset / stride >= ipcBufPool->blockCount)
				cemuLog_log(LogType::APIErrors, "IPCBufPoolFree: 0x{:08x} is not a block of this pool", block);
			RingPut(ipcBufPool->freeRing, block);
		}
		else
		{
			cemuLog_log(LogType::APIErrors, "IPCBufPoolFree: bad pool");
			result = kErrBadPool;
		}
		OSUnlockMutex(&ipcBufPool->mutex);
		return result;
	}

	namespace
	{
		void Export_IPCBufPoolCreate(PPCInterpreter_t* hCPU)
		{
			uint8* area = MEMPTR<uint8>(hCPU->gpr[3]).GetPtr();
			uint32be* countOut = MEMPTR<uint32be>(hCPU->gpr[6]).GetPtr();
			IPCBufPool_t* pool = IPCBufPoolCreate(area, hCPU->gpr[4], hCPU->gpr[5], countOut, hCPU->gpr[7]);
			osLib_returnFromFunction(hCPU, memory_getVirtualOffsetFromPointer(pool));
		}

		void Export_IPCBufPoolAllocate(PPCInterpreter_t* hCPU)
		{
			IPCBufPool_t* pool = MEMPTR<IPCBufPool_t>(hCPU->gpr[3]).GetPtr();
			uint8* block = IPCBufPoolAllocate(pool, hCPU->gpr[4]);
			osLib_returnFromFunction(hCPU, memory_getVirtualOffsetFromPointer(block));
		}

		void Export_IPCBufPoolFree(PPCInterpreter_t* hCPU)
		{
			IPCBufPool_t* pool = MEMPTR<IPCBufPool_t>(hCPU->gpr[3]).GetPtr();
			uint8* block = MEMPTR<uint8>(hCPU->gpr[4]).GetPtr();
			osLib_returnFromFunction(hCPU, (uint32)IPCBufPoolFree(pool, block));
		}
	}

	void InitializeIPCBuf()
	{
		osLib_addFunction("coreinit", "IPCBufPoolCreate", Export_IPCBufPoolCreate);
		osLib_addFunction("coreinit", "IPCBufPoolAllocate", Export_IPCBufPoolAllocate);
		osLib_addFunction("coreinit", "IPCBufPoolFree", Export_IPCBufPoolFree);
	}
}
