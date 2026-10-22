// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/precompiled.h"

// Binary file access on top of std::fstream. Paths are resolved
// case-insensitively when the exact spelling does not exist, so that game
// files with mismatching case can still be found on case-sensitive file
// systems.
class FileStream
{
public:
	// open an existing file (read-only unless allowWrite); nullptr on failure
	static FileStream* openFile(std::string_view path);
	static FileStream* openFile(const wchar_t* path, bool allowWrite = false);
	static FileStream* openFile2(const fs::path& path, bool allowWrite = false);

	// create or truncate a file for reading and writing; nullptr on failure
	static FileStream* createFile(const wchar_t* path);
	static FileStream* createFile(std::string_view path);
	static FileStream* createFile2(const fs::path& path);

	// the whole file, or nullopt if it cannot be read (or is 4GB or larger)
	static std::optional<std::vector<uint8>> LoadIntoMemory(const fs::path& path);

	// writes to a temporary file first and then renames it over path
	static bool WriteFileAtomic(const fs::path& path, std::span<uint8> fileData, bool allowTargetFileRename = false);

	FileStream() {}
	~FileStream();

	void SetPosition(uint64 pos);
	uint64 GetSize();
	bool SetEndOfFile(); // truncates the file at the current position
	void extract(std::vector<uint8>& data); // reads the whole file
	void Flush();

	// raw reads; readData returns the number of bytes read
	uint32 readData(void* data, uint32 length);
	bool readU64(uint64& v);
	bool readU32(uint32& v);
	bool readU16(uint16& v);
	bool readU8(uint8& v);
	// next line without '\r' and '\n'; false at the end of the file
	bool readLine(std::string& line);

	// raw writes
	sint32 writeData(const void* data, sint32 length);
	void writeU64(uint64 v);
	void writeU32(uint32 v);
	void writeU16(uint16 v);
	void writeU8(uint8 v);

	// text output; writeLine appends "\r\n"
	void writeStringFmt(const char* format, ...);
	void writeString(const char* str);
	void writeLine(const char* str);

private:
	FileStream(const fs::path& path, bool openExisting, bool writable);

	// std::fstream shares one position between reads and writes only after
	// an explicit seek, so switching direction re-seeks to the current position
	void SyncReadWriteSeek(bool nextOpIsWrite);
	uint64 currentPosition();

	bool m_isValid{};
	std::fstream m_fileStream;
	bool m_prevOperationWasWrite{false};
	fs::path m_path;
};
