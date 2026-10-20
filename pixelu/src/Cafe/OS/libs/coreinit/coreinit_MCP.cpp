// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/IOSU/legacy/iosu_ioctl.h"
#include "Cafe/IOSU/legacy/iosu_mcp.h"
#include "Cafe/OS/libs/coreinit/coreinit_IOS.h"
#include "Cafe/OS/libs/coreinit/coreinit_MCP.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/libs/nn_act/nn_act.h"
#include "config/CemuConfig.h"
#include "Cafe/CafeSystem.h"

// MCP (title and device management) client calls, plus UCReadSysConfig for system settings.
// Title queries go to the HLE IOSU MCP device through a private request format.

static_assert(sizeof(MCPTitleInfo) == 0x61);
static_assert(offsetof(MCPTitleInfo, appPath) == 0x0C);
static_assert(offsetof(MCPTitleInfo, titleVersion) == 0x48);
static_assert(offsetof(MCPTitleInfo, osVersion) == 0x4A);

namespace
{
	constexpr MCPHANDLE kMcpHandle = 0x10; // there is no real MCP session; every open gets this handle

	uint32 Arg(PPCInterpreter_t* hCPU, uint32 index)
	{
		return PPCInterpreter_getCallParamU32(hCPU, index);
	}

	template<typename T>
	T* ArgPtr(PPCInterpreter_t* hCPU, uint32 index)
	{
		return (T*)memory_getPointerFromVirtualOffsetAllowNull(Arg(hCPU, index));
	}

	uint64 Arg64(PPCInterpreter_t* hCPU, uint32 index)
	{
		return ((uint64)Arg(hCPU, index) << 32) | Arg(hCPU, index + 1);
	}

	// a title list request to the IOSU MCP device; the request and its vector live on the guest stack
	class McpTitleQuery
	{
	public:
		McpTitleQuery(uint32 requestCode)
		{
			memset(m_request.GetPointer(), 0, sizeof(iosuMcpCemuRequest_t));
			memset(m_vector.GetPointer(), 0, sizeof(ioBufferVector_t));
			m_vector->buffer = (uint8*)m_request.GetPointer();
			m_request->requestCode = requestCode;
		}

		iosuMcpCemuRequest_t* operator->() { return m_request.GetPointer(); }

		void Run()
		{
			__depr__IOS_Ioctlv(IOS_DEVICE_MCP, IOSU_MCP_REQUEST_CEMU, 1, 1, m_vector.GetPointer());
		}

	private:
		StackAllocator<iosuMcpCemuRequest_t> m_request;
		StackAllocator<ioBufferVector_t> m_vector;
	};

	struct McpSystemVersion
	{
		uint32be major;
		uint32be minor;
		uint32be patch;
	};
	static_assert(sizeof(McpSystemVersion) == 0xC);

	#pragma pack(push, 1)
	struct McpDeviceInfo
	{
		/* +0x000 */ char typeName[0x8];   // "mlc", "slc", "usb", ...
		/* +0x008 */ char volumeId[0x10];
		/* +0x018 */ uint8 unk018[0x90 - 0x18];
		/* +0x090 */ char mountPath[0x27F];
		/* +0x30F */ uint32be flags;
		/* +0x313 */ uint8 unk313[4];
		/* +0x317 */ uint8 unk317[4];
	};
	#pragma pack(pop)
	static_assert(offsetof(McpDeviceInfo, mountPath) == 0x90);
	static_assert(offsetof(McpDeviceInfo, flags) == 0x30F);
	static_assert(sizeof(McpDeviceInfo) == 0x31B);

	void Export_MCP_Open(PPCInterpreter_t* hCPU)
	{
		osLib_returnFromFunction(hCPU, MCP_Open());
	}

	void Export_MCP_Close(PPCInterpreter_t* hCPU)
	{
		MCP_Close(Arg(hCPU, 0));
		osLib_returnFromFunction(hCPU, 0);
	}

	void Export_MCP_GetSysProdSettings(PPCInterpreter_t* hCPU)
	{
		sint32 r = MCP_GetSysProdSettings(Arg(hCPU, 0), (SysProdSettings*)memory_getPointerFromVirtualOffset(Arg(hCPU, 1)));
		osLib_returnFromFunction(hCPU, (uint32)r);
	}

	// MCP_TitleListByAppType(handle, appType, uint32be* countOut, MCPTitleInfo* list, uint32 listBytes)
	void Export_MCP_TitleListByAppType(PPCInterpreter_t* hCPU)
	{
		uint32 appType = Arg(hCPU, 1);
		uint32be* countOut = ArgPtr<uint32be>(hCPU, 2);
		MCPTitleInfo* list = ArgPtr<MCPTitleInfo>(hCPU, 3);
		uint32 listBytes = Arg(hCPU, 4);
		McpTitleQuery q(IOSU_MCP_GET_TITLE_LIST_BY_APP_TYPE);
		q->titleListRequest.titleCount = listBytes / (uint32)sizeof(MCPTitleInfo);
		q->titleListRequest.titleList = list;
		q->titleListRequest.titleListBufferSize = (uint32)sizeof(MCPTitleInfo);
		q->titleListRequest.appType = appType;
		q.Run();
		*countOut = q->titleListRequest.titleCount;
		osLib_returnFromFunction(hCPU, 0);
	}

	// MCP_TitleList(handle, uint32be* countInOut, MCPTitleInfo* list, uint32 listBytes)
	void Export_MCP_TitleList(PPCInterpreter_t* hCPU)
	{
		uint32be* count = ArgPtr<uint32be>(hCPU, 1);
		McpTitleQuery q(IOSU_MCP_GET_TITLE_LIST);
		q->titleListRequest.titleCount = *count;
		q->titleListRequest.titleList = ArgPtr<MCPTitleInfo>(hCPU, 2);
		q->titleListRequest.titleListBufferSize = Arg(hCPU, 3);
		q.Run();
		*count = q->titleListRequest.titleCount;
		cemuLog_logDebug(LogType::Force, "MCP_TitleList: {} titles", (uint32)q->titleListRequest.titleCount);
		osLib_returnFromFunction(hCPU, q->returnCode);
	}

	void Export_MCP_TitleCount(PPCInterpreter_t* hCPU)
	{
		McpTitleQuery q(IOSU_MCP_GET_TITLE_COUNT);
		q->titleListRequest.titleCount = 0;
		q.Run();
		osLib_returnFromFunction(hCPU, q->titleListRequest.titleCount);
	}

	// looks up one title; returns the number of entries found (0 or 1) through the query
	uint32 QueryTitleInfo(McpTitleQuery& q, uint64 titleId, MCPTitleInfo* out)
	{
		q->titleListRequest.titleCount = 1;
		q->titleListRequest.titleList = out;
		q->titleListRequest.titleListBufferSize = (uint32)sizeof(MCPTitleInfo);
		q->titleListRequest.titleId = titleId;
		q.Run();
		return q->titleListRequest.titleCount;
	}

	// MCP_GetTitleInfo(handle, <pad>, uint64 titleId, MCPTitleInfo* out)
	void Export_MCP_GetTitleInfo(PPCInterpreter_t* hCPU)
	{
		McpTitleQuery q(IOSU_MCP_GET_TITLE_INFO);
		if (QueryTitleInfo(q, Arg64(hCPU, 2), ArgPtr<MCPTitleInfo>(hCPU, 4)) == 0)
			cemuLog_log(LogType::Force, "MCP_GetTitleInfo: title not found");
		osLib_returnFromFunction(hCPU, q->returnCode);
	}

	// MCP_GetTitleInfoByTitleAndDevice(handle, <pad>, uint64 titleId, const char* device, MCPTitleInfo* out)
	void Export_MCP_GetTitleInfoByTitleAndDevice(PPCInterpreter_t* hCPU)
	{
		// the device name ("mlc", "odd", ...) is not used to narrow the search
		McpTitleQuery q(IOSU_MCP_GET_TITLE_INFO);
		if (QueryTitleInfo(q, Arg64(hCPU, 2), ArgPtr<MCPTitleInfo>(hCPU, 5)) == 0)
		{
			// callers such as nn_vctl expect an error when the title does not exist
			osLib_returnFromFunction(hCPU, BUILD_NN_RESULT(NN_RESULT_LEVEL_STATUS, NN_RESULT_MODULE_MCP, 0));
			return;
		}
		osLib_returnFromFunction(hCPU, q->returnCode);
	}

	void Export_MCP_TitleListByDevice(PPCInterpreter_t* hCPU)
	{
		// placeholder answer: a single dummy title
		uint32be* countOut = ArgPtr<uint32be>(hCPU, 2);
		MCPTitleInfo* list = ArgPtr<MCPTitleInfo>(hCPU, 3);
		list[0].titleIdHigh = 0x00050000;
		list[0].titleIdLow = 0x11223344;
		strcpy(list[0].appPath, "titlePathTest");
		*countOut = 1;
		cemuLog_logDebug(LogType::Force, "MCP_TitleListByDevice: placeholder");
		osLib_returnFromFunction(hCPU, 0);
	}

	void Export_MCP_GetSystemVersion(PPCInterpreter_t* hCPU)
	{
		McpSystemVersion* v = ArgPtr<McpSystemVersion>(hCPU, 1);
		v->major = 5;
		v->minor = 5;
		v->patch = 5;
		osLib_returnFromFunction(hCPU, 0);
	}

	void Export_MCP_Get4SecondOffStatus(PPCInterpreter_t* hCPU)
	{
		// 1 would make the system menu warn about the last forced (4 second) power off
		osLib_returnFromFunction(hCPU, 0);
	}

	void FillDevice(McpDeviceInfo& dev, const char* typeName, const char* volumeId, uint32 flags, const char* mountPath)
	{
		strcpy(dev.typeName, typeName);
		if (volumeId)
			strcpy(dev.volumeId, volumeId);
		dev.flags = flags;
		strcpy(dev.mountPath, mountPath);
	}

	void Export_MCP_DeviceList(PPCInterpreter_t* hCPU)
	{
		uint32be* countOut = ArgPtr<uint32be>(hCPU, 1);
		McpDeviceInfo* list = ArgPtr<McpDeviceInfo>(hCPU, 2);
		uint32 listBytes = Arg(hCPU, 3);
		if (listBytes / sizeof(McpDeviceInfo) < 2)
			cemuLog_log(LogType::Force, "MCP_DeviceList: list has room for fewer than two devices");
		// the friend list (frd) waits forever unless both mlc and slc are reported
		memset(list, 0, sizeof(McpDeviceInfo));
		FillDevice(list[0], "mlc", nullptr, 1, "/vol/storage_mlc01");
		FillDevice(list[1], "slc", nullptr, 1, "/vol/storage_slc01");
		*countOut = 2;
		osLib_returnFromFunction(hCPU, 0);
	}

	void Export_MCP_FullDeviceList(PPCInterpreter_t* hCPU)
	{
		uint32be* countOut = ArgPtr<uint32be>(hCPU, 1);
		McpDeviceInfo* list = ArgPtr<McpDeviceInfo>(hCPU, 2);
		uint32 listBytes = Arg(hCPU, 3);
		cemu_assert(listBytes / sizeof(McpDeviceInfo) >= 2);
		memset(list, 0, listBytes);
		// flag 0x2 is needed by the system menu and friend list, without 0x8 the menu shows a disc loading screen
		const uint32 flags = 0x2 | 0x8;
		FillDevice(list[0], "slc", "VOLID_SLC", flags, "/vol/system_slc");
		FillDevice(list[1], "mlc", "VOLID_MLC", flags, "/vol/storage_mlc01");
		*countOut = 2; // no USB storage
		osLib_returnFromFunction(hCPU, 0);
	}

	void Export_MCP_UpdateCheckContext(PPCInterpreter_t* hCPU)
	{
		*ArgPtr<uint32be>(hCPU, 1) = 1;
		osLib_returnFromFunction(hCPU, 0);
	}

	void Export_MCP_TitleListUpdateGetNext(PPCInterpreter_t* hCPU)
	{
		// the callback would tell the caller that the title list changed; it is never invoked since
		// the system menu reacts by querying the whole list again
		osLib_returnFromFunction(hCPU, 0);
	}

	// MCP_GetOverlayAppInfo(handle, appType, ?, MCPTitleInfo* out): first title of the given app type
	void Export_MCP_GetOverlayAppInfo(PPCInterpreter_t* hCPU)
	{
		McpTitleQuery q(IOSU_MCP_GET_TITLE_LIST_BY_APP_TYPE);
		q->titleListRequest.titleCount = 1;
		q->titleListRequest.titleList = ArgPtr<MCPTitleInfo>(hCPU, 3);
		q->titleListRequest.titleListBufferSize = (uint32)sizeof(MCPTitleInfo);
		q->titleListRequest.appType = Arg(hCPU, 1);
		q.Run();
		if (q->titleListRequest.titleCount != 1)
			cemuLog_log(LogType::Force, "MCP_GetOverlayAppInfo: no title of app type 0x{:08x}", Arg(hCPU, 1));
		osLib_returnFromFunction(hCPU, 0);
	}
}

MCPHANDLE MCP_Open()
{
	return kMcpHandle;
}

void MCP_Close(MCPHANDLE mcpHandle)
{
}

sint32 MCP_GetSysProdSettings(MCPHANDLE mcpHandle, SysProdSettings* sysProdSettings)
{
	memset(sysProdSettings, 0, sizeof(SysProdSettings));
	sysProdSettings->gameRegion = (uint8)CafeSystem::GetForegroundTitleRegion();
	sysProdSettings->platformRegion = (uint8)CafeSystem::GetPlatformRegion();
	return 0;
}

namespace coreinit
{
	uint32 MCP_UpdateClearContextAsync(uint32 mcpHandle, betype<MPTR>* callbackPtr)
	{
		// nothing to clear: report success right away
		PPCCoreCallback(*callbackPtr, 0u);
		return 0;
	}

	uint32 MCP_InstallUtilGetTitleEnability(uint32 mcpHandle, uint32be* enabilityOutput, MCPTitleInfo* title)
	{
		*enabilityOutput = 1;
		return 0;
	}

	uint32 MCP_GetEcoSettings(uint32 mcpHandle, uint32be* flagCaffeineEnable, uint32be* uknFlag2, uint32be* uknFlag3)
	{
		*uknFlag2 = 0;
		*uknFlag3 = 0;
		// Quick Start enabled, so the system menu skips its Quick Start setup dialog
		*flagCaffeineEnable = 1;
		return 0;
	}

	uint32 MCP_GetTitleId(uint32 mcpHandle, uint64be* outTitleId)
	{
		*outTitleId = CafeSystem::GetForegroundTitleId();
		return 0;
	}

	uint32 MCP_RightCheckLaunchable(uint32 mcpHandle, uint64 titleId, uint32be* launchableOut)
	{
		*launchableOut = 1; // every title may be launched
		return 0;
	}

	uint32 MCP_DemoLaunchGetRemainder(uint32 mcpHandle, uint64 titleId, uint32be* outRemainder)
	{
		// demos (checked by drmapp) always have uses left
		if (outRemainder)
			*outRemainder = 99;
		return 0;
	}

	void InitializeMCP()
	{
		osLib_addFunction("coreinit", "MCP_Open", Export_MCP_Open);
		osLib_addFunction("coreinit", "MCP_Close", Export_MCP_Close);
		osLib_addFunction("coreinit", "MCP_GetSysProdSettings", Export_MCP_GetSysProdSettings);
		osLib_addFunction("coreinit", "MCP_TitleListByAppType", Export_MCP_TitleListByAppType);
		osLib_addFunction("coreinit", "MCP_TitleList", Export_MCP_TitleList);
		osLib_addFunction("coreinit", "MCP_TitleCount", Export_MCP_TitleCount);
		osLib_addFunction("coreinit", "MCP_GetTitleInfo", Export_MCP_GetTitleInfo);
		osLib_addFunction("coreinit", "MCP_GetTitleInfoByTitleAndDevice", Export_MCP_GetTitleInfoByTitleAndDevice);

		osLib_addFunction("coreinit", "MCP_TitleListByDevice", Export_MCP_TitleListByDevice);
		osLib_addFunction("coreinit", "MCP_GetSystemVersion", Export_MCP_GetSystemVersion);
		osLib_addFunction("coreinit", "MCP_Get4SecondOffStatus", Export_MCP_Get4SecondOffStatus);

		osLib_addFunction("coreinit", "MCP_DeviceList", Export_MCP_DeviceList);
		osLib_addFunction("coreinit", "MCP_FullDeviceList", Export_MCP_FullDeviceList);

		osLib_addFunction("coreinit", "MCP_UpdateCheckContext", Export_MCP_UpdateCheckContext);
		osLib_addFunction("coreinit", "MCP_TitleListUpdateGetNext", Export_MCP_TitleListUpdateGetNext);
		osLib_addFunction("coreinit", "MCP_GetOverlayAppInfo", Export_MCP_GetOverlayAppInfo);
		cafeExportRegister("coreinit", MCP_UpdateClearContextAsync, LogType::Placeholder);

		cafeExportRegister("coreinit", MCP_InstallUtilGetTitleEnability, LogType::Placeholder);
		cafeExportRegister("coreinit", MCP_RightCheckLaunchable, LogType::Placeholder);

		cafeExportRegister("coreinit", MCP_GetEcoSettings, LogType::Placeholder);

		cafeExportRegister("coreinit", MCP_GetTitleId, LogType::Placeholder);
		cafeExportRegister("coreinit", MCP_DemoLaunchGetRemainder, LogType::Placeholder);
	}
}

/* UCReadSysConfig: system configuration (SCI) values */

namespace
{
	// one requested setting
	struct UCSysConfigItem
	{
		/* +0x00 */ char name[0x40];
		/* +0x40 */ uint32 unk40;
		/* +0x44 */ uint32 unk44;
		/* +0x48 */ uint32 unk48;
		/* +0x4C */ uint32 rawSize;    // not byte swapped, see the uuid case
		/* +0x50 */ uint32be resultAddr;
	};
	static_assert(sizeof(UCSysConfigItem) == 0x54);

	enum class SettingKind
	{
		U8,
		U16,
		U32,
		CountryCode,
		Language,
		AccountUuid,
	};

	struct SysConfigSetting
	{
		const char* name;
		SettingKind kind;
		uint32 value;
	};

	// names are matched case-insensitively
	constexpr SysConfigSetting s_sysConfigSettings[] =
	{
		{ "cafe.cntry_reg", SettingKind::CountryCode, 0 },
		{ "cafe.language", SettingKind::Language, 0 },
		{ "cafe.initial_launch", SettingKind::U8, 2 },  // 2 = set up with a user (1 would start user creation)
		{ "cafe.eula_version", SettingKind::U32, 0 },
		{ "cafe.eula_agree", SettingKind::U8, 0 },
		{ "cafe.version", SettingKind::U16, 0 },
		{ "cafe.eco", SettingKind::U8, 0 },
		{ "cafe.fast_boot", SettingKind::U8, 0 },
		{ "parent.enable", SettingKind::U32, 0 },        // parental controls off
		{ "nn.act.account_repaired", SettingKind::U8, 0 },
		{ "p_acct1.net_communication_on_game", SettingKind::U8, 0 }, // 0 = online play allowed
		{ "p_acct1.int_movie", SettingKind::U8, 0 },     // 0 = videos allowed (Pikmin 3)
		{ "p_acct1.network_launcher", SettingKind::U8, 0 }, // Miiverse: 0 = unrestricted
		{ "s_acct01.uuid", SettingKind::AccountUuid, 0 },
		{ "s_acct01.nn.ec.eshop_initialized", SettingKind::U8, 1 }, // skips the eShop welcome screen
		{ "p_acct1.int_browser", SettingKind::U8, 0 },
		{ "caffeine.enable", SettingKind::U8, 1 },       // Quick Start
		{ "caffeine.ad_enable", SettingKind::U8, 0 },
		{ "caffeine.push_enable", SettingKind::U8, 0 },
		{ "caffeine.drcled_enable", SettingKind::U8, 0 },
	};

	bool SameNameIgnoreCase(const char* a, const char* b)
	{
		for (;; a++, b++)
		{
			int ca = tolower((unsigned char)*a);
			int cb = tolower((unsigned char)*b);
			if (ca != cb)
				return false;
			if (ca == 0)
				return true;
		}
	}

	void ReadSysConfigItem(UCSysConfigItem& item)
	{
		const SysConfigSetting* setting = nullptr;
		for (const SysConfigSetting& s : s_sysConfigSettings)
		{
			if (SameNameIgnoreCase(item.name, s.name))
			{
				setting = &s;
				break;
			}
		}
		if (!setting)
		{
			cemuLog_logDebug(LogType::Force, "UCReadSysConfig: unknown setting {} (size field {:08x})", item.name, item.rawSize);
			return;
		}
		MPTR out = item.resultAddr;
		if (out == MPTR_NULL)
			return;
		switch (setting->kind)
		{
		case SettingKind::U8:
			memory_writeU8(out, (uint8)setting->value);
			break;
		case SettingKind::U16:
			memory_writeU16(out, (uint16)setting->value);
			break;
		case SettingKind::U32:
			memory_writeU32(out, setting->value);
			break;
		case SettingKind::CountryCode:
		{
			uint32be simpleAddress = 0;
			nn::act::GetSimpleAddressIdEx(&simpleAddress, nn::act::ACT_SLOT_CURRENT);
			memory_writeU32(out, nn::act::getCountryCodeFromSimpleAddress(simpleAddress));
			break;
		}
		case SettingKind::Language:
			memory_writeU32(out, (uint32)GetConfig().console_language.GetValue());
			break;
		case SettingKind::AccountUuid:
		{
			// written as a 36 character string. The size field is compared without byte swapping, so
			// in practice any small non-zero size is accepted
			if (item.rawSize < 37)
				break;
			StackAllocator<uint8, 16> uuidBuf;
			uint8* u = uuidBuf.GetPointer();
			nn::act::GetUuidEx(u, 1, 0);
			std::string text = fmt::format("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
				u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
			memcpy(memory_getPointerFromVirtualOffset(out), text.c_str(), text.size() + 1);
			break;
		}
		}
	}
}

// UCReadSysConfig(handle, count, UCSysConfigItem* items)
void coreinitExport_UCReadSysConfig(PPCInterpreter_t* hCPU)
{
	uint32 count = hCPU->gpr[4];
	UCSysConfigItem* items = (UCSysConfigItem*)memory_getPointerFromVirtualOffset(hCPU->gpr[5]);
	for (uint32 i = 0; i < count; i++)
		ReadSysConfigItem(items[i]);
	osLib_returnFromFunction(hCPU, 0);
}
