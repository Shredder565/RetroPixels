// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "nn_olv_InitializeTypes.h"
#include "nn_olv.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/OS/libs/nn_act/nn_act.h"
#include "Cafe/OS/libs/coreinit/coreinit_MCP.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "config/CemuConfig.h"
#include "config/NetworkSettings.h"

namespace nn::olv
{
	namespace
	{
		constexpr const char* kClientId = "87cd32617f1985439ea608c2746e4610"; // Olive's independent service client id
		constexpr const char* kLibraryVersion = "5.0.3";

		// --- guest constructors and setters -------------------------------------------

		InitializeParam* InitializeParam_Construct(InitializeParam* p)
		{
			if (!p)
				return nullptr;
			p->flags = 0;
			p->reportTypes = 7039;
			p->work = nullptr;
			p->workSize = 0;
			p->sysArgs = nullptr;
			p->sysArgsSize = 0;
			return p;
		}

		sint32 InitializeParam_SetFlags(InitializeParam* p, uint32 flags)
		{
			p->flags = flags;
			return kOlvSuccess;
		}

		sint32 InitializeParam_SetWork(InitializeParam* p, MEMPTR<uint8> work, uint32 size)
		{
			if (work.IsNull())
				return kOlvInvalidPointer;
			if (size < 0x10000)
				return kOlvNotEnoughSize;
			p->work = work;
			p->workSize = size;
			return kOlvSuccess;
		}

		sint32 InitializeParam_SetReportTypes(InitializeParam* p, uint32 types)
		{
			p->reportTypes = types;
			return kOlvSuccess;
		}

		sint32 InitializeParam_SetSysArgs(InitializeParam* p, MEMPTR<const void> args, uint32 size)
		{
			if (args.IsNull())
				return kOlvInvalidPointer;
			if (size == 0)
				return kOlvInvalidParameter;
			p->sysArgs = args;
			p->sysArgsSize = size;
			return kOlvSuccess;
		}

		PortalAppParam* PortalAppParam_Construct(PortalAppParam* p)
		{
			if (!p)
				return nullptr;
			p->paramPack[0] = 0;
			p->serviceToken[0] = 0;
			p->startUrl[0] = 0;
			return p;
		}

		uint8be* PortalAppParam_GetParamPack(PortalAppParam* p) { return p->paramPack; }
		uint8be* PortalAppParam_GetServiceToken(PortalAppParam* p) { return p->serviceToken; }
		uint8be* PortalAppParam_GetStartUrl(PortalAppParam* p) { return p->startUrl; }

		// --- param pack ---------------------------------------------------------------

		void BuildParamPack(uint64 titleId, uint32 accessKey)
		{
			ParamPackStorage& pp = g_ParamPack;
			pp.titleId = titleId;
			pp.accessKey = accessKey;
			pp.platformId = 1;
			pp.languageId = (uint8)GetConfig().console_language.GetValue();

			uint32be simpleAddress = 0;
			nn::act::GetSimpleAddressIdEx(&simpleAddress, nn::act::ACT_SLOT_CURRENT);
			pp.countryId = (uint8)nn::act::getCountryCodeFromSimpleAddress(simpleAddress);
			pp.areaId = (uint8)((uint32)simpleAddress >> 8);

			SysProdSettings prod;
			MCPHANDLE mcp = MCP_Open();
			MCP_GetSysProdSettings(mcp, &prod);
			MCP_Close(mcp);
			pp.regionId = prod.platformRegion;

			pp.networkRestriction = 0;
			pp.friendRestriction = 0;
			pp.ratingRestriction = 18;
			pp.ratingOrganization = 4;

			uint64 transferableId = 0;
			nn::act::GetTransferableIdEx(&transferableId, (uint32)(titleId >> 8) & 0xFFFFF, nn::act::ACT_SLOT_CURRENT);
			pp.transferableId = transferableId;

			strcpy(pp.tzName, "CEMU/Olive"); // the servers accept any zone name
			pp.utcOffset = nn::act::GetUtcOffset() / 1'000'000; // microseconds -> seconds

			// \key\value\key\value\ ... with a trailing backslash
			std::string text;
			auto add = [&text](std::string_view key, auto value) {
				text += fmt::format("\\{}\\{}", key, value);
			};
			add("title_id", pp.titleId);
			add("access_key", pp.accessKey);
			add("platform_id", pp.platformId);
			add("region_id", (uint32)pp.regionId);
			add("language_id", (uint32)pp.languageId);
			add("country_id", (uint32)pp.countryId);
			add("area_id", (uint32)pp.areaId);
			add("network_restriction", (uint32)pp.networkRestriction);
			add("friend_restriction", (uint32)pp.friendRestriction);
			add("rating_restriction", pp.ratingRestriction);
			add("rating_organization", (uint32)pp.ratingOrganization);
			add("transferable_id", pp.transferableId);
			add("tz_name", std::string_view(pp.tzName));
			add("utc_offset", pp.utcOffset);
			text += "\\";

			const std::string encoded = NCrypto::base64Encode(text.data(), text.size());
			memset(pp.encodedParamPack, 0, sizeof(pp.encodedParamPack));
			memcpy(pp.encodedParamPack, encoded.data(), std::min(encoded.size(), sizeof(pp.encodedParamPack) - 1));
		}

		// --- discovery ------------------------------------------------------------------

		std::string DiscoveryUrl()
		{
			switch (ActiveSettings::GetNetworkService())
			{
			case NetworkService::Pretendo:
				return PretendoURLs::OLVURL;
			case NetworkService::Custom:
				return GetNetworkConfig().urls.OLV.GetValue();
			default:
				return NintendoURLs::OLVURL;
			}
		}

		// ask the discovery server which hosts serve the API and the portal
		nnResult Discover()
		{
			const std::string url = DiscoveryUrl();
			return RunBlockingOnHost([&url]() -> nnResult {
				CurlRequestHelper req;
				BeginOliveRequest(req, url);
				pugi::xml_document reply;
				const nnResult r = FinishOliveRequest(req, url, false, reply);
				if (NN_RESULT_IS_FAILURE(r))
					return r;
				pugi::xml_node endpoint = reply.child("result").child("endpoint");
				if (!endpoint)
				{
					cemuLog_log(LogType::Force, "nn_olv: discovery reply has no <endpoint>");
					return kOlvBadXml;
				}
				// <api_host> and <n3ds_host> are not used by the Wii U library
				snprintf(g_DiscoveryResults.apiEndpoint, sizeof(g_DiscoveryResults.apiEndpoint), "https://%s", endpoint.child_value("host"));
				snprintf(g_DiscoveryResults.portalEndpoint, sizeof(g_DiscoveryResults.portalEndpoint), "https://%s", endpoint.child_value("portal_host"));
				return kOlvSuccess;
			});
		}

		nnResult Fail(nnResult r)
		{
			g_olv.initialized = false;
			return r;
		}
	}

	sint32 GetOlvAccessKey(uint32* pOutKey)
	{
		*pOutKey = 0;
		const uint32 key = CafeSystem::GetForegroundTitleOlvAccesskey();
		if (key == 0xFFFFFFFF)
			return OlvStatus(1102);
		*pOutKey = key;
		return kOlvSuccess;
	}

	sint32 Initialize(InitializeParam* pParam)
	{
		if (g_olv.initialized)
			return kOlvAlreadyInitialized;
		if (!pParam || pParam->work.IsNull())
			return Fail(kOlvInvalidPointer);
		if (pParam->workSize < 0x10000)
			return Fail(kOlvInvalidSize);

		uint32 accessKey;
		const nnResult keyResult = GetOlvAccessKey(&accessKey);
		if (NN_RESULT_IS_FAILURE(keyResult))
			return Fail(keyResult);

		const uint64 titleId = CafeSystem::GetForegroundTitleId();
		BuildParamPack(titleId, accessKey);
		g_olv.initialized = true;

		if (ActiveSettings::GetNetworkService() == NetworkService::Nintendo)
		{
			// Nintendo's Miiverse is gone; posts come from the local archive instead
			g_olv.online = true;
			g_olv.useOfflineDB = true;
			return kOlvSuccess;
		}
		if (pParam->flags & InitializeParam::kFlagOffline)
			return kOlvSuccess;

		g_olv.online = true;
		independentServiceToken_t token;
		const sint32 tokenResult = (sint32)nn::act::AcquireIndependentServiceToken(&token, kClientId, 0);
		if (tokenResult < 0)
			return Fail(tokenResult);

		const uint32 uniqueId = (uint32)(titleId >> 8) & 0xFFFFF; // a retail console
		snprintf(g_DiscoveryResults.userAgent, sizeof(g_DiscoveryResults.userAgent), "WiiU/POLV-%s/%u", kLibraryVersion, uniqueId);
		memcpy(g_DiscoveryResults.serviceToken, token.token, sizeof(g_DiscoveryResults.serviceToken));

		const nnResult discovery = Discover();
		if (NN_RESULT_IS_FAILURE(discovery))
			g_olv.initialized = false;
		return discovery;
	}

	sint32 InitializePortalApp(PortalAppParam* pPortalAppParam, InitializeParam* pInitializeParam)
	{
		const sint32 r = Initialize(pInitializeParam);
		if (r != (sint32)kOlvSuccess)
			return r;
		memcpy(pPortalAppParam->paramPack, g_ParamPack.encodedParamPack, sizeof(pPortalAppParam->paramPack));
		memcpy(pPortalAppParam->serviceToken, g_DiscoveryResults.serviceToken, sizeof(g_DiscoveryResults.serviceToken));
		snprintf((char*)pPortalAppParam->startUrl, sizeof(pPortalAppParam->startUrl), "%s/titles/show?src=menu", g_DiscoveryResults.portalEndpoint);
		return kOlvSuccess;
	}

	bool IsInitialized()
	{
		return g_olv.initialized;
	}

	namespace Report
	{
		uint32 GetReportTypes()
		{
			return g_olv.reportTypes;
		}

		void SetReportTypes(uint32 reportTypes)
		{
			g_olv.reportTypes = reportTypes | 0x1000; // bit 12 is always reported
		}
	}

	void RegisterInitializeExports()
	{
		cafeExportRegisterFunc(Initialize, "nn_olv", "Initialize__Q2_2nn3olvFPCQ3_2nn3olv15InitializeParam", LogType::NN_OLV);
		cafeExportRegisterFunc(IsInitialized, "nn_olv", "IsInitialized__Q2_2nn3olvFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Report::GetReportTypes, "nn_olv", "GetReportTypes__Q3_2nn3olv6ReportFv", LogType::NN_OLV);
		cafeExportRegisterFunc(Report::SetReportTypes, "nn_olv", "SetReportTypes__Q3_2nn3olv6ReportFUi", LogType::NN_OLV);

		cafeExportRegisterFunc(InitializeParam_Construct, "nn_olv", "__ct__Q3_2nn3olv15InitializeParamFv", LogType::NN_OLV);
		cafeExportRegisterFunc(InitializeParam_SetFlags, "nn_olv", "SetFlags__Q3_2nn3olv15InitializeParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(InitializeParam_SetWork, "nn_olv", "SetWork__Q3_2nn3olv15InitializeParamFPUcUi", LogType::NN_OLV);
		cafeExportRegisterFunc(InitializeParam_SetReportTypes, "nn_olv", "SetReportTypes__Q3_2nn3olv15InitializeParamFUi", LogType::NN_OLV);
		cafeExportRegisterFunc(InitializeParam_SetSysArgs, "nn_olv", "SetSysArgs__Q3_2nn3olv15InitializeParamFPCvUi", LogType::NN_OLV);

		cafeExportRegisterFunc(InitializePortalApp, "nn_olv", "InitializePortalApp__Q3_2nn3olv6hiddenFPQ4_2nn3olv6hidden14PortalAppParamPCQ3_2nn3olv15InitializeParam", LogType::NN_OLV);
		cafeExportRegisterFunc(PortalAppParam_Construct, "nn_olv", "__ct__Q4_2nn3olv6hidden14PortalAppParamFv", LogType::NN_OLV);
		cafeExportRegisterFunc(PortalAppParam_GetParamPack, "nn_olv", "GetParamPack__Q4_2nn3olv6hidden14PortalAppParamCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(PortalAppParam_GetServiceToken, "nn_olv", "GetServiceToken__Q4_2nn3olv6hidden14PortalAppParamCFv", LogType::NN_OLV);
		cafeExportRegisterFunc(PortalAppParam_GetStartUrl, "nn_olv", "GetStartUrl__Q4_2nn3olv6hidden14PortalAppParamCFv", LogType::NN_OLV);
	}
}
