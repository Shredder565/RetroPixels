// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include <atomic>
#include "coreinit_Atomic.h"

namespace coreinit
{
	namespace
	{
		// applies op to the stored value atomically and returns the value it replaced
		template<typename T, typename TOp>
		T AtomicUpdate(std::atomic<betype<T>>* mem, TOp op)
		{
			betype<T> expected = mem->load();
			betype<T> desired;
			do
			{
				desired = (T)op((T)expected);
			} while (!mem->compare_exchange_strong(expected, desired));
			return expected;
		}

		// compare and swap; reports the value found in memory through foundValue
		template<typename T>
		bool CompareAndSwap(std::atomic<betype<T>>* mem, T compareValue, T swapValue, betype<T>* foundValue)
		{
			betype<T> expected = compareValue;
			bool swapped = mem->compare_exchange_strong(expected, betype<T>(swapValue));
			if (foundValue)
				*foundValue = expected;
			return swapped;
		}
	}

	uint32 OSSwapAtomic(std::atomic<uint32be>* mem, uint32 newValue)
	{
		return mem->exchange(uint32be(newValue));
	}

	bool OSCompareAndSwapAtomic(std::atomic<uint32be>* mem, uint32 compareValue, uint32 swapValue)
	{
		return CompareAndSwap<uint32>(mem, compareValue, swapValue, nullptr);
	}

	bool OSCompareAndSwapAtomicEx(std::atomic<uint32be>* mem, uint32 compareValue, uint32 swapValue, uint32be* previousValue)
	{
		uint32be found;
		bool swapped = CompareAndSwap<uint32>(mem, compareValue, swapValue, &found);
		*previousValue = found;
		return swapped;
	}

	uint32 OSAddAtomic(std::atomic<uint32be>* mem, uint32 adder)
	{
		return AtomicUpdate<uint32>(mem, [adder](uint32 v) { return v + adder; });
	}

	uint64 OSSwapAtomic64(std::atomic<uint64be>* mem, uint64 newValue)
	{
		return mem->exchange(uint64be(newValue));
	}

	uint64 OSSetAtomic64(std::atomic<uint64be>* mem, uint64 newValue)
	{
		return OSSwapAtomic64(mem, newValue);
	}

	uint64 OSGetAtomic64(std::atomic<uint64be>* mem)
	{
		return mem->load();
	}

	uint64 OSAddAtomic64(std::atomic<uint64be>* mem, uint64 adder)
	{
		return AtomicUpdate<uint64>(mem, [adder](uint64 v) { return v + adder; });
	}

	uint64 OSAndAtomic64(std::atomic<uint64be>* mem, uint64 val)
	{
		return AtomicUpdate<uint64>(mem, [val](uint64 v) { return v & val; });
	}

	uint64 OSOrAtomic64(std::atomic<uint64be>* mem, uint64 val)
	{
		return AtomicUpdate<uint64>(mem, [val](uint64 v) { return v | val; });
	}

	bool OSCompareAndSwapAtomic64(std::atomic<uint64be>* mem, uint64 compareValue, uint64 swapValue)
	{
		return CompareAndSwap<uint64>(mem, compareValue, swapValue, nullptr);
	}

	bool OSCompareAndSwapAtomicEx64(std::atomic<uint64be>* mem, uint64 compareValue, uint64 swapValue, uint64be* previousValue)
	{
		uint64be found;
		bool swapped = CompareAndSwap<uint64>(mem, compareValue, swapValue, &found);
		*previousValue = found;
		return swapped;
	}

	void InitializeAtomic()
	{
		// one HLE index per export, handed out in this order
#define EXPORT_FN(fn) cafeExportRegister("coreinit", fn, LogType::Placeholder)
		EXPORT_FN(OSSwapAtomic); EXPORT_FN(OSCompareAndSwapAtomic); EXPORT_FN(OSCompareAndSwapAtomicEx);
		EXPORT_FN(OSAddAtomic);

		EXPORT_FN(OSSetAtomic64); EXPORT_FN(OSGetAtomic64); EXPORT_FN(OSSwapAtomic64);
		EXPORT_FN(OSAddAtomic64); EXPORT_FN(OSAndAtomic64); EXPORT_FN(OSOrAtomic64);
		EXPORT_FN(OSCompareAndSwapAtomic64); EXPORT_FN(OSCompareAndSwapAtomicEx64);
#undef EXPORT_FN
	}
}
