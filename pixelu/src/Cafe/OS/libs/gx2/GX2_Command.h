// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Espresso/Const.h"

// GX2 command building: every core has a "write target" (either a piece of the shared
// command pool or a display list supplied by the game) that GX2 functions append PM4
// words to. Full command buffers are handed to TCL as indirect buffers.

namespace GX2
{
	struct GX2PerCoreCBState
	{
		uint32be* bufferPtr; // start of the current write target
		uint32 bufferSizeInU32s; // capacity of the write target
		uint32be* currentWritePtr; // where the next word goes (null: no target)
		bool isDisplayList; // true while the target is a game display list
	};

	extern GX2PerCoreCBState s_perCoreCBState[Espresso::CORE_COUNT];
};

void gx2WriteGather_submitU32AsBE(uint32 v);
void gx2WriteGather_submitU32AsLE(uint32 v);
void gx2WriteGather_submitU32AsLEArray(uint32* v, uint32 numValues);

uint32 PPCInterpreter_getCurrentCoreIndex();

namespace GX2
{
	// converts one argument of gx2WriteGather_submit() into the command word that is stored
	// (in big endian) in the command buffer
	template<typename T>
	inline uint32be _CmdWord(const T& v)
	{
		if constexpr (std::is_base_of_v<Latte::LATTEREG, T>)
		{
			return v.getRawValue();
		}
		else if constexpr (std::is_floating_point_v<T>)
		{
			static_assert(sizeof(T) == sizeof(uint32));
			uint32 bits;
			memcpy(&bits, &v, sizeof(bits));
			return bits;
		}
		else
		{
			return (uint32)v;
		}
	}

	// big endian values (betype<...>) are already in command buffer byte order
	template<typename T>
	inline uint32be _CmdWord(const betype<T>& v)
	{
		static_assert(sizeof(betype<T>) == sizeof(uint32be));
		uint32be w;
		memcpy(&w, &v, sizeof(w));
		return w;
	}
}

// appends all arguments to the current core's write target, one 32-bit word each
template <typename ...Targs>
inline void gx2WriteGather_submit(Targs... args)
{
	GX2::GX2PerCoreCBState& target = GX2::s_perCoreCBState[PPCInterpreter_getCurrentCoreIndex()];
	if (target.currentWritePtr == nullptr)
	{
		cemu_assert_suspicious(); // nothing to write to
		return;
	}
	uint32be* p = target.currentWritePtr;
	((*p++ = GX2::_CmdWord(args)), ...);
	target.currentWritePtr = p;
	cemu_assert_debug(target.currentWritePtr <= target.bufferPtr + target.bufferSizeInU32s);
}

namespace GX2
{
	void GX2Command_Flush(uint32 numU32sForNextBuffer, bool triggerMarkerInterrupt = true);
	void GX2ReserveCmdSpace(uint32 reservedFreeSpaceInU32);

	uint64 GX2GetLastSubmittedTimeStamp();
	uint64 GX2GetRetiredTimeStamp();
	bool GX2WaitTimeStamp(uint64 tsWait);

	void GX2BeginDisplayList(MEMPTR<void> displayListAddr, uint32 size);
	void GX2BeginDisplayListEx(MEMPTR<void> displayListAddr, uint32 size, bool profiling);
	uint32 GX2EndDisplayList(MEMPTR<void> displayListAddr);

	void GX2CallDisplayList(MPTR addr, uint32 size);
	void GX2DirectCallDisplayList(void* addr, uint32 size);

	bool GX2GetDisplayListWriteStatus();

	void GX2CommandInit();
	void GX2Init_commandBufferPool(void* bufferBase, uint32 bufferSize);
	void GX2Shutdown_commandBufferPool();
	void GX2CommandResetToDefaultState();
}
