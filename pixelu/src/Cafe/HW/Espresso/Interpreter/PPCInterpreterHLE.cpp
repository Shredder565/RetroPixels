// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// High level emulation calls. Guest stubs for host implemented functions
// contain a pseudo instruction with primary opcode 1 whose low 16 bits index
// the table below. Index 0xFFD0 marks an import nobody implements; the stub
// is followed (8 bytes after the pseudo instruction) by the function name.

#include "PPCInterpreterInternal.h"
#include "PPCInterpreterHelper.h"

namespace
{
	constexpr sint32 kHleCallCapacity = 0x4000;
	constexpr uint32 kUnsupportedImportId = 0xFFD0;

	struct HleCallTable
	{
		std::mutex mutex;
		HLECALL entries[kHleCallCapacity]{};
		std::atomic<sint32> count{0};
	};

	HleCallTable& GetHleTable()
	{
		static HleCallTable s_table;
		return s_table;
	}

	std::mutex s_unsupportedLogMutex;
	std::unordered_set<std::string> s_reportedUnsupported;

	void ReportUnsupportedImport(PPCInterpreter_t* cpu)
	{
		const char* name = (const char*)memory_getPointerFromVirtualOffset(cpu->instructionPointer + 8);
		{
			std::lock_guard lock(s_unsupportedLogMutex);
			std::string message = fmt::format("Unsupported lib call: {}", name);
			if (s_reportedUnsupported.insert(message).second)
				cemuLog_log(LogType::UnsupportedAPI, "{}", message);
		}
		cpu->gpr[3] = 0; // unimplemented imports return 0
		PPCInterpreter_nextInstruction(cpu);
	}
}

// returns the existing index if the same function was registered before
HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)
{
	HleCallTable& table = GetHleTable();
	std::lock_guard lock(table.mutex);
	sint32 count = table.count.load(std::memory_order_relaxed);
	for (sint32 i = 0; i < count; i++)
	{
		if (table.entries[i] == hleCall)
			return i;
	}
	if (count >= kHleCallCapacity)
	{
		cemuLog_log(LogType::Force, "HLE call table is full, cannot register {}", hleName);
		cemu_assert(false);
		return -1;
	}
	table.entries[count] = hleCall;
	table.count.store(count + 1, std::memory_order_release);
	return count;
}

HLECALL PPCInterpreter_getHLECall(HLEIDX funcIndex)
{
	if (funcIndex < 0 || funcIndex >= kHleCallCapacity)
		return nullptr;
	return GetHleTable().entries[funcIndex];
}

void EspressoItp::Op_HLE(PPCInterpreter_t* cpu, uint32 op)
{
	uint32 callId = op & 0xFFFF;
	if (callId == kUnsupportedImportId) [[unlikely]]
	{
		ReportUnsupportedImport(cpu);
		return;
	}
	HLECALL func = PPCInterpreter_getHLECall((HLEIDX)callId);
	cemu_assert(func != nullptr);
	func(cpu);
}
