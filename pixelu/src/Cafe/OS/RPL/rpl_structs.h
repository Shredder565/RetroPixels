// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// On-disk structures of the Cafe OS RPL/RPX format (a big endian 32-bit ELF
// with OSABI 0xCA/0xFE, zlib-compressed sections, import/export sections and
// a trailing CRC table and FILEINFO block) and the loader's per-module state.
#pragma once

#include "util/ChunkedHeap/ChunkedHeap.h"

#define RPL_MODULE_NAME_LENGTH	64
#define RPL_MODULE_PATH_LENGTH	256

// section types (standard ELF plus the Cafe specific ones)
#define SHT_RPL_EXPORTS			(0x80000001)
#define SHT_RPL_IMPORTS			(0x80000002)
#define SHT_RPL_CRCS			(0x80000003)
#define SHT_RPL_FILEINFO		(0x80000004)

#define SHT_PROGBITS			(0x00000001)
#define SHT_SYMTAB				(0x00000002)
#define SHT_STRTAB				(0x00000003)
#define SHT_RELA				(0x00000004)
#define SHT_HASH				(0x00000005)
#define SHT_DYNAMIC				(0x00000006)
#define SHT_NOTE				(0x00000007)
#define SHT_NOBITS				(0x00000008)
#define SHT_REL					(0x00000009)
#define SHT_SHLIB				(0x0000000A)
#define SHT_DYNSYM				(0x0000000B)

// section flags
#define SHF_EXECUTE				0x00000004
#define SHF_RPL_COMPRESSED		0x08000000 // data is u32 inflated size + zlib stream
#define SHF_TLS					0x04000000

// PowerPC relocation types
#define RPL_RELOC_ADDR32	1
#define RPL_RELOC_LO16		4
#define RPL_RELOC_HI16		5
#define RPL_RELOC_HA16		6
#define RPL_RELOC_REL24		10
#define RPL_RELOC_REL14		11

#define R_PPC_DTPMOD32		68
#define R_PPC_DTPREL32		78

#define R_PPC_REL16_HA		251
#define R_PPC_REL16_HI		252
#define R_PPC_REL16_LO		253

// stands in for "module implemented in C++" where a module pointer is expected
#define HLE_MODULE_PTR		((RPLModule*)-1)

// Elf32_Rela
struct RPLFileRelocEntry
{
	/* +0x00 */ uint32be relocOffset; // r_offset
	/* +0x04 */ uint32be symbolIndexAndType; // r_info: symbol << 8 | type
	/* +0x08 */ uint32be relocAddend; // r_addend
};
static_assert(sizeof(RPLFileRelocEntry) == 0xC);

// Elf32_Shdr
struct RPLFileSectionEntry
{
	/* +0x00 */ uint32be nameOffset; // sh_name
	/* +0x04 */ uint32be type; // sh_type
	/* +0x08 */ uint32be flags; // sh_flags
	/* +0x0C */ uint32be virtualAddress; // sh_addr
	/* +0x10 */ uint32be fileOffset; // sh_offset
	/* +0x14 */ uint32be sectionSize; // sh_size (the loader replaces it with the inflated size)
	/* +0x18 */ uint32be symtabSectionIndex; // sh_link
	/* +0x1C */ uint32be relocTargetSectionIndex; // sh_info
	/* +0x20 */ uint32be alignment; // sh_addralign
	/* +0x24 */ uint32be symbolEntrySize; // sh_entsize
};
static_assert(sizeof(RPLFileSectionEntry) == 0x28);

// Elf32_Ehdr
struct RPLFileHeader
{
	/* +0x00 */ uint32be magic1; // 7F 'E' 'L' 'F'
	/* +0x04 */ uint8    version04; // EI_CLASS, 1 = 32-bit
	/* +0x05 */ uint8    ukn05; // EI_DATA, 2 = big endian
	/* +0x06 */ uint8    ukn06; // EI_VERSION
	/* +0x07 */ uint8    magic2_0; // EI_OSABI 0xCA
	/* +0x08 */ uint8    magic2_1; // EI_ABIVERSION 0xFE
	/* +0x09 */ uint8    ukn09;
	/* +0x0A */ uint8    ukn0A;
	/* +0x0B */ uint8    ukn0B;
	/* +0x0C */ uint32be dataRegionSize; // padding bytes of e_ident
	/* +0x10 */ uint8    ukn10; // e_type (0xFE01)
	/* +0x11 */ uint8    ukn11;
	/* +0x12 */ uint16be ukn12; // e_machine, 0x14 = PowerPC
	/* +0x14 */ uint32be ukn14; // e_version
	/* +0x18 */ uint32be entrypoint; // e_entry
	/* +0x1C */ uint32be ukn1C; // e_phoff
	/* +0x20 */ uint32be sectionTableOffset; // e_shoff
	/* +0x24 */ uint32be ukn24; // e_flags
	/* +0x28 */ uint16be ukn28; // e_ehsize
	/* +0x2A */ uint16be programHeaderTableEntrySize;
	/* +0x2C */ uint16be programHeaderTableEntryCount;
	/* +0x2E */ uint16be sectionTableEntrySize;
	/* +0x30 */ uint16be sectionTableEntryCount;
	/* +0x32 */ uint16be nameSectionIndex; // e_shstrndx
};
static_assert(sizeof(RPLFileHeader) == 0x34);
static_assert(offsetof(RPLFileHeader, dataRegionSize) == 0xC);
static_assert(offsetof(RPLFileHeader, programHeaderTableEntrySize) == 0x2A);

// contents of the SHT_RPL_FILEINFO section
struct RPLFileInfoData
{
	/* +0x00 */ uint32be fileInfoMagic; // 0xCAFE0402
	/* +0x04 */ uint32be textRegionSize;
	/* +0x08 */ uint32be ukn08; // text alignment
	/* +0x0C */ uint32be dataRegionSize;
	/* +0x10 */ uint32be baseAlign; // data alignment
	/* +0x14 */ uint32be ukn14; // loader info size
	/* +0x18 */ uint32be ukn18; // loader info alignment
	/* +0x1C */ uint32be ukn1C; // temp size
	/* +0x20 */ uint32be trampolineAdjustment;
	/* +0x24 */ uint32be sdataBase1; // r13 (_SDA_BASE_)
	/* +0x28 */ uint32be sdataBase2; // r2 (_SDA2_BASE_)
	/* +0x2C */ uint32be ukn2C; // stack size
	/* +0x30 */ uint32be ukn30; // file name offset
	/* +0x34 */ uint32be flags; // bit 1: is an RPX
	/* +0x38 */ uint32be ukn38; // heap size
	/* +0x3C */ uint32be ukn3C; // tag offset
	/* +0x40 */ uint32be minimumToolkitVersion;
	/* +0x44 */ uint32be ukn44; // compression level
	/* +0x48 */ uint32be ukn48;
	/* +0x4C */ uint32be ukn4C; // subtracted from the loader info size
	/* +0x50 */ uint32be ukn50; // SDK version
	/* +0x54 */ uint32be ukn54; // SDK revision
	/* +0x58 */ sint16be tlsModuleIndex;
};
static_assert(offsetof(RPLFileInfoData, tlsModuleIndex) == 0x58);

struct RPLSectionAddressEntry
{
	void* ptr; // where the section's data lives once loaded (guest memory, loader work area or host memory)
};

// one entry of an SHT_RPL_EXPORTS section; names are relative to the section start
struct RPLFileExportTableEntry
{
	uint32be virtualOffset;
	uint32be nameOffset;
};

struct RPLModule
{
	// --- file ---
	RPLFileHeader rplHeader;
	std::vector<RPLFileSectionEntry> sectionTable;
	std::span<uint8> RPLRawData; // the file image, only valid while loading
	std::vector<uint8> sectionData_fileInfo;
	std::vector<uint8> sectionData_crc;
	std::vector<uint32> crcTable;

	struct
	{
		uint32 textRegionSize;
		uint32 dataRegionSize;
		uint32 baseAlign;
		uint32 ukn14;
		uint32 trampolineAdjustment;
		uint32 ukn4C;
		sint16 tlsModuleIndex; // replaced with the index assigned by the loader before linking
		uint32 sdataBase1;
		uint32 sdataBase2;
		uint32 flags;
	}fileInfo;

	std::string moduleName; // lower case, without path and extension
	MEMPTR<char> ppcName; // "<name>.rpl" / "<name>.rpx" in guest memory
	uint32 patchCRC; // module checksum used to match patches and graphic packs (Cemuhook compatible)

	// --- memory placement ---
	MEMPTR<void> regionMappingBase_text;
	MPTR regionMappingBase_data;
	MPTR regionMappingBase_loaderInfo;
	uint32 regionSize_text;
	uint32 regionSize_data;
	uint32 regionSize_loaderInfo;
	uint32 regionOrigAddr_text; // lowest text address in the file
	uint32 regionOrigAddr_data; // lowest data address in the file
	uint8* tempRegionPtr; // non-allocated sections (relocations, ...) in the loader work area
	uint32 tempRegionAllocSize;
	std::vector<RPLSectionAddressEntry> sectionAddressTable;
	uint32 entrypoint;
	uint32 tlsStartAddress;
	uint32 tlsEndAddress;

	// --- exports (point into the mapped export sections) ---
	uint32 exportDCount;
	RPLFileExportTableEntry* exportDDataPtr;
	uint32 exportFCount;
	RPLFileExportTableEntry* exportFDataPtr;

	// --- far branch trampolines ---
	ChunkedFlatAllocator<16 * 1024> heapTrampolineArea;
	std::unordered_map<MPTR, MPTR> trampolineMap; // destination -> trampoline

	// --- state ---
	bool isLinked; // relocations and imports are resolved
	bool entrypointCalled;
	betype<MPTR> funcAlloc; // OSDynLoad allocator once the application owns memory
	betype<MPTR> funcFree;
	bool hasError{ false };

	uint32 GetSectionCRC(size_t sectionIndex) const
	{
		return sectionIndex < crcTable.size() ? crcTable[sectionIndex] : 0;
	}

	bool IsRPX() const
	{
		return (fileInfo.flags & 2) != 0;
	}
};

// a module requested by name (imports, OSDynLoad, the main executable) with its reference count
struct RPLDependency
{
	std::string moduleName;
	bool isMainExecutable{false};
	bool loadAttempted{false};
	bool hleEntrypointCalled{false};
	bool isCafeOSModule{false}; // one of the system RPLs of the Cafe OS
	RPLModule* rplLoaderContext{}; // loaded RPL, null for C++ modules and missing ones
	class COSModule* rplHLEModule{}; // C++ implementation, if used
	sint32 referenceCount;
	uint32 coreinitHandle; // module handle given to the guest
	sint16 tlsModuleIndex;
};

RPLModule* RPLLoader_FindModuleByCodeAddr(uint32 addr);
RPLModule* RPLLoader_FindModuleByDataAddr(uint32 addr);
RPLModule* RPLLoader_FindModuleByName(std::string moduleName);
