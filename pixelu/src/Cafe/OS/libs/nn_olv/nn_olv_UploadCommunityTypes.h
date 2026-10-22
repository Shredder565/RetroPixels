// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/nn_olv/nn_olv_Common.h"

namespace nn::olv
{
	// nn::olv::UploadedCommunityData (guest object, 0x12000 bytes)
	struct UploadedCommunityData
	{
		/* +0x00000 */ CommunityRecord rec;
		/* +0x10748 */ uint8 reserved10748[0x18B8];
	};
	static_assert(sizeof(UploadedCommunityData) == 0x12000);

	// nn::olv::UploadCommunityDataParam (guest object, 0x1000 bytes)
	struct UploadCommunityDataParam
	{
		static constexpr uint32 kFlagDelete = 1 << 0;

		/* +0x000 */ uint32be flags;
		/* +0x004 */ uint32be reserved004;
		/* +0x008 */ uint64be titleId;
		/* +0x010 */ uint32be communityId;
		/* +0x014 */ uint16be title[128];
		/* +0x114 */ uint16be description[256];
		/* +0x314 */ uint16be searchKeys[5][152];
		/* +0x904 */ MEMPTR<uint8> appData;
		/* +0x908 */ uint32be appDataSize;
		/* +0x90C */ MEMPTR<uint8> icon; // uncompressed TGA
		/* +0x910 */ uint32be iconSize;
		/* +0x914 */ uint8 reserved914[0x6EC];
	};
	static_assert(sizeof(UploadCommunityDataParam) == 0x1000);
	static_assert(offsetof(UploadCommunityDataParam, title) == 0x14);
	static_assert(offsetof(UploadCommunityDataParam, appData) == 0x904);
	static_assert(offsetof(UploadCommunityDataParam, iconSize) == 0x910);

	sint32 UploadCommunityData(const UploadCommunityDataParam* pParam);
	sint32 UploadCommunityData(UploadedCommunityData* pOutData, const UploadCommunityDataParam* pParam);
}
