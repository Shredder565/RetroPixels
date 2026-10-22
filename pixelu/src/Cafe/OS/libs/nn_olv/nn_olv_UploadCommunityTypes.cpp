// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_UploadCommunityTypes.h"
#include "Cemu/ncrypto/ncrypto.h"

namespace nn::olv
{
	namespace
	{
		using Record = CommunityRecordApi<UploadedCommunityData>;
		using Param = UploadCommunityDataParam;

		Param* Param_Construct(Param* p)
		{
			if (!p)
				return nullptr;
			p->flags = 0;
			p->titleId = 0;
			p->communityId = 0;
			p->appData = nullptr;
			p->appDataSize = 0;
			p->icon = nullptr;
			p->iconSize = 0;
			memset(p->title, 0, sizeof(p->title));
			memset(p->description, 0, sizeof(p->description));
			memset(p->searchKeys, 0, sizeof(p->searchKeys));
			return p;
		}

		sint32 Param_SetFlags(Param* p, uint32 flags)
		{
			p->flags = flags;
			return kOlvSuccess;
		}

		sint32 Param_SetCommunityId(Param* p, uint32 communityId)
		{
			if (communityId == 0xFFFFFFFF)
				return kOlvInvalidParameter;
			p->communityId = communityId;
			return kOlvSuccess;
		}

		sint32 Param_SetAppData(Param* p, MEMPTR<uint8> data, uint32 size)
		{
			if (data.IsNull())
			{
				p->appData = nullptr;
				p->appDataSize = 0;
				return kOlvSuccess;
			}
			if (size == 0 || size > 0x400)
				return kOlvNotEnoughSize;
			p->appData = data;
			p->appDataSize = size;
			return kOlvSuccess;
		}

		sint32 Param_SetTitleText(Param* p, const uint16be* text)
		{
			if (!text)
			{
				memset(p->title, 0, sizeof(p->title));
				return kOlvSuccess;
			}
			return WideAssignChecked(p->title, std::size(p->title), text);
		}

		sint32 Param_SetDescriptionText(Param* p, const uint16be* text)
		{
			if (!text)
			{
				memset(p->description, 0, sizeof(p->description));
				return kOlvSuccess;
			}
			return WideAssignChecked(p->description, std::size(p->description), text);
		}

		sint32 Param_SetIconData(Param* p, MEMPTR<uint8> data, uint32 size)
		{
			if (data.IsNull())
			{
				p->icon = nullptr;
				p->iconSize = 0;
				return kOlvSuccess;
			}
			// a 128x128x4 image plus header, with room for a TGA footer
			if (size < 0x10012 || size > 0x1002C)
				return kOlvNotEnoughSize;
			if (!IsValidTga(data.GetPtr(), size, TgaKind::CommunityIcon))
				return kOlvInvalidData;
			p->icon = data;
			p->iconSize = size;
			return kOlvSuccess;
		}

		// --- upload -----------------------------------------------------------------------

		nnResult MimeError(CURLcode code)
		{
			if (code == CURLE_OK)
				return kOlvSuccess;
			return code == CURLE_OUT_OF_MEMORY ? OlvFatal(25) : OlvUsage(50);
		}

		// multipart form; declared before the request so it outlives the curl handle using it
		struct FormBuilder
		{
			curl_mime* mime = nullptr;
			nnResult result = kOlvSuccess;

			~FormBuilder() { curl_mime_free(mime); }

			void Bind(CURL* curl)
			{
				mime = curl_mime_init(curl);
				if (!mime)
					result = OlvFatal(25);
			}

			void Add(const char* name, const std::string& value)
			{
				if (NN_RESULT_IS_FAILURE(result))
					return;
				curl_mimepart* part = curl_mime_addpart(mime);
				if (!part)
				{
					result = OlvFatal(25);
					return;
				}
				result = MimeError(curl_mime_name(part, name));
				if (NN_RESULT_IS_SUCCESS(result))
					result = MimeError(curl_mime_data(part, value.data(), value.size()));
			}
		};

		nnResult SendCommunity(const std::string& url, const Param* p, UploadedCommunityData* out)
		{
			FormBuilder form;
			CurlRequestHelper req;
			BeginOliveRequest(req, url);
			form.Bind(req.getCURL());

			if (!p->icon.IsNull())
			{
				std::vector<uint8> packed(p->iconSize);
				const sint32 packedSize = DeflateTga(p->icon.GetPtr(), p->iconSize, packed.data(), (uint32)packed.size(), TgaKind::CommunityIcon);
				if (packedSize <= 0)
					return kOlvNotEnoughSize;
				form.Add("icon", NCrypto::base64Encode(packed.data(), (size_t)packedSize));
			}
			if (p->title[0] != 0)
				form.Add("name", StringHelpers::ToUtf8(p->title, std::size(p->title) - 1));
			if (p->description[0] != 0)
				form.Add("description", StringHelpers::ToUtf8(p->description, std::size(p->description) - 1));
			for (const auto& key : p->searchKeys)
				if (key[0] != 0)
					form.Add("search_key", StringHelpers::ToUtf8(key, std::size(key) - 1));
			if (!p->appData.IsNull())
				form.Add("app_data", NCrypto::base64Encode(p->appData.GetPtr(), p->appDataSize));
			if (NN_RESULT_IS_FAILURE(form.result))
			{
				cemuLog_log(LogType::Force, "nn_olv: building the community upload form failed");
				return form.result;
			}

			curl_easy_setopt(req.getCURL(), CURLOPT_MIMEPOST, form.mime);
			req.setUseMultipartFormData(true);
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

	sint32 UploadCommunityData(UploadedCommunityData* pOutData, const UploadCommunityDataParam* pParam)
	{
		if (!g_olv.initialized)
			return kOlvNotInitialized;
		if (!g_olv.online)
			return kOlvOfflineMode;
		if (!pParam)
			return kOlvInvalidPointer;
		if (pOutData)
			Record::Clear(pOutData);

		const uint32 id = pParam->communityId;
		std::string url;
		if (pParam->flags & Param::kFlagDelete)
		{
			if (id == 0)
				return kOlvInvalidParameter;
			url = fmt::format("{}/v1/communities/{}.delete", g_DiscoveryResults.apiEndpoint, id);
		}
		else if (id != 0)
			url = fmt::format("{}/v1/communities/{}", g_DiscoveryResults.apiEndpoint, id);
		else
			url = fmt::format("{}/v1/communities", g_DiscoveryResults.apiEndpoint);

		return RunBlockingOnHost([&]() { return SendCommunity(url, pParam, pOutData); });
	}

	sint32 UploadCommunityData(const UploadCommunityDataParam* pParam)
	{
		return UploadCommunityData(nullptr, pParam);
	}

	void RegisterUploadCommunityExports()
	{
		OLV_REGISTER_COMMUNITY_RECORD(UploadedCommunityData, "21UploadedCommunityData");

		cafeExportRegisterFunc(Param_Construct, "nn_olv", "__ct__Q3_2nn3olv24UploadCommunityDataParamFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetFlags, "nn_olv", "SetFlags__Q3_2nn3olv24UploadCommunityDataParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetCommunityId, "nn_olv", "SetCommunityId__Q3_2nn3olv24UploadCommunityDataParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetAppData, "nn_olv", "SetAppData__Q3_2nn3olv24UploadCommunityDataParamFPCUcUi", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetTitleText, "nn_olv", "SetTitleText__Q3_2nn3olv24UploadCommunityDataParamFPCw", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetDescriptionText, "nn_olv", "SetDescriptionText__Q3_2nn3olv24UploadCommunityDataParamFPCw", LogType::NN_OLV);
		cafeExportRegisterFunc(Param_SetIconData, "nn_olv", "SetIconData__Q3_2nn3olv24UploadCommunityDataParamFPCUcUi", LogType::NN_OLV);

		cafeExportRegisterFunc((sint32(*)(const UploadCommunityDataParam*))UploadCommunityData,
			"nn_olv", "UploadCommunityData__Q2_2nn3olvFPCQ3_2nn3olv24UploadCommunityDataParam", LogType::NN_OLV);
		cafeExportRegisterFunc((sint32(*)(UploadedCommunityData*, const UploadCommunityDataParam*))UploadCommunityData,
			"nn_olv", "UploadCommunityData__Q2_2nn3olvFPQ3_2nn3olv21UploadedCommunityDataPCQ3_2nn3olv24UploadCommunityDataParam", LogType::NN_OLV);
	}
}
