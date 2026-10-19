// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// PixelU log: one log.txt in the user data folder, written by a background
// thread, plus an optional observer (the logging window). Each message has a
// category (LogType); a category is shown only while its bit is set in the
// active mask. The category numbers are bit positions and are stored in the
// settings file, so they must never change.

extern uint64 s_loggingFlagMask;

enum class LogType : sint32
{
	CoreinitFile = 0,
	GX2 = 1,
	UnsupportedAPI = 2,
	CoreinitThreadSync = 3,
	SoundAPI = 4,
	InputAPI = 5,
	Socket = 6,
	Save = 7,
	CoreinitMem = 8,
	H264 = 9,
	OpenGLLogging = 10,
	TextureCache = 11,
	VulkanValidation = 12,
	NN_NFP = 13,
	Patches = 14,
	CoreinitMP = 15,
	CoreinitThread = 16,
	CoreinitLogging = 17,       // OSReport, OSConsoleWrite and friends
	CoreinitMemoryMapping = 18, // virtual address / mapping APIs
	PPC_IPC = 19,
	NN_AOC = 20,
	NN_PDM = 21,
	CoreinitAlarm = 22,
	NN_OLV = 23,
	NN_FP = 24,
	NN_BOSS = 25,
	NN_SL = 26,
	TextureReadback = 29,
	ProcUi = 39,
	PRUDP = 40,
	nlibcurl = 41,
	NFC = 43,
	NTAG = 44,
	SWKBD = 45,
	Recompiler = 60,
	APIErrors = 61,   // invalid arguments passed to OS libraries (useful for homebrew)
	Placeholder = 62, // never shown
	Force = 63,       // always shown
};

// lets fmt print UTF-8 string views as plain text
template <>
struct fmt::formatter<std::u8string_view> : formatter<string_view>
{
	template <typename FormatContext>
	auto format(std::u8string_view text, FormatContext& ctx)
	{
		return formatter<string_view>::format(string_view(reinterpret_cast<const char*>(text.data()), text.size()), ctx);
	}
};

inline uint64 cemuLog_getFlag(LogType type)
{
	return 1ULL << static_cast<uint64>(type);
}

inline bool cemuLog_isLoggingEnabled(LogType type)
{
	return (s_loggingFlagMask & cemuLog_getFlag(type)) != 0;
}

void cemuLog_setActiveLoggingFlags(uint64 flagMask); // Force is always added

// raw access to log.txt (no category check, no observer)
void cemuLog_writeLineToLog(std::string_view text, bool date = true, bool new_line = true);
inline void cemuLog_writePlainToLog(std::string_view text)
{
	cemuLog_writeLineToLog(text, false, false);
}

// category-checked logging; returns false when the category is off
bool cemuLog_log(LogType type, std::string_view text);
bool cemuLog_log(LogType type, std::u8string_view text);

template<typename... TArgs>
bool cemuLog_log(LogType type, fmt::format_string<TArgs...> formatStr, TArgs&&... args)
{
	if (!cemuLog_isLoggingEnabled(type))
		return false;
	cemuLog_log(type, std::string_view(fmt::format(formatStr, std::forward<TArgs>(args)...)));
	return true;
}

// debug-build-only variants
template<typename... TArgs>
bool cemuLog_logDebug(LogType type, fmt::format_string<TArgs...> formatStr, TArgs&&... args)
{
#ifdef CEMU_DEBUG_ASSERT
	return cemuLog_log(type, formatStr, std::forward<TArgs>(args)...);
#else
	return false;
#endif
}

inline bool cemuLog_logDebug(LogType type, std::string_view text)
{
#ifdef CEMU_DEBUG_ASSERT
	return cemuLog_log(type, text);
#else
	return false;
#endif
}

// log only on the first time this source line is reached
#define cemuLog_logOnce(...) { static bool _rpLogOnceDone = false; if (!_rpLogOnceDone) { _rpLogOnceDone = true; cemuLog_log(__VA_ARGS__); } }
#define cemuLog_logDebugOnce(...) { static bool _rpLogOnceDone = false; if (!_rpLogOnceDone) { _rpLogOnceDone = true; cemuLog_logDebug(__VA_ARGS__); } }

// "OOOO: XX XX XX ..." lines, lineSize bytes per line
void cemuLog_logHexDump(LogType type, const void* data, size_t size, size_t lineSize = 16);

void cemuLog_waitForFlush(); // returns once everything queued so far reached log.txt

bool cemuLog_advancedPPCLoggingEnabled();

fs::path cemuLog_GetLogFilePath();
void cemuLog_createLogFile(bool triggeredByCrash);

// hold the returned lock to keep several lines together in the log
[[nodiscard]] std::unique_lock<std::recursive_mutex> cemuLog_acquire();

// observer interface for the logging window; filter is the category's display name
class LoggingCallbacks
{
public:
	virtual ~LoggingCallbacks() = default;
	virtual void Log(std::string_view filter, std::string_view message) {}
	virtual void Log(std::string_view filter, std::wstring_view message) {}
};

void cemuLog_setCallbacks(LoggingCallbacks* loggingCallbacks);
void cemuLog_clearCallbacks();
