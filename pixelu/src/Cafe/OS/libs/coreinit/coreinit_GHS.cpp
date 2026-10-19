// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Green Hills runtime support. Titles built with the GHS toolchain call into
// coreinit for stream locking, a global runtime lock, errno storage, the C++
// exception-handling globals and thread-local storage. Most of the per-thread
// state lives in OSThread_t; the stdio stream table lives in a block of guest
// memory whose fields are published as data exports.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_GHS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/RPL/rpl.h"

namespace coreinit
{
	namespace
	{
		constexpr uint32 kMaxStreams = 100; // FOPEN_MAX of the runtime

		// stream mode bits in GHSStream::mode
		constexpr uint32 kStreamModeRead = 0x1;
		constexpr uint32 kStreamModeWrite = 0x2;
		constexpr uint32 StreamChannel(uint32 channel) { return channel << 18; }

		// One entry of the guest _iob table. Only the mode word is touched here.
		struct GHSStream
		{
			uint32be opaque[3];
			uint32be mode;
		};
		static_assert(sizeof(GHSStream) == 0x10);

		// Guest-resident runtime block. _iob, __gh_FOPEN_MAX, environ and errno
		// are exported as data symbols pointing into it.
		struct GHSRuntimeBlock
		{
			GHSStream streams[kMaxStreams];        // _iob
			uint32be streamLock[kMaxStreams];      // index into the lock pool for each stream
			uint16be fopenMax;                     // __gh_FOPEN_MAX
			MEMPTR<void> environPtr;               // environ
			uint32 errnoValue;                     // errno (data export)
		};
		static_assert(offsetof(GHSRuntimeBlock, streamLock) == 0x640);
		static_assert(offsetof(GHSRuntimeBlock, fopenMax) == 0x7D0);
		static_assert(offsetof(GHSRuntimeBlock, environPtr) == 0x7D4);
		static_assert(offsetof(GHSRuntimeBlock, errnoValue) == 0x7D8);
		static_assert(sizeof(GHSRuntimeBlock) == 0x7DC);

		// order of these two allocations is fixed (system area layout)
		SysAllocator<GHSRuntimeBlock> s_runtime;
		SysAllocator<OSMutex, kMaxStreams> s_streamMutexPool;

		bool s_streamMutexInUse[kMaxStreams];
		std::recursive_mutex s_poolGuard;     // protects s_streamMutexInUse
		std::recursive_mutex s_runtimeLock;   // backs __ghsLock / __ghsUnlock

		OSMutex* StreamMutex(uint32 slot)
		{
			return s_streamMutexPool.GetPtr() + slot;
		}

		// guest layout of the argument to __tls_get_addr
		struct GHSTlsIndex
		{
			uint16be unused;
			uint16be moduleIndex;
			uint32be offset;
		};
		static_assert(sizeof(GHSTlsIndex) == 8);

		// one per TLS module in a thread's table
		struct GHSTlsSlot
		{
			uint32be blockAddr;
			uint32be unused;
		};
		static_assert(sizeof(GHSTlsSlot) == 8);

		// OSThread_t keeps the TLS table address and slot count as raw big-endian values
		uint16be& ThreadTlsSlotCount(OSThread_t* thread)
		{
			static_assert(sizeof(thread->numAllocatedTLSBlocks) == sizeof(uint16be));
			return *reinterpret_cast<uint16be*>(&thread->numAllocatedTLSBlocks);
		}

		uint32be& ThreadTlsTable(OSThread_t* thread)
		{
			static_assert(sizeof(thread->tlsBlocksMPTR) == sizeof(uint32be));
			return *reinterpret_cast<uint32be*>(&thread->tlsBlocksMPTR);
		}
	}

	// slot is the guest's lock word for one stream; it receives the pool index
	void __ghs_flock_create(uint32be* slot)
	{
		std::lock_guard guard(s_poolGuard);
		uint32 slotIndex = kMaxStreams;
		for (uint32 i = 0; i < kMaxStreams; i++)
		{
			if (!s_streamMutexInUse[i])
			{
				slotIndex = i;
				break;
			}
		}
		if (slotIndex == kMaxStreams)
		{
			cemuLog_log(LogType::Force, "GHS: stream lock pool exhausted");
			cemu_assert(false);
			return;
		}
		s_streamMutexInUse[slotIndex] = true;
		OSInitMutexEx(StreamMutex(slotIndex), nullptr);
		*slot = slotIndex;
	}

	void __ghs_flock_destroy(uint32 index)
	{
		std::lock_guard guard(s_poolGuard);
		cemu_assert_debug(index > 2); // the three standard streams are never closed
		cemu_assert(index < kMaxStreams);
		if (index >= kMaxStreams)
			return;
		cemu_assert_debug(s_streamMutexInUse[index]);
		s_streamMutexInUse[index] = false;
	}

	// maps an _iob entry to its lock word
	uint32be* __ghs_flock_ptr(GHSStream* stream)
	{
		ptrdiff_t index = stream - s_runtime->streams;
		return s_runtime->streamLock + index;
	}

	void __ghs_flock_file(uint32 index)
	{
		cemu_assert(index < kMaxStreams);
		OSLockMutex(StreamMutex(index));
	}

	void __ghs_funlock_file(uint32 index)
	{
		cemu_assert(index < kMaxStreams);
		OSUnlockMutex(StreamMutex(index));
	}

	void __ghsLock()
	{
		// the lock may be held by a guest thread that is currently descheduled,
		// so yield to the scheduler instead of blocking the host thread
		while (!s_runtimeLock.try_lock())
			PPCCore_switchToScheduler();
	}

	void __ghsUnlock()
	{
		s_runtimeLock.unlock();
	}

	void* __get_eh_init_block()
	{
		return nullptr;
	}

	void* __get_eh_globals()
	{
		return OSGetCurrentThread()->crt.eh_globals.GetPtr();
	}

	void* __get_eh_mem_manage()
	{
		return OSGetCurrentThread()->crt.eh_mem_manage;
	}

	void* __get_eh_store_globals()
	{
		return OSGetCurrentThread()->crt.eh_store_globals;
	}

	void* __get_eh_store_globals_tdeh()
	{
		return OSGetCurrentThread()->crt.eh_store_globals_tdeh;
	}

	sint32be* __gh_errno_ptr()
	{
		return &OSGetCurrentThread()->context.ghs_errno;
	}

	void __gh_set_errno(sint32 errNo)
	{
		*__gh_errno_ptr() = errNo;
	}

	sint32 __gh_get_errno()
	{
		return *__gh_errno_ptr();
	}

	// guest-side handle of a runtime mutex: just a pointer to a heap-allocated OSMutex
	struct GHSMutexHandle
	{
		MEMPTR<OSMutex> mutex;
	};

	void __ghs_mtx_init(GHSMutexHandle* handle)
	{
		handle->mutex = (OSMutex*)_weak_MEMAllocFromDefaultHeapEx(ppcsizeof<OSMutex>(), 8);
		OSInitMutex(handle->mutex.GetPtr());
	}

	void __ghs_mtx_dst(GHSMutexHandle* handle)
	{
		_weak_MEMFreeToDefaultHeap(handle->mutex.GetPtr());
		handle->mutex = nullptr;
	}

	void __ghs_mtx_lock(GHSMutexHandle* handle)
	{
		OSLockMutex(handle->mutex.GetPtr());
	}

	void __ghs_mtx_unlock(GHSMutexHandle* handle)
	{
		OSUnlockMutex(handle->mutex.GetPtr());
	}

	void* __tls_get_addr(GHSTlsIndex* tlsIndex)
	{
		OSThread_t* thread = OSGetCurrentThread();
		const uint16 moduleIndex = tlsIndex->moduleIndex;
		if (moduleIndex == 0)
			assert_dbg(); // index 0 is never handed out

		// The slot table is created on first use, sized for every TLS module index
		// the loader knows about at that moment. It is not resized afterwards.
		uint16be& slotCount = ThreadTlsSlotCount(thread);
		uint32be& table = ThreadTlsTable(thread);
		if (slotCount == 0)
		{
			const uint32 count = RPLLoader_GetMaxTLSModuleIndex() + 1;
			const uint32 bytes = count * (uint32)sizeof(GHSTlsSlot);
			MPTR tableAddr = coreinit_allocFromSysArea(bytes, 4);
			memset(memory_getPointerFromVirtualOffset(tableAddr), 0, bytes);
			table = tableAddr;
			slotCount = (uint16)count;
		}

		auto* slot = (GHSTlsSlot*)memory_getPointerFromVirtualOffsetAllowNull((uint32)table + (uint32)sizeof(GHSTlsSlot) * moduleIndex);
		if (slot->blockAddr == MPTR_NULL)
		{
			// first access from this thread: give it its own copy of the module's TLS image
			uint8* image = nullptr;
			sint32 imageSize = 0;
			bool found = RPLLoader_GetTLSDataByTLSIndex((sint16)moduleIndex, &image, &imageSize);
			cemu_assert(found);
			cemu_assert(imageSize != 0);
			MPTR block = coreinit_allocFromSysArea(imageSize, 32);
			memcpy(memory_getPointerFromVirtualOffset(block), image, imageSize);
			slot->blockAddr = block;
		}
		return memory_getPointerFromVirtualOffset((uint32)slot->blockAddr + (uint32)tlsIndex->offset);
	}

	void PrepareGHSRuntime()
	{
		GHSRuntimeBlock& rt = *s_runtime.GetPtr();
		rt.environPtr = nullptr;
		rt.fopenMax = kMaxStreams;
		rt.errnoValue = 0;

		std::fill(std::begin(s_streamMutexInUse), std::end(s_streamMutexInUse), false);

		// stdin, stdout, stderr. Only stdin and stdout get their direction bit
		// assigned; stderr keeps whatever mode it had and just gains its channel.
		rt.streams[0].mode = kStreamModeRead | StreamChannel(0);
		rt.streams[1].mode = kStreamModeWrite | StreamChannel(1);
		rt.streams[2].mode = rt.streams[2].mode | StreamChannel(2);
		for (uint32 i = 0; i < 3; i++)
			__ghs_flock_create(__ghs_flock_ptr(rt.streams + i));

		osLib_addVirtualPointer("coreinit", "__gh_FOPEN_MAX", memory_getVirtualOffsetFromPointer(&rt.fopenMax));
		osLib_addVirtualPointer("coreinit", "_iob", memory_getVirtualOffsetFromPointer(rt.streams));
		osLib_addVirtualPointer("coreinit", "environ", memory_getVirtualOffsetFromPointer(&rt.environPtr));
		osLib_addVirtualPointer("coreinit", "errno", memory_getVirtualOffsetFromPointer(&rt.errnoValue));
	}

	void InitializeGHS()
	{
		cafeExportRegister("coreinit", __ghs_flock_create, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_flock_destroy, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_flock_ptr, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_flock_file, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_funlock_file, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghsLock, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghsUnlock, LogType::Placeholder);

		cafeExportRegister("coreinit", __get_eh_init_block, LogType::Placeholder);
		cafeExportRegister("coreinit", __get_eh_globals, LogType::Placeholder);
		cafeExportRegister("coreinit", __get_eh_mem_manage, LogType::Placeholder);
		cafeExportRegister("coreinit", __gh_errno_ptr, LogType::Placeholder);
		cafeExportRegister("coreinit", __gh_set_errno, LogType::Placeholder);
		cafeExportRegister("coreinit", __gh_get_errno, LogType::Placeholder);
		cafeExportRegister("coreinit", __get_eh_store_globals, LogType::Placeholder);
		cafeExportRegister("coreinit", __get_eh_store_globals_tdeh, LogType::Placeholder);

		cafeExportRegister("coreinit", __ghs_mtx_init, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_mtx_dst, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_mtx_lock, LogType::Placeholder);
		cafeExportRegister("coreinit", __ghs_mtx_unlock, LogType::Placeholder);

		cafeExportRegister("coreinit", __tls_get_addr, LogType::Placeholder);
	}
};
