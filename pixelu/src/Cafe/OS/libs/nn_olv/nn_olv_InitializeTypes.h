// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "config/ActiveSettings.h"
#include "Cafe/OS/libs/nn_olv/nn_olv_Common.h"

namespace nn::olv
{
	// nn::olv::InitializeParam (guest object, 0x40 bytes)
	struct InitializeParam
	{
		static constexpr uint32 kFlagOffline = 1 << 0; // do not contact the servers

		/* +0x00 */ uint32be flags;
		/* +0x04 */ uint32be reportTypes;
		/* +0x08 */ MEMPTR<uint8> work;
		/* +0x0C */ uint32be workSize;
		/* +0x10 */ MEMPTR<const void> sysArgs;
		/* +0x14 */ uint32be sysArgsSize;
		/* +0x18 */ uint8 reserved18[0x28];
	};
	static_assert(sizeof(InitializeParam) == 0x40);
	static_assert(offsetof(InitializeParam, sysArgsSize) == 0x14);

	// nn::olv::hidden::PortalAppParam (filled for the Miiverse applet)
	struct PortalAppParam
	{
		/* +0x000 */ uint8be paramPack[0x200];
		/* +0x200 */ uint8be serviceToken[0x201];
		/* +0x401 */ uint8be startUrl[0x7FF];
	};
	static_assert(sizeof(PortalAppParam) == 0xC00);

	namespace Report
	{
		uint32 GetReportTypes();
		void SetReportTypes(uint32 reportTypes);
	}

	bool IsInitialized();
	sint32 Initialize(InitializeParam* pParam);
	sint32 InitializePortalApp(PortalAppParam* pPortalAppParam, InitializeParam* pInitializeParam);
}
