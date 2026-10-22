// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "util/helpers/helpers.h"

// runtime_error whose message includes the last OS error (GetLastError()/errno)
class SystemException : public std::runtime_error
{
public:
	SystemException()
		: std::runtime_error(GetSystemErrorMessage()), m_error_code(GetExceptionError()) {}

	SystemException(const std::exception& ex)
		: std::runtime_error(GetSystemErrorMessage(ex)), m_error_code(GetExceptionError()) {}

	SystemException(const std::error_code& ec)
		: std::runtime_error(GetSystemErrorMessage(ec)), m_error_code(GetExceptionError()) {}

	[[nodiscard]] DWORD GetErrorCode() const { return m_error_code; }

private:
	DWORD m_error_code;
};
