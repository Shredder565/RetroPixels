// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// std::from_chars for float and double, for standard libraries that don't
// have it yet (libc++ before LLVM 20). It parses with the C library under
// the "C" locale and keeps from_chars' rules: no leading spaces or '+', no
// "0x" prefix, and a value out of range is reported and not stored.

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <locale.h>
#include <string>
#include <system_error>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

template<typename T>
std::from_chars_result ParseFloatChars(const char* first, const char* last, T& out)
{
	static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", (locale_t)0);
	const char* p = first;
	if (p != last && *p == '-')
		p++;
	if (p == last || !(std::isdigit((unsigned char)*p) || *p == '.' || *p == 'i' || *p == 'I' || *p == 'n' || *p == 'N'))
		return {first, std::errc::invalid_argument};
	// "0x..." reads as just the 0, as from_chars does
	std::string text(first, last);
	if (p + 1 < last && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		text.resize(p + 1 - first);
	char* end = nullptr;
	errno = 0;
	const double v = strtod_l(text.c_str(), &end, cLocale);
	const char* stop = first + (end - text.c_str());
	if (stop == first)
		return {first, std::errc::invalid_argument};
	if (errno == ERANGE)
		return {stop, std::errc::result_out_of_range};
	out = static_cast<T>(v);
	return {stop, std::errc()};
}
