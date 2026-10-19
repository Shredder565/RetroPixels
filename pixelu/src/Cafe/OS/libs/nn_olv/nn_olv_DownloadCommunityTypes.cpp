// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_DownloadCommunityTypes.h"
#include "Cemu/ncrypto/ncrypto.h"

namespace nn::olv
{
	namespace
	{
		using Record = CommunityRecordApi<DownloadedCommunityData>;
		using ListParam = DownloadCommunityDataListParam;

		// --- DownloadedCommunityData extras -----------------------------------------

		sint32 GetOwnerMiiData(DownloadedCommunityData* self, void* out)
		{
			if (!Record::TestFlags(self, CommunityFlag::Mii))
				return kOlvMissingData;
			if (!out)
				return kOlvInvalidPointer;
			memcpy(out, self->ownerMii, sizeof(self->ownerMii));
			return kOlvSuccess;
		}

		uint16be* GetOwnerMiiNickname(DownloadedCommunityData* self)
		{
			return self->ownerNickname[0] != 0 ? self->ownerNickname : nullptr;
		}

		// --- DownloadCommunityDataListParam ----------------------------------------

		uint32 UsedIdSlots(const ListParam* p)
		{
			uint32 n = 0;
			for (const uint32be& id : p->idList)
				n += (uint32)id != ListParam::kNoId ? 1 : 0;
			return n;
		}

		ListParam* ListParam_Construct(ListParam* p)
		{
			if (!p)
				return nullptr;
			p->flags = 0;
			p->maxCount = 0;
			p->communityId = 0;
			for (uint32be& id : p->idList)
				id = ListParam::kNoId;
			return p;
		}

		sint32 ListParam_SetFlags(ListParam* p, uint32 flags)
		{
			p->flags = flags;
			return kOlvSuccess;
		}

		sint32 ListParam_SetCommunityId(ListParam* p, uint32 communityId)
		{
			if (communityId == 0xFFFFFFFF)
				return kOlvInvalidParameter;
			p->communityId = communityId;
			if (communityId != 0 && p->maxCount == 0)
				p->maxCount = 1;
			return kOlvSuccess;
		}

		sint32 ListParam_SetCommunityIdAt(ListParam* p, uint32 communityId, uint8 slot)
		{
			if (communityId == 0xFFFFFFFF || slot >= ListParam::kIdSlots)
				return kOlvInvalidParameter;
			p->idList[slot] = communityId;
			const uint32 used = UsedIdSlots(p);
			if (used > p->maxCount)
				p->maxCount = used;
			return kOlvSuccess;
		}

		sint32 ListParam_SetCommunityDataMaxNum(ListParam* p, uint32 count)
		{
			if (count == 0 || UsedIdSlots(p) > count)
				return kOlvInvalidParameter;
			p->maxCount = count;
			return kOlvSuccess;
		}

		nnResult BuildListUrl(const ListParam* p, std::string& url)
		{
			const uint32 flags = p->flags;
			const uint32 used = UsedIdSlots(p);
			if (used != 0)
			{
				// a list of ids: the search goes to the last listed community and names all of them
				if (p->communityId != 0 && p->communityId != ListParam::kNoId)
					return kOlvInvalidParameter;
				if (flags & ~(ListParam::kWithMii | ListParam::kWithIcon))
					return kOlvInvalidParameter;
				uint32 lastId = 0;
				for (const uint32be& id : p->idList)
					if ((uint32)id != ListParam::kNoId)
						lastId = id;
				url = fmt::format("{}/v1/communities/{}.search", g_DiscoveryResults.apiEndpoint, lastId);
				for (const uint32be& id : p->idList)
					if ((uint32)id != ListParam::kNoId)
						AppendQuery(url, fmt::format("community_id={}", (uint32)id));
			}
			else
				url = fmt::format("{}/v1/communities", g_DiscoveryResults.apiEndpoint);

			if (p->communityId != 0)
				AppendQuery(url, fmt::format("community_id={}", (uint32)p->communityId));
			else
			{
				const bool favorites = flags & ListParam::kFilterFavorites;
				const bool official = flags & ListParam::kFilterOfficial;
				const bool owned = flags & ListParam::kFilterOwned;
				if ((int)favorites + (int)official + (int)owned != 1)
					return kOlvInvalidParameter;
				AppendQuery(url, fmt::format("limit={}", (uint32)p->maxCount));
				AppendQuery(url, favorites ? "type=favorite" : official ? "type=official" : "type=my");
			}
			if (flags & ListParam::kWithMii)
				AppendQuery(url, "with_mii=1");
			if (flags & ListParam::kWithIcon)
				AppendQuery(url, "with_icon=1");
			return kOlvSuccess;
		}

		sint32 ListParam_GetRawDataUrl(ListParam* p, char* out, uint32 outSize)
		{
			if (!g_olv.online)
				return kOlvOfflineMode;
			if (!out)
				return kOlvInvalidPointer;
			if (outSize == 0)
				return kOlvNotEnoughSize;
			std::string url;
			const nnResult r = BuildListUrl(p, url);
			if (NN_RESULT_IS_FAILURE(r))
				return r;
			// like snprintf: truncate to the buffer
			const size_t n = std::min<size_t>(url.size(), outSize - 1);
			memcpy(out, url.data(), n);
			out[n] = '\0';
			return kOlvSuccess;
		}

		// --- download -------------------------------------------------------------------

		nnResult ReadCommunities(const pugi::xml_document& reply, DownloadedCommunityData* list, uint32 capacity, uint32be* outCount)
		{
			const std::string_view requestName = reply.select_node("//request_name").node().child_value();
			if (requestName.empty())
			{
				cemuLog_log(LogType::Force, "nn_olv: community reply has no <request_name>");
				return kOlvBadXml;
			}
			if (requestName != "communities" && requestName != "specified_communities")
			{
				cemuLog_log(LogType::Force, "nn_olv: unexpected community reply \"{}\"", requestName);
				return kOlvBadXml;
			}
			pugi::xml_node communities = reply.select_node("//communities").node();
			if (!communities)
			{
				cemuLog_log(LogType::Force, "nn_olv: community reply has no <communities>");
				return kOlvBadXml;
			}

			uint32 count = 0;
			for (pugi::xml_node node : communities.children("community"))
			{
				if (count >= capacity)
					break;
				DownloadedCommunityData& entry = list[count];
				CommunityReply fields;
				fields.appData = node.child_value("app_data");
				fields.communityId = node.child_value("community_id");
				fields.name = node.child_value("name");
				fields.description = node.child_value("description");
				fields.pid = node.child_value("pid");
				fields.icon = node.child_value("icon");
				const nnResult r = StoreCommunityReply(entry.rec, fields);
				if (NN_RESULT_IS_FAILURE(r))
					return r;

				const std::string_view mii = node.child_value("mii");
				if (!mii.empty())
				{
					const std::vector<uint8> bytes = NCrypto::base64Decode(mii);
					if (bytes.empty())
						return kOlvBadTextField;
					memcpy(entry.ownerMii, bytes.data(), std::min(bytes.size(), sizeof(entry.ownerMii)));
					entry.rec.flags = entry.rec.flags | CommunityFlag::Mii;
				}
				const std::string_view screenName = node.child_value("screen_name");
				if (!screenName.empty() && Utf8ToWideFixed(entry.ownerNickname, std::size(entry.ownerNickname), screenName) == 0)
					return kOlvBadTextField;
				count++;
			}
			*outCount = count;
			// the library answers 0 rather than the success code once something was found
			return count > 0 ? 0 : kOlvSuccess;
		}
	}

	sint32 DownloadCommunityDataList(DownloadedCommunityData* pOutList, uint32be* pOutNum, uint32 numMaxList, const DownloadCommunityDataListParam* pParam)
	{
		if (!g_olv.initialized)
			return kOlvNotInitialized;
		if (!g_olv.online)
			return kOlvOfflineMode;
		if (!pOutList || !pOutNum || !pParam)
			return kOlvInvalidPointer;
		if (numMaxList == 0)
			return kOlvNotEnoughSize;
		for (uint32 i = 0; i < numMaxList; i++)
			Record::Clear(pOutList + i);

		std::string url;
		const nnResult urlResult = BuildListUrl(pParam, url);
		if (NN_RESULT_IS_FAILURE(urlResult))
			return urlResult;
		if (url.size() >= 2048)
			url.resize(2047);

		return RunBlockingOnHost([&]() -> nnResult {
			CurlRequestHelper req;
			BeginOliveRequest(req, url);
			pugi::xml_document reply;
			const nnResult r = FinishOliveRequest(req, url, false, reply);
			if (NN_RESULT_IS_FAILURE(r))
				return r;
			return ReadCommunities(reply, pOutList, numMaxList, pOutNum);
		});
	}

	void RegisterDownloadCommunityExports()
	{
		OLV_REGISTER_COMMUNITY_RECORD(DownloadedCommunityData, "23DownloadedCommunityData");
		cafeExportRegisterFunc(GetOwnerMiiData, "nn_olv", "GetOwnerMiiData__Q3_2nn3olv23DownloadedCommunityDataCFP12FFLStoreData", LogType::NN_OLV);
		cafeExportRegisterFunc(GetOwnerMiiNickname, "nn_olv", "GetOwnerMiiNickname__Q3_2nn3olv23DownloadedCommunityDataCFv", LogType::NN_OLV);

		cafeExportRegisterFunc(ListParam_Construct, "nn_olv", "__ct__Q3_2nn3olv30DownloadCommunityDataListParamFv", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetFlags, "nn_olv", "SetFlags__Q3_2nn3olv30DownloadCommunityDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetCommunityDataMaxNum, "nn_olv", "SetCommunityDataMaxNum__Q3_2nn3olv30DownloadCommunityDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_GetRawDataUrl, "nn_olv", "GetRawDataUrl__Q3_2nn3olv30DownloadCommunityDataListParamCFPcUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetCommunityId, "nn_olv", "SetCommunityId__Q3_2nn3olv30DownloadCommunityDataListParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(ListParam_SetCommunityIdAt, "nn_olv", "SetCommunityId__Q3_2nn3olv30DownloadCommunityDataListParamFUiUc", LogType::NN_OLV);
		cafeExportRegisterFunc(DownloadCommunityDataList, "nn_olv", "DownloadCommunityDataList__Q2_2nn3olvFPQ3_2nn3olv23DownloadedCommunityDataPUiUiPCQ3_2nn3olv30DownloadCommunityDataListParam", LogType::NN_OLV);
	}
}
