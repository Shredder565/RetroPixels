// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/nn_olv/nn_olv_Common.h"

namespace nn::olv
{
	// nn::olv::DownloadedCommunityData (guest object, 0x12000 bytes)
	struct DownloadedCommunityData
	{
		/* +0x00000 */ CommunityRecord rec;
		/* +0x10748 */ uint8 ownerMii[96]; // FFLStoreData
		/* +0x107A8 */ uint16be ownerNickname[32];
		/* +0x107E8 */ uint8 reserved107E8[0x1818];
	};
	static_assert(sizeof(DownloadedCommunityData) == 0x12000);
	static_assert(offsetof(DownloadedCommunityData, ownerNickname) == 0x107A8);

	// nn::olv::DownloadCommunityDataListParam (guest object, 0x1000 bytes)
	struct DownloadCommunityDataListParam
	{
		static constexpr uint32 kFilterFavorites = 1 << 0;
		static constexpr uint32 kFilterOfficial = 1 << 1;
		static constexpr uint32 kFilterOwned = 1 << 2;
		static constexpr uint32 kWithMii = 1 << 3;
		static constexpr uint32 kWithIcon = 1 << 4;
		static constexpr uint32 kNoId = 0xFFFFFFFE; // unused slot of the id list
		static constexpr size_t kIdSlots = 20;

		/* +0x000 */ uint32be flags;
		/* +0x004 */ uint32be communityId;
		/* +0x008 */ uint32be maxCount;
		/* +0x00C */ uint32be idList[kIdSlots];
		/* +0x05C */ uint8 reserved05C[0xFA4];
	};
	static_assert(sizeof(DownloadCommunityDataListParam) == 0x1000);
	static_assert(offsetof(DownloadCommunityDataListParam, idList) == 0x0C);

	sint32 DownloadCommunityDataList(DownloadedCommunityData* pOutList, uint32be* pOutNum, uint32 numMaxList, const DownloadCommunityDataListParam* pParam);
}
