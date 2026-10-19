// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/IOSU/iosu_types_common.h"

#define CCR_NFC_ERROR 					(-0x2F001E)
#define CCR_NFC_INVALID_LOCKED_SECRET	(-0x2F0029)
#define CCR_NFC_INVALID_UNFIXED_INFOS	(-0x2F002A)

namespace iosu
{
	namespace ccr_nfc
	{
		// Tag image plus the layout of its protected areas. All offsets index
		// into 'data'. Exchanged with /dev/ccr_nfc in host byte order.
		struct CCRNFCCryptData
		{
			uint32 version; // 0 or 2
			uint32 dataSize;
			uint32 seedOffset;
			uint32 keyGenSaltOffset;
			uint32 uuidOffset;
			uint32 unfixedInfosOffset;
			uint32 unfixedInfosSize;
			uint32 lockedSecretOffset;
			uint32 lockedSecretSize;
			uint32 unfixedInfosHmacOffset;
			uint32 lockedSecretHmacOffset;
			uint8 data[540];
		};
		static_assert(sizeof(CCRNFCCryptData) == 0x248);

		IOSUModule* GetModule();
	}
}
