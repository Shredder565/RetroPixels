// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// dmae drives the GPU's DMA engine. Games use it to copy or fill memory in
// 32-bit words (optionally byte-swapping on the way) and to bump semaphores,
// without spending CPU time. Every operation returns a 64-bit timestamp that
// can later be passed to DMAEWaitDone().
//
// Here every operation runs immediately on the calling thread, so it is
// already complete when the call returns: the returned timestamp is "now",
// it becomes the retired timestamp straight away, and waiting always
// succeeds.
//
// Copies tell the GPU buffer cache about the written range, the same way a
// CPU data-cache flush would, so vertex/uniform data uploaded by DMA is seen
// by the renderer.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "dmae.h"

namespace dmae
{
	namespace
	{
		// byte-swap mode of DMAECopyMem
		enum class SwapMode : uint32
		{
			None = 0,
			Swap16 = 1,
			Swap32 = 2,
			Swap64 = 3,
		};

		// action of DMAESemaphore
		enum class SemaphoreAction : uint32
		{
			Wait = 0,
			Signal = 1,
		};

		std::atomic<uint64> g_lastRetired{0};

		uint64 CompleteNow()
		{
			const uint64 now = coreinit::OSGetSystemTime();
			g_lastRetired.store(now);
			return now;
		}

		uint64 CopyMem(MPTR dst, MPTR src, uint32 wordCount, SwapMode mode)
		{
			uint8* to = memory_getPointerFromVirtualOffset(dst);
			const uint8* from = memory_getPointerFromVirtualOffset(src);
			const size_t bytes = (size_t)wordCount * 4;
			switch (mode)
			{
			case SwapMode::None:
				memmove(to, from, bytes);
				break;
			case SwapMode::Swap16:
				for (size_t off = 0; off < bytes; off += 2)
				{
					uint16 v;
					memcpy(&v, from + off, 2);
					v = _swapEndianU16(v);
					memcpy(to + off, &v, 2);
				}
				break;
			case SwapMode::Swap32:
				for (size_t off = 0; off < bytes; off += 4)
				{
					uint32 v;
					memcpy(&v, from + off, 4);
					v = _swapEndianU32(v);
					memcpy(to + off, &v, 4);
				}
				break;
			case SwapMode::Swap64:
				// counts are in 32-bit words; a trailing odd word cannot be
				// swapped as a pair and is copied unchanged
				for (size_t off = 0; off + 8 <= bytes; off += 8)
				{
					uint64 v;
					memcpy(&v, from + off, 8);
					v = _swapEndianU64(v);
					memcpy(to + off, &v, 8);
				}
				if (bytes % 8)
					memcpy(to + bytes - 4, from + bytes - 4, 4);
				break;
			default:
				cemuLog_logDebug(LogType::Force, "DMAECopyMem: unknown swap mode {}", (uint32)mode);
				break;
			}
			const uint64 ts = CompleteNow();
			if (wordCount != 0)
				LatteBufferCache_notifyDCFlush(dst, wordCount * 4);
			return ts;
		}

		uint64 FillMem(MPTR dst, uint32 value, uint32 wordCount)
		{
			uint32be* to = reinterpret_cast<uint32be*>(memory_getPointerFromVirtualOffset(dst));
			// the fill pattern is the 32-bit value as the guest sees it
			std::fill_n(to, wordCount, uint32be(value));
			return CompleteNow();
		}

		uint32 WaitDone(uint64 timestamp)
		{
			return 1; // work is always finished already
		}

		uint64 Semaphore(MPTR semaphore, SemaphoreAction action)
		{
			// The counter is kept in host byte order, as PixelU always has;
			// only DMA engine calls touch it.
			auto* counter = _rawPtrToAtomic(reinterpret_cast<uint64le*>(memory_getPointerFromVirtualOffset(semaphore)));
			switch (action)
			{
			case SemaphoreAction::Signal:
				counter->fetch_add(1);
				break;
			case SemaphoreAction::Wait:
				// a real engine would stall until the count is non-zero;
				// we take it without waiting
				counter->fetch_sub(1);
				break;
			default:
				cemuLog_logDebug(LogType::Force, "DMAESemaphore: unknown action {}", (uint32)action);
				break;
			}
			return CompleteNow();
		}

		uint64 GetRetiredTimeStamp()
		{
			return g_lastRetired.load();
		}

		class DmaeLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "dmae"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(CopyMem, "dmae", "DMAECopyMem", LogType::Placeholder);
				cafeExportRegisterFunc(FillMem, "dmae", "DMAEFillMem", LogType::Placeholder);
				cafeExportRegisterFunc(WaitDone, "dmae", "DMAEWaitDone", LogType::Placeholder);
				cafeExportRegisterFunc(Semaphore, "dmae", "DMAESemaphore", LogType::Placeholder);
				cafeExportRegisterFunc(GetRetiredTimeStamp, "dmae", "DMAEGetRetiredTimeStamp", LogType::Placeholder);
			}
		};

		DmaeLibrary g_dmaeLibrary;
	}

	COSModule* GetModule()
	{
		return &g_dmaeLibrary;
	}
}
