// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "WUHBReader.h"

namespace
{
	uint32 LoadBE32(const uint8* p)
	{
		return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | (uint32)p[3];
	}

	uint64 LoadBE64(const uint8* p)
	{
		return ((uint64)LoadBE32(p) << 32) | LoadBE32(p + 4);
	}

	constexpr size_t kDirEntryFixedSize = 6 * 4;
	constexpr size_t kFileEntryFixedSize = 2 * 4 + 2 * 8 + 2 * 4;

	template<typename TEntry>
	TEntry EmptyEntry()
	{
		TEntry entry{};
		entry.parent = ROMFS_ENTRY_EMPTY;
		entry.listNext = ROMFS_ENTRY_EMPTY;
		entry.hash = ROMFS_ENTRY_EMPTY;
		entry.name_size = 0;
		if constexpr (std::is_same_v<TEntry, romfs_direntry_t>)
		{
			entry.dirListHead = ROMFS_ENTRY_EMPTY;
			entry.fileListHead = ROMFS_ENTRY_EMPTY;
		}
		else
		{
			entry.offset = 0;
			entry.size = 0;
		}
		return entry;
	}

	void DecodeFixed(romfs_direntry_t& entry, const uint8* p)
	{
		entry.parent = LoadBE32(p + 0);
		entry.listNext = LoadBE32(p + 4);
		entry.dirListHead = LoadBE32(p + 8);
		entry.fileListHead = LoadBE32(p + 12);
		entry.hash = LoadBE32(p + 16);
		entry.name_size = LoadBE32(p + 20);
	}

	void DecodeFixed(romfs_fentry_t& entry, const uint8* p)
	{
		entry.parent = LoadBE32(p + 0);
		entry.listNext = LoadBE32(p + 4);
		entry.offset = LoadBE64(p + 8);
		entry.size = LoadBE64(p + 16);
		entry.hash = LoadBE32(p + 24);
		entry.name_size = LoadBE32(p + 28);
	}
}

WUHBReader* WUHBReader::FromPath(const fs::path& path)
{
	std::unique_ptr<FileStream> file(FileStream::openFile2(path));
	if (!file)
		return nullptr;
	std::unique_ptr<WUHBReader> reader(new WUHBReader(std::move(file)));
	if (!reader->ReadHeader())
		return nullptr;
	return reader.release();
}

bool WUHBReader::ReadHeader()
{
	uint8 raw[sizeof(romfs_header_t)];
	m_file->SetPosition(0);
	const uint32 bytesRead = m_file->readData(raw, sizeof(raw));
	if (bytesRead < 4 || std::memcmp(raw, "WUHB", 4) != 0)
		return false;
	if (bytesRead != sizeof(raw))
	{
		cemuLog_log(LogType::Force, "WUHB: header is truncated");
		return false;
	}
	std::memcpy(&m_header.header_magic, raw, 4);
	m_header.header_size = LoadBE32(raw + 4);
	uint64be* tables[] = {&m_header.dir_hash_table_ofs, &m_header.dir_hash_table_size, &m_header.dir_table_ofs, &m_header.dir_table_size,
						  &m_header.file_hash_table_ofs, &m_header.file_hash_table_size, &m_header.file_table_ofs, &m_header.file_table_size,
						  &m_header.file_partition_ofs};
	for (size_t i = 0; i < std::size(tables); i++)
		*tables[i] = LoadBE64(raw + 8 + i * 8);
	if (m_header.dir_hash_table_size < 4 || m_header.file_hash_table_size < 4)
	{
		cemuLog_log(LogType::Force, "WUHB: hash tables are too small");
		return false;
	}
	return true;
}

template<typename TEntry>
TEntry WUHBReader::ReadEntry(uint32 offset, uint64 tableOffset, uint64 tableSize, size_t fixedSize) const
{
	if (offset == ROMFS_ENTRY_EMPTY)
		return EmptyEntry<TEntry>();
	if (offset >= tableSize)
	{
		cemuLog_log(LogType::Force, "WUHB: entry offset 0x{:x} is outside its table", offset);
		return EmptyEntry<TEntry>();
	}
	uint8 fixed[32];
	m_file->SetPosition(tableOffset + offset);
	if (m_file->readData(fixed, (uint32)fixedSize) != fixedSize)
	{
		cemuLog_log(LogType::Force, "WUHB: cannot read the entry at 0x{:x}", offset);
		return EmptyEntry<TEntry>();
	}
	TEntry entry;
	DecodeFixed(entry, fixed);
	// a name longer than the whole table cannot be valid (and must not be allocated)
	if (entry.name_size > tableSize)
	{
		cemuLog_log(LogType::Force, "WUHB: entry name at 0x{:x} is too long", offset);
		return EmptyEntry<TEntry>();
	}
	entry.name.resize(entry.name_size);
	if (m_file->readData(entry.name.data(), entry.name_size) != entry.name_size)
	{
		cemuLog_log(LogType::Force, "WUHB: cannot read the entry name at 0x{:x}", offset);
		return EmptyEntry<TEntry>();
	}
	return entry;
}

romfs_direntry_t WUHBReader::GetDirEntry(uint32 offset) const
{
	return ReadEntry<romfs_direntry_t>(offset, m_header.dir_table_ofs, m_header.dir_table_size, kDirEntryFixedSize);
}

romfs_fentry_t WUHBReader::GetFileEntry(uint32 offset) const
{
	return ReadEntry<romfs_fentry_t>(offset, m_header.file_table_ofs, m_header.file_table_size, kFileEntryFixedSize);
}

uint64 WUHBReader::GetFileSize(uint32 entryOffset) const
{
	return GetFileEntry(entryOffset).size;
}

uint64 WUHBReader::ReadFromFile(uint32 entryOffset, uint64 fileOffset, uint64 length, void* buffer) const
{
	const romfs_fentry_t entry = GetFileEntry(entryOffset);
	const uint64 fileSize = entry.size;
	if (fileOffset >= fileSize)
		return 0;
	const uint64 toRead = std::min(length, fileSize - fileOffset);
	m_file->SetPosition(m_header.file_partition_ofs + entry.offset + fileOffset);
	return m_file->readData(buffer, (uint32)toRead);
}

// RomFS name hash: seeded with the parent's entry offset, case-insensitive
uint32 WUHBReader::NameHash(uint32 parent, std::string_view name)
{
	uint32 hash = parent ^ 123456789u;
	for (char c : name)
	{
		uint8 u = (uint8)c;
		if (u >= 'a' && u <= 'z')
			u = (uint8)(u - 'a' + 'A');
		hash = std::rotr(hash, 5) ^ u;
	}
	return hash;
}

uint32 WUHBReader::BucketHead(uint32 hash, bool fileTable) const
{
	const uint64 tableOffset = fileTable ? m_header.file_hash_table_ofs : m_header.dir_hash_table_ofs;
	const uint64 bucketCount = (fileTable ? m_header.file_hash_table_size : m_header.dir_hash_table_size) / 4;
	const uint64 position = tableOffset + (hash % bucketCount) * 4;
	uint8 raw[4];
	m_file->SetPosition(position);
	if (m_file->readData(raw, 4) != 4)
	{
		cemuLog_log(LogType::Force, "WUHB: cannot read the hash table at 0x{:x}", position);
		return ROMFS_ENTRY_EMPTY;
	}
	return LoadBE32(raw);
}

// The first entry of the bucket with this exact (case-sensitive) name. The
// parent is only part of the hash, entries are not checked against it.
uint32 WUHBReader::FindInBucket(uint32 parent, std::string_view name, bool fileTable) const
{
	uint32 offset = BucketHead(NameHash(parent, name), fileTable);
	while (offset != ROMFS_ENTRY_EMPTY)
	{
		std::string entryName;
		uint32 next;
		if (fileTable)
		{
			romfs_fentry_t entry = GetFileEntry(offset);
			entryName = std::move(entry.name);
			next = entry.hash;
		}
		else
		{
			romfs_direntry_t entry = GetDirEntry(offset);
			entryName = std::move(entry.name);
			next = entry.hash;
		}
		if (entryName == name)
			return offset;
		offset = next;
	}
	return ROMFS_ENTRY_EMPTY;
}

// Walks the path one folder at a time starting at the root folder (the
// folder named ""). A trailing separator ends a folder lookup early; for a
// file lookup only the last name is searched in the file table.
uint32 WUHBReader::Lookup(const std::filesystem::path& path, bool isFile) const
{
	uint32 current = FindInBucket(0, "", false);
	if (current == ROMFS_ENTRY_EMPTY)
		return ROMFS_ENTRY_EMPTY;
	const std::vector<fs::path> parts(path.begin(), path.end());
	for (size_t i = 0; i < parts.size(); i++)
	{
		const fs::path& part = parts[i];
		if (part.empty() && !isFile)
			break;
		if (part == fs::path("/"))
			continue;
		const bool inFileTable = isFile && i + 1 == parts.size();
		current = FindInBucket(current, part.string(), inFileTable);
		if (current == ROMFS_ENTRY_EMPTY)
			return ROMFS_ENTRY_EMPTY;
	}
	return current;
}
