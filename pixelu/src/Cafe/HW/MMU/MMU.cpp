// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/HW/MMU/MMU.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cemu/Logging/CemuLogging.h"
#include "WindowSystem.h"
#include "util/MemMapper/MemMapper.h"
#include "config/ActiveSettings.h"

uint8* memory_base = nullptr;

namespace
{
	// All ranges in declaration order. A function-local static so that ranges
	// defined in any translation unit can register during static init.
	std::vector<MMURange*>& RangeRegistry()
	{
		static std::vector<MMURange*> s_ranges;
		return s_ranges;
	}

	MMURange* FindRangeStartingAt(uint32 base)
	{
		for (MMURange* range : RangeRegistry())
		{
			if (range->getBase() == base)
				return range;
		}
		return nullptr;
	}

	uint8* s_homebrewElfArea = nullptr;
	constexpr uint32 kHomebrewElfAreaBase = 0x00800000;
	constexpr uint32 kHomebrewElfAreaSize = 0x00800000;

	[[noreturn]] void QuitAfterFatalMemoryError()
	{
#if BOOST_OS_WINDOWS
		ExitProcess(-1);
#endif
		exit(-1);
	}
}

// --- address space layout ---

MMURange::MMURange(uint32 baseAddress, uint32 size, MMU_MEM_AREA_ID areaId, std::string_view name, MFLAG flags)
	: m_base(baseAddress), m_defaultSize(size), m_name(name), m_flags(flags), m_area(areaId), m_size(size)
{
	RangeRegistry().push_back(this);
}

void MMURange::mapMem()
{
	cemu_assert_debug(!m_mapped);
	void* committed = MemMapper::AllocateMemory(memory_base + m_base, m_size, MemMapper::PAGE_PERMISSION::P_RW, true);
	if (!committed)
	{
		WindowSystem::ShowErrorDialog(_tr("Unable to allocate {} memory", m_name), _tr("Error"));
		QuitAfterFatalMemoryError();
	}
	m_mapped = true;
}

void MMURange::unmapMem()
{
	MemMapper::FreeMemory(memory_base + m_base, m_size, true);
	m_mapped = false;
}

// Declaration order is the order of memory_getMMURanges() and of lookups.
MMURange mmuRange_LOW0{0x00010000, 0x000F0000, MMU_MEM_AREA_ID::CODE_LOW0, "CODE_LOW0"};
MMURange mmuRange_TRAMPOLINE_AREA{0x00E00000, 0x00200000, MMU_MEM_AREA_ID::CODE_TRAMPOLINE, "TRAMPOLINE_AREA"};
MMURange mmuRange_CODECAVE{0x01800000, 0x00400000, MMU_MEM_AREA_ID::CODE_CAVE, "CODECAVE"};
MMURange mmuRange_TEXT_AREA{0x02000000, 0x0C000000, MMU_MEM_AREA_ID::CODE_MAIN, "TEXT_AREA"};
// emulator-private memory (SysAllocator etc.), mapped at startup
MMURange mmuRange_CEMU_AREA{0x0E000000, 0x02000000, MMU_MEM_AREA_ID::CEMU_PRIVATE, "CEMU_AREA", MMURange::FLAG_MAP_EARLY};
MMURange mmuRange_MEM2{0x10000000, 0x40000000, MMU_MEM_AREA_ID::MEM2_DATA, "MEM2"};
// background app memory, only mapped once a title asks for the overlay arena
MMURange mmuRange_OVERLAY_AREA{0xA0000000, 0x1C000000, MMU_MEM_AREA_ID::OVERLAY, "OVERLAY_AREA", MMURange::FLAG_OPTIONAL};
MMURange mmuRange_FGBUCKET{0xE0000000, 0x04000000, MMU_MEM_AREA_ID::FGBUCKET, "FGBUCKET"};
MMURange mmuRange_TILINGAPERTURE{0xE8000000, 0x02000000, MMU_MEM_AREA_ID::TILING_APERATURE, "TILINGAPERTURE"};
MMURange mmuRange_MEM1{0xF4000000, 0x02000000, MMU_MEM_AREA_ID::MEM1, "MEM1"};
MMURange mmuRange_RPLLOADER{0xF6000000, 0x02000000, MMU_MEM_AREA_ID::RPLLOADER, "RPLLOADER_AREA"};
// mapped at startup because external code hooks may touch it before a title runs
MMURange mmuRange_SHARED_AREA{0xF8000000, 0x02000000, MMU_MEM_AREA_ID::SHAREDDATA, "SHARED_AREA", MMURange::FLAG_MAP_EARLY};
MMURange mmuRange_CORE0_LC{0xFFC00000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC0, "CORE0_LC"};
MMURange mmuRange_CORE1_LC{0xFFC40000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC1, "CORE1_LC"};
MMURange mmuRange_CORE2_LC{0xFFC80000, 0x00005000, MMU_MEM_AREA_ID::CPU_LC2, "CORE2_LC"};
// last page of the address space; some titles write near 0xFFFFFFFF
static MMURange s_mmuRangeTopPage{0xFFFFF000, 0x00001000, MMU_MEM_AREA_ID::CPU_PER_CORE, "PER-CORE"};

std::vector<MMURange*> memory_getMMURanges()
{
	return RangeRegistry();
}

MMURange* memory_getMMURangeByAddress(MPTR address)
{
	for (MMURange* range : RangeRegistry())
	{
		if (range->containsAddress(address))
			return range;
	}
	return nullptr;
}

bool memory_isAddressRangeAccessible(MPTR virtualAddress, uint32 size)
{
	for (MMURange* range : RangeRegistry())
	{
		if (range->isMapped() && range->containsAddress(virtualAddress))
			return size <= range->getEnd() - virtualAddress;
	}
	return false;
}

void memory_initPhysicalLayout()
{
	// The LLE boot path would need the real physical memory map. Not supported.
	cemuLog_log(LogType::Force, "memory_initPhysicalLayout: LLE physical memory layout is not implemented");
	assert_dbg();
}

void memory_init()
{
	if (!memory_base)
		memory_base = (uint8*)MemMapper::ReserveMemory(nullptr, (size_t)0x100000000, MemMapper::PAGE_PERMISSION::P_RW);
	if (!memory_base)
	{
		debug_printf("memory_init: reserving the 4GB guest address space failed\n");
		debugBreakpoint();
		WindowSystem::ShowErrorDialog(_tr("Unable to reserve 4GB of memory"), _tr("Error"));
		exit(-1);
	}
	for (MMURange* range : RangeRegistry())
	{
		if (range->isMappedEarly())
			range->mapMem();
	}
}

// Graphic packs may move the end of a range (for example to give a title more
// MEM2). A request is honoured only if its start is the base of an existing
// range and the new extent does not collide with any other range.
static void ApplyGraphicPackRangeOverrides()
{
	for (const auto& [start, end] : GraphicPack2::GetActiveRAMMappings())
	{
		MMURange* target = FindRangeStartingAt(start);
		if (!target)
		{
			cemuLog_log(LogType::Force, "Graphic pack: RAM mapping {:08x}-{:08x} ignored, its start is not the start of a memory range. Ranges that can be changed:", start, end);
			for (MMURange* range : RangeRegistry())
			{
				if (!range->isMapped())
					cemuLog_log(LogType::Force, "  {:08x}-{:08x} {}", range->getBase(), range->getEnd(), range->getName());
			}
			continue;
		}
		bool collides = false;
		for (MMURange* other : RangeRegistry())
		{
			if (other == target)
				continue;
			if (start < other->getEnd() && end > other->getBase())
			{
				cemuLog_log(LogType::Force, "Graphic pack: RAM mapping {:08x}-{:08x} ignored, it overlaps {:08x}-{:08x} {}", start, end, other->getBase(), other->getEnd(), other->getName());
				collides = true;
			}
		}
		if (!collides)
			target->setEnd(end);
	}
}

void memory_mapForCurrentTitle()
{
	for (MMURange* range : RangeRegistry())
	{
		if (!range->isMapped())
			range->resetConfig();
	}
	ApplyGraphicPackRangeOverrides();
	for (MMURange* range : RangeRegistry())
	{
		if (!range->isOptional() && !range->isMappedEarly())
			range->mapMem();
	}
}

void memory_unmapForCurrentTitle()
{
	for (MMURange* range : RangeRegistry())
	{
		if (range->isMapped() && !range->isMappedEarly())
			range->unmapMem();
	}
}

void memory_logModifiedMemoryRanges()
{
	for (const auto& [start, end] : GraphicPack2::GetActiveRAMMappings())
	{
		MMURange* target = FindRangeStartingAt(start);
		if (!target)
			continue;
		sint32 grownBytes = (sint32)end - (sint32)(target->getBase() + target->getInitSize());
		sint32 grownKiB = (grownBytes + 1023) / 1024;
		cemuLog_log(LogType::Force, "Graphic pack: RAM mapping {:08x}-{:08x} active ({:+}KiB)", start, end, grownKiB);
	}
}

void memory_enableOverlayArena()
{
	if (!mmuRange_OVERLAY_AREA.isMapped())
		mmuRange_OVERLAY_AREA.mapMem();
}

void memory_enableHBLELFCodeArea()
{
	if (s_homebrewElfArea)
		return;
	s_homebrewElfArea = (uint8*)MemMapper::AllocateMemory(memory_base + kHomebrewElfAreaBase, kHomebrewElfAreaSize, MemMapper::PAGE_PERMISSION::P_RW, true);
	if (!s_homebrewElfArea)
	{
		debug_printf("memory_enableHBLELFCodeArea: committing the homebrew ELF area failed\n");
		debugBreakpoint();
	}
}

// --- address conversion (virtual == physical) ---

uint32 memory_virtualToPhysical(uint32 virtualOffset)
{
	return virtualOffset;
}

uint32 memory_physicalToVirtual(uint32 physicalOffset)
{
	return physicalOffset;
}

uint8* memory_getPointerFromPhysicalOffset(uint32 physicalOffset)
{
	return memory_base + physicalOffset;
}

uint8* memory_getPointerFromVirtualOffset(uint32 virtualOffset)
{
	return memory_base + virtualOffset;
}

uint8* memory_getPointerFromVirtualOffsetAllowNull(uint32 virtualOffset)
{
	return virtualOffset == MPTR_NULL ? nullptr : memory_base + virtualOffset;
}

uint32 memory_getVirtualOffsetFromPointer(void* ptr)
{
	if (!ptr)
		return MPTR_NULL;
	return (uint32)((uint8*)ptr - memory_base);
}

// --- big endian scalar access ---

namespace
{
	template<typename T>
	T LoadBE(uint32 address)
	{
		T value;
		std::memcpy(&value, memory_base + address, sizeof(T));
		if constexpr (sizeof(T) == 2)
			return (T)CPU_swapEndianU16(value);
		else if constexpr (sizeof(T) == 4)
			return (T)CPU_swapEndianU32(value);
		else if constexpr (sizeof(T) == 8)
			return (T)CPU_swapEndianU64(value);
		else
			return value;
	}

	template<typename T>
	void StoreBE(uint32 address, T value)
	{
		if constexpr (sizeof(T) == 2)
			value = (T)CPU_swapEndianU16(value);
		else if constexpr (sizeof(T) == 4)
			value = (T)CPU_swapEndianU32(value);
		else if constexpr (sizeof(T) == 8)
			value = (T)CPU_swapEndianU64(value);
		std::memcpy(memory_base + address, &value, sizeof(T));
	}
}

void memory_writeU8(uint32 address, uint8 v) { memory_base[address] = v; }
void memory_writeU16(uint32 address, uint16 v) { StoreBE<uint16>(address, v); }
void memory_writeU32(uint32 address, uint32 v) { StoreBE<uint32>(address, v); }
void memory_writeU64(uint32 address, uint64 v) { StoreBE<uint64>(address, v); }
void memory_writeFloat(uint32 address, float vf) { StoreBE<uint32>(address, std::bit_cast<uint32>(vf)); }
void memory_writeDouble(uint32 address, double vf) { StoreBE<uint64>(address, std::bit_cast<uint64>(vf)); }

uint8 memory_readU8(uint32 address) { return memory_base[address]; }
uint16 memory_readU16(uint32 address) { return LoadBE<uint16>(address); }
uint32 memory_readU32(uint32 address) { return LoadBE<uint32>(address); }
uint64 memory_readU64(uint32 address) { return LoadBE<uint64>(address); }
float memory_readFloat(uint32 address) { return std::bit_cast<float>(LoadBE<uint32>(address)); }
double memory_readDouble(uint32 address) { return std::bit_cast<double>(LoadBE<uint64>(address)); }

// exported for external plugins that need the guest memory base
extern "C" DLLEXPORT void* memory_getBase()
{
	return memory_base;
}

void memory_createDump()
{
	fs::path folder = ActiveSettings::GetUserDataPath("dump/ramDump{:}", (uint32)time(nullptr));
	fs::create_directories(folder);
	for (MMURange* range : RangeRegistry())
	{
		if (!range->isMapped())
			continue;
		FileStream* file = FileStream::createFile2(folder / fmt::format("{:08x}.bin", range->getBase()));
		if (!file)
			continue;
		file->writeData(memory_base + range->getBase(), range->getSize());
		delete file;
	}
}

// --- MMIO dispatch ---

namespace MMU
{
	namespace
	{
		template<typename TFunc>
		std::unordered_map<PAddr, TFunc>& HandlerTable()
		{
			// one table per handler signature, created on first use (handlers
			// register from static initializers)
			static std::unordered_map<PAddr, TFunc> s_table;
			return s_table;
		}

		PAddr InterfaceBase(MMIOInterface interfaceLocation)
		{
			switch (interfaceLocation)
			{
			case MMIOInterface::INTERFACE_0C000000:
				return 0x0C000000;
			case MMIOInterface::INTERFACE_0D000000:
				return 0x0D000000;
			}
			assert_dbg();
			return 0;
		}

		template<typename TFunc>
		void AddHandler(MMIOInterface interfaceLocation, uint32 relativeAddress, TFunc func)
		{
			// the first registration for an address wins
			HandlerTable<TFunc>().try_emplace(InterfaceBase(interfaceLocation) + relativeAddress, func);
		}

		template<typename TFunc>
		TFunc LookupHandler(PAddr address)
		{
			auto& table = HandlerTable<TFunc>();
			auto it = table.find(address);
			return it == table.end() ? nullptr : it->second;
		}
	}

	void RegisterMMIO_W32(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncWrite32 ptr)
	{
		AddHandler(interfaceLocation, relativeAddress, ptr);
	}

	void RegisterMMIO_W16(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncWrite16 ptr)
	{
		AddHandler(interfaceLocation, relativeAddress, ptr);
	}

	void RegisterMMIO_R32(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncRead32 ptr)
	{
		AddHandler(interfaceLocation, relativeAddress, ptr);
	}

	void RegisterMMIO_R16(MMIOInterface interfaceLocation, uint32 relativeAddress, MMIOFuncRead16 ptr)
	{
		AddHandler(interfaceLocation, relativeAddress, ptr);
	}

	void WriteMMIO_32(PAddr address, uint32 value)
	{
		cemu_assert_debug((address & 3) == 0);
		if (auto handler = LookupHandler<MMIOFuncWrite32>(address))
			handler(address, value);
	}

	void WriteMMIO_16(PAddr address, uint16 value)
	{
		cemu_assert_debug((address & 1) == 0);
		if (auto handler = LookupHandler<MMIOFuncWrite16>(address))
			handler(address, value);
	}

	uint32 ReadMMIO_32(PAddr address)
	{
		cemu_assert_debug((address & 3) == 0);
		auto handler = LookupHandler<MMIOFuncRead32>(address);
		return handler ? handler(address) : 0;
	}

	uint16 ReadMMIO_16(PAddr address)
	{
		cemu_assert_debug((address & 1) == 0);
		auto handler = LookupHandler<MMIOFuncRead16>(address);
		return handler ? handler(address) : 0;
	}
}
