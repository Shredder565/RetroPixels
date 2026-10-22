// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"

// Code generation area for titles with their own JIT (e.g. browsers and emulators). The area is
// reserved on first request. To notice rewritten code we keep a shadow copy of the area that
// stands in for the instruction cache: when a cache line is invalidated and its bytes differ from
// the shadow, the recompiler drops what it built from that line.
namespace coreinit
{
	namespace
	{
		constexpr uint32 kCodegenAreaSize = 0x01000000; // fixed; the console takes this from cos.xml
		constexpr uint32 kCacheLine = 0x20;

		struct CodegenArea
		{
			bool reserved = false;
			MPTR start = 0;
			uint32 size = 0;
			uint8* icacheShadow = nullptr; // never freed, the recompiler may look at it until exit
		};
		CodegenArea s_codegen;

		bool s_avoidRecompilingArea = false;

		// invalidates every cache line from the one containing start up to and including the line
		// at the rounded up end address
		void InvalidateLines(uint32 start, uint32 size)
		{
			uint32 line = start & ~(kCacheLine - 1);
			uint32 last = (start + size + kCacheLine - 1) & ~(kCacheLine - 1);
			for (; line <= last; line += kCacheLine)
				codeGenHandleICBI(line);
		}

		// OSCodegenCopy(dest, src, size)
		void Export_OSCodegenCopy(PPCInterpreter_t* hCPU)
		{
			uint32 dest = hCPU->gpr[3];
			uint32 src = hCPU->gpr[4];
			uint32 size = hCPU->gpr[5];
			if (!s_codegen.reserved)
				assert_dbg();
			uint32 areaEnd = s_codegen.start + s_codegen.size;
			if (dest < s_codegen.start || dest >= areaEnd)
				assert_dbg();
			if (dest + size < s_codegen.start || dest + size > areaEnd)
				assert_dbg();
			memcpy(memory_getPointerFromVirtualOffset(dest), memory_getPointerFromVirtualOffset(src), size);
			InvalidateLines(dest, size);
			osLib_returnFromFunction(hCPU, 0);
		}
	}

	void OSGetCodegenVirtAddrRange(betype<uint32>* rangeStart, betype<uint32>* rangeSize)
	{
		if (!s_codegen.reserved)
		{
			s_codegen.start = RPLLoader_AllocateCodeSpace(kCodegenAreaSize, 0x1000);
			s_codegen.size = kCodegenAreaSize;
			s_codegen.icacheShadow = new uint8[kCodegenAreaSize](); // zero filled
			s_codegen.reserved = true;
		}
		*rangeStart = s_codegen.start;
		*rangeSize = s_codegen.size;
	}

	void OSGetCodegenVirtAddrRangeInternal(uint32& rangeStart, uint32& rangeSize)
	{
		rangeStart = s_codegen.reserved ? s_codegen.start : 0;
		rangeSize = s_codegen.reserved ? s_codegen.size : 0;
	}

	void ICInvalidateRange(uint32 startAddress, uint32 size)
	{
		InvalidateLines(startAddress, size);
	}

	void codeGenHandleICBI(uint32 ea)
	{
		cemu_assert_debug((ea & (kCacheLine - 1)) == 0);
		if (!s_codegen.reserved)
			return;
		if (ea < s_codegen.start || ea >= s_codegen.start + s_codegen.size)
			return;
		uint8* shadow = s_codegen.icacheShadow + (ea - s_codegen.start);
		uint8* memory = memory_getPointerFromVirtualOffset(ea);
		if (memcmp(memory, shadow, kCacheLine) == 0)
			return;
		PPCRecompiler_invalidateRange(ea, ea + kCacheLine);
		memcpy(shadow, memory, kCacheLine);
	}

	bool codeGenShouldAvoid()
	{
		return s_avoidRecompilingArea;
	}

	// Switching the area between executable and writable is a sign the title patches its code in
	// place. Writes to the area are not tracked, so from then on the area is only interpreted.
	bool OSSwitchSecCodeGenMode(bool isRXOnly)
	{
		if (!s_avoidRecompilingArea)
			cemuLog_log(LogType::Force, "Code generation area is written in place, it will no longer be recompiled");
		s_avoidRecompilingArea = true;
		return true;
	}

	void InitializeCodeGen()
	{
		cafeExportRegister("coreinit", OSGetCodegenVirtAddrRange, LogType::Placeholder);
		cafeExportRegister("coreinit", ICInvalidateRange, LogType::Placeholder);
		osLib_addFunction("coreinit", "OSCodegenCopy", Export_OSCodegenCopy);
		cafeExportRegister("coreinit", OSSwitchSecCodeGenMode, LogType::Placeholder);
	}
}
