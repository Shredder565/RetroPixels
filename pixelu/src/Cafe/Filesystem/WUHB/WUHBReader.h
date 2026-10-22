// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#include <Common/FileStream.h>
#include "RomFSStructs.h"

// Reads files and folders of a .wuhb bundle. Entries are identified by their
// offset in the file or directory table; ROMFS_ENTRY_EMPTY means none.
class WUHBReader
{
  public:
	static WUHBReader* FromPath(const fs::path& path); // nullptr if not a valid bundle

	// unreadable entries come back with every link set to ROMFS_ENTRY_EMPTY and no name
	romfs_direntry_t GetDirEntry(uint32 offset) const;
	romfs_fentry_t GetFileEntry(uint32 offset) const;

	uint64 GetFileSize(uint32 entryOffset) const;
	uint64 ReadFromFile(uint32 entryOffset, uint64 fileOffset, uint64 length, void* buffer) const;

	// entry offset of a file or folder, ROMFS_ENTRY_EMPTY if it does not exist
	uint32 Lookup(const std::filesystem::path& path, bool isFile) const;

  private:
	explicit WUHBReader(std::unique_ptr<FileStream> file) : m_file(std::move(file)) {}

	bool ReadHeader();
	uint32 BucketHead(uint32 hash, bool fileTable) const;
	uint32 FindInBucket(uint32 parent, std::string_view name, bool fileTable) const;
	static uint32 NameHash(uint32 parent, std::string_view name);

	template<typename TEntry>
	TEntry ReadEntry(uint32 offset, uint64 tableOffset, uint64 tableSize, size_t fixedSize) const;

	std::unique_ptr<FileStream> m_file;
	romfs_header_t m_header{};
};
