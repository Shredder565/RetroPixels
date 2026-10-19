// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Registry of HLE exports. Every system library export implemented in C++ is
// registered here under (library, symbol); the RPL loader looks names up when
// it resolves a module's imports and patches the import to the HLE index.
// Data exports (variables) are stored as plain guest addresses.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCState.h"

namespace
{
	// library and symbol names cannot contain a NUL, so this joins them without ambiguity
	std::string ExportKey(std::string_view lib, std::string_view sym)
	{
		std::string k;
		k.reserve(lib.size() + 1 + sym.size());
		k.append(lib);
		k.push_back('\0');
		k.append(sym);
		return k;
	}

	struct ExportRegistry
	{
		std::unordered_map<std::string, HLEIDX> functions;
		std::unordered_map<std::string, uint32> data;
	};

	// libraries register their exports from static initialisers too, so build on first use
	ExportRegistry& Registry()
	{
		static ExportRegistry s_registry;
		return s_registry;
	}
}

void osLib_generateHashFromName(const char* name, uint32* hashA, uint32* hashB)
{
	// two independent 32-bit FNV-1a variants
	uint32 a = 0x811C9DC5;
	uint32 b = 0x050C5D1F;
	for (const char* p = name; *p; p++)
	{
		const uint8 c = (uint8)*p;
		a = (a ^ c) * 0x01000193;
		b = (b ^ c) * 0x01000193 + 0x9E3779B9;
	}
	*hashA = a;
	*hashB = b;
}

void osLib_addFunctionInternal(const char* libraryName, const char* functionName, void(*osFunction)(PPCInterpreter_t* hCPU))
{
	// registering a name again replaces the earlier implementation
	HLEIDX idx = PPCInterpreter_registerHLECall(osFunction, fmt::format("{}.{}", libraryName, functionName));
	Registry().functions[ExportKey(libraryName, functionName)] = idx;
}

extern "C" DLLEXPORT void osLib_registerHLEFunction(const char* libraryName, const char* functionName, void(*osFunction)(PPCInterpreter_t* hCPU))
{
	osLib_addFunctionInternal(libraryName, functionName, osFunction);
}

sint32 osLib_getFunctionIndex(const char* libraryName, const char* functionName)
{
	auto& fns = Registry().functions;
	auto it = fns.find(ExportKey(libraryName, functionName));
	return it != fns.end() ? it->second : -1;
}

void osLib_addVirtualPointer(const char* libraryName, const char* functionName, uint32 vPtr)
{
	Registry().data[ExportKey(libraryName, functionName)] = vPtr;
}

uint32 osLib_getPointer(const char* libraryName, const char* functionName)
{
	auto& vars = Registry().data;
	auto it = vars.find(ExportKey(libraryName, functionName));
	return it != vars.end() ? it->second : 0xFFFFFFFF;
}

void osLib_returnFromFunction(PPCInterpreter_t* hCPU, uint32 returnValue)
{
	hCPU->gpr[3] = returnValue;
	hCPU->instructionPointer = hCPU->spr.LR;
}

void osLib_returnFromFunction64(PPCInterpreter_t* hCPU, uint64 returnValue64)
{
	hCPU->gpr[3] = (uint32)(returnValue64 >> 32);
	hCPU->gpr[4] = (uint32)returnValue64;
	hCPU->instructionPointer = hCPU->spr.LR;
}
