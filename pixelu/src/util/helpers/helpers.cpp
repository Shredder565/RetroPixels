// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "helpers.h"

#include <random>

#include "config/ActiveSettings.h"

#include <zlib.h>

#if BOOST_OS_WINDOWS
#include <TlHelp32.h>
#endif

// --- trimming ---

std::string& ltrim(std::string& str, const std::string& chars)
{
	const size_t first = str.find_first_not_of(chars);
	str.erase(0, first == std::string::npos ? str.size() : first);
	return str;
}

std::string& rtrim(std::string& str, const std::string& chars)
{
	const size_t last = str.find_last_not_of(chars);
	str.erase(last == std::string::npos ? 0 : last + 1);
	return str;
}

std::string& trim(std::string& str, const std::string& chars)
{
	rtrim(str, chars);
	return ltrim(str, chars);
}

std::string_view& ltrim(std::string_view& str, const std::string& chars)
{
	const size_t first = str.find_first_not_of(chars);
	str.remove_prefix(first == std::string_view::npos ? str.size() : first);
	return str;
}

std::string_view& rtrim(std::string_view& str, const std::string& chars)
{
	const size_t last = str.find_last_not_of(chars);
	str.remove_suffix(last == std::string_view::npos ? str.size() : str.size() - last - 1);
	return str;
}

std::string_view& trim(std::string_view& str, const std::string& chars)
{
	rtrim(str, chars);
	return ltrim(str, chars);
}

std::string ltrim_copy(const std::string& s)
{
	std::string copy = s;
	return ltrim(copy);
}

std::string rtrim_copy(const std::string& s)
{
	std::string copy = s;
	return rtrim(copy);
}

// --- OS error messages ---

#if BOOST_OS_WINDOWS
std::string GetSystemErrorMessage(DWORD error_code)
{
	if (error_code == ERROR_SUCCESS)
		return {};
	char* text = nullptr;
	const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	FormatMessageA(flags, nullptr, error_code, 0, (LPSTR)&text, 0, nullptr);
	if (!text)
		return fmt::format("{}: {:#x}", _tr("Error code"), error_code);
	std::string message = fmt::format("{}: {}", _tr("Error"), text);
	LocalFree(text);
	return message;
}

std::string GetSystemErrorMessage()
{
	return GetSystemErrorMessage(GetLastError());
}
#else
std::string GetSystemErrorMessage()
{
	return {};
}
#endif

std::string GetSystemErrorMessage(const std::exception& ex)
{
	const std::string osText = GetSystemErrorMessage();
	return osText.empty() ? std::string(ex.what()) : fmt::format("{}\n{}", osText, ex.what());
}

std::string GetSystemErrorMessage(const std::error_code& ec)
{
	const std::string osText = GetSystemErrorMessage();
	return osText.empty() ? ec.message() : fmt::format("{}\n{}", osText, ec.message());
}

// --- thread names ---

#if BOOST_OS_WINDOWS && defined(_MSC_VER)
namespace
{
	// legacy way of naming a thread for an attached Visual Studio debugger
	void NameThreadForDebugger(const char* name)
	{
		constexpr DWORD kSetThreadNameException = 0x406D1388;
#pragma pack(push, 8)
		struct
		{
			DWORD type; // 0x1000
			LPCSTR name;
			DWORD threadId; // -1 for the calling thread
			DWORD flags;
		} info{ 0x1000, name, GetCurrentThreadId(), 0 };
#pragma pack(pop)
#pragma warning(push)
#pragma warning(disable : 6320 6322)
		__try
		{
			RaiseException(kSetThreadNameException, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
#pragma warning(pop)
	}
}
#endif

void SetThreadName(const char* name)
{
#if BOOST_OS_WINDOWS
	// SetThreadDescription exists from Windows 10 1607, look it up at runtime
	using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const SetThreadDescriptionFn s_setDescription =
		(SetThreadDescriptionFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
	if (s_setDescription)
	{
		const std::wstring wideName = boost::nowide::widen(name);
		s_setDescription(GetCurrentThread(), wideName.c_str());
	}
#ifdef _MSC_VER
	NameThreadForDebugger(name);
#endif
#elif BOOST_OS_MACOS
	pthread_setname_np(name);
#else
	// Linux limits thread names to 15 characters plus the terminator
	const size_t length = std::strlen(name);
	if (length > 15)
		cemuLog_log(LogType::Force, "Thread name \"{}\" is longer than 15 characters and gets truncated", name);
	const std::string shortName(name, std::min<size_t>(length, 15));
	pthread_setname_np(pthread_self(), shortName.c_str());
#endif
}

// --- Windows version and parent process ---

#if BOOST_OS_WINDOWS
std::pair<DWORD, DWORD> GetWindowsVersion()
{
	// RtlGetVersion reports the real version, unlike GetVersionEx without a manifest
	using RtlGetVersionFn = LONG(WINAPI*)(POSVERSIONINFOEXW);
	static const RtlGetVersionFn s_rtlGetVersion = (RtlGetVersionFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
	cemu_assert(s_rtlGetVersion);
	OSVERSIONINFOEXW info{};
	info.dwOSVersionInfoSize = sizeof(info);
	s_rtlGetVersion(&info);
	return { info.dwMajorVersion, info.dwMinorVersion };
}

bool IsWindows81OrGreater()
{
	const auto [major, minor] = GetWindowsVersion();
	return major > 6 || (major == 6 && minor >= 3);
}

bool IsWindows10OrGreater()
{
	return GetWindowsVersion().first >= 10;
}
#endif

fs::path GetParentProcess()
{
	fs::path parentImage;
#if BOOST_OS_WINDOWS
	const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
		return parentImage;
	const DWORD ownPid = GetCurrentProcessId();
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
	{
		if (entry.th32ProcessID != ownPid)
			continue;
		const HANDLE parent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ParentProcessID);
		if (parent)
		{
			wchar_t imagePath[MAX_PATH];
			DWORD length = MAX_PATH;
			if (QueryFullProcessImageNameW(parent, 0, imagePath, &length) && length > 0)
				parentImage = std::wstring(imagePath, length);
			CloseHandle(parent);
		}
		break;
	}
	CloseHandle(snapshot);
#endif
	return parentImage;
}

// --- misc ---

uint32_t GetPhysicalCoreCount()
{
#if BOOST_OS_WINDOWS
	static const uint32_t s_count = []() -> uint32_t {
		uint32_t logical = std::thread::hardware_concurrency();
		DWORD bytes = 0;
		GetLogicalProcessorInformation(nullptr, &bytes);
		if (bytes == 0)
			return logical;
		std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
		if (!GetLogicalProcessorInformation(info.data(), &bytes))
			return logical;
		uint32_t cores = 0;
		for (size_t i = 0; i < bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++)
		{
			if (info[i].Relationship == RelationProcessorCore)
				cores++;
		}
		return (cores > 0 && cores < logical) ? cores : logical;
	}();
	return s_count;
#else
	return std::thread::hardware_concurrency();
#endif
}

bool TestWriteAccess(const fs::path& p)
{
	std::error_code ec;
	if (!fs::exists(p, ec) || !fs::is_directory(p, ec))
		return false;
	// a few attempts in case the random name is taken
	for (int attempt = 0; attempt < 3; attempt++)
	{
		const fs::path probe = p / fmt::format("_{}.tmp", GenerateRandomString(8));
		if (fs::exists(probe, ec))
			continue;
		{
			std::ofstream file(probe);
			if (!file.is_open())
				return false;
		}
		fs::remove(probe, ec);
		return true;
	}
	return false;
}

fs::path MakeRelativePath(const fs::path& base, const fs::path& path)
{
	try
	{
		return fs::relative(path, base);
	}
	catch (const std::exception&)
	{
		return path;
	}
}

#ifdef HAS_DIRECTINPUT
bool GUIDFromString(const char* string, GUID& guid)
{
	unsigned long data1;
	unsigned int data2, data3, b[8];
	const int fields = sscanf_s(string, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		&data1, &data2, &data3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
	if (fields != 11)
		return false;
	guid.Data1 = data1;
	guid.Data2 = (unsigned short)data2;
	guid.Data3 = (unsigned short)data3;
	for (int i = 0; i < 8; i++)
		guid.Data4[i] = (unsigned char)b[i];
	return true;
}

std::string StringFromGUID(const GUID& guid)
{
	return fmt::format("{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
		(uint32)guid.Data1, guid.Data2, guid.Data3,
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

std::wstring WStringFromGUID(const GUID& guid)
{
	return boost::nowide::widen(StringFromGUID(guid));
}
#endif

namespace
{
	template<typename TPiece>
	std::vector<TPiece> SplitAt(std::string_view str, char delimiter)
	{
		std::vector<TPiece> pieces;
		size_t start = 0;
		while (true)
		{
			const size_t end = str.find(delimiter, start);
			if (end == std::string_view::npos)
			{
				pieces.emplace_back(str.substr(start));
				return pieces;
			}
			pieces.emplace_back(str.substr(start, end - start));
			start = end + 1;
		}
	}
}

std::vector<std::string_view> TokenizeView(std::string_view str, char delimiter)
{
	return SplitAt<std::string_view>(str, delimiter);
}

std::vector<std::string> Tokenize(std::string_view str, char delimiter)
{
	return SplitAt<std::string>(str, delimiter);
}

std::string GenerateRandomString(size_t length)
{
	return GenerateRandomString(length, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890");
}

std::string GenerateRandomString(const size_t length, const std::string_view characters)
{
	cemu_assert_debug(!characters.empty());
	std::random_device seed;
	std::mt19937 rng(seed());
	std::uniform_int_distribution<size_t> pick(0, characters.size() - 1);
	std::string result(length, '\0');
	for (char& c : result)
		c = characters[pick(rng)];
	return result;
}

std::optional<std::vector<uint8>> zlibDecompress(const std::vector<uint8>& compressed, size_t sizeHint)
{
	z_stream zs{};
	zs.next_in = (Bytef*)compressed.data();
	zs.avail_in = (uInt)compressed.size();
	// windowBits 32: take the window size from the header and accept zlib or gzip
	if (inflateInit2(&zs, 32) != Z_OK)
		return std::nullopt;
	std::vector<uint8> output;
	size_t chunk = sizeHint;
	int status = Z_OK;
	while (status != Z_STREAM_END)
	{
		const size_t produced = (size_t)zs.total_out;
		output.resize(produced + chunk);
		zs.next_out = output.data() + produced;
		zs.avail_out = (uInt)chunk;
		status = inflate(&zs, Z_NO_FLUSH);
		if (status != Z_OK && status != Z_STREAM_END)
		{
			inflateEnd(&zs);
			return std::nullopt;
		}
		chunk *= 2;
	}
	output.resize((size_t)zs.total_out);
	inflateEnd(&zs);
	return output;
}
