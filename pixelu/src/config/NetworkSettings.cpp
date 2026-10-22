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
	struct UrlField
	{
		const char* element;
		ConfigValue<std::string>& value;
		const std::string& fallback;
	};
	const UrlField fields[] = {
		{"act", urls.ACT, NintendoURLs::ACTURL}, {"ecs", urls.ECS, NintendoURLs::ECSURL},
		{"nus", urls.NUS, NintendoURLs::NUSURL}, {"ias", urls.IAS, NintendoURLs::IASURL},
		{"ccsu", urls.CCSU, NintendoURLs::CCSUURL}, {"ccs", urls.CCS, NintendoURLs::CCSURL},
		{"idbe", urls.IDBE, NintendoURLs::IDBEURL}, {"boss", urls.BOSS, NintendoURLs::BOSSURL},
		{"tagaya", urls.TAGAYA, NintendoURLs::TAGAYAURL}, {"olv", urls.OLV, NintendoURLs::OLVURL},
	};
	for (const UrlField& f : fields)
		f.value = u.get(f.element, f.fallback);
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
