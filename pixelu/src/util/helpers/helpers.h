// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include <charconv>
#include <filesystem>
#include <string_view>
#include <set>

#include "util/math/vector2.h"
#include "util/math/vector3.h"

#ifdef __clang__
#include "Common/unix/fast_float.h"
#endif

// range adaptor for iterating a container backwards in a range-based for
template<typename T>
class reverse_itr
{
public:
	explicit reverse_itr(T& range) : m_range{ range } {}
	auto begin() const { return std::rbegin(m_range); }
	auto end() const { return std::rend(m_range); }

private:
	T& m_range;
};

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

template<typename T>
T deg2rad(T v) { return v * static_cast<T>(M_PI) / static_cast<T>(180); }
template<typename T>
T rad2deg(T v) { return v * static_cast<T>(180) / static_cast<T>(M_PI); }

template<typename T>
Vector3<T> deg2rad(const Vector3<T>& v) { return { deg2rad(v.x), deg2rad(v.y), deg2rad(v.z) }; }
template<typename T>
Vector3<T> rad2deg(const Vector3<T>& v) { return { rad2deg(v.x), rad2deg(v.y), rad2deg(v.z) }; }

template<typename T>
Vector2<T> deg2rad(const Vector2<T>& v) { return { deg2rad(v.x), deg2rad(v.y) }; }
template<typename T>
Vector2<T> rad2deg(const Vector2<T>& v) { return { rad2deg(v.x), rad2deg(v.y) }; }

// physical (not logical) core count where the OS reports it, else std::thread::hardware_concurrency()
uint32_t GetPhysicalCoreCount();

// true if p is an existing directory in which a file can be created
bool TestWriteAccess(const fs::path& p);

// path relative to base, or path itself if that fails
fs::path MakeRelativePath(const fs::path& base, const fs::path& path);

#ifdef HAS_DIRECTINPUT
bool GUIDFromString(const char* string, GUID& guid);
std::string StringFromGUID(const GUID& guid);
std::wstring WStringFromGUID(const GUID& guid);
#endif

// split at every delimiter; empty pieces are kept, so the result has (number of delimiters + 1) entries
std::vector<std::string_view> TokenizeView(std::string_view string, char delimiter);
std::vector<std::string> Tokenize(std::string_view string, char delimiter);

std::string ltrim_copy(const std::string& s);
std::string rtrim_copy(const std::string& s);
std::string& ltrim(std::string& str, const std::string& chars = "\t\n\v\f\r ");
std::string& rtrim(std::string& str, const std::string& chars = "\t\n\v\f\r ");
std::string& trim(std::string& str, const std::string& chars = "\t\n\v\f\r ");

std::string_view& ltrim(std::string_view& str, const std::string& chars = "\t\n\v\f\r ");
std::string_view& rtrim(std::string_view& str, const std::string& chars = "\t\n\v\f\r ");
std::string_view& trim(std::string_view& str, const std::string& chars = "\t\n\v\f\r ");

// letters and digits
std::string GenerateRandomString(size_t length);
std::string GenerateRandomString(size_t length, std::string_view characters);

// text for the last OS error (empty on non-Windows systems)
std::string GetSystemErrorMessage();
std::string GetSystemErrorMessage(DWORD error_code);
std::string GetSystemErrorMessage(const std::exception& ex);
std::string GetSystemErrorMessage(const std::error_code& ec);

// visitor helper for std::visit with several lambdas
template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// equality with a small absolute tolerance for floating point, strcmp for C strings
template<typename T>
bool equals(T v1, T v2)
{
	if constexpr (std::is_floating_point_v<T>)
		return std::abs(v1 - v2) < (T)0.000001;
	else if constexpr (std::is_same_v<T, const char*>)
		return strcmp(v1, v2) == 0;
	else
		return v1 == v2;
}

// Integer from text in the given base, after leading whitespace. For base 16
// a "0x" prefix is allowed, also after a minus sign; with a minus sign the
// parsed value is negated. Anything unparsable gives T{}.
template<typename T>
T ConvertString(std::string_view str, sint32 base)
{
	static_assert(std::is_integral_v<T>);
	if (str.empty())
		return {};
	ltrim(str);
	if (str.empty())
		return {};
	T value;
	if (base != 16)
	{
		if (std::from_chars(str.data(), str.data() + str.size(), value, base).ec == std::errc())
			return value;
		return {};
	}
	const bool negative = str[0] == '-';
	const size_t prefixPos = negative ? 1 : 0;
	if (str.size() >= prefixPos + 2 && str[prefixPos] == '0' && (str[prefixPos + 1] == 'x' || str[prefixPos + 1] == 'X'))
		str.remove_prefix(prefixPos + 2);
	if (std::from_chars(str.data(), str.data() + str.size(), value, 16).ec != std::errc())
		return {};
	if (negative)
	{
		if constexpr (std::is_unsigned_v<T>)
			value = static_cast<T>(-static_cast<std::make_signed_t<T>>(value));
		else
			value = -value;
	}
	return value;
}

// Value from text, after leading whitespace:
//  bool       "1" or "true" (any case)
//  float      decimal, a leading '+' is allowed
//  enum       as its underlying integer
//  integer    decimal, or hex with a "0x" prefix (optionally after '-')
// Anything unparsable gives T{}.
template<typename T>
T ConvertString(std::string_view str)
{
	if (str.empty())
		return {};
	ltrim(str);
	if constexpr (std::is_same_v<T, bool>)
	{
		return str == "1" || boost::iequals(str, "true");
	}
	else if constexpr (std::is_floating_point_v<T>)
	{
		ltrim(str, "+");
		T value;
#ifdef __clang__
		if (fast_float::from_chars(str.data(), str.data() + str.size(), value).ec == std::errc())
			return value;
#else
		if (std::from_chars(str.data(), str.data() + str.size(), value).ec == std::errc())
			return value;
#endif
		return {};
	}
	else if constexpr (std::is_enum_v<T>)
	{
		return (T)ConvertString<std::underlying_type_t<T>>(str);
	}
	else
	{
		if (str.empty())
			return {};
		const size_t prefixPos = str[0] == '-' ? 1 : 0;
		const bool hex = str.size() >= prefixPos + 2 && str[prefixPos] == '0' && (str[prefixPos + 1] == 'x' || str[prefixPos + 1] == 'X');
		return ConvertString<T>(str, hex ? 16 : 10);
	}
}

template<typename T>
constexpr T DegToRad(T deg) { return (T)((double)deg * M_PI / 180); }
template<typename T>
constexpr T RadToDeg(T rad) { return (T)((double)rad * 180 / M_PI); }

// locale-independent stream conversion
template<typename T>
std::string ToString(T value)
{
	std::ostringstream out;
	out.imbue(std::locale("C"));
	out << value;
	return out.str();
}

template<typename T>
T FromString(std::string value)
{
	std::istringstream in(value);
	in.imbue(std::locale("C"));
	T result;
	in >> result;
	return result;
}

// removes later duplicates, keeps the first occurrence of every value; returns the new size
template<typename T>
size_t RemoveDuplicatesKeepOrder(std::vector<T>& vec)
{
	std::set<T> seen;
	size_t kept = 0;
	for (size_t i = 0; i < vec.size(); i++)
	{
		if (!seen.insert(vec[i]).second)
			continue;
		if (kept != i)
			vec[kept] = std::move(vec[i]);
		kept++;
	}
	vec.erase(vec.begin() + kept, vec.end());
	return vec.size();
}

// names the calling thread for debuggers and system tools (truncated to 15 characters on Linux)
void SetThreadName(const char* name);

inline uint64 MakeU64(uint32 high, uint32 low)
{
	return ((uint64)high << 32) | (uint64)low;
}

// true if sv can be used as a single file name on every host system
static bool IsValidFilename(std::string_view sv)
{
	for (char ch : sv)
	{
		const uint8 c = (uint8)ch;
		if (c < 0x20)
			return false;
		switch (c)
		{
		case '.': case '#': case '/': case '\\': case '<': case '>': case '|': case ':': case '"':
			return false;
		default:
			break;
		}
	}
	return sv.empty() || (sv.back() != ' ' && sv.back() != '.');
}

// Windows only: OS version as (major, minor)
std::pair<DWORD, DWORD> GetWindowsVersion();
bool IsWindows81OrGreater();
bool IsWindows10OrGreater();

// Windows only: executable of the parent process (empty elsewhere)
fs::path GetParentProcess();

// inflates zlib or gzip data; nullopt on any error
std::optional<std::vector<uint8>> zlibDecompress(const std::vector<uint8>& compressed, size_t sizeHint = 32 * 1024);
