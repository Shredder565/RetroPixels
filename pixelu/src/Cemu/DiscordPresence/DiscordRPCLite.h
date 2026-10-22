// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>
#include <mutex>

// Minimal client for Discord's local IPC socket, used only to publish a rich
// presence activity. A background thread keeps (re)connecting to the running
// Discord client and sends the latest activity, at most once per 15 seconds.

struct DiscordLiteRichPresence
{
	std::string state;
	std::string details;
	int64_t startTimestamp;
	int64_t endTimestamp;
	std::string largeImageKey;
	std::string largeImageText;
	std::string smallImageKey;
	std::string smallImageText;
	std::string partyId;
	int partySize;
	int partyMax;
	int partyPrivacy;
	std::string matchSecret;
	std::string joinSecret;
	std::string spectateSecret;
	int8_t instance;
};

class DiscordRPCLite
{
public:
	DiscordRPCLite(const char* applicationId);
	~DiscordRPCLite();

	// queued; the worker sends it once connected and allowed by the rate limit
	void UpdateRichPresence(const DiscordLiteRichPresence& presence);
	// forgets the queued activity without sending anything (the worker clears
	// the activity on shutdown)
	void ClearRichPresence();

private:
	void Run(std::string applicationId);
	bool TakePendingPresence(DiscordLiteRichPresence& out);

	std::thread m_thread;
	std::atomic_bool m_quit{false};
	std::mutex m_lock;
	bool m_pending{true}; // activity not yet sent on the current connection
	DiscordLiteRichPresence m_activity{};
};
