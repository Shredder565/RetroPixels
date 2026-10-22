// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Written from scratch.
//
// nn_spm lists the storage devices attached to the console (internal NAND
// and USB drives) for the Wii U Menu and System Settings. PixelU keeps all
// titles in its emulated internal storage and does not emulate USB storage,
// so this library reports that no extended storage exists.
//
// GetStorageList reports a single entry and leaves the caller's buffer as it
// is: the Wii U Menu complains about extra USB drives if more are listed,
// and nothing is ever opened through that entry.

#include "Cafe/OS/common/OSCommon.h"
#include "nn_spm.h"

namespace nn::spm
{
	namespace
	{
		// 64-bit storage handle, stored as two big-endian halves
		struct StorageIndex
		{
			uint32be high;
			uint32be low;
		};
		static_assert(sizeof(StorageIndex) == 8);

		struct VolumeId
		{
			char text[16];
		};
		static_assert(sizeof(VolumeId) == 16);

		struct StorageListItem
		{
			StorageIndex index;
			uint32be _unknown;
			uint32be format; // 0 = raw, 1 = WFS
		};
		static_assert(sizeof(StorageListItem) == 16);

		struct StorageInfo
		{
			char mountPath[640];
			char connection[8];
			char formatName[8];
			uint8 _unknown[4];
			uint32be format;
			VolumeId volumeId;
		};
		static_assert(sizeof(StorageInfo) == 680);

		constexpr sint32 kOk = 0;
		constexpr sint32 kNoStorage = -1;

		// VolumeId GetDefaultExtendedStorageVolumeId(); the result is written
		// through the hidden return pointer. No USB drive -> zeroed id.
		sint32 GetDefaultExtendedStorageVolumeId(StorageIndex* result)
		{
			if (result)
				*result = {};
			return kOk;
		}

		sint32 GetExtendedStorageIndex(StorageIndex* indexOut)
		{
			if (indexOut)
				*indexOut = {};
			return kNoStorage;
		}

		uint32 GetStorageList(StorageListItem* items, uint32 capacity)
		{
			return 1;
		}

		sint32 GetStorageInfo(StorageInfo* infoOut, StorageIndex* index)
		{
			cemuLog_logDebug(LogType::Force, "nn_spm: GetStorageInfo() called but storage info is not emulated");
			return kOk;
		}

		sint32 CompareVolumeId(VolumeId* self, VolumeId* other)
		{
			return (sint32)strncmp(self->text, other->text, sizeof(self->text));
		}

		sint32 WaitStateUpdated(uint64be* stateOut)
		{
			*stateOut = 1;
			return kOk;
		}

		class SpmLibrary final : public COSModule
		{
		public:
			std::string_view GetName() override { return "nn_spm"; }

			void RPLMapped() override
			{
				cafeExportRegisterFunc(GetDefaultExtendedStorageVolumeId, "nn_spm", "GetDefaultExtendedStorageVolumeId__Q2_2nn3spmFv", LogType::Placeholder);
				cafeExportRegisterFunc(GetExtendedStorageIndex, "nn_spm", "GetExtendedStorageIndex__Q2_2nn3spmFPQ3_2nn3spm12StorageIndex", LogType::Placeholder);
				cafeExportRegisterFunc(GetStorageList, "nn_spm", "GetStorageList__Q2_2nn3spmFPQ3_2nn3spm15StorageListItemUi", LogType::Placeholder);
				cafeExportRegisterFunc(GetStorageInfo, "nn_spm", "GetStorageInfo__Q2_2nn3spmFPQ3_2nn3spm11StorageInfoQ3_2nn3spm12StorageIndex", LogType::Placeholder);
				cafeExportRegisterFunc(CompareVolumeId, "nn_spm", "Compare__Q3_2nn3spm8VolumeIdCFRCQ3_2nn3spm8VolumeId", LogType::Placeholder);
				cafeExportRegisterFunc(WaitStateUpdated, "nn_spm", "WaitStateUpdated__Q2_2nn3spmFPUL", LogType::Placeholder);
			}
		};

		SpmLibrary g_spmLibrary;
	}

	COSModule* GetModule()
	{
		return &g_spmLibrary;
	}
}
