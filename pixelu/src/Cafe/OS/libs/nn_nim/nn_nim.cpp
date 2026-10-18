// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_nim is the network install manager: system updates, title downloads
// queued by the eShop and the download manager, plus the icon database
// (IDBE) of installed titles. PixelU never downloads anything in the
// background, so update and scheduler queries say "nothing pending". The
// package list and icon lookups go to PixelU's /dev/nim service
// (iosu_nim.cpp), which answers them from the installed title list.

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/IOSU/legacy/iosu_ioctl.h"
#include "Cafe/IOSU/legacy/iosu_nim.h"
#include "Cafe/OS/libs/coreinit/coreinit_IOS.h"
#include "Cafe/OS/libs/nn_common.h"
#include "nn_nim.h"

namespace nn::nim
{
	namespace
	{
		using PackageInfo = iosu::nim::titlePackageInfo_t;
		static_assert(sizeof(PackageInfo) == 0x50);

		constexpr uint32 kIconEntrySize = 0x12060; // one IDBE record

		// Sends one request to PixelU's /dev/nim and returns it with the answer filled in.
		struct NimCall
		{
			StackAllocator<iosu::nim::iosuNimCemuRequest_t> request;
			StackAllocator<ioBufferVector_t> vector;

			explicit NimCall(uint32 code)
			{
				memset(request.GetPointer(), 0, sizeof(iosu::nim::iosuNimCemuRequest_t));
				memset(vector.GetPointer(), 0, sizeof(ioBufferVector_t));
				request->requestCode = code;
				vector->buffer = (uint8*)request.GetPointer();
			}

			iosu::nim::iosuNimCemuRequest_t* operator->() { return request.GetPointer(); }

			void Send()
			{
				__depr__IOS_Ioctlv(IOS_DEVICE_NIM, IOSU_NIM_REQUEST_CEMU, 1, 1, vector.GetPointer());
			}
		};

		// UpdatePackageProgress, filled with "no update running"
		struct UpdateProgress
		{
			uint32be field[7];
		};
		static_assert(sizeof(UpdateProgress) == 0x1C);

		// TitlePackageTaskConfig, built by the eShop before queueing a download
		struct TaskConfig
		{
			uint32be titleIdHigh;
			uint32be titleIdLow;
			uint32be languageOrRegion;
			uint8 titleType;
			uint8 targetDevice;
			uint8 unk0E;
			uint8 targetDevice2;
			uint32be unk10;
			uint8 unk14;
			uint8 unk15;
			uint8 afterDownload;
			uint8 unk17;
		};
		static_assert(sizeof(TaskConfig) == 0x18);

		struct ResultError
		{
			uint32be code;
			uint32be extra;
		};
		static_assert(sizeof(ResultError) == 8);

		uint32 NeedsNetworkUpdate(uint8* needsUpdate)
		{
			*needsUpdate = 0; // the console is always up to date
			return 0;
		}

		uint32 GetUpdatePackageProgress(UpdateProgress* progress)
		{
			progress->field[0] = 0;
			progress->field[1] = 0;
			progress->field[6] = 0;
			return 0;
		}

		uint32 NeedsNotifyToUsersUpdate(UpdateProgress* progress)
		{
			return 0; // nothing to tell the user
		}

		uint32 NeedsNotifyToUsersPackage(PackageInfo* info, ResultError* error)
		{
			return 0;
		}

		uint32 GetNumTitlePackages()
		{
			NimCall call(IOSU_NIM_GET_PACKAGE_COUNT);
			call.Send();
			return call->resultU32.u32;
		}

		uint32 ListTitlePackagesStatically(uint64be* titleIds, sint32 maxCount)
		{
			NimCall call(IOSU_NIM_GET_PACKAGES_TITLEID);
			call->maxCount = maxCount;
			call->ptr = titleIds;
			call.Send();
			return 0;
		}

		uint32 GetTitlePackageInfos(PackageInfo* infos, uint64be* titleIds, uint32 count)
		{
			NimCall call(IOSU_NIM_GET_PACKAGES_INFO);
			call->maxCount = count;
			call->ptr = titleIds;
			call->ptr2 = infos;
			call.Send();
			return 0;
		}

		uint32 GetIconDatabaseEntries(uint8* entries, uint64be* titleIds, sint32 count)
		{
			for (sint32 i = 0; i < count; i++)
			{
				NimCall call(IOSU_NIM_GET_ICON_DATABASE_ENTRY);
				call->titleId = titleIds[i];
				call->ptr = entries + (size_t)i * kIconEntrySize;
				call.Send();
			}
			return 0;
		}

		// SchedulerStatus: byte 0 = 1 means "downloading", byte 1 = 1 means "paused".
		// Report an idle, running scheduler.
		uint32 QuerySchedulerStatus(uint32be* status)
		{
			*status = 0;
			return 0;
		}

		// Turns an nn::Result into the code shown by the error viewer. Only IOS
		// results carry a code of their own; everything else shows as 0.
		void ConstructResultError(ResultError* error, uint32be* result, uint32 extra)
		{
			const uint32 code = *result;
			error->code = code;
			error->extra = extra;
			if (code == 0xFFFFFFFF) // "no result", used by the system menu
				return;
			if ((code & 0x18000000) == 0x18000000)
			{
				cemuLog_log(LogType::Force, "nn_nim: ResultError for legacy-format result {:08x} not handled", code);
				return;
			}
			if (nn::nnResult_GetModule(code) == NN_RESULT_MODULE_NN_IOS)
			{
				cemuLog_log(LogType::Force, "nn_nim: ResultError for IOS result {:08x} not handled", code);
				return;
			}
			error->code = 0;
		}

		uint32 GetECommerceInfrastructureCountry(uint32be* country)
		{
			*country = 0;
			return 0;
		}

		uint32 MakeTaskConfigAutoUsingBgInstallPolicy(TaskConfig* config, uint64 titleId, uint32 languageOrRegion, uint32 titleType)
		{
			config->titleIdHigh = (uint32)(titleId >> 32);
			config->titleIdLow = (uint32)titleId;
			config->languageOrRegion = 0;
			config->titleType = (uint8)titleType;
			config->targetDevice = 1; // internal storage (MLC)
			config->unk0E = 0;
			config->targetDevice2 = 1;
			config->unk10 = 0;
			config->unk14 = 0;
			config->unk15 = 1;
			config->afterDownload = 0;
			config->unk17 = 0;
			return 0;
		}

		uint32 CalculateTitleInstallSize(uint64be* sizeOut, TaskConfig* config, uint16be* contentIndices, uint32 contentCount)
		{
			*sizeOut = 0xFFFFFFFFFFFFFFFFull; // unknown
			return 0;
		}

		class NimLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_nim"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(NeedsNetworkUpdate, "nn_nim", "NeedsNetworkUpdate__Q2_2nn3nimFPb", LogType::Placeholder);
				cafeExportRegisterFunc(GetUpdatePackageProgress, "nn_nim", "GetUpdatePackageProgress__Q2_2nn3nimFPQ3_2nn3nim21UpdatePackageProgress", LogType::Placeholder);
				cafeExportRegisterFunc(NeedsNotifyToUsersUpdate, "nn_nim", "NeedsNotifyToUsers__Q3_2nn3nim4utilFPCQ3_2nn3nim21UpdatePackageProgress", LogType::Placeholder);
				cafeExportRegisterFunc(GetNumTitlePackages, "nn_nim", "GetNumTitlePackages__Q2_2nn3nimFv", LogType::Placeholder);
				cafeExportRegisterFunc(GetTitlePackageInfos, "nn_nim", "GetTitlePackageInfos__Q2_2nn3nimFPQ3_2nn3nim16TitlePackageInfoPCULUi", LogType::Placeholder);
				cafeExportRegisterFunc(NeedsNotifyToUsersPackage, "nn_nim", "NeedsNotifyToUsers__Q3_2nn3nim4utilFPCQ3_2nn3nim16TitlePackageInfoPCQ3_2nn3nim11ResultError", LogType::Placeholder);
				cafeExportRegisterFunc(ListTitlePackagesStatically, "nn_nim", "ListTitlePackagesStatically__Q2_2nn3nimFPULUi", LogType::Placeholder);
				cafeExportRegisterFunc(GetECommerceInfrastructureCountry, "nn_nim", "GetECommerceInfrastructureCountry__Q2_2nn3nimFPQ3_2nn3nim7Country", LogType::Placeholder);
				cafeExportRegisterFunc(QuerySchedulerStatus, "nn_nim", "QuerySchedulerStatus__Q2_2nn3nimFPQ3_2nn3nim15SchedulerStatus", LogType::Placeholder);
				cafeExportRegisterFunc(GetIconDatabaseEntries, "nn_nim", "GetIconDatabaseEntries__Q2_2nn3nimFPQ3_2nn3nim17IconDatabaseEntryPCULUi", LogType::Placeholder);
				cafeExportRegisterFunc(ConstructResultError, "nn_nim", "Construct__Q3_2nn3nim11ResultErrorFQ2_2nn6Resulti", LogType::Placeholder);
				cafeExportRegisterFunc(MakeTaskConfigAutoUsingBgInstallPolicy, "nn_nim", "MakeTitlePackageTaskConfigAutoUsingBgInstallPolicy__Q3_2nn3nim4utilFULiQ3_2nn4Cafe9TitleType", LogType::Placeholder);
				cafeExportRegisterFunc(CalculateTitleInstallSize, "nn_nim", "CalculateTitleInstallSize__Q2_2nn3nimFPLRCQ3_2nn3nim22TitlePackageTaskConfigPCUsUi", LogType::Placeholder);
			}
		};

		NimLibrary g_nimLibrary;
	}

	COSModule* GetModule()
	{
		return &g_nimLibrary;
	}
}
