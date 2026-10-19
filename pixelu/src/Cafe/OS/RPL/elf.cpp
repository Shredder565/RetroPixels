// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Loader for plain big endian PowerPC ELF executables (homebrew launched the
// way the Homebrew Launcher does it). Sections are copied to their linked
// addresses; there is no relocation or import processing.
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"

namespace
{
	struct Elf32Header
	{
		uint8 ident[16];
		uint16be type;
		uint16be machine;
		uint32be version;
		uint32be entry;
		uint32be phoff;
		uint32be shoff;
		uint32be flags;
		uint16be ehsize;
		uint16be phentsize;
		uint16be phnum;
		uint16be shentsize;
		uint16be shnum;
		uint16be shstrndx;
	};
	static_assert(sizeof(Elf32Header) == 0x34);

	struct Elf32Section
	{
		uint32be name;
		uint32be type;
		uint32be flags;
		uint32be addr;
		uint32be offset;
		uint32be size;
		uint32be link;
		uint32be info;
		uint32be addralign;
		uint32be entsize;
	};
	static_assert(sizeof(Elf32Section) == 0x28);

	// sections with this flag bit get recompiler coverage
	constexpr uint32 kCodeRangeFlag = 1;
}

uint32 ELF_LoadFromMemory(uint8* elfData, sint32 size, const char* name)
{
	const Elf32Header* hdr = (const Elf32Header*)elfData;
	const Elf32Section* sections = (const Elf32Section*)(elfData + (uint32)hdr->shoff);
	const uint32 fileSize = (uint32)size;
	memory_enableHBLELFCodeArea();
	for (uint32 i = 0; i < (uint32)hdr->shnum; i++)
	{
		const Elf32Section& s = sections[i];
		const uint32 addr = s.addr;
		const uint32 len = s.size;
		const uint32 offset = s.offset;
		if (offset > fileSize)
		{
			cemuLog_log(LogType::Force, "ELF section {} out of bounds", i);
			continue;
		}
		if (addr != 0)
		{
			uint8* dst = memory_getPointerFromVirtualOffset(addr);
			if ((uint32)s.type == SHT_NOBITS)
				memset(dst, 0, len);
			else
				memcpy(dst, elfData + offset, std::min(len, fileSize - offset));
		}
		if (((uint32)s.flags & kCodeRangeFlag) != 0)
			PPCRecompiler_allocateRange(addr, len);
	}
	return hdr->entry;
}
