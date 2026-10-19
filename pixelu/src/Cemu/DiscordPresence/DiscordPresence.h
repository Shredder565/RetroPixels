// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Shows "Idling" / "Playing <title>" as the user's Discord activity.
class DiscordPresence
{
public:
	enum State
	{
		Idling,
		Playing,
	};

	DiscordPresence();
	~DiscordPresence();

	void UpdatePresence(State state, const std::string& text = {}) const;
	void ClearPresence() const;

private:
	class DiscordRPCLite* m_rpcClient = nullptr;
};
