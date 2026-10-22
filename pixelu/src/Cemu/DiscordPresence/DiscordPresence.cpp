// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "DiscordPresence.h"
#include "Common/version.h"
#include "DiscordRPCLite.h"

namespace
{
	// Discord application whose assets (the logo image) the activity uses
	constexpr const char* kDiscordAppId = "460807638964371468";
	constexpr const char* kLogoAsset = "logo_icon_big_png";
}

DiscordPresence::DiscordPresence()
	: m_rpcClient(new DiscordRPCLite(kDiscordAppId))
{
	UpdatePresence(Idling);
}

DiscordPresence::~DiscordPresence()
{
	ClearPresence();
	delete m_rpcClient;
	m_rpcClient = nullptr;
}

void DiscordPresence::UpdatePresence(State state, const std::string& text) const
{
	if (!m_rpcClient)
		return;
	DiscordLiteRichPresence activity{};
	if (state == Playing)
	{
		activity.details = "Ingame";
		activity.state = "Playing " + text;
	}
	else
	{
		cemu_assert_debug(state == Idling);
		activity.details = "Idling";
	}
	activity.startTimestamp = (int64_t)time(nullptr);
	activity.largeImageKey = kLogoAsset;
	activity.largeImageText = BUILD_VERSION_WITH_NAME_STRING;
	m_rpcClient->UpdateRichPresence(activity);
}

void DiscordPresence::ClearPresence() const
{
	if (m_rpcClient)
		m_rpcClient->ClearRichPresence();
}
