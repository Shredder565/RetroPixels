// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"

namespace coreinit
{
	// describes one contiguous range of a block heap, free or used
	struct MEMBlockHeapTrack
	{
		/* +0x00 */ uint32be addrStart;
		/* +0x04 */ uint32be addrEnd;        // last byte (inclusive)
		/* +0x08 */ uint32be isFree;         // 0 = in use
		/* +0x0C */ MEMPTR<MEMBlockHeapTrack> previousBlock;
		/* +0x10 */ MEMPTR<MEMBlockHeapTrack> nextBlock; // for spare tracks: next spare track
	};
	static_assert(sizeof(MEMBlockHeapTrack) == 0x14);

	// header of a chunk of track memory handed to MEMAddBlockHeapTracking
	struct MEMBlockHeapTrackGroup
	{
		/* +0x00 */ uint32be _ukn00;
		/* +0x04 */ uint32be _ukn04;
		/* +0x08 */ MEMPTR<MEMBlockHeapTrack> tracks;
		/* +0x0C */ uint32be trackCount;
	};
	static_assert(sizeof(MEMBlockHeapTrackGroup) == 0x10);

	// block heap: the managed range is described by a list of tracks kept outside of it
	struct MEMBlockHeap : MEMHeapBase
	{
		/* +0x34 */ uint8 _pad34[0x50 - 0x34];
		/* +0x50 */ MEMBlockHeapTrack track;    // built-in track for the initial range
		/* +0x64 */ MEMPTR<MEMBlockHeapTrack> headBlock;
		/* +0x68 */ MEMPTR<MEMBlockHeapTrack> tailBlock;
		/* +0x6C */ MEMPTR<MEMBlockHeapTrack> nextFreeBlock; // first spare track
		/* +0x70 */ uint32be freeBlocksLeft;    // number of spare tracks
		/* +0x74 */ uint8 _pad74[0x80 - 0x74];
	};
	static_assert(offsetof(MEMBlockHeap, track) == 0x50);
	static_assert(offsetof(MEMBlockHeap, freeBlocksLeft) == 0x70);
	static_assert(sizeof(MEMBlockHeap) == 0x80);

	MEMHeapHandle MEMInitBlockHeap(MEMBlockHeap* memStart, void* startAddr, void* endAddr, void* initTrackMem, uint32 initTrackMemSize, uint32 createFlags);
	void* MEMDestroyBlockHeap(MEMHeapHandle hHeap);

	sint32 MEMAddBlockHeapTracking(MEMBlockHeap* heap, void* trackMem, uint32 trackMemSize);

	void InitializeMEMBlockHeap();
}
