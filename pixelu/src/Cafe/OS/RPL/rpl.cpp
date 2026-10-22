// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// RPL/RPX loader.
//
// Loading a module happens in two steps:
//  1. LoadFromMemory() validates the file, computes the patch checksum and
//     maps every section. Allocated sections go to one of three regions
//     (text, data, loader info) at the same offsets they have relative to
//     the lowest address of their region in the file. Text lives in the code
//     area, data and loader info come from the data allocator (a bump
//     allocator at 0x10000000 during boot, the OSDynLoad allocator once the
//     application owns its memory). Everything else (relocations, ...) is
//     copied as one block into the loader work area.
//  2. Link() resolves symbols and applies relocations. Pass 0 rebases the
//     module's own symbols and applies every relocation that does not refer
//     to an import; pass 2 (after all new modules went through pass 0, so
//     export tables are final) resolves imports against other modules'
//     export tables or the C++ libraries and applies all relocations again.
//
// Memory placement, allocation order and relocation results are relied on
// by game patches and graphic packs, so they must stay stable.
#include <zlib.h>

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/RPL/rpl_symbol_storage.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/Espresso/Debugger/Debugger.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "util/ChunkedHeap/ChunkedHeap.h"
#include "util/crypto/crc32.h"
#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "COSModule.h"

namespace
{
	// heap for guest code: freeing memory drops recompiled code of that range
	class GuestCodeHeap : public VHeap
	{
	public:
		GuestCodeHeap(void* base, uint32 size) : VHeap(base, size) {}

		void* alloc(uint32 size, uint32 alignment = 4) override
		{
			return VHeap::alloc(size, alignment);
		}

		void free(void* addr) override
		{
			const uint32 size = getAllocationSizeFromAddr(addr);
			const MPTR start = memory_getVirtualOffsetFromPointer(addr);
			PPCRecompiler_invalidateRange(start, start + size);
			VHeap::free(addr);
		}
	};

	VHeap s_workArea(nullptr, MEMORY_RPLLOADER_AREA_SIZE);
	GuestCodeHeap s_trampolineHeap(nullptr, MEMORY_CODE_TRAMPOLINE_AREA_SIZE);
	GuestCodeHeap s_codeHeap(nullptr, MEMORY_CODEAREA_SIZE);
	ChunkedFlatAllocator<64 * 1024> s_sharedTrampolines; // HLE import stubs and callables

	std::vector<RPLDependency*> s_dependencies;
	std::vector<RPLModule*> s_modules;
	RPLModule* s_mainModule = nullptr;

	bool s_appOwnsMemory = false;
	uint32 s_highestCodeAddr = 0;
	uint32 s_dataCursor = 0x10000000;
	MPTR s_sdaBase = MPTR_NULL; // r13
	MPTR s_sda2Base = MPTR_NULL; // r2
	// handles and TLS indices are never reused within a session
	uint32 s_nextModuleHandle = 0x1000;
	sint16 s_nextTlsIndex = 1;

	std::unordered_map<std::string, MPTR> s_hleImportStubs; // key: library name as imported + symbol name
	std::map<void(*)(PPCInterpreter_t*), MPTR> s_callables;

	constexpr uint32 kOpHle = 1u << 26; // HLE trap opcode, low bits select the handler
	constexpr uint32 kHleUnsupportedImport = 0xFFD0;
	constexpr uint32 kOpBlr = 0x4E800020;
	constexpr uint32 kOpMtctrR11 = 0x7D6903A6;
	constexpr uint32 kOpBctr = 0x4E800420;
	constexpr uint32 kMaxFileField = 500 * 1024 * 1024;
	constexpr uint32 kMaxSectionSize = 1024 * 1024 * 1024;

	enum class LinkPass
	{
		Local = 0, // own symbols, relocations not referring to imports
		Imports = 2, // resolve imports, apply all relocations
	};

	// Elf32_Sym
	struct RPLSymbol
	{
		uint32be name;
		uint32be value;
		uint32be size;
		uint8 info;
		uint8 other;
		uint16be shndx;
	};
	static_assert(sizeof(RPLSymbol) == 0x10);

	constexpr uint8 kSymTypeTLS = 6;
	constexpr uint8 kSymGlobalFunc = 0x12;

	// what an import section of the module being linked refers to
	struct ImportTarget
	{
		RPLModule* module; // null: not an import section, HLE_MODULE_PTR: C++ library
		std::string libName; // normalised module name, or the raw imported name for C++ libraries
	};

	inline uint32 AlignUp(uint32 v, uint32 a)
	{
		return (v + a - 1) & ~(a - 1);
	}

	inline MPTR GuestAddr(const void* p)
	{
		return memory_getVirtualOffsetFromPointer((void*)p);
	}

	template<typename F>
	inline void ForEachSection(RPLModule* rpl, F&& f)
	{
		for (size_t i = 0; i < rpl->sectionTable.size(); i++)
			f((uint32)i, rpl->sectionTable[i]);
	}
}

static void RPLLoader_UnloadModule(RPLDependency* dependency, bool skipPPCCalls);
static void RPLLoader_RemoveDependency(std::string_view name);

/* ---------------- guest memory ---------------- */

static uint8* AllocateTrampoline(RPLModule* owner, sint32 size)
{
	if (owner)
		return (uint8*)owner->heapTrampolineArea.alloc(size, 4);
	uint8* mem = (uint8*)s_sharedTrampolines.alloc(size, 4);
	const uint32 usedEnd = GuestAddr(s_sharedTrampolines.getCurrentBlockPtr()) + s_sharedTrampolines.getCurrentBlockOffset();
	s_highestCodeAddr = std::max(s_highestCodeAddr, usedEnd);
	return mem;
}

uint8* RPLLoader_AllocateTrampolineCodeSpace(sint32 size)
{
	return AllocateTrampoline(nullptr, size);
}

MPTR RPLLoader_AllocateCodeSpace(uint32 size, uint32 alignment)
{
	cemu_assert_debug((alignment & (alignment - 1)) == 0);
	const MPTR addr = GuestAddr(s_codeHeap.alloc(size, alignment));
	s_highestCodeAddr = std::max(s_highestCodeAddr, addr + size);
	PPCRecompiler_allocateRange(addr, size);
	return addr;
}

static uint32 AllocateModuleData(RPLModule* rpl, uint32 size, uint32 alignment)
{
	if (s_appOwnsMemory)
	{
		StackAllocator<uint32be> result;
		*result.GetPointer() = 0;
		PPCCoreCallback(rpl->funcAlloc.value(), size, alignment, result.GetPointer());
		return (uint32)*result.GetPointer();
	}
	s_dataCursor = AlignUp(s_dataCursor, alignment);
	const uint32 addr = s_dataCursor;
	s_dataCursor += size;
	return addr;
}

static void FreeModuleData(RPLModule* rpl, void* ptr)
{
	PPCCoreCallback(rpl->funcFree.value(), ptr);
}

uint32 RPLLoader_GetDataAllocatorAddr()
{
	return AlignUp(s_dataCursor, 0x1000);
}

uint32 RPLLoader_GetMaxCodeOffset()
{
	return s_highestCodeAddr;
}

static void* AllocateWorkArea(uint32 size, uint32 alignment, uint32* allocatedSize)
{
	size = AlignUp(size, 32);
	*allocatedSize = size;
	void* mem = s_workArea.alloc(size, alignment);
	cemu_assert(mem != nullptr);
	memset(mem, 0, size);
	return mem;
}

/* ---------------- generated guest code ---------------- */

static inline uint32 PPC_DForm(uint32 opcd, uint32 rd, uint32 ra, uint32 imm)
{
	return (opcd << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF);
}

// 32-bit absolute jump for branches beyond the +-32MB reach of 'b'. Clobbers r11 and CTR
static MPTR GetFarJump(RPLModule* rpl, MPTR target)
{
	if (auto it = rpl->trampolineMap.find(target); it != rpl->trampolineMap.end())
		return it->second;
	const MPTR code = GuestAddr(AllocateTrampoline(rpl, 16));
	const uint32 lo = target & 0xFFFF;
	const uint32 ha = ((target >> 16) + ((target >> 15) & 1)) & 0xFFFF;
	memory_writeU32(code + 0x0, PPC_DForm(14, 11, 0, lo)); // li r11, lo
	memory_writeU32(code + 0x4, PPC_DForm(15, 11, 11, ha)); // addis r11, r11, ha
	memory_writeU32(code + 0x8, kOpMtctrR11);
	memory_writeU32(code + 0xC, kOpBctr);
	rplSymbolStorage_createJumpProxySymbol(code, target);
	rpl->trampolineMap.emplace(target, code);
	return code;
}

uint32 RPLLoader_MakePPCCallable(void(*ppcCallableExport)(PPCInterpreter_t* hCPU))
{
	if (auto it = s_callables.find(ppcCallableExport); it != s_callables.end())
		return it->second;
	const sint32 hle = PPCInterpreter_registerHLECall(ppcCallableExport, fmt::format("PPCCallback{:x}", (uintptr_t)ppcCallableExport));
	const MPTR code = GuestAddr(RPLLoader_AllocateTrampolineCodeSpace(4));
	memory_write<uint32>(code, kOpHle | hle);
	s_callables[ppcCallableExport] = code;
	return code;
}

// Address of a stub for a C++ library function. Unknown functions get a stub
// that reports the missing import (when functionMustExist is set).
uint32 rpl_mapHLEImport(RPLModule* rplLoaderContext, const char* rplName, const char* funcName, bool functionMustExist)
{
	std::string key = std::string(rplName) + funcName;
	if (auto it = s_hleImportStubs.find(key); it != s_hleImportStubs.end())
		return it->second;
	std::string lib = rplName;
	if (size_t dot = lib.find('.'); dot != std::string::npos)
		lib.resize(dot);
	const sint32 hle = osLib_getFunctionIndex(lib.c_str(), funcName);
	MPTR stub;
	if (hle >= 0)
	{
		stub = GuestAddr(RPLLoader_AllocateTrampolineCodeSpace(4));
		memory_write<uint32>(stub, kOpHle | hle);
	}
	else
	{
		if (!functionMustExist)
			return MPTR_NULL;
		// trap + blr, followed by "lib.function" for the handler to print
		stub = GuestAddr(RPLLoader_AllocateTrampolineCodeSpace(256));
		memory_write<uint32>(stub + 0, kOpHle | kHleUnsupportedImport);
		memory_write<uint32>(stub + 4, kOpBlr);
		const size_t libLen = std::min<size_t>(128, lib.size());
		const size_t fnLen = std::min<size_t>(128, strlen(funcName));
		uint8* text = memory_getPointerFromVirtualOffset(stub + 8);
		memcpy(text, lib.data(), libLen);
		text[libLen] = '.';
		memcpy(text + libLen + 1, funcName, fnLen);
		text[libLen + 1 + fnLen] = '\0';
	}
	s_hleImportStubs.emplace(std::move(key), stub);
	rplSymbolStorage_store(lib.c_str(), funcName, stub);
	return stub;
}

/* ---------------- file parsing ---------------- */

static bool InFile(RPLModule* rpl, uint32 offset, uint32 size)
{
	if (offset > kMaxFileField || size > kMaxFileField)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Failed a bounds check. Offset = {:0x} Size = {:0x}", offset, size);
		return false;
	}
	return offset + size <= rpl->RPLRawData.size_bytes();
}

// module name from a path: file name without extension, lower case, at most 63 characters
static std::string ModuleNameFromPath(std::string_view path)
{
	cemu_assert(!path.empty());
	size_t begin = 0;
	for (size_t i = path.size() - 1; i > 0; i--)
	{
		if (path[i] == '/')
		{
			begin = i + 1;
			break;
		}
	}
	size_t end = begin;
	while (end < path.size() && path[end] != '.')
		end++;
	cemu_assert(end != begin);
	std::string name(path.substr(begin, std::min<size_t>(end - begin, RPL_MODULE_NAME_LENGTH - 1)));
	for (char& c : name)
		c = _ansiToLower(c);
	return name;
}

static RPLModule* ParseModule(std::string_view moduleName, uint8* data, uint32 size)
{
	if (size < sizeof(RPLFileHeader))
		return nullptr;
	const RPLFileHeader* hdr = (const RPLFileHeader*)data;
	const bool identOk = hdr->version04 == 1 && hdr->ukn05 == 2 && hdr->ukn06 <= 1 && hdr->magic2_0 == 0xCA && hdr->magic2_1 == 0xFE;
	if (!identOk || hdr->ukn12 != 0x14 || hdr->ukn14 != 1)
		return nullptr;
	const uint32 numSections = hdr->sectionTableEntryCount;
	if (numSections < 2)
		return nullptr; // CRCS and FILEINFO are always the last two sections
	if (hdr->sectionTableEntrySize != sizeof(RPLFileSectionEntry))
	{
		cemuLog_log(LogType::Force, "RPLLoader: Unsupported section header size {}", (uint32)hdr->sectionTableEntrySize);
		return nullptr;
	}

	std::unique_ptr<RPLModule> rpl(new RPLModule());
	rpl->RPLRawData = std::span<uint8>(data, size);
	rpl->heapTrampolineArea.setBaseAllocator(&s_trampolineHeap);

	const uint32 tableSize = numSections * sizeof(RPLFileSectionEntry);
	if (!InFile(rpl.get(), hdr->sectionTableOffset, tableSize))
	{
		cemuLog_logDebug(LogType::Force, "RPLLoader: Section table out of bounds");
		return nullptr;
	}
	rpl->sectionTable.resize(numSections);
	memcpy(rpl->sectionTable.data(), data + (uint32)hdr->sectionTableOffset, tableSize);
	memcpy(&rpl->rplHeader, hdr, sizeof(RPLFileHeader));

	RPLFileSectionEntry& fileInfoSec = rpl->sectionTable[numSections - 1];
	RPLFileSectionEntry& crcSec = rpl->sectionTable[numSections - 2];
	if (fileInfoSec.fileOffset == 0 || fileInfoSec.fileOffset >= size || (uint32)fileInfoSec.type != SHT_RPL_FILEINFO)
	{
		cemuLog_logDebug(LogType::Force, "RPLLoader: Last section not FILEINFO");
		return nullptr;
	}
	if (crcSec.fileOffset == 0 || crcSec.fileOffset >= size || (uint32)crcSec.type != SHT_RPL_CRCS)
		cemuLog_logDebug(LogType::Force, "RPLLoader: The section before FILEINFO must be CRCs");

	// FILEINFO
	if (fileInfoSec.sectionSize < sizeof(RPLFileInfoData))
	{
		cemuLog_log(LogType::Force, "RPLLoader: FILEINFO section size is below expected size");
		return nullptr;
	}
	if ((uint64)fileInfoSec.fileOffset + (uint64)fileInfoSec.sectionSize > (uint64)size)
	{
		cemuLog_log(LogType::Force, "RPLLoader: FILEINFO section outside of RPL file bounds");
		return nullptr;
	}
	const uint8* fileInfoRaw = data + (uint32)fileInfoSec.fileOffset;
	rpl->sectionData_fileInfo.assign(fileInfoRaw, fileInfoRaw + (uint32)fileInfoSec.sectionSize);
	const RPLFileInfoData* fi = (const RPLFileInfoData*)rpl->sectionData_fileInfo.data();
	if (fi->fileInfoMagic != 0xCAFE0402)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Invalid FILEINFO magic");
		return nullptr;
	}
	rpl->fileInfo.textRegionSize = fi->textRegionSize;
	rpl->fileInfo.dataRegionSize = fi->dataRegionSize;
	rpl->fileInfo.baseAlign = fi->baseAlign;
	rpl->fileInfo.ukn14 = fi->ukn14;
	rpl->fileInfo.trampolineAdjustment = fi->trampolineAdjustment;
	rpl->fileInfo.ukn4C = fi->ukn4C;
	rpl->fileInfo.tlsModuleIndex = fi->tlsModuleIndex;
	rpl->fileInfo.sdataBase1 = fi->sdataBase1;
	rpl->fileInfo.sdataBase2 = fi->sdataBase2;
	rpl->fileInfo.flags = fi->flags;

	rpl->sectionAddressTable.resize(numSections);
	rpl->moduleName.assign(moduleName);

	const std::string fileName = rpl->moduleName + (rpl->IsRPX() ? ".rpx" : ".rpl");
	rpl->ppcName = coreinit_allocFromSysArea((uint32)fileName.size() + 1, 4);
	memcpy(rpl->ppcName.GetPtr(), fileName.c_str(), fileName.size() + 1);

	// CRC table: one CRC32 per section
	const uint32 expectedCrcBytes = numSections * sizeof(uint32be);
	if (!InFile(rpl.get(), crcSec.fileOffset, expectedCrcBytes))
	{
		cemuLog_log(LogType::Force, "RPLLoader: CRC section outside of RPL file bounds");
		crcSec.sectionSize = 0;
	}
	else if (crcSec.sectionSize < expectedCrcBytes)
		cemuLog_log(LogType::Force, "RPLLoader: CRC section size (0x{:x}) less than required (0x{:x})", (uint32)crcSec.sectionSize, expectedCrcBytes);
	else if (crcSec.sectionSize != expectedCrcBytes)
		cemuLog_log(LogType::Force, "RPLLoader: CRC section size (0x{:x}) does not match expected size (0x{:x})", (uint32)crcSec.sectionSize, expectedCrcBytes);
	uint32 crcCount = (uint32)crcSec.sectionSize / sizeof(uint32be);
	if (crcCount != numSections)
	{
		cemuLog_log(LogType::Force, "RPL stores a different amount of section CRCs than it has sections");
		crcCount = std::min(crcCount, numSections);
	}
	rpl->crcTable.assign(numSections, 0);
	const uint32be* crcs = (const uint32be*)(data + (uint32)crcSec.fileOffset);
	for (uint32 i = 0; i < crcCount; i++)
		rpl->crcTable[i] = crcs[i];

	const uint32 fileInfoCrc = crc32_calc(0, rpl->sectionData_fileInfo.data(), rpl->sectionData_fileInfo.size());
	if (fileInfoCrc != rpl->GetSectionCRC(numSections - 1))
		cemuLog_log(LogType::Force, "RPLLoader: FILEINFO section has CRC mismatch - Calculated: {:08x} Actual: {:08x}", fileInfoCrc, rpl->GetSectionCRC(numSections - 1));

	rpl->sectionAddressTable[numSections - 1].ptr = rpl->sectionData_fileInfo.data();
	rpl->sectionAddressTable[numSections - 2].ptr = nullptr;
	return rpl.release();
}

// inflate a zlib stream that must fill 'out' exactly
static bool Inflate(const uint8* in, uint32 inSize, uint8* out, uint32 outSize, bool* initFailed = nullptr)
{
	z_stream zs{};
	if (inflateInit(&zs) != Z_OK)
	{
		if (initFailed)
			*initFailed = true;
		return false;
	}
	zs.next_in = (Bytef*)in;
	zs.avail_in = inSize;
	zs.next_out = out;
	zs.avail_out = outSize;
	const int r = inflate(&zs, Z_FULL_FLUSH);
	inflateEnd(&zs);
	return (r == Z_OK || r == Z_STREAM_END) && zs.avail_in == 0 && zs.avail_out == 0;
}

// the contents of a section as stored in the file, inflated if needed
static std::optional<std::vector<uint8>> ReadSectionData(RPLModule* rpl, uint32 index)
{
	if (index >= rpl->sectionTable.size())
	{
		cemuLog_log(LogType::Force, "RPLLoader: Section index out of bounds");
		rpl->hasError = true;
		return std::nullopt;
	}
	const RPLFileSectionEntry& sec = rpl->sectionTable[index];
	const uint32 offset = sec.fileOffset;
	const uint32 size = sec.sectionSize;
	if ((uint32)sec.type == SHT_NOBITS)
		return std::vector<uint8>(size, 0);
	if (!InFile(rpl, offset, size))
	{
		cemuLog_log(LogType::Force, "RPLLoader: Raw data for section {} exceeds bounds of RPL file", index);
		rpl->hasError = true;
		return std::nullopt;
	}
	const uint8* raw = rpl->RPLRawData.data() + offset;
	if (((uint32)sec.flags & SHF_RPL_COMPRESSED) == 0)
		return std::vector<uint8>(raw, raw + size);
	if (!InFile(rpl, offset, sizeof(uint32be)) || size < sizeof(uint32be))
	{
		cemuLog_log(LogType::Force, "RPLLoader: Compressed section {} has no size field", index);
		rpl->hasError = true;
		return std::nullopt;
	}
	const uint32 inflatedSize = *(const uint32be*)raw;
	if (inflatedSize >= kMaxSectionSize)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Uncompressed data of section {} is too large", index);
		rpl->hasError = true;
		return std::nullopt;
	}
	std::vector<uint8> out(inflatedSize);
	bool initFailed = false;
	if (!Inflate(raw + 4, size - 4, out.data(), inflatedSize, &initFailed))
	{
		if (initFailed)
			return std::vector<uint8>(); // zlib unavailable: section stays empty
		cemuLog_log(LogType::Force, "RPLLoader: Error while inflating data for section {}", index);
		rpl->hasError = true;
		return std::nullopt;
	}
	return out;
}

/* ---------------- section placement ---------------- */

namespace
{
	enum RegionKind
	{
		REGION_DATA = 0,
		REGION_LOADERINFO = 1,
		REGION_TEXT = 2,
		REGION_TEMP = 3, // file offsets of the non-allocated sections
		REGION_COUNT = 4,
	};

	struct MapRegion
	{
		uint32 begin = 0xFFFFFFFF;
		uint32 end = 0;
	};

	constexpr uint32 kSecWrite = 1;
	constexpr uint32 kSecAlloc = 2;
	constexpr uint32 kSecExec = 4;
}

// copy one section to its place in a mapped region and record its address
static bool MapSection(RPLModule* rpl, uint32 index, const MapRegion& region, MPTR regionBase)
{
	if (index >= rpl->sectionTable.size())
	{
		cemuLog_log(LogType::Force, "RPLLoader: Section index out of bounds");
		rpl->hasError = true;
		return false;
	}
	RPLFileSectionEntry& sec = rpl->sectionTable[index];
	const uint32 vaddr = sec.virtualAddress;
	const uint32 offsetInRegion = vaddr - region.begin;
	if (offsetInRegion >= 0x10000000)
		cemuLog_logDebug(LogType::Force, "Suspicious section mapping offset: 0x{:08x}", offsetInRegion);
	const MPTR dest = regionBase + offsetInRegion;
	rpl->sectionAddressTable[index].ptr = memory_getPointerFromVirtualOffset(dest);
	auto contents = ReadSectionData(rpl, index);
	if (!contents)
	{
		rpl->hasError = true;
		return false;
	}
	const uint32 len = (uint32)contents->size();
	if (vaddr < region.begin || vaddr + len > region.end)
		cemuLog_log(LogType::Force, "RPLLoader: Section {} (0x{:08x} to 0x{:08x}) is not fully contained in it's bounding region (0x{:08x} to 0x{:08x})", index, vaddr, vaddr + len, region.begin, region.end);
	if (len)
		memcpy(memory_getPointerFromVirtualOffset(dest), contents->data(), len);
	if (len < sec.sectionSize)
		cemuLog_log(LogType::Force, "RPLLoader: Section {} uncompresses to {} bytes but sectionSize is {}", index, len, (uint32)sec.sectionSize);
	sec.sectionSize = len; // from now on the size is the inflated size
	return true;
}

static void PlaceSections(RPLModule* rpl)
{
	MapRegion regions[REGION_COUNT];
	// bounds of each region in the file
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		const uint32 type = sec.type;
		const uint32 flags = sec.flags;
		const uint32 vaddr = sec.virtualAddress;
		if ((uint32)sec.sectionSize == 0 || type == SHT_RPL_CRCS || type == SHT_RPL_FILEINFO)
			return;
		if ((flags & kSecAlloc) == 0)
		{
			MapRegion& t = regions[REGION_TEMP];
			t.begin = std::min(t.begin, (uint32)sec.fileOffset);
			t.end = std::max(t.end, (uint32)sec.fileOffset + (uint32)sec.sectionSize);
			return;
		}
		// import sections count towards the loader info region even when executable
		RegionKind kind;
		if ((flags & kSecExec) && type != SHT_RPL_EXPORTS && type != SHT_RPL_IMPORTS)
			kind = REGION_TEXT;
		else if (flags & kSecWrite)
			kind = REGION_DATA;
		else
			kind = REGION_LOADERINFO;
		regions[kind].begin = std::min(regions[kind].begin, vaddr);
	});
	for (MapRegion& r : regions)
	{
		if (r.begin == 0xFFFFFFFF)
			r.begin = 0;
	}
	// region sizes come from FILEINFO
	regions[REGION_TEXT].end = regions[REGION_TEXT].begin + rpl->fileInfo.textRegionSize - rpl->fileInfo.trampolineAdjustment;
	regions[REGION_DATA].end = regions[REGION_DATA].begin + rpl->fileInfo.dataRegionSize;
	regions[REGION_LOADERINFO].end = regions[REGION_LOADERINFO].begin + rpl->fileInfo.ukn14 - rpl->fileInfo.ukn4C;
	const uint32 dataSize = regions[REGION_DATA].end - regions[REGION_DATA].begin;
	const uint32 infoSize = regions[REGION_LOADERINFO].end - regions[REGION_LOADERINFO].begin;
	const uint32 textSize = regions[REGION_TEXT].end - regions[REGION_TEXT].begin;

	rpl->regionMappingBase_data = AllocateModuleData(rpl, dataSize, 0x1000);
	rpl->regionMappingBase_loaderInfo = AllocateModuleData(rpl, infoSize, 0x1000);
	// one extra page after the text
	rpl->regionMappingBase_text = s_codeHeap.alloc(textSize + 0x1000, 0x1000);
	s_highestCodeAddr = std::max(s_highestCodeAddr, rpl->regionMappingBase_text.GetMPTR() + textSize + 0x1000);
	PPCRecompiler_allocateRange(rpl->regionMappingBase_text.GetMPTR(), textSize + 0x1000);

	// Donkey Kong Country: Tropical Freeze treats a default heap above 0x3E200000
	// bytes as devkit memory; using 12MB more keeps it below that
	if (rpl->moduleName == "rs10_production")
		AllocateModuleData(rpl, 12 * 1024 * 1024, 0x1000);

	rpl->regionSize_data = dataSize;
	rpl->regionSize_loaderInfo = infoSize;
	rpl->regionSize_text = textSize;
	rpl->regionOrigAddr_text = regions[REGION_TEXT].begin;
	rpl->regionOrigAddr_data = regions[REGION_DATA].begin;

	auto unmapped = [&](uint32 i, RPLFileSectionEntry& sec) {
		return (uint32)sec.sectionSize != 0 && rpl->sectionAddressTable[i].ptr == nullptr && ((uint32)sec.flags & kSecAlloc) != 0;
	};
	// writable sections
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if (unmapped(i, sec) && ((uint32)sec.flags & kSecWrite))
			MapSection(rpl, i, regions[REGION_DATA], rpl->regionMappingBase_data);
	});
	// read-only sections, imports and exports
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		const uint32 type = sec.type;
		const bool isLinkSection = type == SHT_RPL_EXPORTS || type == SHT_RPL_IMPORTS;
		if (!unmapped(i, sec) || (!isLinkSection && ((uint32)sec.flags & (kSecWrite | kSecExec)) != 0))
			return;
		MapSection(rpl, i, regions[REGION_LOADERINFO], rpl->regionMappingBase_loaderInfo);
		if (type == SHT_RPL_EXPORTS)
		{
			// u32 count, u32 signature, entries
			uint8* p = (uint8*)rpl->sectionAddressTable[i].ptr;
			if ((uint32)sec.flags & kSecExec)
			{
				rpl->exportFCount = *(uint32be*)p;
				rpl->exportFDataPtr = (RPLFileExportTableEntry*)(p + 8);
			}
			else
			{
				rpl->exportDCount = *(uint32be*)p;
				rpl->exportDDataPtr = (RPLFileExportTableEntry*)(p + 8);
			}
		}
	});
	// code, placed after the trampoline space at the start of the text region
	const MPTR textBase = rpl->regionMappingBase_text.GetMPTR() + rpl->fileInfo.trampolineAdjustment;
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if (!unmapped(i, sec) || ((uint32)sec.flags & kSecExec) == 0 || (uint32)sec.type == SHT_RPL_EXPORTS)
			return;
		if ((uint32)sec.type == SHT_NOBITS)
			cemuLog_log(LogType::Force, "RPLLoader: Unsupported text section type SHT_NOBITS");
		MapSection(rpl, i, regions[REGION_TEXT], textBase);
	});
	// everything else stays a copy of the file bytes in the work area
	const MapRegion& temp = regions[REGION_TEMP];
	const uint32 tempSize = temp.end - temp.begin;
	uint32 tempAllocSize = 0;
	uint8* tempMem = (uint8*)AllocateWorkArea(tempSize, 0x20, &tempAllocSize);
	rpl->tempRegionPtr = tempMem;
	rpl->tempRegionAllocSize = tempAllocSize;
	memcpy(tempMem, rpl->RPLRawData.data() + temp.begin, tempSize);
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		const uint32 type = sec.type;
		if ((uint32)sec.sectionSize == 0 || rpl->sectionAddressTable[i].ptr != nullptr || type == SHT_RPL_FILEINFO || type == SHT_RPL_CRCS)
			return;
		cemu_assert_debug((uint32)sec.fileOffset >= temp.begin && (uint32)sec.fileOffset + (uint32)sec.sectionSize <= temp.end);
		rpl->sectionAddressTable[i].ptr = tempMem + ((uint32)sec.fileOffset - temp.begin);
		// symbol and string tables are read in place, so a compressed one is inflated here
		// (relocations are inflated where they are applied)
		if (((uint32)sec.flags & SHF_RPL_COMPRESSED) && (type == SHT_SYMTAB || type == SHT_STRTAB))
		{
			auto contents = ReadSectionData(rpl, i);
			if (!contents)
				return;
			sec.sectionSize = (uint32)contents->size();
			rpl->inflatedTempSections.emplace_back(std::move(*contents));
			rpl->sectionAddressTable[i].ptr = rpl->inflatedTempSections.back().data();
		}
	});
}

/* ---------------- patch checksum ---------------- */

// Checksum over the section headers and inflated contents, compatible with the
// module hashes used by Cemuhook-style patches.txt files and graphic packs
static uint32 ComputePatchCRC(RPLModule* rpl)
{
	sint32 symtabIndex = -1;
	sint32 strtabIndex = -1;
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if ((uint32)sec.type == SHT_SYMTAB)
			symtabIndex = (sint32)i;
		if ((uint32)sec.type == SHT_STRTAB && i != (uint32)rpl->rplHeader.nameSectionIndex && strtabIndex == -1)
			strtabIndex = (sint32)i;
	});
	uint32 crc = 0;
	auto feed = [&crc](const void* p, size_t n) { crc = crc32_calc(crc, p, n); };
	auto feedU32 = [&feed](uint32 v) { feed(&v, sizeof(v)); }; // host (little endian) byte order
	static const uint8 kMagic[4] = { 0x7F, 'R', 'P', 'X' };
	feed(kMagic, sizeof(kMagic));
	feedU32((uint32)rpl->sectionTable.size());
	feedU32((uint32)symtabIndex);
	feedU32((uint32)strtabIndex);
	feedU32((uint32)(sint32)rpl->rplHeader.nameSectionIndex);

	const std::span<uint8> file = rpl->RPLRawData;
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		const uint32 type = sec.type;
		const uint32 flags = sec.flags;
		const uint32 offset = sec.fileOffset;
		const uint32 storedSize = sec.sectionSize;
		std::vector<uint8> inflated;
		const uint8* contents = nullptr;
		uint32 contentSize = 0;
		if (type == SHT_NOBITS)
			contentSize = storedSize;
		else if (flags & SHF_RPL_COMPRESSED)
		{
			if ((uint64)offset + 4 > file.size())
				return;
			contentSize = *(const uint32be*)(file.data() + offset);
			if (contentSize >= kMaxSectionSize || storedSize < 4 || (uint64)offset + storedSize > file.size())
			{
				cemuLog_logDebug(LogType::Force, "RPLLoader-CRC: Cannot load section {}", i);
				return;
			}
			inflated.resize(contentSize);
			if (!Inflate(file.data() + offset + 4, storedSize - 4, inflated.data(), contentSize))
			{
				cemuLog_logDebug(LogType::Force, "RPLLoader-CRC: Unable to decompress section {}", i);
				return;
			}
			contents = inflated.data();
		}
		else
		{
			contentSize = storedSize;
			contents = file.data() + offset;
		}
		feedU32(sec.nameOffset);
		feedU32(type);
		feedU32(flags);
		feedU32(sec.virtualAddress);
		feedU32(contentSize);
		feedU32(sec.alignment);
		if (contents && contentSize > 0)
			feed(contents, contentSize);
	});
	return crc;
}

/* ---------------- module loading ---------------- */

static void ForEachImportedLibrary(RPLModule* rpl, void(*fn)(std::string_view))
{
	// the library name follows the 8-byte header of each import section
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if ((uint32)sec.type == SHT_RPL_IMPORTS)
			fn((const char*)rpl->sectionAddressTable[i].ptr + 8);
	});
}

RPLModule* RPLLoader_LoadFromMemory(uint8* rplData, sint32 size, std::string_view name)
{
	const std::string moduleName = ModuleNameFromPath(name);
	RPLModule* rpl = ParseModule(moduleName, rplData, (uint32)size);
	if (!rpl)
		return nullptr;
	if (s_appOwnsMemory)
		coreinit::OSDynLoad_GetAllocator(&rpl->funcAlloc, &rpl->funcFree);
	else
	{
		rpl->funcAlloc = 0;
		rpl->funcFree = 0;
	}
	rpl->patchCRC = ComputePatchCRC(rpl);
	PlaceSections(rpl);
	cemuLog_logDebug(LogType::Force, "Load {} Code-Offset: -0x{:x}", name, rpl->regionMappingBase_text.GetMPTR() - 0x02000000);

	// small data anchors (r13/r2). The last module whose non-code section contains them wins.
	// Both at 0x8000 means the module does not use small data (Wii Party U needs this).
	const uint32 sda = rpl->fileInfo.sdataBase1;
	const uint32 sda2 = rpl->fileInfo.sdataBase2;
	if (!(sda == 0x8000 && sda2 == 0x8000))
	{
		ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
			const uint32 start = sec.virtualAddress;
			const uint32 end = start + (uint32)sec.sectionSize;
			if ((uint32)sec.sectionSize == 0 || ((uint32)sec.flags & kSecExec) != 0)
				return;
			const MPTR mapped = GuestAddr(rpl->sectionAddressTable[i].ptr);
			if (sda - 0x8000 >= start && sda - 0x8000 <= end)
				s_sdaBase = mapped + (sda - start);
			if (sda2 - 0x8000 >= start && sda2 - 0x8000 <= end)
				s_sda2Base = mapped + (sda2 - start);
		});
	}
	if (rpl->hasError)
	{
		cemuLog_log(LogType::Force, "RPLLoader: Unable to load RPL due to errors");
		delete rpl;
		return nullptr;
	}
	// TLS image bounds (file addresses)
	uint32 tlsBegin = 0xFFFFFFFF, tlsEnd = 0;
	ForEachSection(rpl, [&](uint32, RPLFileSectionEntry& sec) {
		if (((uint32)sec.flags & SHF_TLS) == 0)
			return;
		tlsBegin = std::min(tlsBegin, (uint32)sec.virtualAddress);
		tlsEnd = std::max(tlsEnd, (uint32)sec.virtualAddress + (uint32)sec.sectionSize);
	});
	if (tlsBegin == 0xFFFFFFFF)
		tlsBegin = tlsEnd = 0;
	rpl->tlsStartAddress = tlsBegin;
	rpl->tlsEndAddress = tlsEnd;

	s_modules.push_back(rpl);
	ForEachImportedLibrary(rpl, [](std::string_view lib) { RPLLoader_AddDependency(lib); });

	// entry point in mapped memory
	const uint32 fileEntry = rpl->rplHeader.entrypoint;
	uint32 entry = 0xFFFFFFFF;
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		const uint32 start = sec.virtualAddress;
		if (fileEntry >= start && fileEntry < start + (uint32)sec.sectionSize)
		{
			cemu_assert_debug(entry == 0xFFFFFFFF);
			entry = fileEntry - start + GuestAddr(rpl->sectionAddressTable[i].ptr);
		}
	});
	cemu_assert(entry != 0xFFFFFFFF);
	rpl->entrypoint = entry;
	return rpl;
}

/* ---------------- symbols and relocations ---------------- */

static RPLSymbol* SymbolTable(RPLModule* rpl, uint32 symtabIndex, uint32* count)
{
	const RPLFileSectionEntry& symtab = rpl->sectionTable[symtabIndex];
	uint32 entrySize = symtab.symbolEntrySize;
	if (entrySize == 0)
		entrySize = sizeof(RPLSymbol);
	cemu_assert(entrySize == sizeof(RPLSymbol));
	cemu_assert(((uint32)symtab.sectionSize % entrySize) == 0);
	*count = (uint32)symtab.sectionSize / entrySize;
	cemu_assert(*count >= 2);
	return (RPLSymbol*)rpl->sectionAddressTable[symtabIndex].ptr;
}

static uint32 LookupExport(RPLModule* rpl, bool isData, const char* name)
{
	const uint32 count = isData ? rpl->exportDCount : rpl->exportFCount;
	const RPLFileExportTableEntry* table = isData ? rpl->exportDDataPtr : rpl->exportFDataPtr;
	if (!table)
		return 0;
	const char* names = (const char*)table - 8; // name offsets are relative to the section start
	for (uint32 i = 0; i < count; i++)
	{
		if (strcmp(names + (uint32)table[i].nameOffset, name) == 0)
			return table[i].virtualOffset;
	}
	return 0;
}

static bool FindExport(RPLModule* rpl, bool isData, const char* name, uint32* out)
{
	const uint32 count = isData ? rpl->exportDCount : rpl->exportFCount;
	const RPLFileExportTableEntry* table = isData ? rpl->exportDDataPtr : rpl->exportFDataPtr;
	if (!table)
		return false;
	const char* names = (const char*)table - 8;
	for (uint32 i = 0; i < count; i++)
	{
		if (strcmp(names + (uint32)table[i].nameOffset, name) == 0)
		{
			*out = table[i].virtualOffset;
			return true;
		}
	}
	return false;
}

MPTR RPLLoader_FindRPLExport(RPLModule* rplLoaderContext, const char* symbolName, bool isData)
{
	cemu_assert_debug(!isData); // only function exports are searched
	return LookupExport(rplLoaderContext, false, symbolName);
}

static MPTR ResolveHLEImport(RPLModule* rpl, const std::string& lib, const char* symbol, bool isData)
{
	if (!isData)
		return rpl_mapHLEImport(rpl, lib.c_str(), symbol, true);
	const MPTR addr = osLib_getPointer(lib.c_str(), symbol);
	if (addr != 0xFFFFFFFF)
		return addr;
	cemuLog_logDebug(LogType::Force, "Unsupported data export ({}): {}.{}", rpl->moduleName, lib, symbol);
	return MPTR_NULL;
}

static void ResolveSymbols(RPLModule* rpl, uint32 symtabIndex, std::span<ImportTarget> imports, LinkPass pass)
{
	uint32 numSyms;
	RPLSymbol* syms = SymbolTable(rpl, symtabIndex, &numSyms);
	const uint32 strtabIndex = rpl->sectionTable[symtabIndex].symtabSectionIndex;
	const char* strtab = (const char*)rpl->sectionAddressTable[strtabIndex].ptr;
	const uint32 strtabSize = rpl->sectionTable[strtabIndex].sectionSize;
	const uint32 numSections = (uint32)rpl->sectionTable.size();

	for (uint32 i = 0; i < numSyms; i++)
	{
		RPLSymbol& sym = syms[i];
		const uint32 shndx = sym.shndx;
		if (shndx == 0 || shndx >= numSections)
			continue;
		void* secMem = rpl->sectionAddressTable[shndx].ptr;
		if (!secMem)
		{
			sym.value = 0xCD000000 | i; // marks a symbol in a section that was not loaded
			continue;
		}
		const RPLFileSectionEntry& sec = rpl->sectionTable[shndx];
		const uint32 offsetInSection = (uint32)sym.value - (uint32)sec.virtualAddress;
		auto rebase = [&]() {
			cemu_assert((uint8*)secMem >= memory_base && (uint8*)secMem <= memory_base + 0x100000000ULL);
			sym.value = GuestAddr(secMem) + offsetInSection;
		};

		if (shndx >= imports.size())
		{
			cemuLog_log(LogType::Force, "RPL-Loader: Symbol {} references invalid section", i);
			continue;
		}
		const ImportTarget& imp = imports[shndx];
		if (imp.module)
		{
			// symbol in an import section
			if (pass == LinkPass::Local)
				continue;
			if (offsetInSection < 8)
			{
				rebase(); // refers to the section header itself
				continue;
			}
			const uint32 nameOffset = sym.name;
			const char* name = strtab + nameOffset;
			const bool isData = ((uint32)sec.flags & kSecExec) == 0;
			if (imp.module == HLE_MODULE_PTR)
			{
				if (nameOffset >= strtabSize)
				{
					cemuLog_log(LogType::Force, "RPLLoader: Symbol {} in section {} has out-of-bounds name offset", i, shndx);
					continue;
				}
				sym.value = nameOffset == 0 ? 0 : ResolveHLEImport(rpl, imp.libName, name, isData);
			}
			else
			{
				uint32 addr;
				if (FindExport(imp.module, isData, name, &addr))
					sym.value = addr;
			}
			continue;
		}
		// the module's own symbols are rebased in the local pass; TLS symbols keep their offsets
		if ((sym.info & 0xF) == kSymTypeTLS)
			continue;
		if (pass == LinkPass::Local)
			rebase();
	}
}

static void ApplyRelocation(RPLModule* rpl, uint8* targetSection, uint32 type, uint32 offset, uint32 addend, uint32 symbolAddr, sint16 tlsModuleIndex)
{
	const MPTR where = GuestAddr(targetSection) + offset;
	uint8* p = memory_getPointerFromVirtualOffset(where);
	const uint32 value = symbolAddr + addend; // S + A
	const uint32 relative = value - where; // S + A - P
	auto ha = [](uint32 v) { return (uint16)((v >> 16) + ((v >> 15) & 1)); };
	switch (type)
	{
	case RPL_RELOC_ADDR32:
	case R_PPC_DTPREL32: // offset within the TLS block
		*(uint32be*)p = value;
		break;
	case RPL_RELOC_LO16:
		*(uint16be*)p = (uint16)value;
		break;
	case RPL_RELOC_HI16:
		*(uint16be*)p = (uint16)(value >> 16);
		break;
	case RPL_RELOC_HA16:
		*(uint16be*)p = ha(value);
		break;
	case R_PPC_REL16_LO:
		*(uint16be*)p = (uint16)relative;
		break;
	case R_PPC_REL16_HI:
		*(uint16be*)p = (uint16)(relative >> 16);
		break;
	case R_PPC_REL16_HA:
		*(uint16be*)p = ha(relative);
		break;
	case R_PPC_DTPMOD32:
		*(uint32be*)p = (uint32)(sint32)tlsModuleIndex;
		break;
	case RPL_RELOC_REL24:
	{
		uint32 insn = *(uint32be*)p;
		const uint32 top = relative >> 25;
		if (top != 0 && top != 0x7F)
		{
			// out of reach: branch absolute to a trampoline in low memory
			cemu_assert_debug((insn >> 26) == 18);
			const MPTR jump = GetFarJump(rpl, value);
			insn = (insn & ~0x03FFFFFCu) | (jump & 0x03FFFFFC) | 2;
		}
		else
		{
			if (relative & 3)
				cemuLog_log(LogType::Force, "RPL-Loader: Encountered unaligned RPL_RELOC_REL24");
			insn = (insn & ~0x03FFFFFCu) | (relative & 0x03FFFFFC);
		}
		*(uint32be*)p = insn;
		break;
	}
	case RPL_RELOC_REL14:
	{
		const uint32 top = relative & ~0x7FFFu;
		if (top != 0 && top != 0xFFFF8000)
		{
			cemu_assert_debug(false); // out of range, left as is
			break;
		}
		if (relative & 3)
			cemuLog_log(LogType::Force, "RPL-Loader: Encountered unaligned RPL_RELOC_REL14");
		*(uint32be*)p = ((uint32)*(uint32be*)p & ~0xFFFCu) | (relative & 0xFFFC);
		break;
	}
	case 0x6D: // R_PPC_EMB_SDA21: 16-bit offset from r2 or r13
	{
		const uint32 insn = *(uint32be*)p;
		const uint32 reg = (insn >> 16) & 0x1F;
		uint32 base;
		if (reg == 2)
			base = s_sda2Base;
		else if (reg == 13)
			base = s_sdaBase;
		else
		{
			cemuLog_log(LogType::Force, "RPLLoader: sdata reloc uses register other than r2/r13");
			break;
		}
		*(uint32be*)p = (insn & 0xFFE00000) | ((value - base) & 0xFFFF) | (reg << 16);
		break;
	}
	default:
		cemuLog_log(LogType::Force, "RPLLoader: Unsupported reloc type 0x{:02x}", type);
		break;
	}
}

static void ApplyRelocationSection(RPLModule* rpl, uint32 relaIndex, LinkPass pass)
{
	const RPLFileSectionEntry& rela = rpl->sectionTable[relaIndex];
	const uint32 numSections = (uint32)rpl->sectionTable.size();
	const uint32 targetIndex = rela.relocTargetSectionIndex;
	cemu_assert(targetIndex < numSections);
	const uint32 symtabIndex = rela.symtabSectionIndex;
	cemu_assert(symtabIndex < numSections);
	uint8* target = (uint8*)rpl->sectionAddressTable[targetIndex].ptr;
	cemu_assert(target);
	uint32 numSyms;
	const RPLSymbol* syms = SymbolTable(rpl, symtabIndex, &numSyms);

	// relocation sections stay compressed in the work area
	const uint8* stored = (const uint8*)rpl->sectionAddressTable[relaIndex].ptr;
	std::vector<uint8> inflated;
	const uint8* data = stored;
	uint32 dataSize = rela.sectionSize;
	if ((uint32)rela.flags & SHF_RPL_COMPRESSED)
	{
		dataSize = *(const uint32be*)stored;
		inflated.resize(dataSize);
		const bool ok = Inflate(stored + 4, (uint32)rela.sectionSize - 4, inflated.data(), dataSize);
		cemu_assert_debug(ok);
		data = inflated.data();
	}
	const uint32 crc = crc32_calc(0, data, dataSize);
	if (crc != rpl->GetSectionCRC(relaIndex))
		cemuLog_log(LogType::Force, "RPLLoader {} - Relocation section {} has CRC mismatch - Calc: {:08x} Actual: {:08x}", rpl->moduleName, relaIndex, crc, rpl->GetSectionCRC(relaIndex));

	const uint32 targetVaddr = rpl->sectionTable[targetIndex].virtualAddress;
	const RPLFileRelocEntry* relocs = (const RPLFileRelocEntry*)data;
	const uint32 count = dataSize / sizeof(RPLFileRelocEntry);
	for (uint32 i = 0; i < count; i++)
	{
		const RPLFileRelocEntry& r = relocs[i];
		const uint32 info = r.symbolIndexAndType;
		const uint32 type = info & 0xFF;
		const uint32 symIndex = info >> 8;
		if (type == 0)
			continue;
		if (symIndex >= numSyms)
		{
			cemuLog_logDebug(LogType::Force, "reloc with symbol index out of range 0x{:04x}", symIndex);
			continue;
		}
		const RPLSymbol& sym = syms[symIndex];
		const uint32 shndx = sym.shndx;
		if (shndx >= numSections)
		{
			cemuLog_logDebug(LogType::Force, "reloc with sectionIndex out of range 0x{:04x}", shndx);
			continue;
		}
		// imports are not resolved yet in the local pass
		if (pass == LinkPass::Local && (uint32)rpl->sectionTable[shndx].type == SHT_RPL_IMPORTS)
			continue;
		const uint32 symAddr = sym.value;
		if ((symAddr & 0xFF000000) == 0xCD000000)
		{
			cemu_assert_unimplemented(); // symbol in a section that was not loaded
			continue;
		}
		sint16 tlsIndex = -1;
		if (type == R_PPC_DTPMOD32 || type == R_PPC_DTPREL32)
		{
			if ((sym.info & 0xF) != kSymTypeTLS)
				cemuLog_log(LogType::Force, "RPLLoader: TLS relocation against a non-TLS symbol");
			if (rpl->fileInfo.tlsModuleIndex == -1)
				cemuLog_log(LogType::Force, "RPLLoader: TLS relocs applied to non-TLS module");
			tlsIndex = rpl->fileInfo.tlsModuleIndex;
		}
		ApplyRelocation(rpl, target, type, (uint32)r.relocOffset - targetVaddr, r.relocAddend, symAddr, tlsIndex);
	}
}

static void LinkModule(RPLModule* rpl, LinkPass pass)
{
	// what each import section refers to (the last two sections are CRCS and FILEINFO)
	std::vector<ImportTarget> imports(rpl->sectionTable.size() - 2);
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if ((uint32)sec.type != SHT_RPL_IMPORTS || i >= imports.size())
			return;
		cemu_assert((uint32)sec.sectionSize >= 9);
		const char* lib = (const char*)rpl->sectionAddressTable[i].ptr + 8;
		const std::string name = ModuleNameFromPath(lib);
		for (RPLModule* m : s_modules)
		{
			if (m->moduleName == name)
			{
				imports[i] = { m, name };
				return;
			}
		}
		// not a loaded RPL: one of the C++ libraries
		imports[i] = { HLE_MODULE_PTR, std::string(lib).substr(0, RPL_MODULE_NAME_LENGTH - 1) };
	});
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if ((uint32)sec.type == SHT_SYMTAB)
			ResolveSymbols(rpl, i, imports, pass);
	});
	ForEachSection(rpl, [&](uint32 i, RPLFileSectionEntry& sec) {
		if ((uint32)sec.type == SHT_RELA)
			ApplyRelocationSection(rpl, i, pass);
	});
	// code changed, drop stale recompiled code
	PPCRecompiler_invalidateRange(rpl->regionMappingBase_text.GetMPTR(), rpl->regionMappingBase_text.GetMPTR() + rpl->regionSize_text);
	rpl->heapTrampolineArea.forEachBlock([](void* mem, uint32 size) {
		const MPTR start = memory_getVirtualOffsetFromPointer(mem);
		PPCRecompiler_invalidateRange(start, start + size);
	});
}

// global functions of the module for the debugger
static void RegisterModuleSymbols(RPLModule* rpl)
{
	ForEachSection(rpl, [&](uint32 symtabIndex, RPLFileSectionEntry& symtab) {
		if ((uint32)symtab.type != SHT_SYMTAB)
			return;
		uint32 numSyms;
		const RPLSymbol* syms = SymbolTable(rpl, symtabIndex, &numSyms);
		const char* strtab = (const char*)rpl->sectionAddressTable[(uint32)symtab.symtabSectionIndex].ptr;
		for (uint32 i = 0; i < numSyms; i++)
		{
			const RPLSymbol& sym = syms[i];
			const uint32 shndx = sym.shndx;
			if (shndx == 0 || shndx >= rpl->sectionTable.size() || !rpl->sectionAddressTable[shndx].ptr)
				continue;
			const uint32 secType = rpl->sectionTable[shndx].type;
			if (secType == SHT_RPL_EXPORTS || secType == SHT_RPL_IMPORTS)
				continue;
			if ((uint32)sym.name > 0 && sym.info == kSymGlobalFunc)
				rplSymbolStorage_store(rpl->moduleName.c_str(), strtab + (uint32)sym.name, sym.value);
		}
	});
}

static RPLDependency* FindDependency(std::string_view moduleName)
{
	for (RPLDependency* d : s_dependencies)
	{
		if (d->moduleName == moduleName)
			return d;
	}
	return nullptr;
}

static RPLDependency* FindDependencyByHandle(uint32 handle)
{
	for (RPLDependency* d : s_dependencies)
	{
		if (d->coreinitHandle == handle)
			return d;
	}
	return nullptr;
}

void RPLLoader_Link()
{
	// every new module first gets its TLS index, then all go through the local pass,
	// then the import pass (exports of all new modules are final by then)
	for (RPLModule* m : s_modules)
	{
		if (m->isLinked)
			continue;
		RPLDependency* dep = FindDependency(m->moduleName);
		cemu_assert(dep && dep->tlsModuleIndex != -1);
		m->fileInfo.tlsModuleIndex = dep->tlsModuleIndex;
	}
	for (RPLModule* m : s_modules)
	{
		if (!m->isLinked)
			LinkModule(m, LinkPass::Local);
	}
	for (RPLModule* m : s_modules)
	{
		if (m->isLinked)
			continue;
		LinkModule(m, LinkPass::Imports);
		RegisterModuleSymbols(m);
		m->isLinked = true;
		GraphicPack2::NotifyModuleLoaded(m);
		g_debuggerDispatcher.NotifyModuleLoaded(m);
	}
}

uint32 RPLLoader_GetModuleEntrypoint(RPLModule* rplLoaderContext)
{
	return rplLoaderContext->entrypoint;
}

/* ---------------- unloading ---------------- */

static void RPLLoader_UnloadModule(RPLDependency* dependency, bool skipPPCCalls)
{
	// Mario Party 10's minigame mg0408.rpl keeps executing its code after it was
	// released, so released memory is not cleared
	if (dependency->rplHLEModule)
	{
		cemu_assert_debug(!dependency->rplLoaderContext);
		return; // C++ modules are notified by the callers
	}
	RPLModule* rpl = dependency->rplLoaderContext;
	ForEachImportedLibrary(rpl, [](std::string_view lib) { RPLLoader_RemoveDependency(lib); });
	g_debuggerDispatcher.NotifyModuleUnloaded(rpl);
	if (!skipPPCCalls)
	{
		cemu_assert_debug(PPCInterpreter_getCurrentInstance());
		if (rpl->entrypoint)
			PPCCoreCallback(rpl->entrypoint, dependency->coreinitHandle, 2);
	}
	s_codeHeap.free(rpl->regionMappingBase_text.GetPtr());
	rpl->regionMappingBase_text = nullptr;
	// data is not handed back during teardown (Mario Party 10 crashes on boot otherwise)
	if (!skipPPCCalls)
	{
		FreeModuleData(rpl, MEMPTR<void>(rpl->regionMappingBase_data).GetPtr());
		rpl->regionMappingBase_data = 0;
		FreeModuleData(rpl, MEMPTR<void>(rpl->regionMappingBase_loaderInfo).GetPtr());
		rpl->regionMappingBase_loaderInfo = 0;
	}
	rpl->heapTrampolineArea.releaseAll();
	if (rpl->tempRegionPtr)
	{
		s_workArea.free(rpl->tempRegionPtr);
		rpl->tempRegionPtr = nullptr;
	}
	std::erase(s_modules, rpl);
	delete rpl;
}

/* ---------------- dependencies ---------------- */

// system RPLs of Cafe OS 5.5.x
static bool IsCafeOSModuleName(std::string_view name)
{
	static const std::unordered_set<std::string> s_names = {
		"avm", "camera", "coreinit", "dc", "dmae", "drmapp", "erreula", "gx2", "h264", "lzma920", "mic", "nfc", "nio_prof",
		"nlibcurl", "nlibnss", "nlibnss2", "nn_ac", "nn_acp", "nn_act", "nn_aoc", "nn_boss", "nn_ccr", "nn_cmpt", "nn_dlp",
		"nn_ec", "nn_fp", "nn_hai", "nn_hpad", "nn_idbe", "nn_ndm", "nn_nets2", "nn_nfp", "nn_nim", "nn_olv", "nn_pdm",
		"nn_save", "nn_sl", "nn_spm", "nn_temp", "nn_uds", "nn_vctl", "nsysccr", "nsyshid", "nsyskbd", "nsysnet", "nsysuhs",
		"nsysuvd", "ntag", "padscore", "proc_ui", "sndcore2", "snduser2", "snd_core", "snd_user", "swkbd", "sysapp", "tcl",
		"tve", "uac", "uac_rpl", "usb_mic", "uvc", "uvd", "vpad", "vpadbase", "zlib125",
	};
	std::string lower(name);
	for (char& c : lower)
		c = _ansiToLower(c);
	return s_names.contains(lower);
}

static COSModule* FindHLEModule(std::string_view name)
{
	for (COSModule* m : GetCOSModules())
	{
		if (m->GetName() == name)
			return m;
	}
	return nullptr;
}

void RPLLoader_AddDependency(std::string_view name, bool isMainExecutable)
{
	cemu_assert(!name.empty());
	const std::string moduleName = ModuleNameFromPath(name);
	if (RPLDependency* existing = FindDependency(moduleName))
	{
		existing->referenceCount++;
		return;
	}
	RPLDependency* dep = new RPLDependency();
	dep->isMainExecutable = isMainExecutable;
	dep->moduleName = moduleName;
	dep->referenceCount = 1;
	dep->coreinitHandle = s_nextModuleHandle++;
	dep->tlsModuleIndex = s_nextTlsIndex++; // handed out even if the module never loads
	dep->isCafeOSModule = IsCafeOSModuleName(moduleName);
	if (s_nextTlsIndex == 0x7FFF)
		cemuLog_log(LogType::Force, "RPLLoader: Exhausted TLS module indices pool");

	// A copy in the user's cafeLibs folder replaces the C++ implementation, except for
	// swkbd/erreula when their resource files are not installed. Copies of system RPLs
	// in a game's own code folder are never used for libraries with a C++ implementation.
	const std::string fileName = moduleName + (isMainExecutable ? ".rpx" : ".rpl");
	std::error_code ec;
	const bool inCafeLibs = fs::exists(ActiveSettings::GetUserDataPath("cafeLibs/{}", fileName), ec) && ActiveSettings::LoadSharedLibrariesEnabled();
	bool useHLE = !inCafeLibs;
	if (inCafeLibs)
	{
		const bool missingResources =
			(moduleName == "swkbd" && !fsc_doesFileExist("vol/storage_mlc01/sys/title/0005001b/1004f000/content/00/swkbd/swkbd.pack")) ||
			(moduleName == "erreula" && !fsc_doesFileExist("vol/storage_mlc01/sys/title/0005001b/10051000/content/00/erreula/erreula.pack"));
		if (missingResources)
		{
			cemuLog_log(LogType::Force, "Game tried to load {}.rpl from cafeLibs/ but the necessary MLC data files are not present. Using PixelU's implementation instead", moduleName);
			useHLE = true;
		}
	}
	if (useHLE)
		dep->rplHLEModule = FindHLEModule(moduleName);
	s_dependencies.push_back(dep);
}

static void RPLLoader_RemoveDependency(std::string_view name)
{
	cemu_assert_debug(!name.empty());
	if (name.empty())
		return;
	if (RPLDependency* dep = FindDependency(ModuleNameFromPath(name)))
		dep->referenceCount--;
}

bool RPLLoader_HasDependency(std::string_view name)
{
	if (name.empty())
		return false;
	return FindDependency(ModuleNameFromPath(name)) != nullptr;
}

void RPLLoader_RemoveDependency(uint32 handle)
{
	if (RPLDependency* dep = FindDependencyByHandle(handle))
	{
		cemu_assert_debug(dep->referenceCount != 0);
		if (dep->referenceCount > 0)
			dep->referenceCount--;
	}
}

static RPLDependency* DependencyOfModule(RPLModule* rpl)
{
	cemu_assert_debug(rpl);
	for (RPLDependency* d : s_dependencies)
	{
		if (d->rplLoaderContext == rpl)
			return d;
	}
	cemu_assert_suspicious(); // every module is loaded through a dependency
	return nullptr;
}

uint32 RPLLoader_GetHandleByModuleName(const char* name)
{
	RPLDependency* dep = FindDependency(ModuleNameFromPath(name));
	if (!dep)
		return RPL_INVALID_HANDLE;
	cemu_assert_debug(dep->loadAttempted);
	// system modules have a handle even without an implementation
	if (!dep->isCafeOSModule && !dep->rplLoaderContext)
		return RPL_INVALID_HANDLE;
	return dep->coreinitHandle;
}

const std::string RPLLoader_GetModuleNameByHandle(uint32 handle)
{
	RPLDependency* dep = FindDependencyByHandle(handle);
	return dep ? dep->moduleName : "";
}

uint32 RPLLoader_GetMaxTLSModuleIndex()
{
	return s_nextTlsIndex - 1;
}

bool RPLLoader_GetTLSDataByTLSIndex(sint16 tlsModuleIndex, uint8** tlsData, sint32* tlsSize)
{
	RPLModule* rpl = nullptr;
	for (RPLDependency* d : s_dependencies)
	{
		if (d->tlsModuleIndex == tlsModuleIndex)
		{
			rpl = d->rplLoaderContext;
			break;
		}
	}
	if (!rpl)
		return false;
	cemu_assert(rpl->tlsStartAddress != 0);
	const uint32 size = rpl->tlsEndAddress - rpl->tlsStartAddress;
	cemu_assert_debug(size < 0x10000);
	*tlsData = memory_getPointerFromVirtualOffset(rpl->tlsStartAddress);
	*tlsSize = (sint32)size;
	return true;
}

static void LoadDependency(RPLDependency* dep)
{
	if (dep->rplHLEModule)
	{
		dep->rplHLEModule->RPLMapped();
		// e.g. the C++ GX2 uses TCL
		for (std::string_view sub : dep->rplHLEModule->GetDependencies())
			RPLLoader_AddDependency(sub);
		return;
	}
	for (RPLModule* m : s_modules)
	{
		if (m->moduleName == dep->moduleName)
		{
			dep->rplLoaderContext = m; // already mapped (the main executable)
			return;
		}
	}
	const std::string fileName = dep->moduleName + (dep->isMainExecutable ? ".rpx" : ".rpl");
	if (ActiveSettings::LoadSharedLibrariesEnabled() && !dep->isMainExecutable)
	{
		auto data = FileStream::LoadIntoMemory(ActiveSettings::GetUserDataPath("cafeLibs/{}", fileName));
		if (data)
		{
			cemuLog_log(LogType::Force, "Loading RPL: /cafeLibs/{}", fileName);
			dep->rplLoaderContext = RPLLoader_LoadFromMemory(data->data(), (sint32)data->size(), fileName);
			return;
		}
	}
	const std::string titlePath = "/internal/current_title/code/" + fileName;
	uint32 size = 0;
	uint8* data = fsc_extractFile(titlePath.c_str(), &size);
	if (data)
	{
		cemuLog_logDebug(LogType::Force, "Loading: {}", titlePath);
		dep->rplLoaderContext = RPLLoader_LoadFromMemory(data, (sint32)size, titlePath);
		free(data);
		return;
	}
	cemuLog_logDebug(LogType::Force, "Failed to load dependency {}", fileName);
}

void RPLLoader_UpdateDependencies()
{
	// Handle one change at a time and rescan: unloading drops references of other
	// dependencies and loading adds new ones
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (size_t i = 0; i < s_dependencies.size(); i++)
		{
			RPLDependency* dep = s_dependencies[i];
			if (dep->referenceCount == 0)
			{
				if (dep->rplLoaderContext)
				{
					RPLLoader_UnloadModule(dep, false);
					dep->rplLoaderContext = nullptr;
				}
				else if (dep->rplHLEModule)
				{
					dep->rplHLEModule->rpl_entry(dep->coreinitHandle, coreinit::RplEntryReason::Unloaded);
					dep->rplHLEModule->RPLUnmapped();
					for (std::string_view sub : dep->rplHLEModule->GetDependencies())
						RPLLoader_RemoveDependency(sub);
				}
				s_dependencies.erase(s_dependencies.begin() + i);
				delete dep;
				changed = true;
				break;
			}
			if (!dep->loadAttempted)
			{
				dep->loadAttempted = true;
				LoadDependency(dep);
				changed = true;
				break;
			}
		}
	}
	RPLLoader_Link();
}

void RPLLoader_LoadCoreinit()
{
	RPLLoader_AddDependency("coreinit");
	RPLDependency* dep = FindDependency("coreinit");
	if (!dep)
	{
		cemu_assert_suspicious();
		return;
	}
	dep->loadAttempted = true;
	LoadDependency(dep);
}

void RPLLoader_SetMainModule(RPLModule* rplLoaderContext)
{
	rplLoaderContext->entrypointCalled = true; // the main entry point is started by the OS bootstrap
	s_mainModule = rplLoaderContext;
}

uint32 RPLLoader_GetMainModuleHandle()
{
	for (RPLDependency* d : s_dependencies)
	{
		if (d->rplLoaderContext == s_mainModule)
			return d->coreinitHandle;
	}
	cemu_assert(false);
	return 0;
}

RPLModule* RPLLoader_FindModuleByCodeAddr(uint32 addr)
{
	for (RPLModule* m : s_modules)
	{
		const uint32 start = m->regionMappingBase_text.GetMPTR();
		if (addr >= start && addr < start + m->regionSize_text)
			return m;
	}
	return nullptr;
}

RPLModule* RPLLoader_FindModuleByDataAddr(uint32 addr)
{
	for (RPLModule* m : s_modules)
	{
		if (addr >= m->regionMappingBase_data && addr < m->regionMappingBase_data + m->regionSize_data)
			return m;
		if (addr >= m->regionMappingBase_loaderInfo && addr < m->regionMappingBase_loaderInfo + m->regionSize_loaderInfo)
			return m;
	}
	return nullptr;
}

RPLModule* RPLLoader_FindModuleByName(std::string moduleName)
{
	for (RPLModule* m : s_modules)
	{
		if (m->moduleName == moduleName)
			return m;
	}
	return nullptr;
}

void RPLLoader_CallEntrypoints()
{
	for (RPLDependency* d : s_dependencies)
	{
		if (d->rplHLEModule && !d->hleEntrypointCalled)
		{
			d->rplHLEModule->rpl_entry(d->coreinitHandle, coreinit::RplEntryReason::Loaded);
			d->hleEntrypointCalled = true;
		}
	}
	// entry points may load further modules, so the list can grow while iterating
	for (size_t i = 0; i < s_modules.size(); i++)
	{
		RPLModule* m = s_modules[i];
		if (m->entrypointCalled)
			continue;
		const uint32 handle = RPLLoader_GetHandleByModuleName(m->moduleName.c_str());
		PPCCoreCallback(RPLLoader_GetModuleEntrypoint(m), handle, 1);
		m->entrypointCalled = true;
	}
}

void RPLLoader_CallCoreinitEntrypoint()
{
	for (RPLDependency* d : s_dependencies)
	{
		if (d->moduleName == "coreinit" && d->rplHLEModule && !d->hleEntrypointCalled)
		{
			d->rplHLEModule->rpl_entry(d->coreinitHandle, coreinit::RplEntryReason::Loaded);
			d->hleEntrypointCalled = true;
			return;
		}
	}
	cemu_assert_unimplemented(); // an RPL coreinit cannot be started before a guest thread exists
}

void RPLLoader_NotifyControlPassedToApplication()
{
	s_appOwnsMemory = true;
}

uint32 RPLLoader_FindModuleOrHLEExport(uint32 moduleHandle, bool isData, const char* exportName)
{
	RPLDependency* dep = FindDependencyByHandle(moduleHandle);
	if (!dep)
		return 0;
	if (dep->rplLoaderContext)
		return LookupExport(dep->rplLoaderContext, isData, exportName);
	if (isData)
	{
		const MPTR addr = osLib_getPointer(dep->moduleName.c_str(), exportName);
		cemu_assert_debug(addr != 0xFFFFFFFF);
		return addr;
	}
	return rpl_mapHLEImport(nullptr, dep->moduleName.c_str(), exportName, true);
}

uint32 RPLLoader_GetSDA1Base()
{
	cemu_assert(!s_modules.empty()); // only valid once the main executable is loaded
	return s_sdaBase;
}

uint32 RPLLoader_GetSDA2Base()
{
	cemu_assert(!s_modules.empty());
	return s_sda2Base;
}

std::span<RPLModule*> RPLLoader_GetModuleList()
{
	return s_modules;
}

RPLModule* RPLLoader_GetModuleByName(std::string_view name)
{
	return RPLLoader_FindModuleByName(ModuleNameFromPath(name));
}

/* ---------------- code caves (graphic pack patches) ---------------- */

namespace
{
	// first-fit allocator over the code cave area; blocks are aligned to 256 bytes
	class CodeCaveAllocator
	{
	public:
		CodeCaveAllocator(uint32 base, uint32 size) : m_base(base), m_size(size) {}

		uint32 Allocate(uint32 size)
		{
			constexpr uint32 kAlign = 256;
			uint32 addr = AlignUp(m_base, kAlign);
			bool moved = true;
			while (moved)
			{
				moved = false;
				for (const Block& b : m_blocks)
				{
					// blocks that merely touch count as overlapping
					if (addr < b.end && addr + size >= b.begin)
					{
						addr = AlignUp(b.end, kAlign);
						moved = true;
						break;
					}
				}
			}
			if (addr + size > m_base + m_size)
				return 0;
			m_blocks.push_back({ addr, addr + size });
			return addr;
		}

		void Release(uint32 addr)
		{
			auto it = std::find_if(m_blocks.begin(), m_blocks.end(), [addr](const Block& b) { return b.begin == addr; });
			cemu_assert(it != m_blocks.end());
			if (it != m_blocks.end())
				m_blocks.erase(it);
		}

	private:
		struct Block
		{
			uint32 begin;
			uint32 end;
		};
		uint32 m_base;
		uint32 m_size;
		std::vector<Block> m_blocks;
	};

	CodeCaveAllocator s_codeCaves(MEMORY_CODECAVEAREA_ADDR, MEMORY_CODECAVEAREA_SIZE);
}

MEMPTR<void> RPLLoader_AllocateCodeCaveMem(uint32 alignment, uint32 size)
{
	return MEMPTR<void>{ s_codeCaves.Allocate(size) };
}

void RPLLoader_ReleaseCodeCaveMem(MEMPTR<void> addr)
{
	s_codeCaves.Release(addr.GetMPTR());
}

/* ---------------- setup / teardown ---------------- */

void RPLLoader_InitState()
{
	cemu_assert_debug(!s_trampolineHeap.hasAllocations());
	cemu_assert_debug(!s_codeHeap.hasAllocations());
	cemu_assert_debug(!s_workArea.hasAllocations());
	s_trampolineHeap.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_CODE_TRAMPOLINE_AREA_ADDR));
	s_codeHeap.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_CODEAREA_ADDR));
	s_workArea.setHeapBase(memory_getPointerFromVirtualOffset(MEMORY_RPLLOADER_AREA_ADDR));
	s_sharedTrampolines.setBaseAllocator(&s_trampolineHeap);
	RPLLoader_UnloadAll();
}

void RPLLoader_UnloadAll()
{
	while (!s_modules.empty())
		RPLLoader_UnloadModule(DependencyOfModule(s_modules.front()), true);
	// C++ modules: unload notifications in reverse order so that coreinit is last
	RPLLoader_RemoveDependency("coreinit"); // the reference taken by RPLLoader_LoadCoreinit()
	for (size_t i = s_dependencies.size(); i-- > 0;)
	{
		RPLDependency* dep = s_dependencies[i];
		cemu_assert_debug(dep->referenceCount >= 0);
		if (!dep->rplHLEModule)
			continue;
		if (dep->hleEntrypointCalled) // not yet called if the emulation stops right after loading
			dep->rplHLEModule->rpl_entry(dep->coreinitHandle, coreinit::RplEntryReason::Unloaded);
		dep->rplHLEModule->RPLUnmapped();
	}
	for (RPLDependency* dep : s_dependencies)
		delete dep;
	s_dependencies.clear();
	rplSymbolStorage_unloadAll();
	s_sharedTrampolines.releaseAll();
	s_hleImportStubs.clear();
	s_callables.clear();
	s_appOwnsMemory = false;
	s_highestCodeAddr = 0;
	s_dataCursor = 0x10000000;
	s_sdaBase = MPTR_NULL;
	s_sda2Base = MPTR_NULL;
	s_mainModule = nullptr;
}
