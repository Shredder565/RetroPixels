// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/nn_olv/nn_olv_Common.h"

namespace nn::olv
{
	// nn::olv::UploadedFavoriteToCommunityData (guest object, 0x12000 bytes)
	struct UploadedFavoriteToCommunityData
	{
		/* +0x00000 */ CommunityRecord rec;
		/* +0x10748 */ uint8 reserved10748[0x18B8];
	};
	static_assert(sizeof(UploadedFavoriteToCommunityData) == 0x12000);

	// nn::olv::UploadFavoriteToCommunityDataParam (guest object, 0x40 bytes)
	struct UploadFavoriteToCommunityDataParam
	{
		static constexpr uint32 kFlagRemove = 1 << 0; // unfavourite instead

		/* +0x00 */ uint32be flags;
		/* +0x04 */ uint32be communityId;
		/* +0x08 */ uint8 reserved08[0x38];
	};
	static_assert(sizeof(UploadFavoriteToCommunityDataParam) == 0x40);

	sint32 UploadFavoriteToCommunityData(const UploadFavoriteToCommunityDataParam* pParam);
	sint32 UploadFavoriteToCommunityData(UploadedFavoriteToCommunityData* pOutData, const UploadFavoriteToCommunityDataParam* pParam);
}
