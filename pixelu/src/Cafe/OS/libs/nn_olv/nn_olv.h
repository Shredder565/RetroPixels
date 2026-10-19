// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/OS/libs/nn_common.h"
#include "Cafe/OS/libs/nn_act/nn_act.h"
#include "Cafe/CafeSystem.h"
#include "Cemu/napi/napi.h"
#include "Cafe/OS/RPL/COSModule.h"

#include "nn_olv_Common.h"

// nn_olv: the Miiverse client library

namespace nn::olv
{
	extern ParamPackStorage g_ParamPack;
	extern DiscoveryResultStorage g_DiscoveryResults;

	// the running title's Olive access key; fails with status 1102 when the title has none
	sint32 GetOlvAccessKey(uint32* pOutKey);

	COSModule* GetModule();
}
