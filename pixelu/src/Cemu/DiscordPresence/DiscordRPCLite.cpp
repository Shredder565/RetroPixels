// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "DiscordRPCLite.h"

#include <string_view>
#include <optional>
#include <chrono>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

// Discord IPC: every message is a frame of  u32 opcode, u32 payload length
// (both little endian)  followed by a UTF-8 JSON payload.
namespace
{
	enum class FrameOp : uint32_t
	{
		Handshake = 0,
		Frame = 1,
		Close = 2,
		Ping = 3,
		Pong = 4,
	};

	constexpr uint32_t kMaxPayload = 8 * 1024 * 1024;
	constexpr int64_t kPresenceInterval = 15000; // Discord rate-limits SET_ACTIVITY

	int64_t NowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}

	int CurrentPid()
	{
#ifdef _WIN32
		return (int)GetCurrentProcessId();
#else
		return (int)getpid();
#endif
	}

	void AppendJsonString(std::string& out, std::string_view text)
	{
		static const char hexDigits[] = "0123456789abcdef";
		out.push_back('"');
		for (const char ch : text)
		{
			const unsigned char u = (unsigned char)ch;
			switch (u)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (u < 0x20)
				{
					out += "\\u00";
					out.push_back(hexDigits[u >> 4]);
					out.push_back(hexDigits[u & 15]);
				}
				else
					out.push_back(ch);
			}
		}
		out.push_back('"');
	}

	// builds a JSON object member by member
	class JsonObject
	{
	public:
		JsonObject& Str(const char* key, std::string_view value)
		{
			Key(key);
			AppendJsonString(m_text, value);
			return *this;
		}
		JsonObject& Raw(const char* key, std::string_view rawValue)
		{
			Key(key);
			m_text.append(rawValue);
			return *this;
		}
		JsonObject& Num(const char* key, int64_t value)
		{
			return Raw(key, std::to_string(value));
		}
		bool Empty() const { return m_members == 0; }
		std::string Finish() const { return "{" + m_text + "}"; }

	private:
		void Key(const char* key)
		{
			if (m_members++)
				m_text.push_back(',');
			AppendJsonString(m_text, key);
			m_text.push_back(':');
		}
		std::string m_text;
		int m_members = 0;
	};

	std::string BuildActivityCommand(const DiscordLiteRichPresence& p, uint64_t nonce)
	{
		JsonObject args;
		args.Num("pid", CurrentPid());
		if (!p.state.empty() || !p.details.empty())
		{
			JsonObject activity;
			if (!p.state.empty())
				activity.Str("state", p.state);
			if (!p.details.empty())
				activity.Str("details", p.details);
			JsonObject times;
			if (p.startTimestamp)
				times.Num("start", p.startTimestamp);
			if (p.endTimestamp)
				times.Num("end", p.endTimestamp);
			if (!times.Empty())
				activity.Raw("timestamps", times.Finish());
			JsonObject assets;
			if (!p.largeImageKey.empty())
				assets.Str("large_image", p.largeImageKey);
			if (!p.largeImageText.empty())
				assets.Str("large_text", p.largeImageText);
			if (!p.smallImageKey.empty())
				assets.Str("small_image", p.smallImageKey);
			if (!p.smallImageText.empty())
				assets.Str("small_text", p.smallImageText);
			if (!assets.Empty())
				activity.Raw("assets", assets.Finish());
			const bool hasSize = p.partySize > 0 && p.partyMax > 0;
			if (!p.partyId.empty() || hasSize)
			{
				JsonObject party;
				if (!p.partyId.empty())
					party.Str("id", p.partyId);
				if (hasSize)
					party.Raw("size", "[" + std::to_string(p.partySize) + "," + std::to_string(p.partyMax) + "]");
				party.Num("privacy", p.partyPrivacy);
				activity.Raw("party", party.Finish());
			}
			JsonObject secrets;
			if (!p.matchSecret.empty())
				secrets.Str("match", p.matchSecret);
			if (!p.joinSecret.empty())
				secrets.Str("join", p.joinSecret);
			if (!p.spectateSecret.empty())
				secrets.Str("spectate", p.spectateSecret);
			if (!secrets.Empty())
				activity.Raw("secrets", secrets.Finish());
			args.Raw("activity", activity.Finish());
		}
		char nonceText[24];
		snprintf(nonceText, sizeof(nonceText), "%016llx", (unsigned long long)nonce);
		JsonObject cmd;
		cmd.Str("cmd", "SET_ACTIVITY").Str("nonce", nonceText).Raw("args", args.Finish());
		return cmd.Finish();
	}

	// Reads the string value of a top-level member of a JSON object. Nested
	// values are skipped; non-string values yield nullopt.
	class TopLevelReader
	{
	public:
		explicit TopLevelReader(std::string_view text) : m_s(text) {}

		std::optional<std::string> Find(std::string_view wantedKey)
		{
			m_i = 0;
			Ws();
			if (!Take('{'))
				return std::nullopt;
			while (true)
			{
				Ws();
				std::string key;
				if (!ReadString(key))
					return std::nullopt;
				Ws();
				if (!Take(':'))
					return std::nullopt;
				Ws();
				if (key == wantedKey)
				{
					std::string value;
					if (ReadString(value))
						return value;
					return std::nullopt;
				}
				if (!SkipValue(0))
					return std::nullopt;
				Ws();
				if (!Take(','))
					return std::nullopt;
			}
		}

	private:
		void Ws()
		{
			while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r'))
				m_i++;
		}
		bool Take(char c)
		{
			if (m_i < m_s.size() && m_s[m_i] == c)
			{
				m_i++;
				return true;
			}
			return false;
		}
		bool ReadString(std::string& out)
		{
			if (!Take('"'))
				return false;
			while (m_i < m_s.size())
			{
				const char c = m_s[m_i++];
				if (c == '"')
					return true;
				if (c == '\\' && m_i < m_s.size())
				{
					const char e = m_s[m_i++];
					switch (e)
					{
					case 'n': out.push_back('\n'); break;
					case 't': out.push_back('\t'); break;
					case 'r': out.push_back('\r'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'u': m_i = (std::min)(m_s.size(), m_i + 4); out.push_back('?'); break;
					default: out.push_back(e); break;
					}
					continue;
				}
				out.push_back(c);
			}
			return false;
		}
		bool SkipValue(int depth)
		{
			if (depth > 64 || m_i >= m_s.size())
				return false;
			const char c = m_s[m_i];
			if (c == '"')
			{
				std::string ignored;
				return ReadString(ignored);
			}
			if (c == '{' || c == '[')
			{
				const char close = c == '{' ? '}' : ']';
				m_i++;
				Ws();
				if (Take(close))
					return true;
				while (true)
				{
					Ws();
					if (c == '{')
					{
						std::string ignored;
						if (!ReadString(ignored))
							return false;
						Ws();
						if (!Take(':'))
							return false;
						Ws();
					}
					if (!SkipValue(depth + 1))
						return false;
					Ws();
					if (Take(close))
						return true;
					if (!Take(','))
						return false;
				}
			}
			// number, true, false, null
			const size_t start = m_i;
			while (m_i < m_s.size() && m_s[m_i] != ',' && m_s[m_i] != '}' && m_s[m_i] != ']' && m_s[m_i] != ' ' && m_s[m_i] != '\n' && m_s[m_i] != '\r' && m_s[m_i] != '\t')
				m_i++;
			return m_i > start;
		}

		std::string_view m_s;
		size_t m_i = 0;
	};

	bool EqualsIgnoreCase(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); i++)
		{
			char x = a[i], y = b[i];
			if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
			if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
			if (x != y)
				return false;
		}
		return true;
	}

	// connection to the local Discord client
	class IpcChannel
	{
	public:
		~IpcChannel() { Disconnect(); }

#ifdef _WIN32
		bool Connect()
		{
			Disconnect();
			for (int n = 0; n < 10; n++)
			{
				char name[64];
				snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", n);
				HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
				if (h != INVALID_HANDLE_VALUE)
				{
					m_pipe = h;
					m_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
					return true;
				}
			}
			return false;
		}

		void Disconnect()
		{
			if (m_pipe != INVALID_HANDLE_VALUE)
				CloseHandle(m_pipe);
			if (m_event)
				CloseHandle(m_event);
			m_pipe = INVALID_HANDLE_VALUE;
			m_event = nullptr;
		}

		bool Connected() const { return m_pipe != INVALID_HANDLE_VALUE; }

		// >0 bytes read, 0 timeout, <0 connection lost
		int Receive(uint8_t* dst, size_t len, int timeoutMs)
		{
			if (!Connected() || !m_event)
				return -1;
			OVERLAPPED ov{};
			ov.hEvent = m_event;
			ResetEvent(m_event);
			DWORD got = 0;
			if (!ReadFile(m_pipe, dst, (DWORD)len, nullptr, &ov))
			{
				if (GetLastError() != ERROR_IO_PENDING)
				{
					Disconnect();
					return -1;
				}
				const DWORD w = WaitForSingleObject(m_event, (DWORD)timeoutMs);
				if (w == WAIT_TIMEOUT)
				{
					CancelIo(m_pipe);
					GetOverlappedResult(m_pipe, &ov, &got, TRUE); // wait for the cancellation
					return got > 0 ? (int)got : 0;
				}
				if (w != WAIT_OBJECT_0)
				{
					Disconnect();
					return -1;
				}
			}
			if (!GetOverlappedResult(m_pipe, &ov, &got, FALSE))
			{
				Disconnect();
				return -1;
			}
			return (int)got;
		}

		bool Send(const uint8_t* src, size_t len)
		{
			if (!Connected())
				return false;
			OVERLAPPED ov{};
			HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
			ov.hEvent = ev;
			DWORD written = 0;
			BOOL ok = WriteFile(m_pipe, src, (DWORD)len, nullptr, &ov);
			if (!ok && GetLastError() == ERROR_IO_PENDING)
				ok = GetOverlappedResult(m_pipe, &ov, &written, TRUE);
			else if (ok)
				ok = GetOverlappedResult(m_pipe, &ov, &written, TRUE);
			if (ev)
				CloseHandle(ev);
			if (!ok)
			{
				Disconnect();
				return false;
			}
			return true;
		}

	private:
		HANDLE m_pipe = INVALID_HANDLE_VALUE;
		HANDLE m_event = nullptr;
#else
		bool Connect()
		{
			Disconnect();
			const char* dir = nullptr;
			for (const char* var : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" })
			{
				dir = getenv(var);
				if (dir)
					break;
			}
			if (!dir)
				dir = "/tmp";
			for (int n = 0; n < 10; n++)
			{
				sockaddr_un addr{};
				addr.sun_family = AF_UNIX;
				snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/discord-ipc-%d", dir, n);
				const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
				if (fd < 0)
					return false;
#ifdef SO_NOSIGPIPE
				int one = 1;
				setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
				if (connect(fd, (const sockaddr*)&addr, sizeof(addr)) == 0)
				{
					fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
					m_fd = fd;
					return true;
				}
				close(fd);
			}
			return false;
		}

		void Disconnect()
		{
			if (m_fd >= 0)
				close(m_fd);
			m_fd = -1;
		}

		bool Connected() const { return m_fd >= 0; }

		int Receive(uint8_t* dst, size_t len, int timeoutMs)
		{
			if (!Connected() || len == 0)
				return -1;
			pollfd p{};
			p.fd = m_fd;
			p.events = POLLIN;
			const int r = poll(&p, 1, timeoutMs);
			if (r == 0)
				return 0;
			if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			{
				Disconnect();
				return -1;
			}
			const ssize_t got = recv(m_fd, dst, len, Flags());
			if (got <= 0)
			{
				Disconnect();
				return -1;
			}
			return (int)got;
		}

		bool Send(const uint8_t* src, size_t len)
		{
			size_t done = 0;
			while (Connected() && done < len)
			{
				const ssize_t n = send(m_fd, src + done, len - done, Flags());
				if (n < 0)
				{
					if (errno == EAGAIN || errno == EWOULDBLOCK)
					{
						pollfd p{};
						p.fd = m_fd;
						p.events = POLLOUT;
						if (poll(&p, 1, 1000) > 0)
							continue;
					}
					Disconnect();
					return false;
				}
				done += (size_t)n;
			}
			return done == len;
		}

	private:
		static int Flags()
		{
#ifdef MSG_NOSIGNAL
			return MSG_NOSIGNAL;
#else
			return 0;
#endif
		}
		int m_fd = -1;
#endif

	public:
		bool SendFrame(FrameOp op, std::string_view payload)
		{
			std::vector<uint8_t> frame(8 + payload.size());
			const uint32_t code = (uint32_t)op;
			const uint32_t size = (uint32_t)payload.size();
			for (int i = 0; i < 4; i++)
			{
				frame[i] = (uint8_t)(code >> (8 * i));
				frame[4 + i] = (uint8_t)(size >> (8 * i));
			}
			if (!payload.empty())
				memcpy(frame.data() + 8, payload.data(), payload.size());
			return Send(frame.data(), frame.size());
		}
	};

	// collects one incoming frame across several partial reads
	struct FrameAssembler
	{
		uint8_t header[8]{};
		size_t have = 0; // bytes of header + payload received so far
		std::vector<uint8_t> payload;

		uint32_t Field(int i) const
		{
			return (uint32_t)header[i] | ((uint32_t)header[i + 1] << 8) | ((uint32_t)header[i + 2] << 16) | ((uint32_t)header[i + 3] << 24);
		}
		uint32_t Opcode() const { return Field(0); }
		uint32_t Length() const { return Field(4); }
		void Reset() { have = 0; }
	};
}

DiscordRPCLite::DiscordRPCLite(const char* applicationId)
{
	m_thread = std::thread(&DiscordRPCLite::Run, this, std::string(applicationId));
}

DiscordRPCLite::~DiscordRPCLite()
{
	m_quit = true;
	if (m_thread.joinable())
		m_thread.join();
}

void DiscordRPCLite::UpdateRichPresence(const DiscordLiteRichPresence& presence)
{
	std::lock_guard guard(m_lock);
	m_activity = presence;
	m_pending = true;
}

void DiscordRPCLite::ClearRichPresence()
{
	std::lock_guard guard(m_lock);
	m_activity = {};
	m_pending = false;
}

bool DiscordRPCLite::TakePendingPresence(DiscordLiteRichPresence& out)
{
	std::lock_guard guard(m_lock);
	if (!m_pending)
		return false;
	out = m_activity;
	m_pending = false;
	return true;
}

void DiscordRPCLite::Run(std::string applicationId)
{
	enum class Link
	{
		Offline,
		HandshakeSent, // waiting for the READY dispatch
		Ready,
	};
	Link link = Link::Offline;
	IpcChannel channel;
	FrameAssembler incoming;
	uint64_t nonce = 1;
	int64_t lastAttempt = 0;
	int failedAttempts = 0;
	int64_t lastActivitySent = 0;

	auto dropConnection = [&]() {
		channel.Disconnect();
		link = Link::Offline;
		incoming.Reset();
		std::lock_guard guard(m_lock);
		m_pending = true; // send the current activity again on the next connection
	};

	while (!m_quit)
	{
		if (link == Link::Offline)
		{
			// back off: 1s, then 0.5s more per failed attempt, at most 90s
			const int64_t wait = (std::min<int64_t>)(1000 + (int64_t)failedAttempts * 500, 90000);
			const int64_t now = NowMs();
			if (now - lastAttempt < wait)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				continue;
			}
			lastAttempt = now;
			failedAttempts++;
			if (!channel.Connect())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				continue;
			}
			std::string hello = "{\"v\":1,\"client_id\":";
			AppendJsonString(hello, applicationId);
			hello += "}";
			if (!channel.SendFrame(FrameOp::Handshake, hello))
			{
				dropConnection();
				continue;
			}
			link = Link::HandshakeSent;
		}

		if (link == Link::Ready && NowMs() - lastActivitySent >= kPresenceInterval)
		{
			DiscordLiteRichPresence activity{};
			if (TakePendingPresence(activity))
			{
				channel.SendFrame(FrameOp::Frame, BuildActivityCommand(activity, nonce++));
				lastActivitySent = NowMs();
			}
		}

		// read the next piece of the current frame
		int got;
		if (incoming.have < 8)
			got = channel.Receive(incoming.header + incoming.have, 8 - incoming.have, 200);
		else
		{
			const uint32_t length = incoming.Length();
			if (length >= kMaxPayload)
			{
				dropConnection();
				continue;
			}
			if (incoming.payload.size() < length)
				incoming.payload.resize(length);
			const size_t done = incoming.have - 8;
			got = length > done ? channel.Receive(incoming.payload.data() + done, length - done, 1000) : 0;
		}
		if (got < 0)
		{
			dropConnection();
			continue;
		}
		incoming.have += (size_t)got;
		if (incoming.have < 8 || incoming.have != 8 + (size_t)incoming.Length())
			continue;

		// complete frame
		const std::string_view body((const char*)incoming.payload.data(), incoming.Length());
		const uint32_t op = incoming.Opcode();
		incoming.Reset();
		if (op == (uint32_t)FrameOp::Frame)
		{
			TopLevelReader reader(body);
			const std::optional<std::string> cmd = reader.Find("cmd");
			const std::optional<std::string> evt = reader.Find("evt");
			if (link == Link::HandshakeSent && cmd && evt && EqualsIgnoreCase(*cmd, "DISPATCH") && EqualsIgnoreCase(*evt, "READY"))
			{
				link = Link::Ready;
				failedAttempts = 0;
			}
		}
		else if (op == (uint32_t)FrameOp::Ping)
			channel.SendFrame(FrameOp::Pong, body);
		else if (op == (uint32_t)FrameOp::Close)
			dropConnection();
		if (incoming.payload.size() > 2048)
		{
			incoming.payload.resize(2048);
			incoming.payload.shrink_to_fit();
		}
	}

	// leave no stale activity behind
	if (channel.Connected())
	{
		channel.SendFrame(FrameOp::Frame, BuildActivityCommand({}, nonce++));
		std::this_thread::sleep_for(std::chrono::milliseconds(75));
	}
}
