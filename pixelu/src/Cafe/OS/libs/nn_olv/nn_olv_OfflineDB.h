// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Cafe/OS/libs/nn_common.h"
#include "nn_olv_Common.h"
#include "nn_olv_PostTypes.h"

// Offline Miiverse: posts served from resources/miiverse/OfflineDB.zar in the
// user data folder, used while the network service is set to Nintendo.

namespace nn::olv
{
	void OfflineDB_Shutdown();

	nnResult OfflineDB_ListPosts(DownloadedTopicData* topic, DownloadedPostData* posts, uint32be* postCountOut, uint32 maxCount, DownloadPostDataListParam* param);
	nnResult OfflineDB_ReadExternalImage(DownloadedDataBase* post, void* out, uint32be* outSize, uint32 maxSize);
}
