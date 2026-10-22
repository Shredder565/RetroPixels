// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/windows/FileStream_win32.h"
#include <cstdarg>

namespace
{
	constexpr DWORD kShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;

	HANDLE openHandle(const wchar_t* path, DWORD access, DWORD disposition)
	{
		return CreateFileW(path, access, kShareMode, nullptr, disposition, 0, nullptr);
	}

	template<typename T>
	bool readValue(FileStream& f, T& v)
	{
		return f.readData(&v, sizeof(T)) == sizeof(T);
	}
}

FileStream::FileStream(HANDLE hFile) : m_isValid(true), m_hFile(hFile) {}

FileStream::~FileStream()
{
	if (m_isValid)
		CloseHandle(m_hFile);
}

FileStream* FileStream::fromHandle(HANDLE hFile)
{
	if (hFile == INVALID_HANDLE_VALUE)
		return nullptr;
	return new FileStream(hFile);
}

FileStream* FileStream::openFile(const wchar_t* path, bool allowWrite)
{
	const DWORD access = allowWrite ? (FILE_GENERIC_READ | FILE_GENERIC_WRITE) : FILE_GENERIC_READ;
	return fromHandle(openHandle(path, access, OPEN_EXISTING));
}

FileStream* FileStream::openFile(std::string_view path)
{
	const std::wstring widePath = boost::nowide::widen(path.data(), path.size());
	return openFile(widePath.c_str(), false);
}

FileStream* FileStream::openFile2(const fs::path& path, bool allowWrite)
{
	return openFile(path.generic_wstring().c_str(), allowWrite);
}

FileStream* FileStream::createFile(const wchar_t* path)
{
	return fromHandle(openHandle(path, FILE_GENERIC_READ | FILE_GENERIC_WRITE, CREATE_ALWAYS));
}

FileStream* FileStream::createFile(std::string_view path)
{
	const std::wstring widePath = boost::nowide::widen(path.data(), path.size());
	return createFile(widePath.c_str());
}

FileStream* FileStream::createFile2(const fs::path& path)
{
	return createFile(path.generic_wstring().c_str());
}

std::optional<std::vector<uint8>> FileStream::LoadIntoMemory(const fs::path& path)
{
	std::unique_ptr<FileStream> f(openFile2(path));
	if (!f)
		return std::nullopt;
	const uint64 size = f->GetSize();
	if (size > 0xFFFFFFFFull)
		return std::nullopt;
	std::vector<uint8> data(size);
	if (f->readData(data.data(), (uint32)size) != (uint32)size)
		return std::nullopt;
	return data;
}

void FileStream::SetPosition(uint64 pos)
{
	LARGE_INTEGER target;
	target.QuadPart = (LONGLONG)pos;
	SetFilePointerEx(m_hFile, target, nullptr, FILE_BEGIN);
}

uint64 FileStream::GetSize()
{
	LARGE_INTEGER size;
	if (!GetFileSizeEx(m_hFile, &size))
		return 0;
	return (uint64)size.QuadPart;
}

bool FileStream::SetEndOfFile()
{
	return ::SetEndOfFile(m_hFile) != FALSE;
}

void FileStream::extract(std::vector<uint8>& data)
{
	const uint64 size = GetSize();
	data.resize((size_t)size);
	SetPosition(0);
	readData(data.data(), (uint32)size);
}

void FileStream::Flush()
{
}

uint32 FileStream::readData(void* data, uint32 length)
{
	DWORD transferred = 0;
	ReadFile(m_hFile, data, length, &transferred, nullptr);
	return transferred;
}

bool FileStream::readU64(uint64& v) { return readValue(*this, v); }
bool FileStream::readU32(uint32& v) { return readValue(*this, v); }
bool FileStream::readU16(uint16& v) { return readValue(*this, v); }
bool FileStream::readU8(uint8& v) { return readValue(*this, v); }

bool FileStream::readLine(std::string& line)
{
	line.clear();
	bool gotAny = false;
	uint8 c;
	while (readU8(c))
	{
		gotAny = true;
		if (c == '\n')
			break;
		if (c != '\r')
			line += (char)c;
	}
	return gotAny;
}

sint32 FileStream::writeData(const void* data, sint32 length)
{
	DWORD transferred = 0;
	WriteFile(m_hFile, data, (DWORD)length, &transferred, nullptr);
	return (sint32)transferred;
}

void FileStream::writeU64(uint64 v) { writeData(&v, sizeof(v)); }
void FileStream::writeU32(uint32 v) { writeData(&v, sizeof(v)); }
void FileStream::writeU16(uint16 v) { writeData(&v, sizeof(v)); }
void FileStream::writeU8(uint8 v) { writeData(&v, sizeof(v)); }

void FileStream::writeStringFmt(const char* format, ...)
{
	char text[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	writeString(text);
}

void FileStream::writeString(const char* str)
{
	writeData(str, (sint32)strlen(str));
}

void FileStream::writeLine(const char* str)
{
	writeString(str);
	writeData("\r\n", 2);
}

bool FileStream::WriteFileAtomic(const fs::path& path, std::span<uint8> fileData, bool allowTargetFileRename)
{
	// "name.ext" is written as "name.ext_tmp" first
	fs::path tempPath = path;
	tempPath.replace_extension(_utf8ToPath(_pathToUtf8(path.extension()) + "_tmp"));
	auto logFailure = [&](const std::error_code& err) {
		cemuLog_log(LogType::Force, "Failed to atomically replace file {}, error {}", _pathToUtf8(path), err.message());
	};

	std::error_code ec;
	{
		std::unique_ptr<FileStream> out(createFile2(tempPath));
		if (!out)
			return false;
		if ((size_t)out->writeData(fileData.data(), (sint32)fileData.size()) != fileData.size())
		{
			out.reset();
			fs::remove(tempPath, ec);
			return false;
		}
	} // closing the handle completes the write

	fs::rename(tempPath, path, ec);
	if (!ec)
		return true;
	if (!allowTargetFileRename)
	{
		logFailure(ec);
		fs::remove(tempPath, ec);
		return false;
	}

	// the target may be locked by another process: move it out of the way
	// ("name.ext" -> "name.ext.backup") and retry; not atomic
	fs::path backupPath = path;
	backupPath.replace_extension(_utf8ToPath(_pathToUtf8(path.extension()) + ".backup"));
	fs::rename(path, backupPath, ec);
	if (ec)
	{
		logFailure(ec);
		fs::remove(tempPath, ec);
		return false;
	}
	fs::rename(tempPath, path, ec);
	if (ec)
	{
		logFailure(ec);
		std::error_code undoError;
		fs::rename(backupPath, path, undoError);
		if (undoError)
			cemuLog_log(LogType::Force, "Failed to restore original file {}, error {}", _pathToUtf8(path), undoError.message());
		fs::remove(tempPath, ec);
		return false;
	}
	return true;
}
