// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/coreinit/coreinit_FG.h"

// The foreground bucket is a 64MB memory region owned by whichever process is in the foreground.
// It is split into fixed areas: a large free area for the title, buffers the system uses while
// switching titles, a small save area that survives title switches and the copy data area that
// carries arguments from one title to the next. The emulated title is always in the foreground.
namespace coreinit
{
	namespace
	{
		enum BucketArea : uint32
		{
			AREA_FREE = 0,		  // usable by the title
			AREA_TRANSITION = 1,  // audio during title transitions
			AREA_2 = 2,			  // presumably TV/DRC frame storage (2-5)
			AREA_3 = 3,
			AREA_4 = 4,
			AREA_5 = 5,
			AREA_SAVE = 6,
			AREA_COPY = 7,		  // copy data (clipboard, arguments for the next title)
		};

		constexpr uint32 kFreeAreaSize = 0x2800000;
		constexpr uint32 kSaveAreaSize = 0x1000;
		constexpr uint32 kCopyAreaSize = 0x400000;
		constexpr uint32 kSaveAreaMagic = 0x53617665; // 'Save'

		struct AreaLayout
		{
			BucketArea area;
			uint32 offset;
			uint32 size;
		};

		// in address order
		constexpr AreaLayout kAreas[] = {
			{ AREA_FREE, 0x0000000, kFreeAreaSize },
			{ AREA_COPY, 0x2800000, kCopyAreaSize },
			{ AREA_TRANSITION, 0x2C00000, 0x900000 },
			{ AREA_2, 0x3500000, 0x3C0000 },
			{ AREA_3, 0x38C0000, 0x1C0000 },
			{ AREA_4, 0x3A80000, 0x3C0000 },
			{ AREA_5, 0x3E40000, 0x1BF000 },
			{ AREA_SAVE, 0x3FFF000, kSaveAreaSize },
		};

		MEMPTR<void> s_bucketBase = nullptr; // null while not in the foreground

		uint8* AreaPtr(BucketArea area)
		{
			if (!s_bucketBase)
				return nullptr;
			for (const AreaLayout& entry : kAreas)
			{
				if (entry.area == area)
					return s_bucketBase.GetPtr<uint8>() + entry.offset;
			}
			return nullptr;
		}

		// the copy data area starts with the byte count, followed by the data
		uint32be* CopyDataLength()
		{
			return (uint32be*)AreaPtr(AREA_COPY);
		}

		// OSGetForegroundBucket(MEMPTR<void>* base, uint32be* size), either may be null
		void Export_OSGetForegroundBucket(PPCInterpreter_t* hCPU)
		{
			ppcDefineParamMPTR(baseOut, 0);
			ppcDefineParamMPTR(sizeOut, 1);
			bool inForeground = OSGetForegroundBucket((MEMPTR<void>*)memory_getPointerFromVirtualOffsetAllowNull(baseOut), (uint32be*)memory_getPointerFromVirtualOffsetAllowNull(sizeOut));
			osLib_returnFromFunction(hCPU, inForeground ? 1 : 0);
		}

		// The clipboard is not implemented, it always reads as empty
		bool OSCopyFromClipboard(void* data, uint32be* size)
		{
			if (*size == 0 && !data)
				return false;
			if (!OSGetForegroundBucket(nullptr, nullptr))
				return false;
			*size = 0;
			return true;
		}

		void Export_OSCopyFromClipboard(PPCInterpreter_t* hCPU)
		{
			cemuLog_logDebug(LogType::Force, "OSCopyFromClipboard({:#x}, {:#x})", hCPU->gpr[3], hCPU->gpr[4]);
			ppcDefineParamMEMPTR(data, void, 0);
			ppcDefineParamMEMPTR(size, uint32be, 1);
			osLib_returnFromFunction(hCPU, OSCopyFromClipboard(data.GetPtr(), size.GetPtr()) ? 1 : 0);
		}
	}

	bool OSGetForegroundBucket(MEMPTR<void>* offset, uint32be* size)
	{
		if (offset)
			*offset = { (MPTR)MEMORY_FGBUCKET_AREA_ADDR };
		if (size)
			*size = MEMORY_FGBUCKET_AREA_SIZE;
		return true; // always in the foreground
	}

	bool OSGetForegroundBucketFreeArea(MEMPTR<void>* offset, uint32be* size)
	{
		*offset = AreaPtr(AREA_FREE);
		*size = kFreeAreaSize;
		return s_bucketBase != nullptr;
	}

	void InitForegroundBucket()
	{
		uint32be bucketSize;
		OSGetForegroundBucket(&s_bucketBase, &bucketSize);
		memset(AreaPtr(AREA_FREE), 0, kFreeAreaSize);
		// the save and copy areas survive title switches; set them up only on first use
		uint8* saveArea = AreaPtr(AREA_SAVE);
		if (*(uint32be*)saveArea != kSaveAreaMagic)
		{
			memset(saveArea, 0, kSaveAreaSize);
			memset(AreaPtr(AREA_COPY), 0, kCopyAreaSize);
			*(uint32be*)(saveArea + 0x00) = kSaveAreaMagic;
			*(uint32be*)(saveArea + 0x08) = 0x300;
		}
	}

	void __OSClearCopyData()
	{
		if (uint32be* length = CopyDataLength())
			*length = 0;
	}

	uint32 __OSGetCopyDataSize()
	{
		uint32be* length = CopyDataLength();
		return length ? (uint32)*length : 0;
	}

	uint8* __OSGetCopyDataPtr()
	{
		uint8* area = AreaPtr(AREA_COPY);
		return area ? area + 4 : nullptr;
	}

	bool __OSAppendCopyData(uint8* data, sint32 length)
	{
		uint32be* stored = CopyDataLength();
		if (!stored)
			return false;
		uint32 used = *stored;
		if (used + (uint32)length > kCopyAreaSize - 4)
			return false;
		memcpy((uint8*)stored + 4 + used, data, length);
		*stored = used + length;
		return true;
	}

	// copy data can only shrink
	bool __OSResizeCopyData(sint32 length)
	{
		uint32be* stored = CopyDataLength();
		if (!stored)
			return false;
		if (length >= (sint32)(uint32)*stored)
			return false;
		*stored = length;
		return true;
	}

	void InitializeFG()
	{
		osLib_addFunction("coreinit", "OSGetForegroundBucket", Export_OSGetForegroundBucket);
		cafeExportRegister("coreinit", OSGetForegroundBucket, LogType::CoreinitMem);
		cafeExportRegister("coreinit", OSGetForegroundBucketFreeArea, LogType::CoreinitMem);
		osLib_addFunction("coreinit", "OSCopyFromClipboard", Export_OSCopyFromClipboard);
	}
}
