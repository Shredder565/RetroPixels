// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_UploadFavoriteTypes.h"

namespace nn::olv
{
	namespace
	{
		using Record = CommunityRecordApi<UploadedFavoriteToCommunityData>;
		using Param = UploadFavoriteToCommunityDataParam;

		Param* Param_Construct(Param* p)
		{
			if (!p)
				return nullptr;
			p->communityId = 0;
			p->flags = 0;
			return p;
		}

		sint32 Param_SetFlags(Param* p, uint32 flags)
		{
			p->flags = flags;
			return kOlvSuccess;
		}

		sint32 Param_SetCommunityCode(Param* p, const char* code)
		{
			if (!code || strnlen(code, 13) != 12)
				return kOlvBadTextField;
			uint32 id;
			if (!CommunityIdFromCode(&id, code))
				return OlvStatus(1901);
			p->communityId = id;
			return kOlvSuccess;
		}

		sint32 Param_SetCommunityId(Param* p, uint32 communityId)
		{
			if (communityId == 0xFFFFFFFF)
				return kOlvInvalidParameter;
			p->communityId = communityId;
			return kOlvSuccess;
		}

		nnResult SendFavorite(const std::string& url, UploadedFavoriteToCommunityData* out)
		{
			CurlRequestHelper req;
			BeginOliveRequest(req, url);
			pugi::xml_document reply;
			const nnResult r = FinishOliveRequest(req, url, true, reply);
			if (NN_RESULT_IS_FAILURE(r) || !out)
				return r;
			CommunityReply fields;
			fields.appData = reply.select_node("//app_data").node().child_value();
			fields.communityId = reply.select_node("//community_id").node().child_value();
			fields.name = reply.select_node("//name").node().child_value();
			fields.description = reply.select_node("//description").node().child_value();
			fields.pid = reply.select_node("//pid").node().child_value();
			fields.icon = reply.select_node("//icon").node().child_value();
			return StoreCommunityReply(out->rec, fields);
		}
	}

	sint32 UploadFavoriteToCommunityData(UploadedFavoriteToCommunityData* pOutData, const UploadFavoriteToCommunityDataParam* pParam)
	{
		if (!g_olv.initialized)
			return kOlvNotInitialized;
		if (!g_olv.online)
			return kOlvOfflineMode;
		if (!pParam)
			return kOlvInvalidPointer;
		if (pOutData)
			Record::Clear(pOutData);
		const char* action = (pParam->flags & Param::kFlagRemove) ? "unfavorite" : "favorite";
		const std::string url = fmt::format("{}/v1/communities/{}.{}", g_DiscoveryResults.apiEndpoint, (uint32)pParam->communityId, action);
		return RunBlockingOnHost([&]() { return SendFavorite(url, pOutData); });
	}

	sint32 UploadFavoriteToCommunityData(const UploadFavoriteToCommunityDataParam* pParam)
	{
		return UploadFavoriteToCommunityData(nullptr, pParam);
	}

	void RegisterUploadFavoriteExports()
	{
		OLV_REGISTER_COMMUNITY_RECORD(UploadedFavoriteToCommunityData, "31UploadedFavoriteToCommunityData");

		cafeExportRegisterFunc(Param_Construct, "nn_olv", "__ct__Q3_2nn3olv34UploadFavoriteToCommunityDataParamFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetFlags, "nn_olv", "SetFlags__Q3_2nn3olv34UploadFavoriteToCommunityDataParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetCommunityCode, "nn_olv", "SetCommunityCode__Q3_2nn3olv34UploadFavoriteToCommunityDataParamFPCc", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetCommunityId, "nn_olv", "SetCommunityId__Q3_2nn3olv34UploadFavoriteToCommunityDataParamFUi", LogType::NN_OLV);

		cafeExportRegisterFunc((sint32(*)(const UploadFavoriteToCommunityDataParam*))UploadFavoriteToCommunityData,
			"nn_olv", "UploadFavoriteToCommunityData__Q2_2nn3olvFPCQ3_2nn3olv34UploadFavoriteToCommunityDataParam", LogType::NN_OLV);
		cafeExportRegisterFunc((sint32(*)(UploadedFavoriteToCommunityData*, const UploadFavoriteToCommunityDataParam*))UploadFavoriteToCommunityData,
			"nn_olv", "UploadFavoriteToCommunityData__Q2_2nn3olvFPQ3_2nn3olv31UploadedFavoriteToCommunityDataPCQ3_2nn3olv34UploadFavoriteToCommunityDataParam", LogType::NN_OLV);
	}
}
