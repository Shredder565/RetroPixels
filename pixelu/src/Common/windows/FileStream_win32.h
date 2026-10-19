// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include "Common/precompiled.h"

// Binary file access on top of a Win32 file handle. Files are opened with
// read and write sharing so that other programs can keep them open.
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

	FileStream() = default;
	~FileStream();

	void SetPosition(uint64 pos);
	uint64 GetSize();
	bool SetEndOfFile(); // truncates the file at the current position
	void extract(std::vector<uint8>& data); // reads the whole file
	void Flush(); // no-op: writes go straight to the handle

	// raw reads; readData returns the number of bytes read
	uint32 readData(void* data, uint32 length);
	bool readU64(uint64& v);
	bool readU32(uint32& v);
	bool readU16(uint16& v);
	bool readU8(uint8& v);
	// next line without '\r' and '\n'; false at the end of the file
	bool readLine(std::string& line);

	// raw writes; writeData returns the number of bytes written
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
	FileStream(HANDLE hFile);

	static FileStream* fromHandle(HANDLE hFile);

	bool m_isValid{};
	HANDLE m_hFile{INVALID_HANDLE_VALUE};
};
