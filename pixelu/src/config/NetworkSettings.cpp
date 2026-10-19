// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "NetworkSettings.h"
#include "ActiveSettings.h"
#include "LaunchSettings.h"
#include "CemuConfig.h"
#include "Common/FileStream.h"

XMLNetworkConfig_t n_config(L"network_services.xml");

namespace
{
	constexpr const char* kFileName = "network_services.xml";
}

void NetworkConfig::LoadOnce()
{
	n_config.SetFilename(ActiveSettings::GetConfigPath(kFileName).generic_wstring());
	if (XMLExists())
		n_config.Load();
}

void NetworkConfig::Load(XMLConfigParser& parser)
{
	XMLConfigParser content = parser.get("content");
	networkname = content.get("networkname", "Custom");
	disablesslver = content.get("disablesslverification", disablesslver);
	// URLs that are not set fall back to the official servers
	XMLConfigParser u = content.get("urls");
	urls.ACT = u.get("act", NintendoURLs::ACTURL);
	urls.ECS = u.get("ecs", NintendoURLs::ECSURL);
	urls.NUS = u.get("nus", NintendoURLs::NUSURL);
	urls.IAS = u.get("ias", NintendoURLs::IASURL);
	urls.CCSU = u.get("ccsu", NintendoURLs::CCSUURL);
	urls.CCS = u.get("ccs", NintendoURLs::CCSURL);
	urls.IDBE = u.get("idbe", NintendoURLs::IDBEURL);
	urls.BOSS = u.get("boss", NintendoURLs::BOSSURL);
	urls.TAGAYA = u.get("tagaya", NintendoURLs::TAGAYAURL);
	urls.OLV = u.get("olv", NintendoURLs::OLVURL);
}

bool NetworkConfig::XMLExists()
{
	static std::optional<bool> s_cachedResult;
	if (!s_cachedResult)
	{
		std::error_code ec;
		s_cachedResult = fs::exists(ActiveSettings::GetConfigPath(kFileName), ec);
	}
	return *s_cachedResult;
}
