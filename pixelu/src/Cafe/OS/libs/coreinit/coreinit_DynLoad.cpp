// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// OSDynLoad sits on top of the RPL loader: it resolves module names to
// handles (loading and linking on demand), looks up exports, reports module
// layout, and lets titles register callbacks that fire when modules come and go.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/RPL/rpl.h"
#include "Cafe/OS/RPL/rpl_structs.h"
#include "Cafe/OS/libs/coreinit/coreinit_DynLoad.h"
#include "Cafe/OS/libs/coreinit/coreinit_MEM.h"

namespace coreinit
{
	namespace
	{
		constexpr uint32 kErrModuleNotFound = 0xFFFCFFE9;
		constexpr uint32 kErrNullNotifyFunc = 0xBAD1000E;
		constexpr uint32 kMainModuleAlias = 0xFFFFFFFF; // some titles pass this for the main executable

		constexpr sint32 kNotifyLoaded = 1;
		constexpr sint32 kNotifyUnloaded = 2;

		// guest functions installed by OSDynLoad_SetAllocator / SetTLSAllocator
		struct
		{
			MPTR allocFn = MPTR_NULL;
			MPTR freeFn = MPTR_NULL;
		} s_moduleAllocator, s_tlsAllocator;

		struct NotifyRegistration
		{
			MEMPTR<OSDynLoadNotifyFunc> func;
			MEMPTR<void> context;
		};
		std::vector<NotifyRegistration> s_notifyList;

		void DescribeModule(OSDynLoad_NotifyData& out, const RPLModule* module)
		{
			const MPTR textBase = module->regionMappingBase_text.GetMPTR();
			const MPTR dataBase = module->regionMappingBase_data;
			out.name = module->ppcName;
			out.textAddr = textBase;
			out.textOffset = textBase - module->regionOrigAddr_text;
			out.textSize = module->regionSize_text;
			out.dataAddr = dataBase;
			out.dataOffset = dataBase - module->regionOrigAddr_data;
			out.dataSize = module->regionSize_data;
			// read-only data is reported with the data region
			out.readAddr = dataBase;
			out.readOffset = dataBase - module->regionOrigAddr_data;
			out.readSize = module->regionSize_data;
		}

		// Calls every registered notify callback for one module. A fresh info block is
		// allocated through the guest allocator for each callback. If the module cannot
		// be found, notification stops and that block is not released.
		template<typename THandleArg>
		void NotifyAll(std::string_view moduleName, THandleArg handleArg, sint32 reason)
		{
			for (const NotifyRegistration& reg : s_notifyList)
			{
				MEMPTR<OSDynLoad_NotifyData> info = (OSDynLoad_NotifyData*)OSDynLoad_AllocatorAlloc(sizeof(OSDynLoad_NotifyData), 4);
				const RPLModule* module = RPLLoader_GetModuleByName(moduleName);
				if (!module)
					break;
				DescribeModule(*info, module);
				PPCCoreCallback(reg.func.GetMPTR(), handleArg, reg.context.GetMPTR(), reason, info.GetMPTR());
				OSDynLoad_AllocatorFree(info.GetPtr());
			}
		}

		// "/path/to/foo.rpl" -> "foo"
		std::string ModuleBaseName(const char* libName)
		{
			std::string_view name(libName);
			if (size_t slash = name.rfind('/'); slash != std::string_view::npos)
				name.remove_prefix(slash + 1);
			if (size_t dot = name.rfind('.'); dot != std::string_view::npos)
				name = name.substr(0, dot);
			return std::string(name);
		}

		uint32 ResolveModule(const char* libName, uint32be* handleOut, bool lookupOnly)
		{
			uint32 handle = RPLLoader_GetHandleByModuleName(libName);
			if (handle == RPL_INVALID_HANDLE && !RPLLoader_HasDependency(libName))
			{
				if (lookupOnly)
				{
					*handleOut = 0;
					return 0;
				}
				// not loaded yet: pull it in as a dependency and run its entry point
				RPLLoader_AddDependency(libName);
				RPLLoader_UpdateDependencies();
				RPLLoader_Link();
				RPLLoader_CallEntrypoints();
				handle = RPLLoader_GetHandleByModuleName(libName);
			}
			if (handle == RPL_INVALID_HANDLE)
			{
				*handleOut = 0;
				cemuLog_logDebug(LogType::Force, "OSDynLoad: could not load module \"{}\"", libName);
				return kErrModuleNotFound;
			}
			*handleOut = handle;
			NotifyAll(ModuleBaseName(libName), handleOut, kNotifyLoaded);
			return 0;
		}

		uint32 ResolveHandleAlias(uint32 moduleHandle)
		{
			return moduleHandle == kMainModuleAlias ? RPLLoader_GetMainModuleHandle() : moduleHandle;
		}
	}

	uint32 OSDynLoad_SetAllocator(MPTR allocFunc, MPTR freeFunc)
	{
		s_moduleAllocator.allocFn = allocFunc;
		s_moduleAllocator.freeFn = freeFunc;
		return 0;
	}

	void OSDynLoad_SetTLSAllocator(MPTR allocFunc, MPTR freeFunc)
	{
		s_tlsAllocator.allocFn = allocFunc;
		s_tlsAllocator.freeFn = freeFunc;
	}

	uint32 OSDynLoad_GetAllocator(betype<MPTR>* funcAlloc, betype<MPTR>* funcFree)
	{
		*funcAlloc = s_moduleAllocator.allocFn;
		*funcFree = s_moduleAllocator.freeFn;
		return 0;
	}

	void OSDynLoad_GetTLSAllocator(betype<MPTR>* funcAlloc, betype<MPTR>* funcFree)
	{
		*funcAlloc = s_tlsAllocator.allocFn;
		*funcFree = s_tlsAllocator.freeFn;
	}

	void* OSDynLoad_AllocatorAlloc(sint32 size, sint32 alignment)
	{
		if (s_moduleAllocator.allocFn == MPTR_NULL)
			return nullptr;
		// guest allocator signature: int alloc(size, align, void** out)
		StackAllocator<MEMPTR<void>> result;
		if (PPCCoreCallback(s_moduleAllocator.allocFn, size, alignment, result.GetMPTR()) != 0)
		{
			cemu_assert_debug(false);
			return nullptr;
		}
		return result->GetPtr();
	}

	void OSDynLoad_AllocatorFree(void* mem)
	{
		if (s_moduleAllocator.freeFn == MPTR_NULL)
			return;
		PPCCoreCallback(s_moduleAllocator.freeFn, MEMPTR<void>(mem));
	}

	uint32 OSDynLoad_Acquire(const char* libName, uint32be* moduleHandleOut)
	{
		return ResolveModule(libName, moduleHandleOut, false);
	}

	uint32 OSDynLoad_IsModuleLoaded(const char* libName, uint32be* moduleHandleOut)
	{
		return ResolveModule(libName, moduleHandleOut, true);
	}

	void OSDynLoad_Release(uint32 moduleHandle)
	{
		if (moduleHandle == RPL_INVALID_HANDLE)
			return;
		NotifyAll(RPLLoader_GetModuleNameByHandle(moduleHandle), moduleHandle, kNotifyUnloaded);
		RPLLoader_RemoveDependency(moduleHandle);
		RPLLoader_UpdateDependencies();
	}

	uint32 OSDynLoad_FindExport(uint32 moduleHandle, uint32 isData, const char* exportName, betype<MPTR>* addrOut)
	{
		MPTR addr = RPLLoader_FindModuleOrHLEExport(ResolveHandleAlias(moduleHandle), isData, exportName);
		*addrOut = addr;
		return addr == MPTR_NULL ? 0xFFFFFFFF : 0;
	}

	uint32 OSDynLoad_GetModuleName(uint32 moduleHandle, char* nameBuf, sint32be* nameBufSize)
	{
		const std::string name = RPLLoader_GetModuleNameByHandle(ResolveHandleAlias(moduleHandle));
		// strncpy semantics: zero-filled up to the buffer size, not terminated if the name is longer
		std::strncpy(nameBuf, name.c_str(), (sint32)*nameBufSize);
		return 0;
	}

	sint32 OSDynLoad_GetNumberOfRPLs()
	{
		return (sint32)RPLLoader_GetModuleList().size();
	}

	uint32 OSDynLoad_GetRPLInfo(uint32 first, uint32 count, OSDynLoad_NotifyData* outInfos)
	{
		if (count == 0)
			return 1;
		std::span<RPLModule*> modules = RPLLoader_GetModuleList();
		for (uint32 n = 0; n < count; n++)
		{
			const uint32 moduleIndex = first + n;
			if (moduleIndex < modules.size())
				DescribeModule(outInfos[n], modules[moduleIndex]);
			else
				memset((void*)(outInfos + n), 0, sizeof(OSDynLoad_NotifyData));
		}
		return 1;
	}

	uint32 OSDynLoad_AddNotifyCallback(MEMPTR<OSDynLoadNotifyFunc> notifyFn, MEMPTR<void> userContext)
	{
		if (!notifyFn)
			return kErrNullNotifyFunc;
		s_notifyList.push_back({ notifyFn, userContext });
		return 0;
	}

	uint32 OSDynLoad_DelNotifyCallback(MEMPTR<OSDynLoadNotifyFunc> notifyFn, MEMPTR<void> userContext)
	{
		std::erase_if(s_notifyList, [&](const NotifyRegistration& reg) {
			return reg.func.GetMPTR() == notifyFn.GetMPTR() && reg.context.GetMPTR() == userContext.GetMPTR();
		});
		return 0;
	}

	void InitializeDynLoad()
	{
		cafeExportRegister("coreinit", OSDynLoad_SetAllocator, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_SetTLSAllocator, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetAllocator, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetTLSAllocator, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDynLoad_Acquire, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_Release, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_IsModuleLoaded, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_FindExport, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetModuleName, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetNumberOfRPLs, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_GetRPLInfo, LogType::Placeholder);

		cafeExportRegister("coreinit", OSDynLoad_AddNotifyCallback, LogType::Placeholder);
		cafeExportRegister("coreinit", OSDynLoad_DelNotifyCallback, LogType::Placeholder);
	}
}
