// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "napi.h"
#include "napi_helper.h"

#include "curl/curl.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "util/crypto/md5.h"
#include "config/LaunchSettings.h"
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"
#include "pugixml.hpp"
#include <charconv>

// Shop side services: NUS (system tickets), IAS (device registration),
// ECS (account tickets) and the CCS content servers.

namespace NAPI
{
	namespace
	{
		/* ---- service endpoints ---- */

		// ECS GetAccountStatus may hand out replacements for the built-in URLs
		enum class Endpoint { NUS, IAS, ECS, CCSUncached, CCS };

		struct EndpointOverrides
		{
			std::string nus, ias, ecs, cas, ccsUncached, ccs;
		};

		std::mutex s_overridesMtx;
		std::unordered_map<NetworkService, EndpointOverrides> s_overrides;

		std::string BuiltinEndpoint(NetworkService service, Endpoint endpoint)
		{
			if (service == NetworkService::Custom)
			{
				auto& urls = GetNetworkConfig().urls;
				switch (endpoint)
				{
				case Endpoint::NUS: return urls.NUS.GetValue();
				case Endpoint::IAS: return urls.IAS.GetValue();
				case Endpoint::ECS: return urls.ECS.GetValue();
				case Endpoint::CCSUncached: return urls.CCSU.GetValue();
				case Endpoint::CCS: return urls.CCS.GetValue();
				}
			}
			const bool pretendo = service == NetworkService::Pretendo;
			switch (endpoint)
			{
			case Endpoint::NUS: return pretendo ? PretendoURLs::NUSURL : NintendoURLs::NUSURL;
			case Endpoint::IAS: return pretendo ? PretendoURLs::IASURL : NintendoURLs::IASURL;
			case Endpoint::ECS: return pretendo ? PretendoURLs::ECSURL : NintendoURLs::ECSURL;
			case Endpoint::CCSUncached: return pretendo ? PretendoURLs::CCSUURL : NintendoURLs::CCSUURL;
			case Endpoint::CCS: return pretendo ? PretendoURLs::CCSURL : NintendoURLs::CCSURL;
			}
			return {};
		}

		std::string ResolveEndpoint(NetworkService service, Endpoint endpoint)
		{
			{
				std::lock_guard lock(s_overridesMtx);
				const EndpointOverrides& o = s_overrides[service];
				const std::string* value = nullptr;
				switch (endpoint)
				{
				case Endpoint::NUS: value = &o.nus; break;
				case Endpoint::IAS: value = &o.ias; break;
				case Endpoint::ECS: value = &o.ecs; break;
				case Endpoint::CCSUncached: value = &o.ccsUncached; break;
				case Endpoint::CCS: value = &o.ccs; break;
				}
				if (value && !value->empty())
					return *value;
			}
			return BuiltinEndpoint(service, endpoint);
		}

		/* ---- request building ---- */

		void AddDeviceFields(CurlSOAPHelper& soap, AuthInfo& authInfo)
		{
			soap.SOAP_addRequestField("DeviceId", fmt::format("{}", authInfo.getDeviceIdWithPlatform()));
			soap.SOAP_addRequestField("Region", NCrypto::GetRegionAsString(authInfo.region));
			soap.SOAP_addRequestField("Country", authInfo.country);
		}

		// device tokens are sent as "WT-" followed by their MD5 in lowercase hex
		std::string WrapDeviceToken(std::string_view deviceToken)
		{
			MD5_CTX ctx;
			uint8 digest[16];
			MD5_Init(&ctx);
			MD5_Update(&ctx, deviceToken.data(), (unsigned long)deviceToken.size());
			MD5_Final(digest, &ctx);
			std::string text = "WT-";
			for (uint8 b : digest)
				fmt::format_to(std::back_inserter(text), "{:02x}", b);
			return text;
		}

		void AddAccountFields(CurlSOAPHelper& soap, AuthInfo& authInfo)
		{
			if (!authInfo.IASToken.accountId.empty())
				soap.SOAP_addRequestField("AccountId", authInfo.IASToken.accountId);
			if (!authInfo.IASToken.deviceToken.empty())
				soap.SOAP_addRequestField("DeviceToken", WrapDeviceToken(authInfo.IASToken.deviceToken));
		}

		// Decodes the base64 payload `payloadName` and all <Certs> elements.
		// On bad data logs "<logPrefix>: Invalid ..." and sets DATA_ERROR.
		bool ReadTicketAndCerts(pugi::xml_node response, const char* payloadName, const char* payloadLabel, const char* logPrefix,
			_NAPI_CommonResultSOAP& result, std::vector<uint8>& payload, std::vector<std::vector<uint8>>& certs)
		{
			payload = NCrypto::base64Decode(response.child_value(payloadName));
			if (payload.empty())
			{
				cemuLog_log(LogType::Force, "{}: Invalid {} data in response", logPrefix, payloadLabel);
				result.apiError = NAPI_RESULT::DATA_ERROR;
				return false;
			}
			for (pugi::xml_node cert = response.child("Certs"); cert; cert = cert.next_sibling("Certs"))
			{
				std::vector<uint8> data = NCrypto::base64Decode(cert.child_value());
				if (data.empty())
				{
					cemuLog_log(LogType::Force, "{}: Invalid cert data in response", logPrefix);
					result.apiError = NAPI_RESULT::DATA_ERROR;
					return false;
				}
				certs.push_back(std::move(data));
			}
			return true;
		}

		template<typename TResult>
		bool SendSoap(CurlSOAPHelper& soap, const char* responseName, TResult& result, pugi::xml_document& doc, pugi::xml_node& response)
		{
			if (!soap.submitRequest())
			{
				result.apiError = NAPI_RESULT::FAILED;
				return false;
			}
			return _parseResponseInit(soap, responseName, response, result, doc, response);
		}

		/* ---- plain downloads from the content servers ---- */

		bool DownloadInto(NetworkService service, const std::string& url, std::optional<sint32> timeout, std::vector<uint8>& out, const std::string& failMessage)
		{
			CurlRequestHelper req;
			req.initate(service, url, CurlRequestHelper::SERVER_SSL_CONTEXT::CCS);
			if (timeout)
				req.setTimeout(*timeout);
			if (!req.submitRequest(false))
			{
				cemuLog_log(LogType::Force, "{}", failMessage);
				return false;
			}
			out = req.getReceivedData();
			return true;
		}
	}

	/* ---- NUS ---- */

	// fetches the shared ticket of a title that has one (updates, system titles)
	NAPI_NUSGetSystemCommonETicket_Result NUS_GetSystemCommonETicket(AuthInfo& authInfo, uint64 titleId)
	{
		NAPI_NUSGetSystemCommonETicket_Result result{};
		CurlSOAPHelper soap(authInfo.GetService());
		soap.SOAP_initate("nus", ResolveEndpoint(authInfo.GetService(), Endpoint::NUS), "GetSystemCommonETicket", "1.0");
		// NUS names the locale fields differently and wants the serial
		soap.SOAP_addRequestField("DeviceId", fmt::format("{}", authInfo.getDeviceIdWithPlatform()));
		soap.SOAP_addRequestField("RegionId", NCrypto::GetRegionAsString(authInfo.region));
		soap.SOAP_addRequestField("CountryCode", authInfo.country);
		soap.SOAP_addRequestField("SerialNo", authInfo.serial);
		soap.SOAP_addRequestField("TitleId", fmt::format("{:016X}", titleId));

		pugi::xml_document doc;
		pugi::xml_node response;
		if (!SendSoap(soap, "GetSystemCommonETicketResponse", result, doc, response))
			return result;
		ReadTicketAndCerts(response, "CommonETicket", "eTicket", "GetSystemCommonETicketResponse", result, result.eTicket, result.certs);
		return result;
	}

	/* ---- IAS ---- */

	NAPI_IASGetChallenge_Result IAS_GetChallenge(AuthInfo& authInfo)
	{
		NAPI_IASGetChallenge_Result result{};
		CurlSOAPHelper soap(authInfo.GetService());
		soap.SOAP_initate("ias", ResolveEndpoint(authInfo.GetService(), Endpoint::IAS), "GetChallenge", "2.0");
		AddDeviceFields(soap, authInfo); // the challenge is tied to this DeviceId

		pugi::xml_document doc;
		pugi::xml_node response;
		if (!SendSoap(soap, "GetChallengeResponse", result, doc, response))
			return result;
		result.challenge = response.child_value("Challenge");
		return result;
	}

	NAPI_IASGetRegistrationInfo_Result IAS_GetRegistrationInfo_QueryInfo(AuthInfo& authInfo, std::string challenge)
	{
		NAPI_IASGetRegistrationInfo_Result result{};
		CurlSOAPHelper soap(authInfo.GetService());
		soap.SOAP_initate("ias", ResolveEndpoint(authInfo.GetService(), Endpoint::IAS), "GetRegistrationInfo", "2.0");
		AddDeviceFields(soap, authInfo); // must be the DeviceId the challenge was issued for
		soap.SOAP_addRequestField("Challenge", challenge);

		NCrypto::CertECC deviceCert;
		if (!deviceCert.decodeFromBase64(authInfo.deviceCertBase64))
		{
			result.apiError = NAPI_RESULT::FAILED;
			return result;
		}

		// prove possession of the device key: sign the challenge element with a
		// certificate issued by the device for the system settings title
		const std::string signedText = fmt::format("<Challenge>{}</Challenge>", challenge);
		NCrypto::CHash256 digest;
		NCrypto::GenerateHashSHA256(signedText.data(), signedText.size(), digest);
		NCrypto::CertECC signerCert;
		NCrypto::ECCSig sig = NCrypto::signHash(0x00050010, 0x10040300, digest.b, sizeof(digest.b), signerCert);

		soap.SOAP_addRequestField("Signature", sig.encodeToBase64());
		soap.SOAP_addRequestField("CertChain", signerCert.encodeToBase64());
		soap.SOAP_addRequestField("DeviceCert", deviceCert.encodeToBase64());

		pugi::xml_document doc;
		pugi::xml_node response;
		if (!SendSoap(soap, "GetRegistrationInfoResponse", result, doc, response))
			return result;
		result.accountId = response.child_value("AccountId");
		result.deviceToken = response.child_value("DeviceToken");
		if (boost::iequals(response.child_value("DeviceTokenExpired"), "true"))
			cemuLog_log(LogType::Force, "Unexpected server response: Device token expired");
		return result;
	}

	/* ---- ECS ---- */

	NAPI_ECSGetAccountStatus_Result ECS_GetAccountStatus(AuthInfo& authInfo)
	{
		using Status = NAPI_ECSGetAccountStatus_Result::AccountStatus;
		NAPI_ECSGetAccountStatus_Result result{};
		CurlSOAPHelper soap(authInfo.GetService());
		soap.SOAP_initate("ecs", ResolveEndpoint(authInfo.GetService(), Endpoint::ECS), "GetAccountStatus", "2.0");
		AddDeviceFields(soap, authInfo);
		AddAccountFields(soap, authInfo);

		pugi::xml_document doc;
		pugi::xml_node response;
		if (!SendSoap(soap, "GetAccountStatusResponse", result, doc, response))
			return result;

		result.accountId = response.child_value("AccountId");
		const char* statusText = response.child_value("AccountStatus");
		switch (statusText[0])
		{
		case 'R': result.accountStatus = Status::REGISTERED; break;
		case 'T': result.accountStatus = Status::TRANSFERRED; break;
		case 'U': result.accountStatus = Status::UNREGISTERED; break;
		default:
			cemuLog_log(LogType::Force, "ECS_GetAccountStatus: Account has unknown status code {}", statusText);
			break;
		}

		auto& urls = result.serviceURLs;
		const std::pair<const char*, std::string*> knownNames[] = {
			{ "ContentPrefixURL", &urls.ContentPrefixURL },
			{ "UncachedContentPrefixURL", &urls.UncachedContentPrefixURL },
			{ "SystemContentPrefixURL", &urls.SystemContentPrefixURL },
			{ "SystemUncachedContentPrefixURL", &urls.SystemUncachedContentPrefixURL },
			{ "EcsURL", &urls.EcsURL },
			{ "IasURL", &urls.IasURL },
			{ "CasURL", &urls.CasURL },
			{ "NusURL", &urls.NusURL },
		};
		for (pugi::xml_node entry = response.child("ServiceURLs"); entry; entry = entry.next_sibling("ServiceURLs"))
		{
			std::string_view name = entry.child_value("Name");
			std::string_view uri = entry.child_value("URI");
			if (name.empty() || uri.empty())
				continue;
			auto it = std::find_if(std::begin(knownNames), std::end(knownNames), [&](const auto& k) { return boost::iequals(name, k.first); });
			if (it == std::end(knownNames))
				cemuLog_log(LogType::Force, "GetAccountStatus: Unknown service URI type {}", name);
			else
				it->second->assign(uri);
		}

		// later requests to this service go to the advertised servers
		std::lock_guard lock(s_overridesMtx);
		EndpointOverrides& o = s_overrides[authInfo.GetService()];
		auto take = [](std::string& dst, const std::string& src) { if (!src.empty()) dst = src; };
		take(o.ccs, urls.ContentPrefixURL);
		take(o.ccsUncached, urls.UncachedContentPrefixURL);
		take(o.ias, urls.IasURL);
		take(o.cas, urls.CasURL);
		take(o.nus, urls.NusURL);
		take(o.ecs, urls.EcsURL);
		return result;
	}

	NAPI_ECSAccountListETicketIds_Result ECS_AccountListETicketIds(AuthInfo& authInfo)
	{
		NAPI_ECSAccountListETicketIds_Result result{};
		CurlSOAPHelper soap(authInfo.GetService());
		soap.SOAP_initate("ecs", ResolveEndpoint(authInfo.GetService(), Endpoint::ECS), "AccountListETicketIds", "2.0");
		AddDeviceFields(soap, authInfo);
		AddAccountFields(soap, authInfo);

		pugi::xml_document doc;
		pugi::xml_node response;
		if (!SendSoap(soap, "AccountListETicketIdsResponse", result, doc, response))
			return result;

		// each <TIV> is "<ticketId>.<ticketVersion>"; the version counts ticket updates
		for (pugi::xml_node tiv = response.child("TIV"); tiv; tiv = tiv.next_sibling("TIV"))
		{
			std::string_view text = tiv.child_value();
			const size_t dot = text.find('.');
			std::string_view idPart = text.substr(0, dot);
			std::string_view versionPart = dot == std::string_view::npos ? std::string_view() : text.substr(dot + 1);
			sint64 ticketId = 0;
			uint32 ticketVersion = 0;
			if (std::from_chars(idPart.data(), idPart.data() + idPart.size(), ticketId).ec != std::errc() ||
				std::from_chars(versionPart.data(), versionPart.data() + versionPart.size(), ticketVersion).ec != std::errc())
			{
				result.apiError = NAPI_RESULT::XML_ERROR;
				return result;
			}
			result.tivs.push_back({ ticketId, ticketVersion });
		}
		return result;
	}

	NAPI_ECSAccountGetETickets_Result ECS_AccountGetETickets(AuthInfo& authInfo, sint64 ticketId)
	{
		NAPI_ECSAccountGetETickets_Result result{};
		CurlSOAPHelper soap(authInfo.GetService());
		soap.SOAP_initate("ecs", ResolveEndpoint(authInfo.GetService(), Endpoint::ECS), "AccountGetETickets", "2.0");
		AddDeviceFields(soap, authInfo);
		AddAccountFields(soap, authInfo);
		soap.SOAP_addRequestField("DeviceCert", authInfo.deviceCertBase64);
		soap.SOAP_addRequestField("TicketId", fmt::format("{}", ticketId));

		pugi::xml_document doc;
		pugi::xml_node response;
		if (!SendSoap(soap, "AccountGetETicketsResponse", result, doc, response))
			return result;
		ReadTicketAndCerts(response, "ETickets", "eTickets", "AccountGetETickets", result, result.eTickets, result.certs);
		return result;
	}

	/* ---- CCS ---- */

	NAPI_CCSGetTMD_Result CCS_GetTMD(AuthInfo& authInfo, uint64 titleId, uint16 titleVersion)
	{
		NAPI_CCSGetTMD_Result result{};
		const NetworkService service = authInfo.GetService();
		std::string url = fmt::format("{}/{:016x}/tmd.{}?deviceId={}&accountId={}", ResolveEndpoint(service, Endpoint::CCSUncached), titleId, titleVersion, authInfo.getDeviceIdWithPlatform(), authInfo.IASToken.accountId);
		result.isValid = DownloadInto(service, url, 180, result.tmdData, fmt::format("Failed to request TMD for title {:016X} v{}", titleId, titleVersion));
		return result;
	}

	NAPI_CCSGetTMD_Result CCS_GetTMD(AuthInfo& authInfo, uint64 titleId)
	{
		NAPI_CCSGetTMD_Result result{};
		const NetworkService service = authInfo.GetService();
		std::string url = fmt::format("{}/{:016x}/tmd?deviceId={}&accountId={}", ResolveEndpoint(service, Endpoint::CCSUncached), titleId, authInfo.getDeviceIdWithPlatform(), authInfo.IASToken.accountId);
		result.isValid = DownloadInto(service, url, 180, result.tmdData, fmt::format("Failed to request TMD for title {:016X}", titleId));
		return result;
	}

	NAPI_CCSGetETicket_Result CCS_GetCETK(NetworkService service, uint64 titleId, uint16 titleVersion)
	{
		NAPI_CCSGetETicket_Result result{};
		std::string url = fmt::format("{}/{:016x}/cetk", ResolveEndpoint(service, Endpoint::CCSUncached), titleId);
		result.isValid = DownloadInto(service, url, 180, result.cetkData, fmt::format("Failed to request eTicket for title {:016X} v{}", titleId, titleVersion));
		return result;
	}

	bool CCS_GetContentFile(NetworkService service, uint64 titleId, uint32 contentId, bool(*cbWriteCallback)(void* userData, const void* ptr, size_t len, bool isLast), void* userData)
	{
		CurlRequestHelper req;
		req.initate(service, fmt::format("{}/{:016x}/{:08x}", ResolveEndpoint(service, Endpoint::CCS), titleId, contentId), CurlRequestHelper::SERVER_SSL_CONTEXT::CCS);
		req.setWriteCallback(cbWriteCallback, userData);
		req.setTimeout(0); // content files can be gigabytes
		if (req.submitRequest(false))
			return true;
		cemuLog_log(LogType::Force, "Failed to request content file {:08x} for title {:016X}", contentId, titleId);
		return false;
	}

	NAPI_CCSGetContentH3_Result CCS_GetContentH3File(NetworkService service, uint64 titleId, uint32 contentId)
	{
		NAPI_CCSGetContentH3_Result result{};
		std::string url = fmt::format("{}/{:016x}/{:08x}.h3", ResolveEndpoint(service, Endpoint::CCS), titleId, contentId);
		result.isValid = DownloadInto(service, url, std::nullopt, result.tmdData, fmt::format("Failed to request content hash file {:08x}.h3 for title {:016X}", contentId, titleId));
		return result;
	}
};
