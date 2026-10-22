// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "Common/unix/FileStream_unix.h"
#include <cstdarg>

namespace
{
	// Returns path itself if it exists. Otherwise every missing component is
	// looked up case-insensitively in its parent directory; components that
	// cannot be found are kept as spelled.
	fs::path resolveCaseInsensitive(const fs::path& path)
	{
		if (fs::exists(path))
			return path;
		fs::path dir = path.parent_path();
		const fs::path name = path.filename();
		if (dir.empty())
			dir = ".";
		else if (!fs::exists(dir))
			dir = resolveCaseInsensitive(dir);
		const std::string wanted = name.string();
		std::error_code ec;
		for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
		{
			if (boost::iequals(entry.path().filename().string(), wanted))
				return entry.path();
		}
		return dir / name;
	}

	template<typename T>
	bool readValue(FileStream& f, T& v)
	{
		return f.readData(&v, sizeof(T)) == sizeof(T);
	}
}

FileStream::FileStream(const fs::path& path, bool openExisting, bool writable)
{
	m_path = resolveCaseInsensitive(path);
	std::ios_base::openmode mode = std::ios_base::in | std::ios_base::binary;
	if (!openExisting)
		mode |= std::ios_base::out | std::ios_base::trunc;
	else if (writable)
		mode |= std::ios_base::out;
	m_fileStream.open(m_path, mode);
	m_isValid = m_fileStream.is_open();
	// directories can be opened as streams on some systems, reject them
	if (m_isValid && fs::is_directory(path))
	{
		m_fileStream.close();
		m_isValid = false;
	}
}

FileStream::~FileStream()
{
	if (m_isValid)
		m_fileStream.close();
}

FileStream* FileStream::openFile2(const fs::path& path, bool allowWrite)
{
	auto* f = new FileStream(path, true, allowWrite);
	if (!f->m_isValid)
	{
		delete f;
		return nullptr;
	}
	return f;
}

FileStream* FileStream::openFile(std::string_view path)
{
	return openFile2(fs::path(path), false);
}

FileStream* FileStream::openFile(const wchar_t* path, bool allowWrite)
{
	return openFile2(fs::path(path), allowWrite);
}

FileStream* FileStream::createFile2(const fs::path& path)
{
	auto* f = new FileStream(path, false, false);
	if (!f->m_isValid)
	{
		delete f;
		return nullptr;
	}
	return f;
}

FileStream* FileStream::createFile(const wchar_t* path)
{
	return createFile2(fs::path(path));
}

FileStream* FileStream::createFile(std::string_view path)
{
	return createFile2(fs::path(path));
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

uint64 FileStream::currentPosition()
{
	m_fileStream.clear(); // a short read sets eof/fail, which would block tell and seek
	const std::streampos pos = m_prevOperationWasWrite ? m_fileStream.tellp() : m_fileStream.tellg();
	return pos < 0 ? 0 : (uint64)pos;
}

void FileStream::SetPosition(uint64 pos)
{
	cemu_assert(m_isValid);
	m_fileStream.clear();
	if (m_prevOperationWasWrite)
		m_fileStream.seekp((std::streamoff)pos, std::ios::beg);
	else
		m_fileStream.seekg((std::streamoff)pos, std::ios::beg);
}

uint64 FileStream::GetSize()
{
	cemu_assert(m_isValid);
	m_fileStream.clear();
	const std::streampos restore = m_fileStream.tellg();
	m_fileStream.seekg(0, std::ios::end);
	const std::streampos end = m_fileStream.tellg();
	m_fileStream.seekg(restore, std::ios::beg);
	return (uint64)end;
}

bool FileStream::SetEndOfFile()
{
	cemu_assert(m_isValid);
	const uint64 length = currentPosition();
	m_fileStream.flush();
	std::error_code ec;
	fs::resize_file(m_path, length, ec);
	return !ec;
}

void FileStream::extract(std::vector<uint8>& data)
{
	const uint64 size = GetSize();
	SetPosition(0);
	data.resize(size);
	readData(data.data(), (uint32)size);
}

void FileStream::Flush()
{
	m_fileStream.flush();
}

void FileStream::SyncReadWriteSeek(bool nextOpIsWrite)
{
	if (m_prevOperationWasWrite == nextOpIsWrite)
		return;
	const uint64 pos = currentPosition();
	if (nextOpIsWrite)
		m_fileStream.seekp((std::streamoff)pos, std::ios::beg);
	else
		m_fileStream.seekg((std::streamoff)pos, std::ios::beg);
	m_prevOperationWasWrite = nextOpIsWrite;
}

uint32 FileStream::readData(void* data, uint32 length)
{
	SyncReadWriteSeek(false);
	m_fileStream.read((char*)data, length);
	return (uint32)m_fileStream.gcount();
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
	SyncReadWriteSeek(true);
	m_fileStream.write((const char*)data, length);
	return length;
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
		out->Flush();
	}

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
