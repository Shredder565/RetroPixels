// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/precompiled.h"
#include "napi.h"
#include "napi_helper.h"

#include "curl/curl.h"
#include "pugixml.hpp"
#include "Cemu/ncrypto/ncrypto.h"
#include <charconv>
#include "config/ActiveSettings.h"
#include "config/NetworkSettings.h"

// TAGAYA publishes, per region and country, a list of the newest version of
// every title. A small "latest_version" document says which list is current
// and on which host it lives.

namespace NAPI
{
	namespace
	{
		std::string TagayaBaseUrl(NetworkService service)
		{
			if (service == NetworkService::Pretendo)
				return PretendoURLs::TAGAYAURL;
			if (service == NetworkService::Custom)
				return GetNetworkConfig().urls.TAGAYA.GetValue();
			return NintendoURLs::TAGAYAURL;
		}

		std::string_view CountryOrDefault(const AuthInfo& authInfo)
		{
			return authInfo.country.empty() ? std::string_view("NN") : std::string_view(authInfo.country);
		}

		bool FetchXml(AuthInfo& authInfo, const std::string& url, pugi::xml_document& doc, const char* requestFailMsg, const char* parseFailMsg)
		{
			CurlRequestHelper req;
			req.initate(authInfo.GetService(), url, CurlRequestHelper::SERVER_SSL_CONTEXT::TAGAYA);
			if (!req.submitRequest(false))
			{
				cemuLog_log(LogType::Force, std::string_view(requestFailMsg));
				return false;
			}
			const std::vector<uint8>& body = req.getReceivedData();
			if (!doc.load_buffer(body.data(), body.size()))
			{
				cemuLog_log(LogType::Force, std::string_view(parseFailMsg));
				return false;
			}
			return true;
		}
	}

	NAPI_VersionListVersion_Result TAG_GetVersionListVersion(AuthInfo& authInfo)
	{
		NAPI_VersionListVersion_Result result;
		std::string url = TagayaBaseUrl(authInfo.GetService());
		url.append(fmt::format("/{}/{}/latest_version", NCrypto::GetRegionAsString(authInfo.region), CountryOrDefault(authInfo)));

		pugi::xml_document doc;
		if (!FetchXml(authInfo, url, doc, "Failed to request version of update list", "Failed to parse title list XML"))
			return result;

		// <version_list_info><version>N</version><fqdn>host</fqdn></version_list_info>
		pugi::xml_node info = doc.child("version_list_info");
		pugi::xml_node versionNode = info.child("version");
		pugi::xml_node hostNode = info.child("fqdn");
		if (!versionNode || !hostNode)
		{
			cemuLog_log(LogType::Force, "Title list XML has missing field");
			return result;
		}
		result.version = (uint32)atoi(versionNode.child_value());
		result.fqdnURL = hostNode.child_value();
		result.isValid = true;
		return result;
	}

	NAPI_VersionList_Result TAG_GetVersionList(AuthInfo& authInfo, std::string_view fqdnURL, uint32 versionListVersion)
	{
		NAPI_VersionList_Result result;
		std::string url = fmt::format("https://{}/tagaya/versionlist/{}/{}/list/{}.versionlist", fqdnURL, NCrypto::GetRegionAsString(authInfo.region), CountryOrDefault(authInfo), versionListVersion);

		pugi::xml_document doc;
		if (!FetchXml(authInfo, url, doc, "Failed to request update list", "Failed to parse update list XML"))
			return result;

		// <version_list><titles><title><id>hex</id><version>dec</version></title>...</titles></version_list>
		pugi::xml_node titles = doc.child("version_list").child("titles");
		for (pugi::xml_node title = titles.child("title"); title; title = title.next_sibling("title"))
		{
			std::string_view idText = title.child_value("id");
			std::string_view versionText = title.child_value("version");
			uint64 titleId = 0;
			uint32 version = 0;
			if (std::from_chars(idText.data(), idText.data() + idText.size(), titleId, 16).ec != std::errc())
				continue;
			if (std::from_chars(versionText.data(), versionText.data() + versionText.size(), version, 10).ec != std::errc())
				continue;
			result.titleVersionList.emplace(titleId, (uint16)version); // first entry for a title wins
		}
		result.isValid = true;
		return result;
	}
};
