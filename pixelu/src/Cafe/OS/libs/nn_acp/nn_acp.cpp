// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_acp is the application control library: save directories, title
// metadata (meta.xml, launch info), the main application's title id and the
// network clock. Save and metadata work is done by PixelU's /dev/acp_main
// service (iosu_acp.cpp); this file is the game-facing side of it.
//
// The ACP* functions that return an ACPStatus report success whenever the
// service accepted the request; the ones that forward a service answer
// return that answer as is.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/libs/coreinit/coreinit_IOS.h"
#include "Cafe/OS/libs/coreinit/coreinit_Time.h"
#include "Cafe/OS/libs/sysapp/sysapp.h"
#include "Cafe/IOSU/legacy/iosu_acp.h"
#include "Cafe/IOSU/legacy/iosu_ioctl.h"
#include "Cafe/Filesystem/fsc.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "nn_acp.h"

namespace nn::acp
{
	namespace
	{
		constexpr uint32 kStorageMlc = 3; // ACPDeviceType / application box of internal storage

		// One request to PixelU's /dev/acp_main.
		struct AcpCall
		{
			StackAllocator<iosuAcpCemuRequest_t> request;
			StackAllocator<ioBufferVector_t> vector;

			explicit AcpCall(uint32 code)
			{
				memset(request.GetPointer(), 0, sizeof(iosuAcpCemuRequest_t));
				memset(vector.GetPointer(), 0, sizeof(ioBufferVector_t));
				request->requestCode = code;
				vector->buffer = (uint8*)request.GetPointer();
			}

			iosuAcpCemuRequest_t* operator->() { return request.GetPointer(); }

			uint32 Send()
			{
				__depr__IOS_Ioctlv(IOS_DEVICE_ACP_MAIN, IOSU_ACP_REQUEST_CEMU, 1, 1, vector.GetPointer());
				return request->returnCode;
			}
		};

		bool IsSystemTitle(uint64 titleId)
		{
			const uint32 kind = (uint32)(titleId >> 32) & 0xFF;
			return kind == 0x10 || kind == 0x30; // system titles and applets
		}

		void WarnUnusualDevice(const char* function, uint32 device)
		{
			if (device != kStorageMlc)
				cemuLog_logDebug(LogType::Force, "nn_acp: {} called for storage device {}, answering for internal storage", function, device);
		}

		// game-facing exports

		ACPStatus CheckApplicationDeviceEmulation(uint32be* emulated)
		{
			*emulated = 0;
			return ACPStatus::SUCCESS;
		}

		ACPStatus CreateSaveDirEx(uint8 accountSlot, uint64 titleId)
		{
			AcpCall call(IOSU_ACP_CREATE_SAVE_DIR_EX);
			call->accountSlot = accountSlot;
			call->titleId = titleId;
			call.Send();
			return ACPStatus::SUCCESS;
		}

		uint32 GetSaveDataTitleIdList(uint32 device, acpTitleId_t* titleIds, uint32 maxCount, uint32be* countOut)
		{
			WarnUnusualDevice("ACPGetSaveDataTitleIdList", device);
			AcpCall call(IOSU_ACP_GET_SAVE_DATA_TITLE_ID_LIST);
			call->ptr = titleIds;
			call->maxCount = maxCount;
			call->type = device;
			const uint32 result = call.Send();
			*countOut = call->resultU32.u32;
			return result;
		}

		uint32 GetTitleSaveMetaXml(uint64 titleId, acpMetaXml_t* metaXml, uint32 device)
		{
			WarnUnusualDevice("ACPGetTitleSaveMetaXml", device);
			AcpCall call(IOSU_ACP_GET_TITLE_SAVE_META_XML);
			call->ptr = metaXml;
			call->titleId = titleId;
			call->type = device;
			return call.Send();
		}

		uint32 GetTitleSaveDirEx(uint64 titleId, uint32 device, uint32 unknown, acpSaveDirInfo_t* dirs, uint32 maxCount, uint32be* countOut)
		{
			WarnUnusualDevice("ACPGetTitleSaveDirEx", device);
			AcpCall call(IOSU_ACP_GET_TITLE_SAVE_DIR);
			call->ptr = dirs;
			call->titleId = titleId;
			call->type = device;
			call->maxCount = maxCount;
			const uint32 result = call.Send();
			*countOut = call->resultU32.u32;
			return result;
		}

		uint32 CheckTitleNotReferAccountLaunch(uint64 titleId)
		{
			return 0;
		}

		uint32 GetLaunchMetaData(acpMetaData_t* metaData)
		{
			AcpCall call(IOSU_ACP_GET_TITLE_META_DATA);
			call->ptr = metaData;
			call->titleId = CafeSystem::GetForegroundTitleId();
			return call.Send();
		}

		uint32 GetLaunchMetaXml(acpMetaXml_t* metaXml)
		{
			AcpCall call(IOSU_ACP_GET_TITLE_META_XML);
			call->ptr = metaXml;
			call->titleId = CafeSystem::GetForegroundTitleId();
			return call.Send();
		}

		// An applet's "main application" is the system menu.
		uint32 GetTitleIdOfMainApplication(uint64be* titleIdOut)
		{
			const uint64 running = CafeSystem::GetForegroundTitleId();
			if (((running >> 32) & 0xFF) == 0x30)
				*titleIdOut = _SYSGetSystemApplicationTitleId(0);
			else
				*titleIdOut = running;
			return 0;
		}

		uint32 GetTitleMetaDirByDevice(uint64 titleId, char* pathOut, uint32 pathSize, uint32 device)
		{
			WarnUnusualDevice("ACPGetTitleMetaDirByDevice", device);
			const std::string path = fmt::format("/vol/storage_mlc01/{}/title/{:08x}/{:08x}/meta",
				IsSystemTitle(titleId) ? "sys" : "usr", (uint32)(titleId >> 32), (uint32)titleId);
			const size_t room = pathSize ? pathSize : path.size() + 1;
			const size_t len = std::min(path.size(), room - 1);
			memcpy(pathOut, path.data(), len);
			pathOut[len] = '\0';
			return 0;
		}

		uint32 GetTitleMetaXmlByDevice(uint64 titleId, acpMetaXml_t* metaXml, uint32 device)
		{
			WarnUnusualDevice("ACPGetTitleMetaXmlByDevice", device);
			AcpCall call(IOSU_ACP_GET_TITLE_META_XML);
			call->ptr = metaXml;
			call->titleId = titleId;
			return call.Send();
		}

		// Parental controls are not emulated: every account is old enough.
		uint32 IsOverAgeEx(uint32 age, uint8 accountSlot)
		{
			return 1;
		}

		// The network clock is the console clock moved onto UTC.
		uint32 GetNetworkTime(uint64be* timeOut, uint32be* valid)
		{
			const sint64 utcOffsetTicks = (sint64)(ppcCyclesSince2000_UTC - ppcCyclesSince2000) / 20;
			*timeOut = (uint64)((sint64)coreinit::OSGetTime() + utcOffsetTicks);
			*valid = 1; // the eShop and some games refuse a clock that isn't marked valid
			return 0;
		}

		uint32 ConvertNetworkTimeToOSCalendarTime(uint64 networkTime, coreinit::OSCalendarTime_t* calendar)
		{
			coreinit::OSTicksToCalendarTime(networkTime, calendar);
			return 0;
		}
	}

	ACPStatus ACPGetApplicationBox(uint32be* applicationBox, uint64 titleId)
	{
		*applicationBox = kStorageMlc;
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPGetOlvAccesskey(uint32be* accessKey)
	{
		iosu::acp::ACPGetOlvAccesskey(accessKey);
		return ACPStatus::SUCCESS;
	}

	// Maps the running title's save folder in the MLC to /vol/save/.
	ACPStatus ACPMountSaveDir()
	{
		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		// saves of a title, its updates and its DLC share one folder: drop the 0xC type bits
		const uint32 high = (uint32)(titleId >> 32) & ~0xCu;
		const uint32 low = (uint32)titleId;
		const fs::path hostPath = ActiveSettings::GetMlcPath("usr/save/{:08x}/{:08x}/user/", high, low);
		FSCDeviceHostFS_Mount("/vol/save/", _pathToUtf8(hostPath), FSC_PRIORITY_BASE);
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPUnmountSaveDir()
	{
		fsc_unmount("/vol/save/", FSC_PRIORITY_BASE);
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPCreateSaveDir(uint32 persistentId, ACPDeviceType type)
	{
		iosu::acp::ACPCreateSaveDir(persistentId, type);
		return ACPStatus::SUCCESS;
	}

	ACPStatus ACPUpdateSaveTimeStamp(uint32 persistentId, uint64 titleId, ACPDeviceType deviceType)
	{
		iosu::acp::ACPUpdateSaveTimeStamp(persistentId, titleId, deviceType);
		return ACPStatus::SUCCESS;
	}

	uint32 ACPGetTitleMetaXml(uint64 titleId, acpMetaXml_t* metaXml)
	{
		AcpCall call(IOSU_ACP_GET_TITLE_META_XML);
		call->ptr = metaXml;
		call->titleId = titleId;
		return call.Send();
	}

	namespace
	{
		class AcpLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_acp"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(CheckApplicationDeviceEmulation, "nn_acp", "ACPCheckApplicationDeviceEmulation", LogType::Placeholder);
				cafeExportRegisterFunc(CreateSaveDirEx, "nn_acp", "ACPCreateSaveDirEx", LogType::Placeholder);
				cafeExportRegisterFunc(ACPUpdateSaveTimeStamp, "nn_acp", "ACPUpdateSaveTimeStamp", LogType::Placeholder);
				cafeExportRegisterFunc(GetSaveDataTitleIdList, "nn_acp", "ACPGetSaveDataTitleIdList", LogType::Placeholder);
				cafeExportRegisterFunc(GetTitleSaveMetaXml, "nn_acp", "ACPGetTitleSaveMetaXml", LogType::Placeholder);
				cafeExportRegisterFunc(GetTitleSaveDirEx, "nn_acp", "ACPGetTitleSaveDirEx", LogType::Placeholder);
				cafeExportRegisterFunc(CheckTitleNotReferAccountLaunch, "nn_acp", "ACPCheckTitleNotReferAccountLaunch", LogType::Placeholder);
				cafeExportRegisterFunc(GetLaunchMetaData, "nn_acp", "ACPGetLaunchMetaData", LogType::Placeholder);
				cafeExportRegisterFunc(GetLaunchMetaXml, "nn_acp", "ACPGetLaunchMetaXml", LogType::Placeholder);
				cafeExportRegisterFunc(GetTitleIdOfMainApplication, "nn_acp", "ACPGetTitleIdOfMainApplication", LogType::Placeholder);
				cafeExportRegisterFunc(GetTitleMetaDirByDevice, "nn_acp", "ACPGetTitleMetaDirByDevice", LogType::Placeholder);
				cafeExportRegisterFunc(GetTitleMetaXmlByDevice, "nn_acp", "ACPGetTitleMetaXmlByDevice", LogType::Placeholder);
				cafeExportRegisterFunc(ACPGetTitleMetaXml, "nn_acp", "ACPGetTitleMetaXml", LogType::Placeholder);
				cafeExportRegisterFunc(ACPGetApplicationBox, "nn_acp", "ACPGetApplicationBox", LogType::Placeholder);
				cafeExportRegisterFunc(ACPGetOlvAccesskey, "nn_acp", "ACPGetOlvAccesskey", LogType::Placeholder);
				cafeExportRegisterFunc(IsOverAgeEx, "nn_acp", "ACPIsOverAgeEx", LogType::Placeholder);
				cafeExportRegisterFunc(GetNetworkTime, "nn_acp", "ACPGetNetworkTime", LogType::Placeholder);
				cafeExportRegisterFunc(ConvertNetworkTimeToOSCalendarTime, "nn_acp", "ACPConvertNetworkTimeToOSCalendarTime", LogType::Placeholder);
			}
		};

		AcpLibrary g_acpLibrary;
	}

	COSModule* GetModule()
	{
		return &g_acpLibrary;
	}
}
