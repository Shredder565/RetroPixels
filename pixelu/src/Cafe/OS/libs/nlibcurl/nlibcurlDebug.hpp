// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Optional dumping of the HTTP traffic games send through nlibcurl
// (enabled with ActiveSettings::DumpLibcurlRequestsEnabled()).
//
// Layout on disk, relative to the working directory:
//   dump/curl/session<id>/request<NNNN>_param.txt          options set for the request
//   dump/curl/session<id>/request<NNNN>_responseHeaders.txt
//   dump/curl/session<id>/request<NNNN>_responseRaw.bin    response body
#pragma once
#include <atomic>
#include <ctime>
#include <memory>
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"

namespace nlibcurl
{
	class RequestDump
	{
	public:
		~RequestDump() { Close(); }

		// options are being collected for a new request; opens the parameter file
		void BeginIfIdle()
		{
			if (!ActiveSettings::DumpLibcurlRequestsEnabled() || m_pendingIndex != 0)
				return;
			m_pendingIndex = s_nextIndex.fetch_add(1);
			m_param.reset(FileStream::createFile2(PathFor(m_pendingIndex, "param.txt")));
			if (m_param)
			{
				std::time_t now = std::time(nullptr);
				std::tm local{};
#if BOOST_OS_WINDOWS
				localtime_s(&local, &now);
#else
				localtime_r(&now, &local);
#endif
				m_param->writeStringFmt("Request %u %04d-%02d-%02d %02d:%02d:%02d\r\n", m_pendingIndex,
					local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
			}
		}

		// the request is being sent; responses now belong to it
		void Submit()
		{
			m_responseIndex = m_pendingIndex;
			m_pendingIndex = 0;
			m_param.reset();
			m_headers.reset();
			m_body.reset();
		}

		void LogOption(const char* name, const char* value)
		{
			BeginIfIdle();
			if (m_param)
				m_param->writeStringFmt("SetOpt %s: %s\r\n", name, value ? value : "NULL");
		}

		void LogOption(const char* name, uint32 guestAddress)
		{
			BeginIfIdle();
			if (m_param)
				m_param->writeStringFmt("SetOpt %s: 0x%08x\r\n", name, guestAddress);
		}

		void ResponseHeader(const void* data, size_t length)
		{
			Append(m_headers, "responseHeaders.txt", data, length);
		}

		void ResponseBody(const void* data, size_t length)
		{
			Append(m_body, "responseRaw.bin", data, length);
		}

		void Close()
		{
			m_param.reset();
			m_headers.reset();
			m_body.reset();
		}

	private:
		void Append(std::unique_ptr<FileStream>& file, const char* suffix, const void* data, size_t length)
		{
			if (!ActiveSettings::DumpLibcurlRequestsEnabled() || m_responseIndex == 0)
				return;
			if (!file)
				file.reset(FileStream::createFile2(PathFor(m_responseIndex, suffix)));
			if (file)
				file->writeData(data, (sint32)length);
		}

		static fs::path SessionDir()
		{
			static const fs::path dir = []() {
				uint32 id = (uint32)std::time(nullptr);
				if (id == 0)
					id = 1;
				fs::path p = fs::path("dump") / "curl" / fmt::format("session{}", id);
				std::error_code ec;
				fs::create_directories(p, ec);
				return p;
			}();
			return dir;
		}

		static fs::path PathFor(uint32 index, const char* suffix)
		{
			return SessionDir() / fmt::format("request{:04}_{}", index, suffix);
		}

		inline static std::atomic<uint32> s_nextIndex{1};

		uint32 m_pendingIndex{0};
		uint32 m_responseIndex{0};
		std::unique_ptr<FileStream> m_param;
		std::unique_ptr<FileStream> m_headers;
		std::unique_ptr<FileStream> m_body;
	};
}
